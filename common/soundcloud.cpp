#include "soundcloud.hpp"
#include "http.hpp"
#include "json.hpp"
#include "crashlog.hpp"
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>
#include <switch.h>

static const char *API = "https://api.soundcloud.com";
static const char *TOKEN_URL = "https://secure.soundcloud.com/oauth/token";

static std::string enc(const std::string &s) {
    static const char *hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

static std::string jsonSafe(const std::string &s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        if ((unsigned char)c >= 0x20) o += c;
    }
    return o;
}

static std::string readAll(const char *path) {
    std::string s;
    if (FILE *f = fopen(path, "rb")) {
        char buf[1024];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
        fclose(f);
    }
    return s;
}

static std::string trim(std::string s) {
    while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && isspace((unsigned char)s[i])) i++;
    return s.substr(i);
}

static std::vector<std::string> keyLines() {
    std::string text = readAll(SC_KEYS_PATH), line;
    std::vector<std::string> lines;
    for (size_t i = 0; i <= text.size(); i++) {
        if (i == text.size() || text[i] == '\n') {
            line = trim(line);
            if (!line.empty() && line[0] != '#') lines.push_back(line);
            line.clear();
        } else {
            line += text[i];
        }
    }
    return lines;
}

// Адрес переадресации после входа — как указан в настройках приложения на developers.soundcloud.com.
// Третья строка soundcloud.txt, иначе — адрес по умолчанию.
static std::string redirectUri() {
    std::vector<std::string> lines = keyLines();
    if (lines.size() >= 3 && lines[2].compare(0, 4, "http") == 0) return lines[2];
    return SC_DEFAULT_REDIRECT;
}

// Ключи: две непустые строки — Client ID и Client Secret (можно и через «:» в одной).
static bool loadKeys(std::string &id, std::string &secret) {
    std::string text = readAll(SC_KEYS_PATH), line;
    std::vector<std::string> lines;
    for (size_t i = 0; i <= text.size(); i++) {
        if (i == text.size() || text[i] == '\n') {
            line = trim(line);
            if (!line.empty() && line[0] != '#') lines.push_back(line);
            line.clear();
        } else {
            line += text[i];
        }
    }
    if (lines.size() == 1 && lines[0].find(':') != std::string::npos) {
        size_t c = lines[0].find(':');
        id = trim(lines[0].substr(0, c));
        secret = trim(lines[0].substr(c + 1));
    } else if (lines.size() >= 2) {
        id = lines[0];
        secret = lines[1];
    }
    return !id.empty() && !secret.empty();
}

bool SoundCloud::configured() {
    std::string id, secret;
    return loadKeys(id, secret);
}

// ---------- вход в аккаунт (OAuth 2.1, код + PKCE) ----------

static std::string b64url(const unsigned char *d, size_t n) {
    unsigned char out[128];
    size_t olen = 0;
    mbedtls_base64_encode(out, sizeof(out), &olen, d, n);
    std::string s((char *)out, olen);
    for (char &c : s) c = c == '+' ? '-' : c == '/' ? '_' : c;
    while (!s.empty() && s.back() == '=') s.pop_back();
    return s;
}

std::string SoundCloud::newVerifier() {
    static const char *abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
    unsigned char rnd[64];
    randomGet(rnd, sizeof(rnd));
    std::string v;
    for (unsigned char b : rnd) v += abc[b % 66];
    return v;
}

std::string SoundCloud::loginUrl(const std::string &verifier, const std::string &state) {
    std::string id, secret;
    if (!loadKeys(id, secret)) return "";
    unsigned char hash[32];
    mbedtls_sha256_ret((const unsigned char *)verifier.data(), verifier.size(), hash, 0);
    return "https://secure.soundcloud.com/authorize?client_id=" + enc(id) + "&redirect_uri=" + enc(redirectUri()) +
           "&response_type=code&code_challenge=" + b64url(hash, 32) + "&code_challenge_method=S256&state=" + enc(state);
}

std::string SoundCloud::redirectAddress() { return redirectUri(); }

bool SoundCloud::loggedIn() { return !Json::parse(readAll(SC_USER_PATH))["refresh"].asString().empty(); }

