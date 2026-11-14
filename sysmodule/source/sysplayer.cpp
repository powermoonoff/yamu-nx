#include "sysplayer.hpp"
#include "http.hpp"
#include "mp4demux.hpp"
#include "soundcloud.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <malloc.h>
#include <mpg123.h>
#include <syn123.h>
#include <neaacdec.h>

static const u64 MS = 1000000ULL;
// Окно загрузки — кольцо фиксированного размера, выделяется один раз. Раньше это был растущий vector:
// при каждом росте он держал старый и новый буфер сразу (до ~3 МБ), и при двух деках куча в 10 МБ
// кончалась — bad_alloc в потоке загрузки ронял весь модуль.
static const size_t WINDOW_MAX = 1 << 20;
static const size_t PCM_AHEAD = 48000 * 2; // декодер забегает вперёд не больше чем на 2 с
void ymLog(const char *fmt, ...);

// ---------- дека: загрузка и декодирование одного трека в очередь PCM (стерео 48 кГц) ----------

class SysDeck {
public:
    SysDeck(const std::string &src, const std::string &codec, double startSec, int kbps, double durSec);
    ~SysDeck();

    size_t read(s16 *out, size_t frames); // забрать до frames кадров
    size_t available();
    bool decodeDone() const { return decDone_; }
    bool ended() { return decDone_ && finished_ && available() == 0; }
    bool failed() { return decDone_ && !finished_ && available() == 0; }
    double position() const { return framesPlayed_.load() / 48000.0; }
    bool ready() const;
    std::string error() const;

private:
    static void netEntry(void *arg) {
        static_cast<SysDeck *>(arg)->netLoop();
        httpThreadDone(); // соединения через прокси, которые держал этот поток
    }
    static void decodeEntry(void *arg) { static_cast<SysDeck *>(arg)->decodeLoop(); }
    void netLoop();
    void decodeLoop();
    bool decodeMp3();
    bool decodeAdts();
    bool decodeM4a();
    // Взять из окна до max байт; 0 — пока нечего, -1 — данные кончились.
    long pull(u8 *dst, size_t max);
    // PCM любого формата → стерео 48 кГц → очередь деки (ждёт, если забежали вперёд).
    bool emit(const s16 *pcm, size_t frames, unsigned rate, unsigned channels);
    void setError(const std::string &e);

    std::string src_, codec_;
    double startSec_, durSec_;
    int kbps_;
    bool local_, netRunning_;
    Thread netThread_, decThread_;

    mutable std::mutex m_; // окно загрузки
    u8 *ring_ = nullptr;    // WINDOW_MAX байт, malloc: без исключений нехватку видно по nullptr
    size_t head_r_ = 0;     // откуда читать
    size_t fill_ = 0;       // сколько непрочитанного
    bool netDone_ = false;
    std::string error_;

    std::mutex pm_; // готовый звук
    std::vector<s16> pcm_;
    size_t head_ = 0;

    // Пересчёт частоты до 48 кГц (только в потоке декодера): ресемплер syn123 из mpg123 — как swresample
    // в приложении по качеству, а не простая интерполяция.
    syn123_handle *rs_ = nullptr;
    long rsRate_ = 0;
    std::vector<float> fin_, fout_;
    std::vector<s16> conv_, tmp_;

    std::atomic<bool> stop_{false}, decDone_{false}, finished_{false};
    std::atomic<u64> framesPlayed_{0};
};

SysDeck::SysDeck(const std::string &src, const std::string &codec, double startSec, int kbps, double durSec)
    : src_(src), codec_(codec), startSec_(startSec > 0 ? startSec : 0), durSec_(durSec), kbps_(kbps) {
    local_ = src.compare(0, 5, "sdmc:") == 0;
    netRunning_ = codec != "m4a"; // m4a декодер читает файл сам
    netDone_ = !netRunning_;
    framesPlayed_ = (u64)(startSec_ * 48000);
    if (netRunning_) {
        threadCreate(&netThread_, netEntry, this, nullptr, 0x10000, 0x2C, -2);
        threadStart(&netThread_);
    }
    // FAAD2 держит на стеке массивы SBR — стек побольше.
    threadCreate(&decThread_, decodeEntry, this, nullptr, 0x20000, 0x2B, -2);
    threadStart(&decThread_);
}

