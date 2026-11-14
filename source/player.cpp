#include "player.hpp"
#include "http.hpp"
#include "soundcloud.hpp"
#include "crashlog.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
}

static const u64 MS = 1000000ULL;
static const int OUT_RATE = 48000;          // Свич играет только 48 кГц стерео s16
static const size_t PCM_AHEAD = OUT_RATE * 2; // декодер забегает вперёд не больше чем на 2 с

// ---------- дека: загрузка и декодирование одного трека в очередь PCM ----------

class Deck {
public:
    Deck(const std::string &url, size_t expectedBytes);
    ~Deck();

    size_t read(s16 *out, size_t frames); // забрать до frames кадров
    size_t available();
    bool decodeDone() const { return decDone_; }
    bool ended() { return decDone_ && finished_ && available() == 0; }
    bool failed() { return decDone_ && !finished_ && available() == 0; }
    void seek(double sec);
    double position() const { return framesPlayed_.load() / (double)OUT_RATE; }
    double downloaded() const;
    std::string error() const;

private:
    static void netEntry(void *arg) {
        static_cast<Deck *>(arg)->netLoop();
        httpThreadDone(); // поток загрузки живёт один трек — закрываем его соединения через прокси
    }
    static void decodeEntry(void *arg) { static_cast<Deck *>(arg)->decodeLoop(); }
    static int avioRead(void *opaque, uint8_t *buf, int size);
    static int64_t avioSeek(void *opaque, int64_t offset, int whence);
    void netLoop();
    void decodeLoop();
    void setError(const std::string &e);
    bool push(const std::vector<s16> &pcm); // false — остановлены или перематываем

    std::string url_;
    Thread netThread_, decThread_;

    mutable std::mutex m_; // загрузка
    std::vector<u8> data_;
    size_t total_ = 0;     // Content-Length или оценка
    bool totalExact_ = false;
    size_t readPos_ = 0;   // позиция чтения FFmpeg в data_
    bool netDone_ = false;
    std::string error_;

    std::mutex pm_; // декодированный звук
    std::vector<s16> pcm_;
    size_t head_ = 0; // сколько сэмплов из pcm_ уже забрали

    std::atomic<bool> stop_{false}, decDone_{false}, finished_{false};
    std::atomic<u64> framesPlayed_{0};
    std::atomic<long long> seekMs_{-1}; // запрошенная перемотка, -1 — нет
};

Deck::Deck(const std::string &url, size_t expectedBytes) : url_(url) {
    data_.reserve(expectedBytes ? expectedBytes + 65536 : 8 << 20);
    total_ = expectedBytes;
    // Свои потоки libnx с большим стеком: TLS в curl и FFmpeg прожорливы.
    threadCreate(&netThread_, netEntry, this, nullptr, 0x80000, 0x2C, -2);
    threadCreate(&decThread_, decodeEntry, this, nullptr, 0x80000, 0x2B, -2);
    threadStart(&netThread_);
    threadStart(&decThread_);
}

Deck::~Deck() {
    stop_ = true;
    threadWaitForExit(&decThread_);
    threadWaitForExit(&netThread_);
    threadClose(&decThread_);
    threadClose(&netThread_);
}

size_t Deck::available() {
    std::lock_guard<std::mutex> lk(pm_);
    return (pcm_.size() - head_) / 2;
}