std::string SoundCloud::userName() { return Json::parse(readAll(SC_USER_PATH))["name"].asString(); }

void SoundCloud::saveToken() {
    std::string name = user_ ? userName_ : "";
    if (FILE *f = fopen(user_ ? SC_USER_PATH : SC_TOKEN_PATH, "w")) {
        fprintf(f, "{\"access\":\"%s\",\"refresh\":\"%s\",\"expires\":%lld,\"name\":\"%s\"}\n", access_.c_str(),
                refresh_.c_str(), expires_, jsonSafe(name).c_str());
        fclose(f);
    }
}

bool SoundCloud::login(const std::string &code, const std::string &verifier) {
    if (!loadKeys(clientId_, clientSecret_)) {
        lastError = "Нет ключей SoundCloud: файл switch/yamu-nx/soundcloud.txt";
        return false;
    }
    std::string form = "grant_type=authorization_code&client_id=" + enc(clientId_) + "&client_secret=" + enc(clientSecret_) +
                       "&redirect_uri=" + enc(redirectUri()) + "&code_verifier=" + enc(verifier) + "&code=" + enc(code);
    HttpResult r = httpPost(TOKEN_URL, {"Accept: application/json; charset=utf-8", "Content-Type: application/x-www-form-urlencoded"}, form);
    Json j = Json::parse(r.body);
    if (r.code != 200 || j["access_token"].asString().empty()) {
        std::string msg = j["error_description"].asString(j["error"].asString());
        crashLog("[sc] вход в аккаунт: HTTP %ld %s %s", r.code, r.error.c_str(), msg.c_str());
        lastError = r.code < 0 ? "SoundCloud (вход): " + r.error
                               : "SoundCloud не принял код" + (msg.empty() ? "" : ": " + msg) + " — попробуйте ещё раз";
        return false;
    }
    keysLoaded_ = true;
    user_ = true;
    access_ = j["access_token"].asString();
    refresh_ = j["refresh_token"].asString();
    expires_ = (long long)time(nullptr) + j["expires_in"].asInt(3600) - 60;
    std::string me;
    userName_.clear();
    if (get("/me", me)) userName_ = Json::parse(me)["username"].asString();
    saveToken();
    crashLog("[sc] вошли в аккаунт");
    return true;
}

void SoundCloud::logout() {
    remove(SC_USER_PATH);
    user_ = false;
    keysLoaded_ = false; // при следующем запросе — снова ключи приложения
    access_.clear();
    refresh_.clear();
    expires_ = 0;
}

// ---------- токен ----------

bool SoundCloud::requestToken(bool refresh) {
    unsigned char b64[512];
    size_t olen = 0;
    std::string pair = clientId_ + ":" + clientSecret_;
    mbedtls_base64_encode(b64, sizeof(b64), &olen, (const unsigned char *)pair.data(), pair.size());
    std::vector<std::string> hdr = {"Authorization: Basic " + std::string((char *)b64, olen),
                                    "Accept: application/json; charset=utf-8",
                                    "Content-Type: application/x-www-form-urlencoded"};
    std::string form = refresh ? "grant_type=refresh_token&client_id=" + enc(clientId_) + "&client_secret=" +
                                     enc(clientSecret_) + "&refresh_token=" + enc(refresh_)
                               : std::string("grant_type=client_credentials");
    HttpResult r = httpPost(TOKEN_URL, hdr, form);
    Json j = Json::parse(r.body);
    std::string token = j["access_token"].asString();
    tokenCode_ = r.code;
    if (r.code != 200 || token.empty()) {
        std::string msg = j["error_description"].asString(j["error"].asString());
        crashLog("[sc] вход: HTTP %ld %s", r.code, r.error.c_str());
        lastError = r.code < 0 ? "SoundCloud (вход): " + r.error
                  : r.code == 401 || r.code == 400 ? "SoundCloud не принял ключи — проверьте soundcloud.txt"
                  : r.code == 429 ? "SoundCloud: слишком много входов, попробуйте позже"
                                  : "SoundCloud: HTTP " + std::to_string(r.code) + (msg.empty() ? "" : " — " + msg);
        return false;
    }
    access_ = token;
    refresh_ = j["refresh_token"].asString(refresh_);
    expires_ = (long long)time(nullptr) + j["expires_in"].asInt(3600) - 60;
    // Токены ограничены (50 за 12 часов на приложение) — храним между запусками.
    saveToken();
    return true;
}