SysDeck::~SysDeck() {
    stop_ = true;
    threadWaitForExit(&decThread_);
    threadClose(&decThread_);
    if (netRunning_) {
        threadWaitForExit(&netThread_);
        threadClose(&netThread_);
    }
    if (rs_) syn123_del(rs_);
    free(ring_);
}

size_t SysDeck::available() {
    std::lock_guard<std::mutex> lk(pm_);
    return (pcm_.size() - head_) / 2;
}

size_t SysDeck::read(s16 *out, size_t frames) {
    std::lock_guard<std::mutex> lk(pm_);
    size_t n = std::min(frames, (pcm_.size() - head_) / 2);
    memcpy(out, pcm_.data() + head_, n * 4);
    head_ += n * 2;
    if (head_ > pcm_.size() / 2) { // сдвигаем, чтобы буфер не рос
        pcm_.erase(pcm_.begin(), pcm_.begin() + head_);
        head_ = 0;
    }
    framesPlayed_ += n;
    return n;
}

bool SysDeck::ready() const {
    std::lock_guard<std::mutex> lk(m_);
    if (local_) return true; // с карты читается мгновенно
    return netDone_ || fill_ >= 48 * 1024; // ~1 с звука — хватит, чтобы начать без запинки
}

std::string SysDeck::error() const {
    std::lock_guard<std::mutex> lk(m_);
    return error_;
}

void SysDeck::setError(const std::string &e) {
    std::lock_guard<std::mutex> lk(m_);
    if (error_.empty()) error_ = e;
}

