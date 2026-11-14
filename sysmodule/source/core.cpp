#include "core.hpp"
#include "crashlog.hpp"
#include <algorithm>
#include <random>
#include <sstream>
#include "http.hpp"
#include "json.hpp"

static const u64 MS = 1000000ULL;
void ymLog(const char *fmt, ...);

static bool loginJustHappened = false; // после входа подгружаем библиотеку, чтобы оверлей не ждал

static std::string esc(const std::string &s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if ((unsigned char)c < 0x20) o += ' ';
        else o += c;
    }
    return o;
}

// Число из настроек приложения (settings.json): громкость, плавный переход; def — если нет.
static int appSetting(const char *key, int def) {
    FILE *f = fopen("sdmc:/switch/yamu-nx/settings.json", "r");
    if (!f) return def;
    char buf[1024] = {0};
    fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    return (int)Json::parse(buf)[key].asInt(def);
}

// Записать громкость в настройки приложения, не трогая остальное.
static void saveAppVolume(int v) {
    const char *path = "sdmc:/switch/yamu-nx/settings.json";
    std::string text;
    if (FILE *f = fopen(path, "r")) {
        char buf[1024];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
        fclose(f);
    }
    size_t p = text.find("\"volume\":");
    if (p != std::string::npos) {
        size_t s = p + 9, e = text.find_first_not_of("0123456789", s);
        text.replace(s, (e == std::string::npos ? text.size() : e) - s, std::to_string(v));
    } else if (!text.empty() && text[0] == '{') {
        text.insert(1, "\"volume\":" + std::to_string(v) + ",");
    } else {
        text = "{\"volume\":" + std::to_string(v) + "}\n";
    }
    if (FILE *f = fopen(path, "w")) {
        fwrite(text.data(), 1, text.size(), f);
        fclose(f);
    }
}

bool Core::init(const char *tokenPath) {
    tokenPath_ = tokenPath;
    api_.noMp4 = true; // mp3 и AAC-поток умеем, контейнер MP4 на лету — нет
    sc_.allowOriginal = false; // SoundCloud: только mp3 (оригиналы и AAC в fMP4 модуль не играет)
    sc_.preferMp3 = true;
    if (!player_.init()) return false;
    player_.setVolume(appSetting("volume", 100));
    crossfade_ = appSetting("crossfade", 0);
    threadCreate(&thread_, entry, this, nullptr, 0x20000, 0x2C, -2);
    threadStart(&thread_);
    return true;
}

void Core::shutdown() {
    exit_ = true;
    threadWaitForExit(&thread_);
    threadClose(&thread_);
    player_.shutdown();
}

void Core::command(const std::string &cmd) {
    std::lock_guard<std::mutex> lk(cmdM_);
    cmds_.push_back(cmd);
}

void Core::entry(void *arg) { static_cast<Core *>(arg)->loop(); }

static bool isNetError(const std::string &e);
static bool ensureLogin(YandexApi &api, bool &loggedIn, const std::string &tokenPath, std::string &err,
                        bool waitNet = false, bool quiet = false);

void Core::loop() {
    while (!exit_) {
        std::string cmd;
        {
            std::lock_guard<std::mutex> lk(cmdM_);
            if (!cmds_.empty()) { cmd = cmds_.front(); cmds_.pop_front(); }
        }
        if (!cmd.empty()) exec(cmd);
        else { tick(); svcSleepThread(50 * MS); }
        // После включения консоли сети ещё нет: пробуем войти сами каждые 5 с, чтобы к первому нажатию
        // в оверлее всё было готово. Не сеть (нет токена и т. п.) — больше не пробуем.
        static u64 bgLoginAt = 0;
        static bool bgLoginGiveUp = false;
        if (!loggedIn_ && !bgLoginGiveUp && armTicksToNs(armGetSystemTick() - bgLoginAt) > 5000000000ULL) {
            bgLoginAt = armGetSystemTick();
            std::string err;
            if (!ensureLogin(api_, loggedIn_, tokenPath_, err, false, true) && !isNetError(err)) bgLoginGiveUp = true;
        }
        if (loginJustHappened) {
            loginJustHappened = false;
            if (!libLoaded_) loadLibrary();
        }
    }
}