bool SoundCloud::ensureToken() {
    if (!keysLoaded_) {
        if (!loadKeys(clientId_, clientSecret_)) {
            lastError = "Нет ключей SoundCloud: положите Client ID и Client Secret в switch/yamu-nx/soundcloud.txt";
            return false;
        }
        keysLoaded_ = true;
        // Вошли в аккаунт — работаем от его имени (нужно для «моих» плейлистов и лайков), иначе — ключами приложения.
        Json u = Json::parse(readAll(SC_USER_PATH));
        user_ = !u["refresh"].asString().empty();
        Json j = user_ ? u : Json::parse(readAll(SC_TOKEN_PATH));
        access_ = j["access"].asString();
        refresh_ = j["refresh"].asString();
        expires_ = j["expires"].asInt(0);
        userName_ = user_ ? u["name"].asString() : "";
    }
    if (!access_.empty() && (long long)time(nullptr) < expires_) return true;
    // Токен мог обновить другой поток (у загрузчика своя копия SoundCloud): SoundCloud при обновлении
    // отменяет старый refresh-токен, поэтому сначала берём свежий с карты.
    if (user_) {
        Json u = Json::parse(readAll(SC_USER_PATH));
        if (u["refresh"].asString().empty()) { // из аккаунта вышли (в другом потоке) — дальше ключами приложения
            logout();
            return ensureToken();
        }
        if (u["refresh"].asString() != refresh_) {
            access_ = u["access"].asString();
            refresh_ = u["refresh"].asString();
            expires_ = u["expires"].asInt(0);
            if (!access_.empty() && (long long)time(nullptr) < expires_) return true;
        }
    }
    if (!refresh_.empty() && requestToken(true)) return true;
    if (user_) {
        if (tokenCode_ != 400 && tokenCode_ != 401) return false; // нет сети — вход не трогаем
        // Пока мы обновляли, токен мог обновить другой поток — тогда берём его, а не выходим.
        Json u = Json::parse(readAll(SC_USER_PATH));
        if (!u["refresh"].asString().empty() && u["refresh"].asString() != refresh_) {
            access_ = u["access"].asString();
            refresh_ = u["refresh"].asString();
            expires_ = u["expires"].asInt(0);
            return !access_.empty();
        }
        // Вход отозван или истёк — выходим из аккаунта и дальше работаем ключами приложения.
        crashLog("[sc] вход в аккаунт больше не действует (HTTP %ld) — выходим", tokenCode_);
        logout();
        keysLoaded_ = true;
        Json j = Json::parse(readAll(SC_TOKEN_PATH));
        access_ = j["access"].asString();
        refresh_ = j["refresh"].asString();
        expires_ = j["expires"].asInt(0);
        if (!access_.empty() && (long long)time(nullptr) < expires_) return true;
    }
    return requestToken(false);
}

bool SoundCloud::get(const std::string &pathAndQuery, std::string &body) {
    if (!ensureToken()) return false;
    std::string url = pathAndQuery.compare(0, 4, "http") == 0 ? pathAndQuery : std::string(API) + pathAndQuery;
    for (int attempt = 0; attempt < 2; attempt++) {
        HttpResult r = httpGet(url, {"Authorization: OAuth " + access_, "Accept: application/json; charset=utf-8"});
        if (r.code == 401 && attempt == 0) { // токен отозвали раньше срока — берём новый и пробуем ещё раз
            access_.clear();
            expires_ = 0;
            if (!ensureToken()) return false;
            continue;
        }
        if (r.code == 200) { body = r.body; return true; }
        crashLog("[sc] %s: HTTP %ld %s", url.substr(0, url.find('?')).c_str(), r.code, r.error.c_str());
        lastError = r.code < 0 ? "SoundCloud (запрос): " + r.error
                  : r.code == 404 ? "SoundCloud: не найдено (или закрыто автором)"
                  : r.code == 429 ? "SoundCloud: лимит запросов на сегодня исчерпан"
                                  : "SoundCloud: HTTP " + std::to_string(r.code);
        return false;
    }
    return false;
}