void SysDeck::netLoop() {
    // Кладём данные в окно; окно полно — ждём, пока декодер прочитает.
    auto put = [this](const char *d, size_t n) {
        while (n > 0) {
            if (stop_) return false;
            size_t wrote = 0;
            {
                std::lock_guard<std::mutex> lk(m_);
                size_t room = WINDOW_MAX - fill_;
                wrote = n < room ? n : room;
                size_t at = (head_r_ + fill_) % WINDOW_MAX;
                size_t first = std::min(wrote, WINDOW_MAX - at);
                memcpy(ring_ + at, d, first);
                memcpy(ring_, d + first, wrote - first);
                fill_ += wrote;
            }
            d += wrote;
            n -= wrote;
            if (n) svcSleepThread(50 * MS);
        }
        return true;
    };
    {
        std::lock_guard<std::mutex> lk(m_);
        ring_ = (u8 *)malloc(WINDOW_MAX);
        if (!ring_) {
            ymLog("[player] no memory for download window");
            netDone_ = true;
            if (error_.empty()) error_ = "Мало памяти для загрузки";
            return;
        }
    }

    if (local_) { // mp3/aac с карты: читаем так же, окном
        FILE *f = fopen(src_.c_str(), "rb");
        if (!f) {
            setError("Файл не найден на карте");
            std::lock_guard<std::mutex> lk(m_);
            netDone_ = true;
            return;
        }
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        long from = 0;
        if (startSec_ > 0 && durSec_ > 0) from = (long)(size * (startSec_ / durSec_));
        else if (startSec_ > 0 && kbps_ > 0) from = (long)(startSec_ * kbps_ * 125);
        if (from >= size) from = size > 4096 ? size - 4096 : 0;
        fseek(f, from, SEEK_SET);
        std::vector<char> chunk(64 * 1024);
        size_t n;
        while (!stop_ && (n = fread(chunk.data(), 1, chunk.size(), f)) > 0)
            if (!put(chunk.data(), n)) break;
        fclose(f);
        std::lock_guard<std::mutex> lk(m_);
        netDone_ = true;
        return;
    }

    // HLS (SoundCloud, mp3): плейлист из кусков — качаем их по порядку в то же окно, для mpg123 это
    // один поток. С середины — пропускаем куски по их длительностям. Кусок оборвался — докачиваем с того же байта.
    if (src_.compare(0, 4, "hls:") == 0) {
        std::string listUrl = src_.substr(4), err;
        HttpResult pl = httpGet(listUrl, {});
        std::vector<double> durs;
        std::vector<std::string> parts =
            pl.code == 200 ? hlsSegments(pl.body, pl.finalUrl.empty() ? listUrl : pl.finalUrl, &durs) : std::vector<std::string>();
        if (pl.code != 200) err = pl.code < 0 ? pl.error : "HTTP " + std::to_string(pl.code);
        else if (parts.empty()) err = "пустой плейлист";
        size_t first = 0;
        double acc = 0;
        for (; first + 1 < parts.size() && acc + durs[first] <= startSec_; first++) acc += durs[first];
        // Внутри первого куска — пропускаем байты до нужной секунды (mp3 почти всегда CBR).
        size_t skip = startSec_ > acc && kbps_ > 0 ? (size_t)((startSec_ - acc) * kbps_ * 125) : 0;
        ymLog("[hls] кусков %d, с %d", (int)parts.size(), (int)first);
        for (size_t i = first; err.empty() && i < parts.size() && !stop_; i++) {
            size_t got = i == first ? skip : 0;
            HttpResult r;
            for (int attempt = 0; attempt < 4 && !stop_; attempt++) {
                if (attempt) svcSleepThread(700 * MS);
                std::vector<std::string> hdr;
                if (got) hdr.push_back("Range: bytes=" + std::to_string(got) + "-");
                r = httpGet(parts[i], hdr, [&](const char *d, size_t n) {
                    if (!put(d, n)) return false;
                    got += n;
                    return true;
                });
                if (r.code == 200 || r.code == 206 || r.code > 0) break;
            }
            // 416 — пропуск вышел за конец куска: просто идём к следующему.
            if (!stop_ && r.code != 200 && r.code != 206 && r.code != 416) err = r.code < 0 ? r.error : "HTTP " + std::to_string(r.code);
        }
        if (!stop_ && !err.empty()) {
            ymLog("[hls] ошибка: %s", err.c_str());
            setError("Download: " + err);
        }
        std::lock_guard<std::mutex> lk(m_);
        netDone_ = true;
        return;
    }

    // Поток почти всегда CBR: позиция в байтах ≈ секунды × битрейт. Декодер сам найдёт начало кадра.
    size_t startByte = (startSec_ > 0 && kbps_ > 0) ? (size_t)(startSec_ * kbps_ * 125) : 0;
    // Если соединение оборвалось — докачиваем с того же байта, а не бросаем трек.
    size_t received = 0;
    HttpResult r;
    for (int attempt = 0; attempt < 4 && !stop_; attempt++) {
        if (attempt) {
            ymLog("[player] resume at %u after: %s", (unsigned)(startByte + received), r.error.c_str());
            svcSleepThread(700 * MS);
        }
        std::vector<std::string> hdr;
        size_t from = startByte + received;
        if (from) hdr.push_back("Range: bytes=" + std::to_string(from) + "-");
        r = httpGet(src_, hdr, [&](const char *d, size_t n) {
            if (!put(d, n)) return false;
            received += n;
            return true;
        });
        if (r.code == 200 || r.code == 206) break; // докачали
        if (r.code > 0) break;                     // HTTP-ошибка — повтор не поможет
    }
    if (!stop_ && r.code != 200 && r.code != 206) {
        ymLog("[player] download: code=%ld err=%s", r.code, r.error.c_str());
        setError(r.code < 0 ? "Download: " + r.error : "Download: HTTP " + std::to_string(r.code));
    }
    std::lock_guard<std::mutex> lk(m_);
    netDone_ = true;
}

long SysDeck::pull(u8 *dst, size_t max) {
    std::lock_guard<std::mutex> lk(m_);
    if (fill_ > 0) {
        size_t n = fill_ < max ? fill_ : max;
        size_t first = std::min(n, WINDOW_MAX - head_r_);
        memcpy(dst, ring_ + head_r_, first);
        memcpy(dst + first, ring_, n - first);
        head_r_ = (head_r_ + n) % WINDOW_MAX;
        fill_ -= n;
        return (long)n;
    }
    return netDone_ ? -1 : 0;
}