size_t Deck::read(s16 *out, size_t frames) {
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

void Deck::seek(double sec) {
    if (sec < 0) sec = 0;
    {
        std::lock_guard<std::mutex> lk(pm_);
        pcm_.clear();
        head_ = 0;
    }
    framesPlayed_ = (u64)(sec * OUT_RATE); // интерфейс сразу показывает новую позицию
    seekMs_ = (long long)(sec * 1000);
}

bool Deck::push(const std::vector<s16> &pcm) {
    while (!stop_ && seekMs_ < 0) {
        {
            std::lock_guard<std::mutex> lk(pm_);
            if ((pcm_.size() - head_) / 2 < PCM_AHEAD) {
                pcm_.insert(pcm_.end(), pcm.begin(), pcm.end());
                return true;
            }
        }
        svcSleepThread(10 * MS);
    }
    return false;
}

double Deck::downloaded() const {
    std::lock_guard<std::mutex> lk(m_);
    if (netDone_) return 1.0;
    if (!total_) return -1;
    double f = double(data_.size()) / total_;
    return f > 1 ? 1 : f;
}

std::string Deck::error() const {
    std::lock_guard<std::mutex> lk(m_);
    return error_;
}

void Deck::setError(const std::string &e) {
    std::lock_guard<std::mutex> lk(m_);
    if (error_.empty()) {
        error_ = e;
        crashLog("[play] ошибка: %s", e.c_str());
    }
}

// Хост и расширение файла из адреса — для журнала, без подписей и токенов.
static std::string urlBrief(const std::string &u) {
    size_t s = u.find("://");
    size_t h = s == std::string::npos ? 0 : s + 3;
    size_t ps = u.find('/', h);
    std::string host = u.substr(h, ps == std::string::npos ? std::string::npos : ps - h);
    std::string path = ps == std::string::npos ? "" : u.substr(ps, u.find('?', ps) == std::string::npos ? std::string::npos : u.find('?', ps) - ps);
    size_t dot = path.rfind('.'), slash = path.rfind('/');
    std::string ext = dot != std::string::npos && (slash == std::string::npos || dot > slash) ? path.substr(dot) : "";
    return host + " …" + ext;
}

void Deck::netLoop() {
    // Скачанный трек: читаем с карты в тот же буфер, остальное не меняется.
    if (url_.compare(0, 5, "sdmc:") == 0) {
        FILE *f = fopen(url_.c_str(), "rb");
        if (!f) {
            std::lock_guard<std::mutex> lk(m_);
            error_ = "Файл не найден на карте";
            netDone_ = true;
            return;
        }
        fseek(f, 0, SEEK_END);
        size_t size = ftell(f);
        fseek(f, 0, SEEK_SET);
        {
            std::lock_guard<std::mutex> lk(m_);
            total_ = size;
            totalExact_ = true;
            data_.reserve(size);
        }
        std::vector<u8> chunk(256 * 1024);
        size_t n;
        while (!stop_ && (n = fread(chunk.data(), 1, chunk.size(), f)) > 0) {
            std::lock_guard<std::mutex> lk(m_);
            data_.insert(data_.end(), chunk.begin(), chunk.begin() + n);
        }
        fclose(f);
        std::lock_guard<std::mutex> lk(m_);
        netDone_ = true;
        return;
    }

    // HLS (SoundCloud): плейлист из кусков — качаем их по порядку в тот же буфер, для FFmpeg это
    // один непрерывный поток (mp3 склеивается как есть, AAC в fMP4 — с начальным куском первым).
    if (url_.compare(0, 4, "hls:") == 0) {
        // Плейлист качаем потоком (у потоковой загрузки нет общего тайм-аута — через прокси бывает медленно).
        // Если вместо плейлиста пришёл сам аудиофайл — он сразу идёт в буфер плееру.
        bool isFile = false, sniffed = false;
        std::string plBody;
        size_t fileLen = 0;
        HttpResult pl;
        std::string cachedFinal;
        if (hlsCacheTake(url_.substr(4), cachedFinal, plBody)) { // плейлист скачан заранее (SoundCloud::warm)
            pl.code = 200;
            pl.finalUrl = cachedFinal;
            sniffed = true;
        } else pl = httpGet(url_.substr(4), {}, [&](const char *d, size_t n) {
            if (stop_) return false;
            if (!sniffed && plBody.size() + n >= 7) {
                std::string head = plBody + std::string(d, n < 7 ? n : 7);
                isFile = head.compare(0, 7, "#EXTM3U") != 0;
                sniffed = true;
                if (isFile) {
                    std::lock_guard<std::mutex> lk(m_);
                    data_.insert(data_.end(), plBody.begin(), plBody.end());
                    plBody.clear();
                    if (fileLen) { // точный размер — FFmpeg сможет сразу прыгнуть в конец (M4A с moov в конце)
                        total_ = fileLen;
                        totalExact_ = true;
                    }
                }
            }
            if (isFile) {
                std::lock_guard<std::mutex> lk(m_);
                data_.insert(data_.end(), d, d + n);
            } else plBody.append(d, n);
            return true;
        }, [&](size_t len) { fileLen = len; });
        if (isFile) {
            std::lock_guard<std::mutex> lk(m_);
            netDone_ = true;
            crashLog("[hls] не плейлист, а файл: %u КБ, %s", (unsigned)(data_.size() / 1024),
                     pl.code < 0 ? pl.error.c_str() : ("HTTP " + std::to_string(pl.code)).c_str());
            if (!stop_ && pl.code != 200) error_ = "Download: " + (pl.code < 0 ? pl.error : "HTTP " + std::to_string(pl.code));
            return;
        }
        std::vector<std::string> parts = pl.code == 200 ? hlsSegments(plBody, pl.finalUrl.empty() ? url_.substr(4) : pl.finalUrl)
                                                        : std::vector<std::string>();
        std::string err = pl.code < 0 ? pl.error : pl.code != 200 ? "HTTP " + std::to_string(pl.code) : parts.empty() ? "пустой плейлист" : "";
        crashLog("[hls] плейлист: %s, кусков %d, первый %s%s", pl.code < 0 ? pl.error.c_str() : std::to_string(pl.code).c_str(),
                 (int)parts.size(), parts.empty() ? "-" : urlBrief(parts[0]).c_str(),
                 plBody.find("#EXT-X-KEY") != std::string::npos ? ", ЗАШИФРОВАН" : "");
        for (size_t i = 0; err.empty() && i < parts.size() && !stop_; i++) {
            HttpResult r;
            for (int attempt = 0; attempt < 3 && !stop_; attempt++) { // кусок оборвался — качаем его заново
                size_t mark;
                {
                    std::lock_guard<std::mutex> lk(m_);
                    mark = data_.size();
                }
                r = httpGet(parts[i], {}, [this](const char *d, size_t n) {
                    if (stop_) return false;
                    std::lock_guard<std::mutex> lk(m_);
                    data_.insert(data_.end(), d, d + n);
                    return true;
                });
                if (r.code == 200) break;
                std::lock_guard<std::mutex> lk(m_);
                if (data_.size() > mark && readPos_ <= mark) data_.resize(mark); // недокачанный кусок — выкидываем
                else if (data_.size() > mark) { err = "обрыв посреди куска"; break; }
            }
            if (err.empty() && r.code != 200 && !stop_) err = r.code < 0 ? r.error : "HTTP " + std::to_string(r.code);
        }
        std::lock_guard<std::mutex> lk(m_);
        netDone_ = true;
        crashLog("[hls] скачано %u КБ%s%s", (unsigned)(data_.size() / 1024), err.empty() ? "" : ", ошибка: ", err.c_str());
        if (!stop_ && !err.empty()) error_ = "Download: " + err;
        return;
    }

    HttpResult r = httpGet(
        url_, {},
        [this](const char *d, size_t n) {
            if (stop_) return false;
            std::lock_guard<std::mutex> lk(m_);
            data_.insert(data_.end(), d, d + n);
            return true;
        },
        [this](size_t len) {
            std::lock_guard<std::mutex> lk(m_);
            total_ = len;
            totalExact_ = true;
        });
    std::lock_guard<std::mutex> lk(m_);
    netDone_ = true;
    if (stop_) return;
    if (r.code < 0) error_ = "Download: " + r.error;
    else if (r.code != 200) error_ = "Download: HTTP " + std::to_string(r.code);
}

// FFmpeg читает из растущего буфера: если данных ещё нет — ждём загрузку.
int Deck::avioRead(void *opaque, uint8_t *buf, int size) {
    Deck *p = static_cast<Deck *>(opaque);
    while (!p->stop_) {
        {
            std::lock_guard<std::mutex> lk(p->m_);
            if (p->readPos_ < p->data_.size()) {
                size_t n = p->data_.size() - p->readPos_;
                if (n > (size_t)size) n = size;
                memcpy(buf, p->data_.data() + p->readPos_, n);
                p->readPos_ += n;
                return (int)n;
            }
            if (p->netDone_) return AVERROR_EOF;
        }
        svcSleepThread(10 * MS);
    }
    return AVERROR_EXIT;
}

int64_t Deck::avioSeek(void *opaque, int64_t offset, int whence) {
    Deck *p = static_cast<Deck *>(opaque);
    std::lock_guard<std::mutex> lk(p->m_);
    int64_t size = p->netDone_ ? (int64_t)p->data_.size() : (p->totalExact_ ? (int64_t)p->total_ : -1);
    whence &= ~AVSEEK_FORCE;
    if (whence == AVSEEK_SIZE) return size;
    int64_t pos;
    if (whence == SEEK_SET) pos = offset;
    else if (whence == SEEK_CUR) pos = (int64_t)p->readPos_ + offset;
    else if (whence == SEEK_END && size >= 0) pos = size + offset;
    else return -1;
    if (pos < 0) return -1;
    p->readPos_ = (size_t)pos; // если дальше скачанного — avioRead дождётся
    return pos;
}

void Deck::decodeLoop() {
    const int IO_SIZE = 32768;
    AVFormatContext *fmt = nullptr;
    AVCodecContext *cc = nullptr;
    SwrContext *swr = nullptr;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    AVIOContext *avio = avio_alloc_context((unsigned char *)av_malloc(IO_SIZE), IO_SIZE, 0, this, avioRead, nullptr, avioSeek);
    bool finished = false;
    int si = -1;
    const AVCodec *codec = nullptr;
    AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_STEREO;
    std::vector<s16> out;
    int64_t skipTs = AV_NOPTS_VALUE; // после перемотки: пакеты раньше этого — выкидываем

    fmt = avformat_alloc_context();
    fmt->pb = avio;
    fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    // SoundCloud AAC приходит как fMP4 из кусков. Без IGNIDX демуксер MP4 ещё до старта обходит все
    // фрагменты ради индекса перемотки — то есть ждёт весь трек. Перемотку вперёд за прочитанное
    // делаем сами: пропуском пакетов до нужного времени (skipTs ниже).
    if (url_.compare(0, 4, "hls:") == 0) fmt->flags |= AVFMT_FLAG_IGNIDX;
    if (avformat_open_input(&fmt, nullptr, nullptr, nullptr) < 0) { fmt = nullptr; setError("Unknown audio format"); goto done; }
    if (avformat_find_stream_info(fmt, nullptr) < 0) { setError("Can't read stream info"); goto done; }
    si = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (si < 0 || !codec) { setError("No audio stream / codec"); goto done; }
    if (url_.compare(0, 4, "hls:") == 0) crashLog("[hls] формат %s, кодек %s", fmt->iformat ? fmt->iformat->name : "?", codec->name);

    cc = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(cc, fmt->streams[si]->codecpar);
    cc->thread_count = 1;
    if (avcodec_open2(cc, codec, nullptr) < 0) { setError(std::string("Can't open codec ") + codec->name); goto done; }
    if (cc->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
        av_channel_layout_default(&cc->ch_layout, cc->ch_layout.nb_channels > 0 ? cc->ch_layout.nb_channels : 2);
    if (swr_alloc_set_opts2(&swr, &outLayout, AV_SAMPLE_FMT_S16, OUT_RATE, &cc->ch_layout, cc->sample_fmt,
                            cc->sample_rate, 0, nullptr) < 0 || swr_init(swr) < 0) {
        setError("Resampler init failed");
        goto done;
    }

    {
        auto convert = [&](AVFrame *f) {
            int maxOut = swr_get_out_samples(swr, f ? f->nb_samples : 0);
            if (maxOut <= 0) return;
            size_t old = out.size();
            out.resize(old + (size_t)maxOut * 2);
            uint8_t *outPtr = (uint8_t *)(out.data() + old);
            int got = swr_convert(swr, &outPtr, maxOut, f ? (const uint8_t **)f->extended_data : nullptr,
                                  f ? f->nb_samples : 0);
            out.resize(old + (got > 0 ? (size_t)got * 2 : 0));
        };

        while (!stop_) {
            long long sm = seekMs_.exchange(-1);
            if (sm >= 0) {
                int64_t ts = av_rescale_q(sm * 1000, AVRational{1, 1000000}, fmt->streams[si]->time_base);
                if (av_seek_frame(fmt, si, ts, AVSEEK_FLAG_BACKWARD) >= 0) {
                    avcodec_flush_buffers(cc);
                    swr_close(swr);
                    swr_init(swr);
                }
                skipTs = ts; // демуксер мог встать раньше (в fMP4 — на последний прочитанный фрагмент)
                out.clear();
                std::lock_guard<std::mutex> lk(pm_);
                pcm_.clear();
                head_ = 0;
                framesPlayed_ = (u64)sm * 48;
                continue;
            }
            int rc = av_read_frame(fmt, pkt);
            if (rc < 0) {
                if (rc != AVERROR_EOF && rc != AVERROR_EXIT) setError("Read error");
                if (rc == AVERROR_EOF) finished = true;
                break;
            }
            if (skipTs != AV_NOPTS_VALUE && pkt->stream_index == si && pkt->pts != AV_NOPTS_VALUE) {
                if (pkt->pts + pkt->duration < skipTs) { av_packet_unref(pkt); continue; }
                skipTs = AV_NOPTS_VALUE;
            }
            if (pkt->stream_index == si && avcodec_send_packet(cc, pkt) == 0) {
                while (avcodec_receive_frame(cc, frame) == 0) {
                    convert(frame);
                    av_frame_unref(frame);
                }
            }
            av_packet_unref(pkt);
            if (!out.empty()) {
                if (push(out)) out.clear();
                else if (seekMs_ < 0) break; // остановлены
            }
        }

        if (finished && !stop_) {
            avcodec_send_packet(cc, nullptr);
            while (avcodec_receive_frame(cc, frame) == 0) {
                convert(frame);
                av_frame_unref(frame);
            }
            convert(nullptr);
            if (!out.empty()) push(out);
        }
    }

done:
    swr_free(&swr);
    avcodec_free_context(&cc);
    if (fmt) avformat_close_input(&fmt);
    if (avio) { av_freep(&avio->buffer); avio_context_free(&avio); }
    av_frame_free(&frame);
    av_packet_free(&pkt);
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!error_.empty()) finished = false; // ошибка загрузки важнее ошибки декодера
    }
    if (!finished && !stop_ && error().empty()) setError("Playback stopped");
    finished_ = finished;
    decDone_ = true;
}