// ---------- разбор ----------

// Обложка: artworks-…-large.jpg (100×100) → t500x500. В Track храним без https:// — как у Яндекса.
static std::string artwork(const Json &j) {
    std::string a = j["artwork_url"].asString();
    if (a.empty()) a = j["user"]["avatar_url"].asString();
    size_t p = a.find("-large.");
    if (p != std::string::npos) a.replace(p, 7, "-t500x500.");
    if (a.compare(0, 8, "https://") == 0) a = a.substr(8);
    return a;
}

static std::string secretOf(const Json &j) {
    std::string s = j["secret_uri"].asString();
    size_t p = s.find("secret_token=");
    if (p == std::string::npos) return "";
    s = s.substr(p + 13);
    size_t amp = s.find('&');
    return amp == std::string::npos ? s : s.substr(0, amp);
}

bool SoundCloud::parseTrack(const Json &j, Track &t, const std::string &secret) {
    std::string urn = j["urn"].asString();
    if (urn.empty() || j["access"].asString() == "blocked") return false;
    std::string sec = secret.empty() ? secretOf(j) : secret;
    t.id = "sc:" + urn + (sec.empty() ? "" : "?" + sec);
    t.title = j["title"].asString();
    if (j["access"].asString() == "preview") t.title += " (отрывок)";
    std::string meta = j["metadata_artist"].asString(), user = j["user"]["username"].asString();
    t.artists = !meta.empty() ? meta : user;
    t.artistNames = {user};
    t.artistIds = {"sc:" + j["user"]["urn"].asString()};
    t.durationMs = j["duration"].asInt(0);
    if (j["downloadable"].type == Json::Bool && !j["downloadable"].asBool()) noDownload_.insert(urn); // /download не спрашиваем
    t.coverUri = artwork(j);
    t.albumTitle = "SoundCloud";
    return !t.title.empty();
}

// Ответ со списком: при linked_partitioning — {collection, next_href}, иначе — просто массив.
static const Json &collection(const Json &root) { return root.type == Json::Array ? root : root["collection"]; }

bool SoundCloud::searchTracks(const std::string &q, std::vector<Track> &out, int limit) {
    std::string body;
    if (!get("/tracks?q=" + enc(q) + "&access=playable,preview&linked_partitioning=true&limit=" + std::to_string(limit), body))
        return false;
    Json root = Json::parse(body);
    const Json &list = collection(root);
    for (size_t i = 0; i < list.size(); i++) {
        Track t;
        if (parseTrack(list[i], t)) out.push_back(t);
    }
    return true;
}

bool SoundCloud::searchPlaylists(const std::string &q, std::vector<Card> &out, int limit) {
    std::string body;
    if (!get("/playlists?q=" + enc(q) + "&access=playable&show_tracks=false&linked_partitioning=true&limit=" +
                 std::to_string(limit),
             body))
        return false;
    Json root = Json::parse(body);
    const Json &list = collection(root);
    for (size_t i = 0; i < list.size(); i++) {
        Card c;
        if (parsePlaylistCard(list[i], c)) out.push_back(c);
    }
    return true;
}

// Плейлист SoundCloud → карточка: id "@sc", urn (и секрет закрытой ссылки) — в owner.
bool SoundCloud::parsePlaylistCard(const Json &p, Card &c) {
    c.kind = Card::PlaylistCard;
    c.id = "@sc";
    c.owner = p["urn"].asString();
    if (c.owner.empty()) return false;
    std::string sec = secretOf(p);
    if (!sec.empty()) c.owner += "?" + sec;
    c.title = p["title"].asString();
    c.trackCount = (int)p["track_count"].asInt(0);
    c.sub = p["user"]["username"].asString();
    c.coverUri = artwork(p);
    return true;
}