bool SysDeck::emit(const s16 *pcm, size_t frames, unsigned rate, unsigned channels) {
    if (!frames || !channels) return !stop_;
    const s16 *st = pcm;
    if (channels != 2) { // моно — дублируем, больше двух — берём первые два
        tmp_.resize(frames * 2);
        for (size_t i = 0; i < frames; i++) {
            tmp_[i * 2] = pcm[i * channels];
            tmp_[i * 2 + 1] = channels > 1 ? pcm[i * channels + 1] : pcm[i * channels];
        }
        st = tmp_.data();
    }
    conv_.clear();
    if (rate == 48000) {
        conv_.assign(st, st + frames * 2);
    } else {
        if (!rs_ || rsRate_ != (long)rate) {
            if (!rs_) rs_ = syn123_new(48000, 2, MPG123_ENC_FLOAT_32, 0, nullptr);
            if (!rs_ || syn123_setup_resample(rs_, rate, 48000, 2, 0, 0) != SYN123_OK) {
                setError("Не настроился пересчёт частоты");
                return false;
            }
            rsRate_ = rate;
        }
        fin_.resize(frames * 2);
        for (size_t i = 0; i < frames * 2; i++) fin_[i] = st[i] / 32768.0f;
        fout_.resize(syn123_resample_count(rate, 48000, frames) * 2);
        size_t outFrames = syn123_resample(rs_, fout_.data(), fin_.data(), frames);
        conv_.resize(outFrames * 2);
        for (size_t i = 0; i < outFrames * 2; i++) {
            float v = fout_[i] * 32768.0f;
            conv_[i] = (s16)(v > 32767.0f ? 32767.0f : v < -32768.0f ? -32768.0f : v);
        }
    }
    while (!stop_) {
        {
            std::lock_guard<std::mutex> lk(pm_);
            if ((pcm_.size() - head_) / 2 < PCM_AHEAD) {
                pcm_.insert(pcm_.end(), conv_.begin(), conv_.end());
                return true;
            }
        }
        svcSleepThread(10 * MS);
    }
    return false;
}

bool SysDeck::decodeMp3() {
    int err = 0;
    mpg123_handle *mh = mpg123_new(nullptr, &err);
    if (!mh) { setError("mpg123_new failed"); return false; }
    // Стерео s16 в родной частоте; до 48 кГц пересчитывает syn123 в emit() — встроенный пересчёт mpg123 грубее.
    mpg123_param(mh, MPG123_ADD_FLAGS, MPG123_FORCE_STEREO | MPG123_QUIET, 0);
    mpg123_format_none(mh);
    const long *rates = nullptr;
    size_t nrates = 0;
    mpg123_rates(&rates, &nrates);
    for (size_t i = 0; i < nrates; i++) mpg123_format(mh, rates[i], MPG123_STEREO, MPG123_ENC_SIGNED_16);
    mpg123_open_feed(mh);

    std::vector<u8> in(16384);
    std::vector<s16> pcm(8192);
    long rate = 44100;
    bool ok = true;
    while (!stop_) {
        size_t done = 0;
        int rc = mpg123_read(mh, (u8 *)pcm.data(), pcm.size() * sizeof(s16), &done);
        if (rc == MPG123_NEW_FORMAT) {
            int ch = 0, enc = 0;
            mpg123_getformat(mh, &rate, &ch, &enc);
        }
        if (done && !emit(pcm.data(), done / 4, (unsigned)rate, 2)) break;
        if (rc == MPG123_OK || rc == MPG123_NEW_FORMAT) continue;
        if (rc == MPG123_DONE) break;
        if (rc != MPG123_NEED_MORE) {
            setError(std::string("Decode: ") + mpg123_plain_strerror(rc));
            ok = false;
            break;
        }
        long n = pull(in.data(), in.size());
        if (n > 0) mpg123_feed(mh, in.data(), n);
        else if (n < 0) break; // данные кончились
        else svcSleepThread(10 * MS);
    }
    mpg123_delete(mh);
    return ok;
}

static NeAACDecHandle openFaad() {
    NeAACDecHandle h = NeAACDecOpen();
    if (!h) return nullptr;
    NeAACDecConfigurationPtr cfg = NeAACDecGetCurrentConfiguration(h);
    cfg->outputFormat = FAAD_FMT_16BIT;
    cfg->downMatrix = 1; // 5.1 → стерео
    cfg->defObjectType = LC;
    NeAACDecSetConfiguration(h, cfg);
    return h;
}