static bool isNetError(const std::string &e) {
    return e.find("Network") != std::string::npos || e.find("resolve") != std::string::npos ||
           e.find("connect") != std::string::npos || e.find("Timeout") != std::string::npos;
}

// Вход выполняется лениво — модуль стартует при включении консоли раньше, чем появится сеть.
// waitNet: команда от пользователя — если сети ещё нет, ждём её до 30 с, а не отказываем сразу.
// quiet: фоновая попытка — неудачу без сети в журнал не пишем.
static bool ensureLogin(YandexApi &api, bool &loggedIn, const std::string &tokenPath, std::string &err,
                        bool waitNet, bool quiet) {
    if (loggedIn) return true;
    for (int attempt = 0;; attempt++) {
        if (api.loadToken(tokenPath.c_str()) && api.accountStatus()) break;
        err = api.lastError;
        bool net = isNetError(err);
        if (!waitNet || !net || attempt >= 14) {
            if (!quiet || !net) ymLog("[login] failed: %s", err.c_str());
            return false;
        }
        if (attempt == 0) ymLog("[login] нет сети — жду до 30 с");
        svcSleepThread(2000000000ULL);
    }
    ymLog("[login] ok");
    loggedIn = true;
    loginJustHappened = true;
    return true;
}

void Core::exec(const std::string &cmdLine) {
    ymLog("[cmd] %s", cmdLine.c_str());
    std::istringstream in(cmdLine);
    std::string cmd;
    in >> cmd;

    if (cmd == "TOGGLE") {
        if (current_ < 0) { startWave(); return; } // ничего не играло — включаем волну
        player_.setPaused(!player_.paused());
        return;
    }
    if (cmd == "PAUSE") { player_.setPaused(true); return; }
    if (cmd == "RESUME") { player_.setPaused(false); return; }
    if (cmd == "PREPARE") { // PREPARE <trackId>: войти и найти ссылку заранее
        std::string id;
        in >> id;
        std::string err, url, codec;
        int kbps = 0;
        if (!id.empty() && offlineFile(id).empty() && (isSoundCloudId(id) || ensureLogin(api_, loggedIn_, tokenPath_, err)) &&
            streamUrl(id, url, kbps, codec, err)) {
            prepId_ = id;
            prepUrl_ = url;
            prepCodec_ = codec;
            prepKbps_ = kbps;
            prepTick_ = armGetSystemTick();
            ymLog("[prepare] %s ok", id.c_str());
        }
        return;
    }
    if (cmd == "HANDOFF") { handoffFromFile(); return; }
    if (cmd == "VOLUME" || cmd == "VOLREL") { // VOLUME <0..100> или VOLREL <±шаг>
        int v = 0;
        in >> v;
        player_.setVolume(cmd == "VOLUME" ? v : player_.volume() + v);
        saveAppVolume(player_.volume()); // приложение подхватит её, когда заберёт музыку
        return;
    }
    if (cmd == "SHUFFLE") { toggleShuffle(); return; }
    if (cmd == "OFFLINE") { // OFFLINE <1|0>: играть скачанное (сеть не нужна)
        int shuffle = 0;
        in >> shuffle;
        playOffline(shuffle != 0);
        return;
    }
    if (cmd == "SEEK" || cmd == "SEEKREL") { // SEEK <сек> или SEEKREL <±сек>
        double v = 0;
        in >> v;
        seekTo(cmd == "SEEK" ? v : player_.position() + v);
        return;
    }
    if (cmd == "RELEASE") { releaseToFile(); return; }
    if (cmd == "STOP") {
        player_.stop();
        std::lock_guard<std::mutex> lk(stateM_);
        current_ = -1;
        return;
    }

    // Без сети переключать треки можно: скачанные играют с карты.
    std::string err;
    if (cmd != "NEXT" && cmd != "PREV" && !ensureLogin(api_, loggedIn_, tokenPath_, err, true)) {
        std::lock_guard<std::mutex> lk(stateM_);
        error_ = "Вход: " + err;
        return;
    }

    if (cmd == "WAVE") { player_.setPaused(false); startWave(); }
    else if (cmd == "NEXT") {
        if (wave_ && loggedIn_ && current_ >= 0) api_.waveFeedback("skip", &queue_[current_], player_.position());
        playIndex(current_ + 1);
    } else if (cmd == "PREV") {
        playIndex(player_.position() > 3 || current_ <= 0 ? current_ : current_ - 1);
    } else if (cmd == "LIKE") {
        Track t;
        bool like;
        {
            std::lock_guard<std::mutex> lk(stateM_);
            if (current_ < 0 || current_ >= (int)queue_.size()) return;
            t = queue_[current_];
            like = !liked_.count(t.id);
            if (like) liked_.insert(t.id);
            else liked_.erase(t.id);
        }
        if (!(isSoundCloudId(t.id) ? sc_.setLike(t.id, like) : api_.setLike(t, like))) {
            std::lock_guard<std::mutex> lk(stateM_);
            if (like) liked_.erase(t.id);
            else liked_.insert(t.id);
            error_ = "Лайк: " + (isSoundCloudId(t.id) ? sc_.lastError : api_.lastError);
        }
    } else if (cmd == "LIBRARY") {
        loadLibrary();
    } else if (cmd == "PLAYLIST") {
        std::string owner, kind;
        int shuffle = 0;
        in >> owner >> kind >> shuffle;
        Playlist pl;
        pl.owner = owner;
        pl.kind = kind == "-" ? "" : kind;
        std::vector<Track> tracks;
        bool ok;
        if (pl.kind.empty()) {
            std::vector<std::string> ids;
            ok = api_.likedIds(ids);
            if (ok) {
                if (ids.size() > 200) ids.resize(200); // память модуля не резиновая
                ok = api_.tracksByIds(ids, tracks);
                std::lock_guard<std::mutex> lk(stateM_);
                liked_.insert(ids.begin(), ids.end());
            }
            pl.title = "Мне нравится";
        } else {
            ok = api_.playlistTracks(pl, tracks);
            std::lock_guard<std::mutex> lk(stateM_);
            for (auto &p : playlists_)
                if (p.kind == pl.kind && p.owner == pl.owner) pl.title = p.title;
        }
        if (!ok || tracks.empty()) {
            std::lock_guard<std::mutex> lk(stateM_);
            error_ = ok ? "Плейлист пуст" : "Плейлист: " + api_.lastError;
            return;
        }
        if (shuffle) std::shuffle(tracks.begin(), tracks.end(), std::mt19937((unsigned)armGetSystemTick()));
        {
            std::lock_guard<std::mutex> lk(stateM_);
            wave_ = false;
            scWave_ = false;
            sourceTitle_ = pl.title.empty() ? "Плейлист" : pl.title;
            queue_ = tracks;
            current_ = -1;
            shuffle_ = false;
            unshuffled_.clear();
        }
        player_.setPaused(false);
        playIndex(0);
    }
}