// Список треков с переходом по страницам (next_href), не больше max.
bool SoundCloud::playlistTracks(const std::string &ref, std::vector<Track> &out) {
    size_t q = ref.find('?');
    std::string urn = ref.substr(0, q), secret = q == std::string::npos ? "" : ref.substr(q + 1);
    std::string next = "/playlists/" + urn + "/tracks?access=playable,preview&linked_partitioning=true&limit=200" +
                       (secret.empty() ? "" : "&secret_token=" + enc(secret));
    for (int page = 0; page < 10 && !next.empty(); page++) {
        std::string body;
        if (!get(next, body)) return !out.empty();
        Json root = Json::parse(body);
        const Json &list = collection(root);
        for (size_t i = 0; i < list.size(); i++) {
            Track t;
            if (parseTrack(list[i], t, secret)) out.push_back(t);
        }
        next = root.type == Json::Object ? root["next_href"].asString() : "";
    }
    return true;
}

bool SoundCloud::userTracks(const std::string &userUrn, std::vector<Track> &out) {
    std::string next = "/users/" + userUrn + "/tracks?access=playable,preview&linked_partitioning=true&limit=200";
    for (int page = 0; page < 5 && !next.empty(); page++) {
        std::string body;
        if (!get(next, body)) return !out.empty();
        Json root = Json::parse(body);
        const Json &list = collection(root);
        for (size_t i = 0; i < list.size(); i++) {
            Track t;
            if (parseTrack(list[i], t)) out.push_back(t);
        }
        next = root.type == Json::Object ? root["next_href"].asString() : "";
    }
    return true;
}


// ---------- своё: нужен вход в аккаунт ----------

// Треки по страницам (next_href), не больше maxPages страниц.
bool SoundCloud::pagedTracks(std::string next, std::vector<Track> &out, int maxPages) {
    for (int page = 0; page < maxPages && !next.empty(); page++) {
        std::string body;
        if (!get(next, body)) return !out.empty();
        Json root = Json::parse(body);
        const Json &list = collection(root);
        for (size_t i = 0; i < list.size(); i++) {
            Track t;
            // В ленте и активностях трек лежит внутри «origin».
            const Json &item = list[i]["origin"].type == Json::Object ? list[i]["origin"] : list[i];
            if (parseTrack(item, t)) out.push_back(t);
        }
        next = root.type == Json::Object ? root["next_href"].asString() : "";
    }
    return true;
}

static bool needUser(bool user, std::string &err) {
    if (user) return true;
    err = "Войдите в SoundCloud: Настройки → SoundCloud";
    return false;
}

bool SoundCloud::myLikes(std::vector<Track> &out, int maxPages) {
    if (!ensureToken() || !needUser(user_, lastError)) return false;
    return pagedTracks("/me/likes/tracks?access=playable,preview&linked_partitioning=true&limit=200", out, maxPages);
}

bool SoundCloud::setLike(const std::string &trackId, bool like) {
    if (!ensureToken() || !needUser(user_, lastError)) return false;
    std::string urn = trackId.substr(3);
    urn = urn.substr(0, urn.find('?'));
    std::string url = std::string(API) + "/likes/tracks/" + urn;
    std::vector<std::string> hdr = {"Authorization: OAuth " + access_, "Accept: application/json; charset=utf-8"};
    HttpResult r = like ? httpPost(url, hdr, "") : httpDelete(url, hdr);
    if (r.code >= 200 && r.code < 300) return true;
    if (!like && r.code == 404) return true; // лайка уже нет
    crashLog("[sc] лайк: HTTP %ld %s", r.code, r.error.c_str());
    lastError = r.code < 0 ? "SoundCloud: " + r.error : r.code == 429 ? "SoundCloud: лимит запросов" : "SoundCloud: HTTP " + std::to_string(r.code);
    return false;
}

bool SoundCloud::myFeed(std::vector<Track> &out) {
    if (!ensureToken() || !needUser(user_, lastError)) return false;
    return pagedTracks("/me/followings/tracks?access=playable,preview&linked_partitioning=true&limit=100", out, 1);
}