// ---------- плеер: вывод в audout и смешивание при переходе ----------

bool Player::init() {
    av_log_set_level(AV_LOG_QUIET);
    if (R_FAILED(audoutInitialize())) return false;
    if (R_FAILED(audoutStartAudioOut())) return false;
    for (int i = 0; i < NUM_BUFS; i++) {
        void *mem = memalign(0x1000, BUF_BYTES);
        if (!mem) return false;
        memset(mem, 0, BUF_BYTES);
        bufs_[i].next = nullptr;
        bufs_[i].buffer = mem;
        bufs_[i].buffer_size = BUF_BYTES;
        bufs_[i].data_size = 0;
        bufs_[i].data_offset = 0;
        free_.push_back(&bufs_[i]);
    }
    mixA_.resize(BUF_FRAMES * 2);
    mixB_.resize(BUF_FRAMES * 2);
    quit_ = false;
    threadCreate(&outThread_, outEntry, this, nullptr, 0x20000, 0x2A, -2);
    threadStart(&outThread_);
    outRunning_ = true;
    return true;
}

void Player::shutdown() {
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
}

void Player::play(const std::string &url, size_t expectedBytes) {
    auto deck = std::make_shared<Deck>(url, expectedBytes);
    std::shared_ptr<Deck> oldA, oldF; // старые деки останавливаются вне замка
    {
        std::lock_guard<std::mutex> lk(m_);
        oldA = std::move(active_);
        oldF = std::move(fading_);
        active_ = deck;
        state_ = Buffering;
    }
    paused_ = false;
    flushReq_ = true;
}