void Core::startWave() {
    std::string err;
    if (!ensureLogin(api_, loggedIn_, tokenPath_, err, true)) {
        std::lock_guard<std::mutex> lk(stateM_);
        error_ = "Вход: " + err;
        return;
    }
    player_.stop();
    std::vector<Track> tracks;
    if (!api_.waveTracks(tracks)) {
        std::lock_guard<std::mutex> lk(stateM_);
        error_ = "Волна: " + api_.lastError;
        return;
    }
    api_.waveFeedback("radioStarted");
    {
        std::lock_guard<std::mutex> lk(stateM_);
        wave_ = true;
        scWave_ = false;
        sourceTitle_ = "Моя волна";
        queue_ = tracks;
        current_ = -1;
        shuffle_ = false;
        unshuffled_.clear();
    }
    playIndex(0);
}

static const char *OFFLINE_INDEX = "sdmc:/switch/yamu-nx/offline/index.json";

static std::string readOfflineIndex() {
    // Список скачанного ведёт приложение; читаем заново — он мог измениться.
    FILE *f = fopen(OFFLINE_INDEX, "r");
    if (!f) return "";
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    fclose(f);
    return text;
}

std::string Core::offlineFile(const std::string &id) {
    std::string text = readOfflineIndex();
    if (text.find("\"" + id + "\"") == std::string::npos) return ""; // быстро: такого id нет вовсе
    Json arr = Json::parse(text);
    for (size_t i = 0; i < arr.size(); i++) {
        if (arr[i]["id"].asString() != id) continue;
        std::string file = arr[i]["file"].asString();
        // SoundCloud: с карты модуль играет только mp3 (AAC в fMP4 и оригиналы — нет, тогда — поток).
        if (isSoundCloudId(id) && (file.size() < 4 || file.compare(file.size() - 4, 4, ".mp3") != 0)) return "";
        FILE *t = file.empty() ? nullptr : fopen(file.c_str(), "rb");
        if (!t) return "";
        fclose(t);
        return file;
    }
    return "";
}