bool SoundCloud::myPlaylists(std::vector<Card> &out) {
    if (!ensureToken() || !needUser(user_, lastError)) return false;
    // Свои плейлисты и те, что лайкнул.
    const char *paths[] = {"/me/playlists?show_tracks=false&linked_partitioning=true&limit=100",
                           "/me/likes/playlists?show_tracks=false&linked_partitioning=true&limit=100"};
    bool any = false;
    for (const char *path : paths) {
        std::string body;
        if (!get(path, body)) continue;
        any = true;
        Json root = Json::parse(body);
        const Json &list = collection(root);
        for (size_t i = 0; i < list.size(); i++) {
            Card c;
            if (parsePlaylistCard(list[i], c)) out.push_back(c);
        }
    }
    return any;
}

bool SoundCloud::related(const std::string &trackId, std::vector<Track> &out) {
    std::string urn = trackId.substr(3);
    urn = urn.substr(0, urn.find('?'));
    std::string body;
    if (!get("/tracks/" + urn + "/related?access=playable&linked_partitioning=true&limit=30", body)) return false;
    Json root = Json::parse(body);
    const Json &list = collection(root);
    for (size_t i = 0; i < list.size(); i++) {
        Track t;
        if (parseTrack(list[i], t)) out.push_back(t);
    }
    return true;
}
bool SoundCloud::resolve(const std::string &rawUrl, ScResolved &out) {
    std::string url = trim(rawUrl);
    if (url.compare(0, 4, "http") != 0) url = "https://" + url;
    // Короткие ссылки «Поделиться» (on.soundcloud.com/…) — сначала узнаём, куда они ведут.
    if (url.find("on.soundcloud.com") != std::string::npos) {
        HttpResult r = httpGet(url, {});
        if (!r.finalUrl.empty()) url = r.finalUrl;
    }
    std::string body;
    if (!get("/resolve?url=" + enc(url), body)) return false;
    Json j = Json::parse(body);
    std::string kind = j["kind"].asString();
    if (kind == "track") {
        Track t;
        if (!parseTrack(j, t)) { lastError = "Этот трек нельзя проиграть"; return false; }
        out.kind = ScResolved::Track_;
        out.title = t.title;
        out.tracks.push_back(t);
        return true;
    }
    if (kind == "playlist") {
        std::string ref = j["urn"].asString(), sec = secretOf(j);
        if (!sec.empty()) ref += "?" + sec;
        out.kind = ScResolved::Playlist_;
        out.title = j["title"].asString();
        return playlistTracks(ref, out.tracks);
    }
    if (kind == "user") {
        out.kind = ScResolved::User_;
        out.title = j["username"].asString();
        return userTracks(j["urn"].asString(), out.tracks);
    }
    lastError = "По ссылке нет трека, плейлиста или автора SoundCloud";
    return false;
}

static const int RESOLVED_TTL = 150; // с: подписанные адреса SoundCloud живут недолго

bool SoundCloud::streamUrl(const std::string &trackId, std::string &url, int &kbps, std::string &codec) {
    time_t now = time(nullptr);
    auto it = resolved_.find(trackId);
    if (it != resolved_.end() && now - it->second.at < RESOLVED_TTL) { // подготовлен заранее (warm)
        url = it->second.url;
        kbps = it->second.kbps;
        codec = it->second.codec;
        originalBytes = it->second.originalBytes;
        crashLog("[sc] поток готов заранее (%d с назад)", (int)(now - it->second.at));
        return true;
    }
    for (auto i = resolved_.begin(); i != resolved_.end();) {
        if (now - i->second.at >= RESOLVED_TTL) i = resolved_.erase(i);
        else ++i;
    }
    if (!resolveStream(trackId, url, kbps, codec)) return false;
    resolved_[trackId] = {url, codec, kbps, originalBytes, now};
    return true;
}

bool SoundCloud::warm(const std::string &trackId) {
    std::string url, codec;
    int kbps = 0;
    resolved_.erase(trackId); // освежаем: старые подписи вот-вот истекут
    if (!streamUrl(trackId, url, kbps, codec)) return false;
    if (codec == "оригинал" || url.compare(0, 4, "hls:") != 0) return true; // файл целиком заранее не качаем
    std::string body;
    HttpResult r = httpGet(url.substr(4), {}, [&](const char *d, size_t n) {
        body.append(d, n);
        return body.size() < (1u << 20); // плейлист — килобайты; если вдруг файл — бросаем
    });
    if (r.code == 200 && body.compare(0, 7, "#EXTM3U") == 0) {
        hlsCachePut(url.substr(4), r.finalUrl.empty() ? url.substr(4) : r.finalUrl, body);
        crashLog("[sc] следующий трек подготовлен");
    }
    return true;
}

