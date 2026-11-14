#include "worker.hpp"
#include "http.hpp"
#include "crashlog.hpp"
#include <cctype>
#include <algorithm>
#include <random>
#include <cstdio>
#include <sys/stat.h>
#include <SDL2/SDL_image.h>

static const size_t LIKED_CHUNK = 300; // «Мне нравится» грузим порциями по столько треков

void Worker::start(const char *tokenPath) {
    tokenPath_ = tokenPath;
    stop_ = false;
    threadCreate(&thread_, entry, this, nullptr, 0x80000, 0x2C, -2);
    threadStart(&thread_);
    running_ = true;
}

void Worker::stop() {
    if (!running_) return;
    stop_ = true;
    threadWaitForExit(&thread_);
    threadClose(&thread_);
    running_ = false;
    std::lock_guard<std::mutex> lk(m_);
    for (auto &r : results_)
        if (r.surface) SDL_FreeSurface(r.surface);
    results_.clear();
    jobs_.clear();
}

void Worker::post(const Job &job) {
    std::lock_guard<std::mutex> lk(m_);
    jobs_.push_back(job);
}

void Worker::cancel(Job::Kind kind) {
    std::lock_guard<std::mutex> lk(m_);
    jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(), [kind](const Job &j) { return j.kind == kind; }),
                jobs_.end());
}

size_t Worker::pending(Job::Kind kind) {
    std::lock_guard<std::mutex> lk(m_);
    return std::count_if(jobs_.begin(), jobs_.end(), [kind](const Job &j) { return j.kind == kind; });
}

bool Worker::poll(JobResult &out) {
    std::lock_guard<std::mutex> lk(m_);
    if (results_.empty()) return false;
    out = results_.front();
    results_.pop_front();
    return true;
}

void Worker::entry(void *arg) { static_cast<Worker *>(arg)->loop(); }

void Worker::loop() {
    while (!stop_) {
        Job job;
        {
            std::lock_guard<std::mutex> lk(m_);
            if (!jobs_.empty()) {
                // Отметки волны и обложки ждут, пока не разберёмся с треком.
                auto it = std::find_if(jobs_.begin(), jobs_.end(), [](const Job &j) {
                    return j.kind != Job::Cover && j.kind != Job::Feedback;
                });
                if (it == jobs_.end()) it = jobs_.begin();
                job = *it;
                jobs_.erase(it);
            } else {
                job.kind = Job::Kind(-1);
            }
        }
        if (job.kind == Job::Kind(-1)) { svcSleepThread(10000000ULL); continue; }
        JobResult r = run(job);
        if (job.kind == Job::Feedback) continue;
        std::lock_guard<std::mutex> lk(m_);
        results_.push_back(r);
    }
}