static std::string codecOfFile(const std::string &file) {
    size_t dot = file.rfind('.');
    std::string ext = dot == std::string::npos ? "" : file.substr(dot + 1);
    return ext == "m4a" || ext == "mp4" ? "m4a" : ext == "aac" ? "aac" : "mp3";
}

void Core::playIndex(int idx, double startSec) {
    player_.stop();
    if (idx < 0) idx = 0;
    std::string err;
    // Не стучимся в сеть на каждом треке: после неудачи ждём минуту.
    static u64 lastLoginFail = 0;
    bool online = loggedIn_;
    if (!online) { // треку SoundCloud вход в Яндекс не нужен
        std::lock_guard<std::mutex> lk(stateM_);
        if (idx < (int)queue_.size() && isSoundCloudId(queue_[idx].id)) online = true;
    }
    if (!online && (!lastLoginFail || armTicksToNs(armGetSystemTick() - lastLoginFail) > 60000000000ULL)) {
        online = ensureLogin(api_, loggedIn_, tokenPath_, err);
        if (!online) lastLoginFail = armGetSystemTick();
    }
    if (!online) { // без сети — ближайший скачанный трек, остальные не сыграть
        int from = idx;
        while (idx < (int)queue_.size() && offlineFile(queue_[idx].id).empty()) idx++;
        if (idx >= (int)queue_.size()) {
            std::lock_guard<std::mutex> lk(stateM_);
            current_ = from < (int)queue_.size() ? from : -1;
            error_ = "Нет сети, а дальше ничего не скачано";
            failStreak_ = 100; // не перебирать по кругу
            return;
        }
        if (idx != from) startSec = 0;
    }
    if (scWave_ && !queue_.empty() && idx >= (int)queue_.size() - 2) { // волна SoundCloud: похожие на последний
        std::vector<Track> more;
        if (sc_.related(queue_.back().id, more)) {
            std::lock_guard<std::mutex> lk(stateM_);
            std::set<std::string> have;
            for (auto &q : queue_) have.insert(q.id);
            for (auto &m : more) if (have.insert(m.id).second) queue_.push_back(m);
            if (current_ > 20) { // старое не держим — экономим память
                queue_.erase(queue_.begin(), queue_.begin() + (current_ - 5));
                idx -= current_ - 5;
                current_ = 5;
            }
        }
    }
    if (online && wave_ && idx >= (int)queue_.size() - 1) { // подгружаем волну заранее
        std::vector<Track> more;
        if (api_.waveTracks(more, queue_.empty() ? "" : queue_.back().id)) {
            std::lock_guard<std::mutex> lk(stateM_);
            queue_.insert(queue_.end(), more.begin(), more.end());
            // Старые треки волны не нужны — экономим память.
            if (current_ > 20) {
                queue_.erase(queue_.begin(), queue_.begin() + (current_ - 5));
                idx -= current_ - 5;
                current_ = 5;
            }
        }
    }
    Track t;
    {
        std::lock_guard<std::mutex> lk(stateM_);
        if (idx >= (int)queue_.size()) { current_ = -1; error_ = "Очередь закончилась"; return; }
        current_ = idx;
        error_.clear();
        t = queue_[idx];
    }
    std::string url, codec;
    int bitrate = 0;
    double dur = t.durationMs / 1000.0;
    // Скачанный трек играем с карты — без сети и без запроса ссылки.
    std::string file = offlineFile(t.id);
    if (!file.empty()) {
        url = file;
        codec = codecOfFile(file);
        ymLog("[track] %s: from sd (%s)", t.id.c_str(), codec.c_str());
    } else {
        // Ссылку могли найти заранее (PREPARE из приложения) — тогда передача почти мгновенная.
        bool cached = t.id == prepId_ && armTicksToNs(armGetSystemTick() - prepTick_) < 300000000000ULL;
        if (cached) {
            url = prepUrl_;
            codec = prepCodec_;
            bitrate = prepKbps_;
            ymLog("[track] %s: prepared url", t.id.c_str());
        }
        std::string uerr;
        if (!cached && !streamUrl(t.id, url, bitrate, codec, uerr)) {
            std::lock_guard<std::mutex> lk(stateM_);
            error_ = uerr;
            ymLog("[track] %s: %s", t.id.c_str(), error_.c_str());
            failStreak_++;
            return; // tick() пропустит трек
        }
        if (codec.empty()) codec = "mp3"; // старый способ (download-info) отдаёт mp3
        ymLog("[track] %s: %s %d kbps", t.id.c_str(), codec.c_str(), bitrate);
    }
    failStreak_ = 0;
    curSrc_ = url; // для перемотки: подписанная ссылка живёт дольше трека
    curCodec_ = codec;
    curKbps_ = bitrate;
    curDur_ = dur;
    player_.play(url, codec, startSec, bitrate, dur);
    if (wave_ && online) api_.waveFeedback("trackStarted", &t);
}