void Player::crossfade(const std::string &url, size_t expectedBytes, int seconds) {
    auto deck = std::make_shared<Deck>(url, expectedBytes);
    std::shared_ptr<Deck> oldF;
    {
        std::lock_guard<std::mutex> lk(m_);
        oldF = std::move(fading_);
        fading_ = std::move(active_);
        active_ = deck;
        cfTotal_ = (u64)std::max(1, seconds) * OUT_RATE;
        cfPos_ = 0;
        if (state_ != Playing) state_ = Buffering;
    }
}

void Player::stop() {
    std::shared_ptr<Deck> oldA, oldF;
    {
        std::lock_guard<std::mutex> lk(m_);
        oldA = std::move(active_);
        oldF = std::move(fading_);
        state_ = Idle;
    }
    paused_ = false;
    flushReq_ = true;
}

void Player::seek(double sec) {
    std::shared_ptr<Deck> a, oldF;
    {
        std::lock_guard<std::mutex> lk(m_);
        a = active_;
        oldF = std::move(fading_); // перемотка посреди перехода — старый трек больше не нужен
    }
    if (!a) return;
    a->seek(sec);
    flushReq_ = true; // выкинуть уже поставленный в очередь звук
}

void Player::setVolume(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    audoutSetAudioOutVolume(percent / 100.0f);
}

void Player::togglePause() {
    State s = state();
    if (s == Playing || s == Buffering) paused_ = true;
    else if (s == Paused) paused_ = false;
}

Player::State Player::state() const {
    State s = state_;
    if (paused_ && (s == Playing || s == Buffering)) return Paused;
    return s;
}

std::shared_ptr<Deck> Player::active() const {
    std::lock_guard<std::mutex> lk(m_);
    return active_;
}

double Player::position() const {
    auto a = active();
    return a ? a->position() : 0;
}

double Player::downloaded() const {
    auto a = active();
    return a ? a->downloaded() : 0;
}

std::string Player::error() const {
    auto a = active();
    return a ? a->error() : std::string();
}

void Player::outEntry(void *arg) { static_cast<Player *>(arg)->outLoop(); }

AudioOutBuffer *Player::takeBuffer() {
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

void Player::submit(AudioOutBuffer *b, size_t frames) {
    b->data_size = frames * 4;
    audoutAppendAudioOutBuffer(b);
    queued_++;
}

void Player::flushOut() {
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

void Player::outLoop() {
    const size_t F = BUF_FRAMES;
    while (!quit_) {
        if (flushReq_.exchange(false)) flushOut();
        std::shared_ptr<Deck> a, f;
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