JobResult Worker::run(const Job &job) {
    JobResult r;
    r.kind = job.kind;
    switch (job.kind) {
        case Job::Startup:
            r.fresh = true;
            if (!api_.loadToken(tokenPath_.c_str())) { r.error = api_.lastError; break; }
            if (!api_.accountStatus()) { r.error = api_.lastError; break; }
            // Волну не грузим: при старте ничего не играет, пользователь выбирает сам.
            r.login = api_.login;
            r.hasPlus = api_.hasPlus;
            r.ok = true;
            break;

        case Job::LoadWave:
            r.fresh = job.fresh;
            if (job.fresh && !job.mood.empty()) api_.waveSettings(job.mood, job.diversity); // не вышло — сыграем как есть
            r.ok = api_.waveTracks(r.tracks, job.lastTrackId);
            if (!r.ok) r.error = api_.lastError;
            else if (job.fresh) api_.waveFeedback("radioStarted");
            break;

        case Job::Resolve:
            r.index = job.index;
            r.preload = job.preload;
            r.gen = job.gen;
            if (isSoundCloudId(job.track.id)) { // SoundCloud: адрес потока HLS
                r.ok = sc_.streamUrl(job.track.id, r.url, r.bitrate, r.codec);
                // Оригинал: битрейт — средний по размеру файла (по нему же плеер заранее берёт память).
                if (r.ok && sc_.originalBytes && job.track.durationMs > 0)
                    r.bitrate = (int)((unsigned long long)sc_.originalBytes * 8 / (unsigned long long)job.track.durationMs);
                if (!r.ok) r.error = sc_.lastError;
                break;
            }
            r.ok = api_.trackUrl(job.track.id, r.url, r.bitrate, r.codec);
            if (!r.ok) r.error = api_.lastError;
            break;

        case Job::Feedback:
            api_.waveFeedback(job.feedbackType, job.track.id.empty() ? nullptr : &job.track, job.played);
            r.ok = true;
            break;

        case Job::Library:
            r.ok = api_.playlists(r.playlists);
            if (!r.ok) { r.error = api_.lastError; break; }
            api_.likedIds(r.likedIds); // без лайков библиотека всё равно работает
            break;

        case Job::OpenList:
            r.playlist = job.playlist;
            r.gen = job.gen;
            if (job.playlist.kind.empty()) { // «Мне нравится»
                std::vector<std::string> ids;
                r.ok = api_.likedIds(ids);
                if (r.ok) {
                    // Первые 300 (свежие лайки идут первыми) — сразу, остальное догружаем порциями
                    // отдельными задачами, чтобы список открылся быстро и не ждал тысячи треков.
                    std::vector<std::string> first(ids.begin(), ids.begin() + std::min<size_t>(ids.size(), LIKED_CHUNK));
                    r.ok = api_.tracksByIds(first, r.tracks);
                    if (r.ok && ids.size() > first.size()) {
                        Job more{Job::ListMore};
                        more.playlist = job.playlist;
                        more.gen = job.gen;
                        more.ids.assign(ids.begin() + first.size(), ids.end());
                        r.more = true;
                        post(more);
                    }
                }
                r.likedIds = ids;
            } else if (job.playlist.kind == "@sc") { // SoundCloud: urn плейлиста (и секрет) — в owner
                const std::string &o = job.playlist.owner;
                if (o == "@likes") r.ok = sc_.myLikes(r.tracks);
                else if (o == "@feed") r.ok = sc_.myFeed(r.tracks);
                else if (o == "@wave") { // волна: случайный трек из лайков и похожие на него
                    std::vector<Track> likes;
                    r.ok = sc_.myLikes(likes);
                    if (r.ok && likes.empty()) { r.ok = false; sc_.lastError = "Для волны нужны лайки в SoundCloud"; }
                    if (r.ok) {
                        std::mt19937 rng((unsigned)armGetSystemTick());
                        const Track &seed = likes[rng() % likes.size()];
                        r.tracks.push_back(seed);
                        std::vector<Track> rel;
                        sc_.related(seed.id, rel);
                        std::shuffle(rel.begin(), rel.end(), rng);
                        for (auto &t : rel) if (t.id != seed.id) r.tracks.push_back(t);
                    }
                } else r.ok = sc_.playlistTracks(o, r.tracks);
                if (!r.ok) r.error = sc_.lastError;
                break;
            } else if (job.playlist.kind == "@album") { // альбом: id — в owner
                r.ok = api_.albumTracks(job.playlist.owner, r.tracks);
            } else {
                r.ok = api_.playlistTracks(job.playlist, r.tracks);
            }
            if (!r.ok) r.error = api_.lastError;
            break;

        case Job::Like:
            r.trackId = job.track.id;
            r.like = job.like;
            if (isSoundCloudId(job.track.id)) { // SoundCloud — от имени вошедшего аккаунта
                r.ok = sc_.setLike(job.track.id, job.like);
                if (!r.ok) r.error = sc_.lastError;
                break;
            }
            r.ok = api_.setLike(job.track, job.like);
            if (!r.ok) r.error = api_.lastError;
            break;

        case Job::LoginStart:
            r.ok = YandexApi::deviceCodeStart(job.code, "Nintendo Switch (YaMu-nx)", r.device, r.error);
            break;

        case Job::LoginPoll: {
            int rc = YandexApi::deviceCodePoll(job.code, r.token, r.error);
            r.ok = rc == 1;
            r.pending = rc == 0;
            if (r.ok) { // новый токен — в файл и в API
                mkdir("sdmc:/switch/yamu-nx", 0777);
                if (FILE *f = fopen(tokenPath_.c_str(), "w")) {
                    fputs(r.token.c_str(), f);
                    fclose(f);
                }
                api_ = YandexApi();
            }
            break;
        }

        case Job::SearchAll:
            r.query = job.query;
            r.ok = api_.searchAll(job.query, r.search);
            if (!r.ok) r.error = api_.lastError;
            break;

        case Job::Artist:
            r.artistId = job.artistId;
            if (isSoundCloudId(job.artistId)) { // автор SoundCloud: его треки
                r.ok = sc_.userTracks(job.artistId.substr(3), r.tracks);
                if (!r.ok) r.error = sc_.lastError;
                break;
            }
            r.ok = api_.artistPage(job.artistId, r.tracks, r.cards, r.cards2);
            if (!r.ok) r.error = api_.lastError;
            break;

        case Job::LikedAlbums:
            r.ok = api_.likedAlbums(r.cards);
            if (!r.ok) r.error = api_.lastError;
            break;

        case Job::ScSearch: // поиск в SoundCloud: треки — в tracks, плейлисты — в cards
            r.query = job.query;
            r.ok = sc_.searchTracks(job.query, r.tracks);
            if (r.ok) sc_.searchPlaylists(job.query, r.cards); // без плейлистов страница всё равно полезна
            if (!r.ok) r.error = sc_.lastError;
            break;

        case Job::ScResolve: { // ссылка на SoundCloud: трек, плейлист или автор → список треков
            r.query = job.query;
            ScResolved res;
            r.ok = sc_.resolve(job.query, res);
            if (!r.ok) { r.error = sc_.lastError; break; }
            r.tracks = res.tracks;
            r.lyrics = res.title; // название найденного (поле свободно у этого вида задач)
            if (r.tracks.empty()) { r.ok = false; r.error = "По ссылке нечего играть"; }
            break;
        }

        case Job::ScLogin: // код из страницы входа (query) + verifier (url) → токен аккаунта; пустой код — выход
            r.query = job.query;
            if (job.query.empty()) { sc_.logout(); r.ok = true; break; }
            r.ok = sc_.login(job.query, job.url);
            if (r.ok) r.login = SoundCloud::userName();
            else r.error = sc_.lastError;
            break;

        case Job::ScHome: // главная SoundCloud: плейлисты — в cards, свежее от подписок — в tracks
            r.ok = sc_.myPlaylists(r.cards);
            if (r.ok || SoundCloud::loggedIn()) {
                sc_.myFeed(r.tracks);
                std::vector<Track> likes; // свежие лайки — чтобы в плеере было видно, что трек лайкнут
                sc_.myLikes(likes, 1);
                for (auto &t : likes) r.likedIds.push_back(t.id);
                r.ok = true;
            }
            if (!r.ok) r.error = sc_.lastError;
            break;

        case Job::ScRelated: // волна SoundCloud: похожие на трек (track) — продолжение очереди
            r.ok = sc_.related(job.track.id, r.tracks);
            if (r.ok) std::shuffle(r.tracks.begin(), r.tracks.end(), std::mt19937((unsigned)armGetSystemTick()));
            else r.error = sc_.lastError;
            break;

        case Job::ScWarm: // следующий трек SoundCloud — адрес и плейлист заранее
            r.ok = sc_.warm(job.track.id);
            break;

        case Job::TrackMeta: { // полные данные трека (исполнители с id, альбом) — для старых записей без них
            std::vector<Track> got;
            r.ok = api_.tracksByIds({job.track.id}, got) && !got.empty();
            if (r.ok) r.track = got[0];
            else r.error = api_.lastError.empty() ? "Трек не найден" : api_.lastError;
            break;
        }

        case Job::ListMore: { // следующая порция «Мне нравится»
            r.playlist = job.playlist;
            r.gen = job.gen;
            size_t n = std::min<size_t>(job.ids.size(), LIKED_CHUNK);
            std::vector<std::string> part(job.ids.begin(), job.ids.begin() + n);
            r.ok = api_.tracksByIds(part, r.tracks);
            if (!r.ok) { r.error = api_.lastError; break; }
            if (job.ids.size() > n) {
                Job more = job;
                more.ids.erase(more.ids.begin(), more.ids.begin() + n);
                r.more = true;
                post(more);
            }
            break;
        }

        case Job::ConnectQueue:
            r.likedIds = job.ids; // порядок, в котором прислали
            r.ok = api_.tracksByIds(job.ids, r.tracks);
            if (!r.ok) r.error = api_.lastError;
            break;

        case Job::Search:
            r.query = job.query;
            r.ok = api_.searchTracks(job.query, r.tracks);
            if (!r.ok) r.error = api_.lastError;
            break;

        case Job::Download: {
            r.track = job.track;
            if (isSoundCloudId(job.track.id)) { // SoundCloud: оригинал (если разрешено) или AAC 160 / mp3 128
                downloadSoundCloud(job.track, r);
                break;
            }
            if (api_.uid.empty() && (!api_.loadToken(tokenPath_.c_str()) || !api_.accountStatus())) {
                r.error = api_.lastError;
                break;
            }
            std::string url, codec;
            int bitrate = 0;
            if (!api_.trackUrl(job.track.id, url, bitrate, codec)) { r.error = api_.lastError; break; }
            const char *ext = codec == "mp3" ? "mp3" : codec.find("mp4") != std::string::npos ? "m4a" : "aac";
            r.filePath = std::string(OFFLINE_DIR) + "/" + job.track.id + "." + ext;
            std::string part = r.filePath + ".part";
            FILE *f = fopen(part.c_str(), "wb");
            if (!f) { r.error = "Не удалось создать файл на карте"; break; }
            bool writeOk = true;
            HttpResult h = httpGet(url, {}, [&](const char *d, size_t n) {
                if (stop_) return false;
                writeOk = fwrite(d, 1, n, f) == n;
                return writeOk;
            });
            fclose(f);
            if (h.code != 200 || !writeOk || stop_) {
                remove(part.c_str());
                r.error = !writeOk ? "Карта переполнена?" : h.code < 0 ? "Сеть: " + h.error : "HTTP " + std::to_string(h.code);
                break;
            }
            remove(r.filePath.c_str());
            rename(part.c_str(), r.filePath.c_str());
            // Обложка рядом с треком — чтобы «Скачанное» было красивым и без сети.
            std::string coverUrl = job.track.coverUrl(400);
            if (!coverUrl.empty()) {
                HttpResult c = httpGet(coverUrl, {});
                if (c.code == 200 && !c.body.empty()) {
                    r.coverPath = std::string(OFFLINE_DIR) + "/" + job.track.id + ".jpg";
                    if (FILE *cf = fopen(r.coverPath.c_str(), "wb")) {
                        fwrite(c.body.data(), 1, c.body.size(), cf);
                        fclose(cf);
                    } else {
                        r.coverPath.clear();
                    }
                }
            }
            r.ok = true;
            break;
        }

        case Job::Lyrics: {
            // Кэш на карте: однажды открытый текст доступен и без сети.
            r.trackId = job.track.id;
            std::string base = std::string(LYRICS_DIR) + "/" + job.track.id;
            for (const char *ext : {".lrc", ".txt"}) {
                FILE *f = fopen((base + ext).c_str(), "rb");
                if (!f) continue;
                char buf[4096];
                size_t n;
                while ((n = fread(buf, 1, sizeof(buf), f)) > 0) r.lyrics.append(buf, n);
                fclose(f);
                r.synced = std::string(ext) == ".lrc";
                r.ok = !r.lyrics.empty();
                if (r.ok) break;
            }
            if (r.ok) break;
            if (api_.uid.empty() && (!api_.loadToken(tokenPath_.c_str()) || !api_.accountStatus())) {
                r.error = api_.lastError;
                break;
            }
            r.ok = api_.lyrics(job.track.id, r.lyrics, r.synced);
            if (!r.ok) { r.error = api_.lastError; break; }
            if (!job.cache) break;
            mkdir(LYRICS_DIR, 0777);
            if (FILE *f = fopen((base + (r.synced ? ".lrc" : ".txt")).c_str(), "wb")) {
                fwrite(r.lyrics.data(), 1, r.lyrics.size(), f);
                fclose(f);
            }
            break;
        }

        case Job::Cover: {
            r.url = job.url;
            HttpResult h = httpGet(job.url, {});
            if (h.code == 200 && !h.body.empty()) {
                SDL_RWops *rw = SDL_RWFromConstMem(h.body.data(), (int)h.body.size());
                r.surface = IMG_Load_RW(rw, 1);
            }
            r.ok = r.surface != nullptr;
            break;
        }
    }
    return r;
}