// Переключение по окончании трека или при ошибке.
void Core::tick() {
    if (current_ < 0) return;
    SysPlayer::State st = player_.state();
    bool unresolved = st == SysPlayer::Idle && !error_.empty();
    if (st == SysPlayer::Finished) {
        if (wave_ && loggedIn_) api_.waveFeedback("trackFinished", &queue_[current_], player_.position());
        int next = repeat_ == 2 ? current_ : current_ + 1; // 2 — повтор трека
        if (repeat_ == 1 && !wave_ && next >= (int)queue_.size()) next = 0; // 1 — повтор плейлиста
        playIndex(next);
    } else if ((st == SysPlayer::Error || unresolved) && failStreak_ < 10) {
        if (st == SysPlayer::Error) {
            std::lock_guard<std::mutex> lk(stateM_);
            error_ = player_.error();
        }
        svcSleepThread(1500 * MS);
        playIndex(current_ + 1);
    } else if (st == SysPlayer::Playing) {
        maybeCrossfade();
    }
}

// Плавный переход: за ~20 с до конца готовим ссылку на следующий трек, за crossfade_ секунд — запускаем
// его поверх текущего. Не успели подготовить — обычная смена по окончании.
void Core::maybeCrossfade() {
    if (crossfade_ <= 0 || repeat_ == 2) return;
    int next = current_ + 1;
    if (next >= (int)queue_.size()) {
        if (repeat_ == 1 && !wave_ && !queue_.empty()) next = 0;
        else return; // волну playIndex подгружает заранее, так что здесь это конец очереди
    }
    const Track &cur = queue_[current_];
    Track nt = queue_[next];
    double dur = cur.durationMs / 1000.0, pos = player_.position();
    if (dur < crossfade_ * 2 + 10) return; // слишком короткий трек

    if (xfId_ != nt.id) {
        if (pos < dur - crossfade_ - 20) return;
        xfId_ = nt.id;
        xfSrc_.clear();
        std::string file = offlineFile(nt.id), url, codec, err;
        int kbps = 0;
        if (!file.empty()) {
            xfSrc_ = file;
            xfCodec_ = codecOfFile(file);
            xfKbps_ = 0;
        } else if ((loggedIn_ || isSoundCloudId(nt.id)) && streamUrl(nt.id, url, kbps, codec, err)) {
            xfSrc_ = url;
            xfCodec_ = codec.empty() ? "mp3" : codec;
            xfKbps_ = kbps;
        }
        ymLog("[xfade] %s: %s", nt.id.c_str(), xfSrc_.empty() ? "no source" : xfCodec_.c_str());
        return;
    }
    if (xfSrc_.empty() || pos < dur - crossfade_) return;

    {
        unsigned used = 0, freeKb = 0;
        heapKb(used, freeKb); // пик памяти: сейчас живут две деки
        ymLog("[xfade] -> %s (%d s), куча %u КБ", nt.id.c_str(), crossfade_, used);
    }
    if (wave_ && loggedIn_) api_.waveFeedback("trackFinished", &cur, pos);
    double ndur = nt.durationMs / 1000.0;
    player_.crossfade(xfSrc_, xfCodec_, xfKbps_, ndur, crossfade_);
    {
        std::lock_guard<std::mutex> lk(stateM_);
        current_ = next;
        error_.clear();
    }
    curSrc_ = xfSrc_;
    curCodec_ = xfCodec_;
    curKbps_ = xfKbps_;
    curDur_ = ndur;
    xfSrc_.clear();
    if (wave_ && loggedIn_) {
        api_.waveFeedback("trackStarted", &nt);
        if (next >= (int)queue_.size() - 1) { // волна кончается — подгружаем дальше
            std::vector<Track> more;
            if (api_.waveTracks(more, queue_.back().id)) {
                std::lock_guard<std::mutex> lk(stateM_);
                queue_.insert(queue_.end(), more.begin(), more.end());
            }
        }
    }
}