// Поток ADTS: каждый кадр начинается с синхрослова 0xFFF и сам знает свою длину.
bool SysDeck::decodeAdts() {
    NeAACDecHandle h = openFaad();
    if (!h) { setError("FAAD2: нет памяти"); return false; }
    std::vector<u8> buf;
    size_t off = 0;
    bool inited = false, eofData = false, ok = true;
    int errors = 0;
    std::vector<u8> chunk(16384);
    while (!stop_) {
        size_t avail = buf.size() - off;
        bool needMore = true;
        while (avail >= 7) {
            u8 *a = buf.data() + off;
            if (a[0] != 0xFF || (a[1] & 0xF6) != 0xF0) { off++; avail--; continue; } // ищем синхрослово
            size_t len = ((a[3] & 3) << 11) | (a[4] << 3) | (a[5] >> 5);
            if (len < 7) { off++; avail--; continue; }
            if (avail < len) break;
            if (!inited) {
                // Начали с середины — убеждаемся, что это кадр, а не случайные байты: следом идёт ещё один.
                if (avail < len + 2 && !eofData) break;
                if (avail >= len + 2 && (a[len] != 0xFF || (a[len + 1] & 0xF6) != 0xF0)) { off++; avail--; continue; }
                unsigned long rate = 0;
                unsigned char ch = 0;
                if (NeAACDecInit(h, a, len, &rate, &ch) < 0) { off++; avail--; continue; }
                ymLog("[player] aac: %lu Hz, %u ch", rate, ch);
                inited = true;
            }
            NeAACDecFrameInfo info;
            void *s = NeAACDecDecode(h, &info, a, len);
            off += len;
            avail -= len;
            needMore = false;
            if (info.error) {
                if (++errors > 50) { setError(std::string("AAC: ") + NeAACDecGetErrorMessage(info.error)); ok = false; break; }
                continue;
            }
            errors = 0;
            if (s && info.samples && info.channels &&
                !emit((const s16 *)s, info.samples / info.channels, info.samplerate, info.channels))
                break;
            if (stop_) break;
        }
        if (!ok || stop_) break;
        if (!needMore) continue;
        if (eofData) break;
        if (off > 65536) { // съеденное выкидываем
            buf.erase(buf.begin(), buf.begin() + off);
            off = 0;
        }
        long n = pull(chunk.data(), chunk.size());
        if (n > 0) buf.insert(buf.end(), chunk.begin(), chunk.begin() + n);
        else if (n < 0) eofData = true;
        else svcSleepThread(10 * MS);
    }
    NeAACDecClose(h);
    if (ok && !inited && !stop_) { setError("AAC: не нашёл кадров"); ok = false; }
    return ok;
}

// Скачанный m4a: кадры берём прямо из файла по таблице — перемотка точная.
bool SysDeck::decodeM4a() {
    FILE *f = fopen(src_.c_str(), "rb");
    if (!f) { setError("Файл не найден на карте"); return false; }
    Mp4Audio mp;
    std::string err;
    if (!mp.open(f, err)) {
        fclose(f);
        setError("MP4: " + err);
        return false;
    }
    NeAACDecHandle h = openFaad();
    unsigned long rate = 0;
    unsigned char ch = 0;
    if (!h || NeAACDecInit2(h, mp.asc.data(), (unsigned long)mp.asc.size(), &rate, &ch) < 0) {
        if (h) NeAACDecClose(h);
        fclose(f);
        setError("AAC: неизвестный конфиг");
        return false;
    }
    ymLog("[player] m4a: %lu Hz, %u ch, %u frames", rate, ch, (unsigned)mp.sizes.size());
    bool ok = true;
    int errors = 0;
    std::vector<u8> fr;
    for (size_t i = mp.frameAt(startSec_); i < mp.offsets.size() && !stop_; i++) {
        fr.resize(mp.sizes[i]);
        if (fseek(f, (long)mp.offsets[i], SEEK_SET) != 0 || fread(fr.data(), 1, fr.size(), f) != fr.size()) {
            setError("Файл обрезан");
            ok = false;
            break;
        }
        NeAACDecFrameInfo info;
        void *s = NeAACDecDecode(h, &info, fr.data(), (unsigned long)fr.size());
        if (info.error) {
            if (++errors > 50) { setError(std::string("AAC: ") + NeAACDecGetErrorMessage(info.error)); ok = false; break; }
            continue;
        }
        errors = 0;
        if (s && info.samples && info.channels &&
            !emit((const s16 *)s, info.samples / info.channels, info.samplerate, info.channels))
            break;
    }
    NeAACDecClose(h);
    fclose(f);
    return ok;
}

void SysDeck::decodeLoop() {
    bool ok = codec_ == "mp3" ? decodeMp3() : codec_ == "m4a" ? decodeM4a() : decodeAdts();
    // Сеть оборвалась насовсем — это ошибка, а не конец трека.
    finished_ = ok && !stop_ && error().empty();
    decDone_ = true;
}