struct HlsCached {
    std::string finalUrl, body;
    time_t at;
};
static std::mutex g_hlsMx;
static std::map<std::string, HlsCached> g_hls;

void hlsCachePut(const std::string &url, const std::string &finalUrl, const std::string &body) {
    std::lock_guard<std::mutex> lk(g_hlsMx);
    time_t now = time(nullptr);
    for (auto i = g_hls.begin(); i != g_hls.end();) {
        if (now - i->second.at >= RESOLVED_TTL) i = g_hls.erase(i);
        else ++i;
    }
    g_hls[url] = HlsCached{finalUrl, body, now};
}

bool hlsCacheTake(const std::string &url, std::string &finalUrl, std::string &body) {
    std::lock_guard<std::mutex> lk(g_hlsMx);
    auto it = g_hls.find(url);
    if (it == g_hls.end()) return false;
    bool fresh = time(nullptr) - it->second.at < RESOLVED_TTL;
    if (fresh) {
        finalUrl = it->second.finalUrl;
        body = it->second.body;
    }
    g_hls.erase(it);
    return fresh;
}

bool SoundCloud::resolveStream(const std::string &trackId, std::string &url, int &kbps, std::string &codec) {
    std::string ref = trackId.substr(3); // без "sc:"
    size_t q = ref.find('?');
    std::string urn = ref.substr(0, q), secret = q == std::string::npos ? "" : ref.substr(q + 1);
    std::string body;
    if (!get("/tracks/" + urn + "/streams" + (secret.empty() ? "" : "?secret_token=" + enc(secret)), body)) return false;
    Json j = Json::parse(body);
    originalBytes = 0;

    // 1. Оригинал (FLAC, WAV, M4A… как загрузил автор) — если у трека разрешено скачивание.
    //    Файл целиком ляжет в память, поэтому слишком большие пропускаем.
    if (allowOriginal && noDownload_.count(urn)) crashLog("[sc] оригинал: скачивание закрыто автором");
    else if (allowOriginal) {
        HttpResult d = httpRedirect(std::string(API) + "/tracks/" + urn + "/download" +
                                        (secret.empty() ? "" : "?secret_token=" + enc(secret)),
                                    {"Authorization: OAuth " + access_});
        std::string loc = d.code >= 300 && d.code < 400 ? d.finalUrl : "";
        if (d.code == 200 && !d.body.empty() && d.body[0] == '{') loc = Json::parse(d.body)["redirectUri"].asString();
        if (!loc.empty()) {
            // Узнаём размер: берём только заголовки и сразу обрываем загрузку.
            size_t len = 0;
            httpGet(loc, {}, [](const char *, size_t) { return false; }, [&](size_t n) { len = n; });
            crashLog("[sc] оригинал: %u КБ", (unsigned)(len / 1024));
            if (len > 0 && len <= ORIGINAL_MAX) {
                url = "hls:" + loc; // плеер увидит, что это не плейлист, и сыграет файл как есть
                originalBytes = len;
                kbps = 0;
                codec = "оригинал";
                return true;
            }
        } else crashLog("[sc] оригинал: скачивание не разрешено (%s)", d.code < 0 ? d.error.c_str() : ("HTTP " + std::to_string(d.code)).c_str());
    }

    // 2. AAC с самым высоким битрейтом, какой отдаёт официальное API (обычно 160; если появится
    //    hls_aac_256_url или выше — возьмём его), затем mp3 128, отрывок — в последнюю очередь.
    std::string keys, bestKey;
    int bestKbps = 0;
    for (auto &kv : j.obj) {
        if (kv.second.asString().empty()) continue;
        keys += (keys.empty() ? "" : ", ") + kv.first;
        int k = 0;
        if (sscanf(kv.first.c_str(), "hls_aac_%d_url", &k) == 1 && k > bestKbps) { bestKbps = k; bestKey = kv.first; }
    }
    crashLog("[sc] потоки в ответе API: %s", keys.empty() ? "нет" : keys.c_str());
    struct Pref { std::string key; const char *codec; int kbps; };
    std::vector<Pref> prefs;
    if (preferMp3) prefs.push_back({"hls_mp3_128_url", "mp3", 128}); // фоновый модуль: AAC в fMP4 не играет
    else {
        if (!bestKey.empty() && bestKey != "hls_aac_160_url") prefs.push_back({bestKey, "aac", bestKbps});
        prefs.push_back({"hls_aac_160_url", "aac", 160});
        prefs.push_back({"hls_mp3_128_url", "mp3", 128});
    }
    // Отрывок — только если полного трека нет вовсе (модулю — нет: лучше пропустить, чем играть 30 с).
    if (!preferMp3 || keys.find("hls_aac") == std::string::npos) prefs.push_back({"preview_mp3_128_url", "mp3, отрывок", 128});
    for (auto &p : prefs) {
        std::string api = j[p.key.c_str()].asString();
        if (api.empty()) continue;
        // Адрес из ответа ведёт на api.soundcloud.com и переадресует на подписанный адрес CDN.
        // Здесь берём только адрес переадресации (с токеном), а сам плейлист качает плеер — без общего
        // тайм-аута: через прокси переадресация + CDN + плейлист не укладывались в 20 с.
        HttpResult r = httpRedirect(api, {"Authorization: OAuth " + access_});
        std::string target = r.code >= 300 && r.code < 400 ? r.finalUrl : "";
        if (r.code == 200 && !r.body.empty() && r.body[0] == '{') target = Json::parse(r.body)["url"].asString();
        crashLog("[sc] поток %s: %s%s", p.key.c_str(), r.code < 0 ? r.error.c_str() : ("HTTP " + std::to_string(r.code)).c_str(),
                 target.empty() ? ", адреса нет" : "");
        if (target.empty()) continue;
        url = "hls:" + target; // плеер сам разберёт: плейлист HLS или сразу файл
        kbps = p.kbps;
        codec = p.codec;
        return true;
    }
    lastError = "SoundCloud не отдаёт этот трек для проигрывания";
    return false;
}