// Перемешать оставшееся после текущего трека; повторно — вернуть исходный порядок. Волну не трогаем.
void Core::toggleShuffle() {
    std::lock_guard<std::mutex> lk(stateM_);
    if (wave_ || current_ < 0 || current_ >= (int)queue_.size()) return;
    if (!shuffle_) {
        unshuffled_ = queue_;
        std::shuffle(queue_.begin() + current_ + 1, queue_.end(), std::mt19937((unsigned)armGetSystemTick()));
        shuffle_ = true;
    } else {
        std::string id = queue_[current_].id;
        queue_ = unshuffled_;
        for (int i = 0; i < (int)queue_.size(); i++)
            if (queue_[i].id == id) { current_ = i; break; }
        unshuffled_.clear();
        shuffle_ = false;
    }
}

// «Скачанное» из оверлея: очередь из offline/index.json приложения, сеть не нужна.
void Core::playOffline(bool shuffle) {
    Json arr = Json::parse(readOfflineIndex());
    std::vector<Track> tracks;
    for (size_t i = 0; i < arr.size(); i++) {
        Track t;
        t.id = arr[i]["id"].asString();
        t.albumId = arr[i]["album"].asString();
        t.title = arr[i]["title"].asString();
        t.artists = arr[i]["artists"].asString();
        t.durationMs = arr[i]["dur"].asInt();
        t.coverUri = arr[i]["coverUri"].asString();
        if (!t.id.empty()) tracks.push_back(t);
    }
    if (tracks.empty()) {
        std::lock_guard<std::mutex> lk(stateM_);
        error_ = "Ничего не скачано";
        return;
    }
    if (shuffle) std::shuffle(tracks.begin(), tracks.end(), std::mt19937((unsigned)armGetSystemTick()));
    {
        std::lock_guard<std::mutex> lk(stateM_);
        wave_ = false;
        scWave_ = false;
        shuffle_ = false;
        unshuffled_.clear();
        sourceTitle_ = "Скачанное";
        queue_ = tracks;
        current_ = -1;
    }
    player_.setPaused(false);
    playIndex(0);
}

void Core::loadLibrary() {
    std::vector<Playlist> pls;
    std::vector<std::string> ids;
    bool ok = api_.playlists(pls);
    api_.likedIds(ids);
    std::lock_guard<std::mutex> lk(stateM_);
    if (!ok) { error_ = "Библиотека: " + api_.lastError; return; }
    playlists_ = pls;
    liked_.insert(ids.begin(), ids.end());
    libLoaded_ = true;
}