// ---------- скачивание трека SoundCloud ----------

// Имя файла из id: в "sc:soundcloud:tracks:123?секрет" есть «:» и «?», которых на карте быть не может.
static std::string safeName(const std::string &id) {
    std::string s;
    for (char c : id) s += isalnum((unsigned char)c) ? c : '_';
    return s;
}

// Формат по первым байтам файла.
static const char *extByMagic(const std::string &h, const std::string &codec) {
    if (h.compare(0, 4, "fLaC") == 0) return "flac";
    if (h.compare(0, 4, "RIFF") == 0) return "wav";
    if (h.compare(0, 4, "OggS") == 0) return "ogg";
    if (h.compare(0, 4, "FORM") == 0) return "aiff";
    if (h.size() >= 8 && h.compare(4, 4, "ftyp") == 0) return "m4a";
    if (h.compare(0, 3, "ID3") == 0 || (h.size() >= 2 && (unsigned char)h[0] == 0xFF && ((unsigned char)h[1] & 0xE0) == 0xE0)) return "mp3";
    return codec.compare(0, 3, "aac") == 0 ? "m4a" : "mp3";
}

void Worker::downloadSoundCloud(const Track &t, JobResult &r) {
    std::string url, codec;
    int kbps = 0;
    if (!sc_.streamUrl(t.id, url, kbps, codec)) { r.error = sc_.lastError; return; }
    if (url.compare(0, 4, "hls:") == 0) url = url.substr(4);
    std::string base = std::string(OFFLINE_DIR) + "/" + safeName(t.id), part = base + ".part";
    FILE *f = fopen(part.c_str(), "wb");
    if (!f) { r.error = "Не удалось создать файл на карте"; return; }
    std::string head, playlist, err;
    bool isFile = false, sniffed = false, writeOk = true;
    // Первый запрос: плейлист HLS (куски — дальше) или сразу файл (оригинал) — его пишем как есть.
    HttpResult h = httpGet(url, {}, [&](const char *d, size_t n) {
        if (stop_) return false;
        if (!sniffed) {
            playlist.append(d, n);
            if (playlist.size() < 7) return true;
            sniffed = true;
            isFile = playlist.compare(0, 7, "#EXTM3U") != 0;
            if (!isFile) return true;
            head = playlist.substr(0, 16);
            writeOk = fwrite(playlist.data(), 1, playlist.size(), f) == playlist.size();
            playlist.clear();
            return writeOk;
        }
        if (!isFile) { playlist.append(d, n); return true; }
        writeOk = fwrite(d, 1, n, f) == n;
        return writeOk;
    });
    if (h.code != 200) err = h.code < 0 ? "Сеть: " + h.error : "HTTP " + std::to_string(h.code);
    if (err.empty() && !isFile) {
        std::vector<std::string> parts = hlsSegments(playlist, h.finalUrl.empty() ? url : h.finalUrl);
        if (parts.empty()) err = "пустой плейлист";
        for (size_t i = 0; err.empty() && writeOk && i < parts.size() && !stop_; i++) {
            long mark = ftell(f);
            HttpResult s;
            for (int attempt = 0; attempt < 3 && !stop_; attempt++) { // кусок оборвался — качаем заново с того же места
                fseek(f, mark, SEEK_SET);
                s = httpGet(parts[i], {}, [&](const char *d, size_t n) {
                    if (stop_) return false;
                    if (head.size() < 16) head.append(d, std::min<size_t>(n, 16 - head.size()));
                    writeOk = fwrite(d, 1, n, f) == n;
                    return writeOk;
                });
                if (s.code == 200 || !writeOk) break;
            }
            if (s.code != 200 && writeOk && !stop_) err = s.code < 0 ? "Сеть: " + s.error : "HTTP " + std::to_string(s.code);
        }
    }
    fclose(f);
    if (!writeOk) err = "Карта переполнена?";
    if (!err.empty() || stop_) {
        remove(part.c_str());
        r.error = err.empty() ? "Остановлено" : err;
        return;
    }
    r.filePath = base + "." + extByMagic(head, codec);
    remove(r.filePath.c_str());
    rename(part.c_str(), r.filePath.c_str());
    crashLog("[sc] скачано: %s (%s)", r.filePath.substr(r.filePath.rfind('.') + 1).c_str(), codec.c_str());
    // Обложка рядом с треком — для «Скачанного» без сети.
    std::string coverUrl = t.coverUrl(400);
    if (!coverUrl.empty()) {
        HttpResult c = httpGet(coverUrl, {});
        if (c.code == 200 && !c.body.empty()) {
            r.coverPath = base + ".jpg";
            if (FILE *cf = fopen(r.coverPath.c_str(), "wb")) {
                fwrite(c.body.data(), 1, c.body.size(), cf);
                fclose(cf);
            } else r.coverPath.clear();
        }
    }
    r.ok = true;
}