// ---------- HLS ----------

static std::string joinUrl(const std::string &base, const std::string &ref) {
    if (ref.compare(0, 4, "http") == 0) return ref;
    size_t scheme = base.find("://");
    if (!ref.empty() && ref[0] == '/') {
        size_t hostEnd = base.find('/', scheme == std::string::npos ? 0 : scheme + 3);
        return (hostEnd == std::string::npos ? base : base.substr(0, hostEnd)) + ref;
    }
    std::string b = base.substr(0, base.find('?'));
    size_t slash = b.rfind('/');
    return (slash == std::string::npos ? b + "/" : b.substr(0, slash + 1)) + ref;
}

std::vector<std::string> hlsSegments(const std::string &playlist, const std::string &baseUrl, std::vector<double> *durations) {
    std::vector<std::string> out;
    double nextDur = 0;
    size_t pos = 0;
    while (pos < playlist.size()) {
        size_t nl = playlist.find('\n', pos);
        std::string line = trim(playlist.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
        pos = nl == std::string::npos ? playlist.size() : nl + 1;
        if (line.empty()) continue;
        if (line.compare(0, 11, "#EXT-X-MAP:") == 0) { // начальный кусок (fMP4) — первым
            size_t a = line.find("URI=\"");
            if (a != std::string::npos) {
                size_t b = line.find('"', a + 5);
                out.insert(out.begin(), joinUrl(baseUrl, line.substr(a + 5, b - a - 5)));
                if (durations) durations->insert(durations->begin(), 0.0);
            }
            continue;
        }
        if (line.compare(0, 8, "#EXTINF:") == 0) nextDur = atof(line.c_str() + 8);
        if (line[0] == '#') continue;
        out.push_back(joinUrl(baseUrl, line));
        if (durations) durations->push_back(nextDur);
        nextDur = 0;
    }
    return out;
}