// ---------- плеер: вывод в audout и смешивание при переходе ----------

bool SysPlayer::init() {
    if (mpg123_init() != MPG123_OK) { ymLog("[player] mpg123_init failed"); return false; }
    Result rc = audoutInitialize();
    ymLog("[player] audoutInitialize: 0x%x", rc);
    if (R_FAILED(rc)) return false;
    rc = audoutStartAudioOut();
    ymLog("[player] audoutStart: 0x%x", rc);
    if (R_FAILED(rc)) return false;
    for (int i = 0; i < NUM_BUFS; i++) {
        void *mem = memalign(0x1000, BUF_BYTES);
        if (!mem) return false;
        memset(mem, 0, BUF_BYTES);
        bufs_[i] = AudioOutBuffer{};
        bufs_[i].buffer = mem;
        bufs_[i].buffer_size = BUF_BYTES;
        free_.push_back(&bufs_[i]);
    }
    mixA_.resize(BUF_FRAMES * 2);
    mixB_.resize(BUF_FRAMES * 2);
    quit_ = false;
    threadCreate(&outThread_, outEntry, this, nullptr, 0x8000, 0x2A, -2);
    threadStart(&outThread_);
    outRunning_ = true;
    return true;
}

void SysPlayer::shutdown() {
    stop();
    if (outRunning_) {
        quit_ = true;
        threadWaitForExit(&outThread_);
        threadClose(&outThread_);
        outRunning_ = false;
    }
    flushOut();
    audoutStopAudioOut();
    audoutExit();
    for (int i = 0; i < NUM_BUFS; i++) free(bufs_[i].buffer);
    mpg123_exit();
}

void SysPlayer::play(const std::string &src, const std::string &codec, double startSec, int kbps, double durSec) {
    std::shared_ptr<SysDeck> oldA, oldF; // старые деки останавливаются вне замка
    {
        std::lock_guard<std::mutex> lk(m_);
        oldA = std::move(active_);
        oldF = std::move(fading_);
    }
    oldA.reset();
    oldF.reset();
    auto deck = std::make_shared<SysDeck>(src, codec, startSec, kbps, durSec);
    {
        std::lock_guard<std::mutex> lk(m_);
        active_ = deck;
        state_ = Buffering;
    }
    flushReq_ = true;
}

void SysPlayer::crossfade(const std::string &src, const std::string &codec, int kbps, double durSec, int seconds) {
    auto deck = std::make_shared<SysDeck>(src, codec, 0, kbps, durSec);
    std::shared_ptr<SysDeck> oldF;
    {
        std::lock_guard<std::mutex> lk(m_);
        oldF = std::move(fading_);
        fading_ = std::move(active_);
        active_ = deck;
        cfTotal_ = (u64)std::max(1, seconds) * 48000;
        cfPos_ = 0;
        if (state_ != Playing) state_ = Buffering;
    }
}

void SysPlayer::stop() {
    std::shared_ptr<SysDeck> oldA, oldF;
    {
        std::lock_guard<std::mutex> lk(m_);
        oldA = std::move(active_);
        oldF = std::move(fading_);
        state_ = Idle;
    }
    flushReq_ = true;
}

void SysPlayer::setPaused(bool p) { paused_ = p; }

void SysPlayer::setVolume(int percent) {
    volume_ = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    audoutSetAudioOutVolume(volume_ / 100.0f);
}

SysPlayer::State SysPlayer::state() const {
    State s = state_;
    if (paused_ && (s == Playing || s == Buffering)) return Paused;
    return s;
}

std::shared_ptr<SysDeck> SysPlayer::active() const {
    std::lock_guard<std::mutex> lk(m_);
    return active_;
}

double SysPlayer::position() const {
    auto a = active();
    return a ? a->position() : 0;
}

std::string SysPlayer::error() const {
    auto a = active();
    return a ? a->error() : std::string();
}

bool SysPlayer::ready() const {
    auto a = active();
    return a && a->ready();
}

void SysPlayer::outEntry(void *arg) { static_cast<SysPlayer *>(arg)->outLoop(); }