std::string Core::statusJson() {
    std::lock_guard<std::mutex> lk(stateM_);
    std::ostringstream o;
    SysPlayer::State st = player_.state();
    const char *s = st == SysPlayer::Playing ? "playing" : st == SysPlayer::Paused ? "paused"
                  : st == SysPlayer::Buffering ? "buffering" : st == SysPlayer::Error ? "error" : "idle";
    o << "{\"state\":\"" << s << "\",\"source\":\"" << esc(sourceTitle_) << "\"";
    o << ",\"shuffle\":" << (shuffle_ ? "true" : "false") << ",\"wave\":" << (wave_ ? "true" : "false");
    o << ",\"vol\":" << player_.volume();
    if (current_ >= 0 && current_ < (int)queue_.size()) {
        const Track &t = queue_[current_];
        o << ",\"title\":\"" << esc(t.title) << "\",\"artists\":\"" << esc(t.artists) << "\""
          << ",\"pos\":" << (int)player_.position() << ",\"dur\":" << (int)(t.durationMs / 1000)
          << ",\"liked\":" << (liked_.count(t.id) ? "true" : "false")
          << ",\"ready\":" << (player_.ready() ? "true" : "false")
          << ",\"id\":\"" << esc(t.id) << "\"";
    }
    if (!error_.empty()) o << ",\"error\":\"" << esc(error_) << "\"";
    o << "}";
    return o.str();
}

std::string Core::libraryJson() {
    bool loaded;
    {
        std::lock_guard<std::mutex> lk(stateM_);
        loaded = libLoaded_;
    }
    if (!loaded) {
        command("LIBRARY");
        for (int i = 0; i < 40 && !loaded; i++) { // до 4 секунд — дольше оверлей держать нельзя
            svcSleepThread(100 * MS);
            std::lock_guard<std::mutex> lk(stateM_);
            loaded = libLoaded_;
        }
    }
    std::lock_guard<std::mutex> lk(stateM_);
    std::ostringstream o;
    o << "[{\"t\":\"Мне нравится\",\"o\":\"" << esc(api_.uid) << "\",\"k\":\"-\",\"n\":" << liked_.size() << "}";
    for (auto &p : playlists_)
        o << ",{\"t\":\"" << esc(p.title) << "\",\"o\":\"" << esc(p.owner) << "\",\"k\":\"" << esc(p.kind)
          << "\",\"n\":" << p.trackCount << "}";
    o << "]";
    return o.str();
}

// ---------- передача музыки между приложением и модулем ----------

static const char *HANDOFF_PATH = "sdmc:/switch/yamu-nx/handoff.json";

// Приложение записало, что играло, — продолжаем с того же места.
void Core::handoffFromFile() {
    FILE *f = fopen(HANDOFF_PATH, "r");
    if (!f) { ymLog("[handoff] no file"); return; }
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    fclose(f);
    Json j = Json::parse(text);
    std::vector<Track> tracks;
    std::vector<std::string> likedIds;
    const Json &arr = j["tracks"];
    for (size_t i = 0; i < arr.size(); i++) {
        Track t;
        t.id = arr[i]["id"].asString();
        t.albumId = arr[i]["album"].asString();
        t.title = arr[i]["title"].asString();
        t.artists = arr[i]["artists"].asString();
        t.durationMs = arr[i]["dur"].asInt();
        t.coverUri = arr[i]["coverUri"].asString();
        if (arr[i]["liked"].asBool()) likedIds.push_back(t.id); // лайки SoundCloud модуль сам не знает
        if (!t.id.empty()) tracks.push_back(t);
    }
    int cur = (int)j["current"].asInt(-1);
    double pos = j["pos"].type == Json::Number ? j["pos"].num : 0;
    ymLog("[handoff] %d tracks, current=%d pos=%d", (int)tracks.size(), cur, (int)pos);
    if (tracks.empty() || cur < 0 || cur >= (int)tracks.size()) return;

    // Без сети не страшно: скачанные треки сыграем с карты (playIndex разберётся сам).
    {
        std::lock_guard<std::mutex> lk(stateM_);
        wave_ = j["wave"].asBool();
        scWave_ = j["scWave"].asBool();
        liked_.insert(likedIds.begin(), likedIds.end());
        repeat_ = (int)j["repeat"].asInt(0);
        sourceTitle_ = j["source"].asString(wave_ ? "Моя волна" : "Плейлист");
        queue_ = tracks;
        current_ = -1;
        shuffle_ = false;
        unshuffled_.clear();
    }
    player_.setVolume(appSetting("volume", 100)); // громкость и переход — как в приложении
    crossfade_ = appSetting("crossfade", 0);
    player_.setPaused(j["paused"].asBool());
    playIndex(cur, pos);
}