AudioOutBuffer *SysPlayer::takeBuffer() {
    while (!quit_ && !flushReq_ && !paused_) {
        if (!free_.empty()) {
            AudioOutBuffer *b = free_.back();
            free_.pop_back();
            return b;
        }
        AudioOutBuffer *rel = nullptr;
        u32 cnt = 0;
        audoutWaitPlayFinish(&rel, &cnt, 50 * MS);
        if (cnt > 0 && rel) { queued_--; return rel; }
    }
    return nullptr;
}

void SysPlayer::submit(AudioOutBuffer *b, size_t frames) {
    b->data_size = frames * 4;
    audoutAppendAudioOutBuffer(b);
    queued_++;
}

void SysPlayer::flushOut() {
    bool flushed = false;
    audoutFlushAudioOutBuffers(&flushed);
    for (int tries = 0; queued_ > 0 && tries < 100; tries++) {
        AudioOutBuffer *rel = nullptr;
        u32 cnt = 0;
        audoutWaitPlayFinish(&rel, &cnt, 20 * MS);
        if (cnt > 0 && rel) { queued_--; free_.push_back(rel); }
    }
    if (queued_ > 0) { // на всякий случай не теряем буферы
        free_.clear();
        for (int i = 0; i < NUM_BUFS; i++) free_.push_back(&bufs_[i]);
        queued_ = 0;
    }
}

void SysPlayer::outLoop() {
    const size_t F = BUF_FRAMES;
    while (!quit_) {
        if (flushReq_.exchange(false)) flushOut();
        std::shared_ptr<SysDeck> a, f;
        {
            std::lock_guard<std::mutex> lk(m_);
            a = active_;
            f = fading_;
        }
        if (!a || paused_) { svcSleepThread(10 * MS); continue; }
        // Состояние меняем, только если дека всё ещё текущая (её могли сменить или остановить).
        auto setState = [&](State s) {
            std::lock_guard<std::mutex> lk(m_);
            if (active_ == a) state_ = s;
        };

        bool aReady = a->available() >= F || a->decodeDone();
        if (!f) {
            if (a->failed()) { setState(Error); svcSleepThread(20 * MS); continue; }
            if (a->ended()) { // трек кончился: ждём, пока доиграет хвост
                AudioOutBuffer *rel = nullptr;
                u32 cnt = 0;
                if (queued_ > 0) audoutWaitPlayFinish(&rel, &cnt, 50 * MS);
                if (cnt > 0 && rel) { queued_--; free_.push_back(rel); }
                if (queued_ == 0) { setState(Finished); svcSleepThread(10 * MS); }
                continue;
            }
            if (!aReady) {
                setState(Buffering);
                svcSleepThread(10 * MS);
                continue;
            }
        }

        AudioOutBuffer *b = takeBuffer();
        if (!b) continue;
        s16 *out = (s16 *)b->buffer;
        size_t n;
        if (f && !aReady) {
            n = f->read(out, F); // новый трек ещё грузится — доигрываем старый как есть
        } else if (f) {
            size_t na = a->read(mixA_.data(), F), nf = f->read(mixB_.data(), F);
            n = std::max(na, nf);
            std::fill(mixA_.begin() + na * 2, mixA_.begin() + n * 2, 0);
            std::fill(mixB_.begin() + nf * 2, mixB_.begin() + n * 2, 0);
            u64 pos, total;
            {
                std::lock_guard<std::mutex> lk(m_);
                pos = cfPos_;
                total = cfTotal_;
                cfPos_ += na;
            }
            for (size_t i = 0; i < n; i++) {
                float t = std::min(1.0f, (float)(pos + std::min(i, na)) / total);
                float gIn = sinf(t * 1.5707963f), gOut = cosf(t * 1.5707963f); // равная мощность
                for (int c = 0; c < 2; c++) {
                    float v = mixA_[i * 2 + c] * gIn + mixB_[i * 2 + c] * gOut;
                    out[i * 2 + c] = (s16)std::max(-32768.0f, std::min(32767.0f, v));
                }
            }
        } else {
            n = a->read(out, F);
        }
        if (f && (f->ended() || f->failed() || (aReady && cfPos_ >= cfTotal_))) {
            std::lock_guard<std::mutex> lk(m_);
            if (fading_ == f) fading_.reset();
        }
        if (n == 0) { free_.push_back(b); svcSleepThread(5 * MS); continue; }
        submit(b, n);
        if (state_ == Buffering) setState(Playing);
    }
}