// Приложение забирает музыку: записываем состояние и замолкаем.
void Core::releaseToFile() {
    bool had = false;
    {
        std::lock_guard<std::mutex> lk(stateM_);
        if (current_ >= 0 && current_ < (int)queue_.size()) {
            FILE *f = fopen(HANDOFF_PATH, "w");
            if (f) {
                // Волну отдаём с текущего трека (дальше приложение подгрузит само), плейлист — целиком.
                int from = wave_ ? current_ : 0, to = (int)queue_.size();
                if (wave_ && to - from > 30) to = from + 30;
                fprintf(f, "{\"wave\":%s,\"source\":\"%s\",\"current\":%d,\"pos\":%.1f,\"paused\":%s,\"repeat\":%d,\"tracks\":[",
                        wave_ ? "true" : "false", esc(sourceTitle_).c_str(), current_ - from, player_.position(),
                        player_.paused() ? "true" : "false", repeat_);
                for (int i = from; i < to; i++) {
                    const Track &t = queue_[i];
                    fprintf(f, "%s{\"id\":\"%s\",\"album\":\"%s\",\"title\":\"%s\",\"artists\":\"%s\",\"dur\":%lld,\"coverUri\":\"%s\"}",
                            i > from ? "," : "", esc(t.id).c_str(), esc(t.albumId).c_str(), esc(t.title).c_str(),
                            esc(t.artists).c_str(), t.durationMs, esc(t.coverUri).c_str());
                }
                fputs("]}", f);
                fclose(f);
                had = true;
            }
        }
    }
    ymLog("[release] had=%d", had ? 1 : 0);
    player_.stop();
    {
        std::lock_guard<std::mutex> lk(stateM_);
        current_ = -1;
    }
    releaseHad_ = had;
    releaseDone_ = true;
}

bool Core::releaseSync() {
    releaseDone_ = false;
    command("RELEASE");
    for (int i = 0; i < 60 && !releaseDone_; i++) svcSleepThread(50000000ULL); // до 3 секунд
    return releaseDone_ && releaseHad_;
}
// Перемотка: поток перезапрашиваем с нужного байта (HTTP Range), m4a с карты — точно по кадрам; пауза сохраняется.
void Core::seekTo(double sec) {
    double dur = 0;
    {
        std::lock_guard<std::mutex> lk(stateM_);
        if (current_ < 0 || current_ >= (int)queue_.size() || curSrc_.empty()) return;
        dur = queue_[current_].durationMs / 1000.0;
    }
    if (sec < 0) sec = 0;
    if (dur > 3 && sec > dur - 2) sec = dur - 2;
    ymLog("[seek] %d s", (int)sec);
    player_.play(curSrc_, curCodec_, sec, curKbps_, curDur_);
}
// Адрес потока по id трека: SoundCloud ("sc:…") — свой mp3-поток HLS, остальное — Яндекс.
bool Core::streamUrl(const std::string &id, std::string &url, int &kbps, std::string &codec, std::string &err) {
    if (isSoundCloudId(id)) {
        if (!sc_.streamUrl(id, url, kbps, codec)) { err = sc_.lastError; return false; }
        codec = "mp3"; // модулю выбираем только mp3 (preferMp3), «отрывок» в названии кодека ему не нужен
        return true;
    }
    if (!api_.trackUrl(id, url, kbps, codec)) { err = api_.lastError; return false; }
    return true;
}
