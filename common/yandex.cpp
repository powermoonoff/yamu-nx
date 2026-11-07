#include "yandex.hpp"
#include "http.hpp"
#include "json.hpp"
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <set>
#include <mbedtls/base64.h>
#include <mbedtls/md.h>
#include <mbedtls/md5.h>

static const char *API = "https://api.music.yandex.net";
static const char *WAVE = "user:onyourwave";
// Соль подписи ссылок на mp3 (как в yandex-music-api).
static const char *SIGN_SALT = "XGRlBW9FXlekgbPrRHuSiA";
// get-file-info: ключ подписи и клиент десктопного приложения (как в yandex-music-api).
static const char *FILE_INFO_KEY = "kzqU4XhfCaY6B6JTHODeq5";
static const char *FILE_INFO_CLIENT = "YandexMusicDesktopAppWindows/5.95.0";
// Без FLAC: lossless слишком тяжёлый для потока. Порядок важен для подписи.
static const char *FILE_INFO_CODECS[] = {"aac-mp4", "he-aac-mp4", "aac", "he-aac", "mp3"};

static std::string trim(const std::string &t) {
    size_t b = t.find_first_not_of(" \t\r\n\xEF\xBB\xBF");
    size_t e = t.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? "" : t.substr(b, e - b + 1);
}

static std::string xmlTag(const std::string &xml, const char *tag) {
    std::string open = std::string("<") + tag + ">", close = std::string("</") + tag + ">";
    size_t b = xml.find(open);
    if (b == std::string::npos) return "";
    b += open.size();
    size_t e = xml.find(close, b);
    return e == std::string::npos ? "" : xml.substr(b, e - b);
}

static std::string md5hex(const std::string &s) {
    unsigned char d[16];
    mbedtls_md5_ret(reinterpret_cast<const unsigned char *>(s.data()), s.size(), d);
    char hex[33];
    for (int i = 0; i < 16; i++) snprintf(hex + i * 2, 3, "%02x", d[i]);
    return std::string(hex, 32);
}

static std::string hmacSha256Base64(const std::string &key, const std::string &msg) {
    unsigned char mac[32];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), reinterpret_cast<const unsigned char *>(key.data()),
                    key.size(), reinterpret_cast<const unsigned char *>(msg.data()), msg.size(), mac);
    unsigned char out[64];
    size_t olen = 0;
    mbedtls_base64_encode(out, sizeof(out), &olen, mac, sizeof(mac));
    return std::string(reinterpret_cast<char *>(out), olen);
}

static std::string urlEncode(const std::string &s) {
    std::string out;
    char buf[4];
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += c;
        else { snprintf(buf, sizeof(buf), "%%%02X", c); out += buf; }
    }
    return out;
}

// Текущее время: системные часы, а если они недоступны (в модуле) — часы сервера.
static long long nowEpoch() {
    long long t = (long long)time(nullptr);
    if (t < 1600000000LL) {
        long long s = httpServerTime();
        if (s > 0) t = s;
    }
    return t;
}

static std::string isoNow() {
    time_t t = (time_t)nowEpoch();
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S.000Z", gmtime(&t));
    return buf;
}

std::string Track::coverUrl(int size) const {
    if (coverUri.empty()) return "";
    std::string u = coverUri;
    size_t p = u.find("%%");
    std::string dim = std::to_string(size) + "x" + std::to_string(size);
    if (p != std::string::npos) u.replace(p, 2, dim);
    return "https://" + u;
}

bool YandexApi::loadToken(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { lastError = "token.txt not found"; return false; }
    char buf[512] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    token_ = trim(std::string(buf, n));
    if (token_.empty()) { lastError = "token.txt is empty"; return false; }
    return true;
}

std::vector<std::string> YandexApi::headers() const {
    return {"Authorization: OAuth " + token_, "X-Yandex-Music-Client: YandexMusicAndroid/24023621"};
}

static bool checkHttp(const HttpResult &r, std::string &err) {
    if (r.code < 0) { err = "Network: " + r.error; return false; }
    if (r.code == 401 || r.code == 403) { err = "Token rejected (HTTP " + std::to_string(r.code) + ")"; return false; }
    if (r.code != 200) { err = "HTTP " + std::to_string(r.code) + ": " + r.body.substr(0, 200); return false; }
    return true;
}

bool YandexApi::accountStatus() {
    HttpResult r = httpGet(std::string(API) + "/account/status", headers());
    if (!checkHttp(r, lastError)) return false;
    Json j = Json::parse(r.body)["result"];
    login = j["account"]["login"].asString();
    uid = j["account"]["uid"].asString();
    hasPlus = j["plus"]["hasPlus"].asBool();
    return true;
}

bool YandexApi::waveTracks(std::vector<Track> &out, const std::string &lastTrackId) {
    std::string url = std::string(API) + "/rotor/station/" + WAVE + "/tracks?settings2=true";
    if (!lastTrackId.empty()) url += "&queue=" + lastTrackId;
    HttpResult r = httpGet(url, headers());
    if (!checkHttp(r, lastError)) return false;

    Json res = Json::parse(r.body)["result"];
    batchId_ = res["batchId"].asString();
    const Json &seq = res["sequence"];
    for (size_t i = 0; i < seq.size(); i++) {
        Track tr;
        if (parseTrack(seq[i]["track"], tr)) out.push_back(tr);
    }
    if (out.empty()) { lastError = "Wave returned no tracks"; return false; }
    return true;
}

bool YandexApi::parseTrack(const Json &t, Track &tr) {
    if (t.isNull()) return false;
    // Недоступные (снятые с площадки) треки всё равно не сыграть.
    if (t["available"].type == Json::Bool && !t["available"].asBool()) return false;
    tr.id = t["id"].asString();
    tr.title = t["title"].asString();
    if (!t["version"].asString().empty()) tr.title += " (" + t["version"].asString() + ")";
    tr.durationMs = t["durationMs"].asInt();
    tr.albumId = t["albums"][size_t(0)]["id"].asString();
    tr.coverUri = t["coverUri"].asString();
    if (tr.coverUri.empty()) tr.coverUri = t["albums"][size_t(0)]["coverUri"].asString();
    tr.albumTitle = t["albums"][size_t(0)]["title"].asString();
    const Json &ar = t["artists"];
    for (size_t k = 0; k < ar.size(); k++) {
        if (k) tr.artists += ", ";
        tr.artists += ar[k]["name"].asString();
        tr.artistIds.push_back(ar[k]["id"].asString());
        tr.artistNames.push_back(ar[k]["name"].asString());
    }
    return !tr.id.empty();
}

bool YandexApi::playlists(std::vector<Playlist> &out) {
    HttpResult r = httpGet(std::string(API) + "/users/" + uid + "/playlists/list", headers());
    if (!checkHttp(r, lastError)) return false;
    Json root = Json::parse(r.body);
    const Json &list = root["result"];
    for (size_t i = 0; i < list.size(); i++) {
        const Json &p = list[i];
        Playlist pl;
        pl.kind = p["kind"].asString();
        pl.owner = p["owner"]["uid"].asString();
        if (pl.owner.empty()) pl.owner = uid;
        pl.title = p["title"].asString();
        pl.trackCount = (int)p["trackCount"].asInt();
        const Json &cover = p["cover"];
        pl.coverUri = cover["uri"].asString();
        if (pl.coverUri.empty()) pl.coverUri = cover["itemsUri"][size_t(0)].asString();
        if (pl.coverUri.empty()) pl.coverUri = p["ogImage"].asString();
        if (!pl.kind.empty()) out.push_back(pl);
    }
    return true;
}

bool YandexApi::playlistTracks(const Playlist &pl, std::vector<Track> &out) {
    HttpResult r = httpGet(std::string(API) + "/users/" + pl.owner + "/playlists/" + pl.kind + "?rich-tracks=true",
                           headers());
    if (!checkHttp(r, lastError)) return false;
    Json root = Json::parse(r.body);
    const Json &items = root["result"]["tracks"];
    for (size_t i = 0; i < items.size(); i++) {
        Track tr;
        if (parseTrack(items[i]["track"], tr)) out.push_back(tr);
    }
    return true;
}

bool YandexApi::likedIds(std::vector<std::string> &out) {
    HttpResult r = httpGet(std::string(API) + "/users/" + uid + "/likes/tracks", headers());
    if (!checkHttp(r, lastError)) return false;
    Json root = Json::parse(r.body);
    const Json &tracks = root["result"]["library"]["tracks"];
    for (size_t i = 0; i < tracks.size(); i++) {
        std::string id = tracks[i]["id"].asString();
        if (!id.empty()) out.push_back(id);
    }
    return true;
}

bool YandexApi::tracksByIds(const std::vector<std::string> &ids, std::vector<Track> &out) {
    const size_t BATCH = 100;
    for (size_t b = 0; b < ids.size(); b += BATCH) {
        std::string form = "track-ids=";
        for (size_t i = b; i < ids.size() && i < b + BATCH; i++) form += (i > b ? "," : "") + ids[i];
        HttpResult r = httpPost(std::string(API) + "/tracks", headers(), form);
        if (!checkHttp(r, lastError)) return !out.empty();
        Json root = Json::parse(r.body);
        const Json &list = root["result"];
        for (size_t i = 0; i < list.size(); i++) {
            Track tr;
            if (parseTrack(list[i], tr)) out.push_back(tr);
        }
    }
    return true;
}

bool YandexApi::searchTracks(const std::string &text, std::vector<Track> &out, int pages) {
    for (int page = 0; page < pages; page++) {
        std::string url = std::string(API) + "/search?type=track&nocorrect=false&page=" + std::to_string(page) +
                          "&text=" + urlEncode(text);
        HttpResult r = httpGet(url, headers());
        if (!checkHttp(r, lastError)) return !out.empty();
        Json root = Json::parse(r.body);
        const Json &tracks = root["result"]["tracks"];
        const Json &list = tracks["results"];
        for (size_t i = 0; i < list.size(); i++) {
            Track tr;
            if (parseTrack(list[i], tr)) out.push_back(tr);
        }
        // Дальше страниц нет — выходим, не тратя запросы.
        long long total = tracks["total"].asInt(), perPage = tracks["perPage"].asInt(10);
        if (list.size() == 0 || (page + 1) * perPage >= total) break;
    }
    return true;
}

bool YandexApi::setLike(const Track &t, bool like) {
    std::string tid = t.albumId.empty() ? t.id : t.id + ":" + t.albumId;
    std::string url = std::string(API) + "/users/" + uid + "/likes/tracks/" + (like ? "add-multiple" : "remove");
    HttpResult r = httpPost(url, headers(), "track-ids=" + tid);
    return checkHttp(r, lastError);
}

bool YandexApi::waveSettings(const std::string &mood, const std::string &diversity) {
    std::vector<std::string> h = headers();
    h.push_back("Content-Type: application/json");
    HttpResult r = httpPost(std::string(API) + "/rotor/station/" + WAVE + "/settings2", h,
                            "{\"moodEnergy\":\"" + mood + "\",\"diversity\":\"" + diversity + "\",\"language\":\"any\"}");
    return checkHttp(r, lastError);
}

void YandexApi::waveFeedback(const std::string &type, const Track *track, double playedSec) {
    std::string url = std::string(API) + "/rotor/station/" + WAVE + "/feedback";
    if (!batchId_.empty()) url += "?batch-id=" + batchId_;
    std::string body = "{\"type\":\"" + type + "\",\"timestamp\":\"" + isoNow() + "\",\"from\":\"YaMu-nx\"";
    if (track) {
        std::string tid = track->albumId.empty() ? track->id : track->id + ":" + track->albumId;
        body += ",\"trackId\":\"" + tid + "\"";
        if (type != "trackStarted") body += ",\"totalPlayedSeconds\":" + std::to_string((int)playedSec);
    }
    body += "}";
    auto h = headers();
    h.push_back("Content-Type: application/json");
    httpPost(url, h, body); // ошибки фидбэка не критичны
}

bool YandexApi::trackUrl(const std::string &trackId, std::string &url, int &bitrate, std::string &codec) {
    // Новый способ — основной; старый download-info у многих треков уже пустой.
    if (fileInfoUrl(trackId, url, bitrate, codec)) return true;
    std::string fileInfoError = lastError;
    if (downloadInfoUrl(trackId, url, bitrate, codec)) return true;
    lastError = "file-info: " + fileInfoError + " | download-info: " + lastError;
    return false;
}

bool YandexApi::fileInfoUrl(const std::string &trackId, std::string &url, int &bitrate, std::string &codec) {
    const std::string quality = "hq", transport = "raw";
    std::string codecsJoined, codecsList;
    for (const char *c : FILE_INFO_CODECS) {
        if (mp3Only && std::string(c) != "mp3") continue;
        // Модулю: без MP4-контейнера и без HE-AAC (64 кбит/с — на слух заметно хуже приложения).
        if (noMp4 && (strstr(c, "mp4") || !strcmp(c, "he-aac"))) continue;
        codecsJoined += c;
        codecsList += (codecsList.empty() ? "" : ",") + std::string(c);
    }
    std::string ts = std::to_string(nowEpoch());
    std::string sign = hmacSha256Base64(FILE_INFO_KEY, ts + trackId + quality + codecsJoined + transport);
    if (!sign.empty()) sign.pop_back(); // как в yandex-music-api: без последнего символа

    std::string req = std::string(API) + "/get-file-info?ts=" + ts + "&trackId=" + trackId + "&quality=" + quality +
                      "&codecs=" + urlEncode(codecsList) + "&transports=" + transport + "&sign=" + urlEncode(sign);
    std::vector<std::string> h = {"Authorization: OAuth " + token_,
                                  std::string("X-Yandex-Music-Client: ") + FILE_INFO_CLIENT};
    HttpResult r = httpGet(req, h);
    if (!checkHttp(r, lastError)) return false;

    Json root = Json::parse(r.body); // держим корень живым: info ссылается внутрь него
    const Json &info = root["result"]["downloadInfo"];
    url = info["url"].asString();
    if (url.empty()) url = info["urls"][size_t(0)].asString();
    if (url.compare(0, 4, "http") != 0) { lastError = "bad url: " + r.body.substr(0, 120); return false; }
    if (info["transport"].asString() != "raw" || !info["key"].isNull()) {
        lastError = "encrypted stream (" + info["transport"].asString() + ")";
        return false;
    }
    codec = info["codec"].asString();
    bitrate = (int)info["bitrate"].asInt();
    if (mp3Only && codec != "mp3") { lastError = "no mp3 (" + codec + ")"; return false; }
    if (noMp4 && codec.find("mp4") != std::string::npos) { lastError = "mp4 container (" + codec + ")"; return false; }
    return true;
}

bool YandexApi::downloadInfoUrl(const std::string &trackId, std::string &url, int &bitrate, std::string &codec) {
    HttpResult r = httpGet(std::string(API) + "/tracks/" + trackId + "/download-info", headers());
    if (!checkHttp(r, lastError)) return false;

    // Берём mp3 лучшего битрейта, если его нет — AAC. FFmpeg играет оба.
    Json root = Json::parse(r.body);
    const Json &list = root["result"];
    std::string infoUrl, seen;
    int bestScore = -1;
    bitrate = 0;
    for (size_t i = 0; i < list.size(); i++) {
        const Json &v = list[i];
        std::string c = v["codec"].asString();
        bool preview = v["preview"].asBool();
        seen += (seen.empty() ? "" : ",") + c + (preview ? "(preview)" : "");
        if (preview || (c != "mp3" && c != "aac" && c != "he-aac") || (mp3Only && c != "mp3") || (noMp4 && c == "he-aac")) continue;
        int br = (int)v["bitrateInKbps"].asInt();
        int score = (c == "mp3" ? 10000 : 0) + br;
        if (score > bestScore) {
            bestScore = score;
            bitrate = br;
            codec = c;
            infoUrl = v["downloadInfoUrl"].asString();
        }
    }
    if (infoUrl.empty()) {
        lastError = "No playable format [" + (seen.empty() ? std::string("none") : seen) + "]";
        return false;
    }

    HttpResult x = httpGet(infoUrl, headers());
    if (!checkHttp(x, lastError)) return false;
    std::string host = xmlTag(x.body, "host"), path = xmlTag(x.body, "path");
    std::string ts = xmlTag(x.body, "ts"), s = xmlTag(x.body, "s");
    if (host.empty() || path.empty()) { lastError = "Bad download-info XML"; return false; }

    std::string sign = md5hex(SIGN_SALT + path.substr(1) + s);
    url = "https://" + host + "/get-mp3/" + sign + "/" + ts + path;
    return true;
}

// Тексты песен: подпись проверяется вместе с клиентом — ключ Android-приложения идёт с Android-заголовком,
// десктопный — с десктопным (как в yandex-music-api).
static const char *ANDROID_SIGN_KEY = "p93jhgh689SBReK6ghtw62";
static const char *ANDROID_CLIENT = "YandexMusicAndroid/24023621";

bool YandexApi::lyrics(const std::string &trackId, std::string &text, bool &synced) {
    // Сначала синхронный LRC, если его нет — обычный текст.
    struct Signer { const char *key, *client; };
    static const Signer signers[] = {{ANDROID_SIGN_KEY, ANDROID_CLIENT}, {FILE_INFO_KEY, FILE_INFO_CLIENT}};
    for (int step = 0; step < 4; step++) {
        const char *format = step < 2 ? "LRC" : "TEXT";
        int si = step % 2;
        std::string ts = std::to_string(nowEpoch());
        std::string sign = hmacSha256Base64(signers[si].key, trackId + ts);
        std::string req = std::string(API) + "/tracks/" + trackId + "/lyrics?format=" + format + "&timeStamp=" + ts +
                          "&sign=" + urlEncode(sign);
        std::vector<std::string> h = {"Authorization: OAuth " + token_,
                                      std::string("X-Yandex-Music-Client: ") + signers[si].client};
        HttpResult r = httpGet(req, h);
        // Подпись не приняли — пробуем вторую пару ключ+клиент для того же формата.
        if ((r.code == 400 || r.code == 403) && si == 0) continue;
        if (r.code == 403) { lastError = "Тексты: HTTP 403 " + r.body.substr(0, 160); return false; }
        // 400 от обеих пар — так Яндекс отвечает, когда текста в этом формате нет.
        if (r.code == 400 || r.code == 404) { lastError = "У этого трека нет текста"; if (si == 0) step++; continue; }
        if (!checkHttp(r, lastError)) return false;
        Json root = Json::parse(r.body);
        std::string dl = root["result"]["downloadUrl"].asString();
        if (dl.compare(0, 4, "http") != 0) { lastError = "Нет ссылки на текст"; continue; }
        HttpResult f = httpGet(dl, {});
        if (!checkHttp(f, lastError)) return false;
        if (trim(f.body).empty()) { lastError = "У этого трека нет текста"; continue; }
        text = f.body;
        synced = std::string(format) == "LRC";
        return true;
    }
    return false;
}

// Вход по коду: клиент приложения Яндекс Музыки (как в yandex-music-api).
static const char *OAUTH_CLIENT_ID = "23cabbbdc6cd418abb4b39c32c41195d";
static const char *OAUTH_CLIENT_SECRET = "53bc75238f0c4d08a118e51fe9203300";

bool YandexApi::deviceCodeStart(const std::string &deviceId, const std::string &name, DeviceCode &out, std::string &err) {
    HttpResult r = httpPost("https://oauth.yandex.ru/device/code", {},
                            std::string("client_id=") + OAUTH_CLIENT_ID + "&device_id=" + urlEncode(deviceId) +
                                "&device_name=" + urlEncode(name));
    if (r.code < 0) { err = "Нет сети: " + r.error; return false; }
    Json j = Json::parse(r.body);
    out.deviceCode = j["device_code"].asString();
    out.userCode = j["user_code"].asString();
    out.url = j["verification_url"].asString("https://ya.ru/device");
    out.interval = (int)j["interval"].asInt(5);
    out.expiresIn = (int)j["expires_in"].asInt(300);
    if (r.code != 200 || out.deviceCode.empty() || out.userCode.empty()) {
        err = "HTTP " + std::to_string(r.code) + ": " + r.body.substr(0, 160);
        return false;
    }
    return true;
}

int YandexApi::deviceCodePoll(const std::string &deviceCode, std::string &token, std::string &err) {
    HttpResult r = httpPost("https://oauth.yandex.ru/token", {},
                            "grant_type=device_code&code=" + urlEncode(deviceCode) + "&client_id=" + OAUTH_CLIENT_ID +
                                "&client_secret=" + OAUTH_CLIENT_SECRET);
    if (r.code < 0) return 0; // сеть моргнула — просто спросим ещё раз
    Json j = Json::parse(r.body);
    token = j["access_token"].asString();
    if (r.code == 200 && !token.empty()) return 1;
    std::string e = j["error"].asString();
    if (e == "authorization_pending" || e == "slow_down") return 0;
    err = e == "expired_token" ? "Код устарел" : e == "access_denied" ? "Вход отклонён"
        : j["error_description"].asString(e.empty() ? "HTTP " + std::to_string(r.code) : e);
    return -1;
}

// ---------- поиск по всему, альбомы, исполнители ----------

static std::string artistNames(const Json &ar) {
    std::string s;
    for (size_t k = 0; k < ar.size(); k++) {
        if (k) s += ", ";
        s += ar[k]["name"].asString();
    }
    return s;
}

bool YandexApi::parseArtist(const Json &a, Card &c) {
    if (a.isNull()) return false;
    c.kind = Card::Artist;
    c.id = a["id"].asString();
    c.title = a["name"].asString();
    c.coverUri = a["cover"]["uri"].asString();
    if (c.coverUri.empty()) c.coverUri = a["ogImage"].asString();
    c.trackCount = (int)a["counts"]["tracks"].asInt(0);
    c.sub = "Исполнитель";
    return !c.id.empty();
}

bool YandexApi::parseAlbum(const Json &a, Card &c) {
    if (a.isNull()) return false;
    c.kind = Card::Album;
    c.id = a["id"].asString();
    c.title = a["title"].asString();
    if (!a["version"].asString().empty()) c.title += " (" + a["version"].asString() + ")";
    c.coverUri = a["coverUri"].asString();
    c.trackCount = (int)a["trackCount"].asInt(0);
    std::string who = artistNames(a["artists"]);
    long long year = a["year"].asInt(0);
    c.sub = year ? who + " · " + std::to_string(year) : who;
    return !c.id.empty();
}

bool YandexApi::parsePlaylistCard(const Json &p, Card &c) const {
    if (p.isNull()) return false;
    c.kind = Card::PlaylistCard;
    c.id = p["kind"].asString();
    c.owner = p["owner"]["uid"].asString();
    if (c.owner.empty()) c.owner = p["uid"].asString();
    c.title = p["title"].asString();
    c.trackCount = (int)p["trackCount"].asInt(0);
    const Json &cover = p["cover"];
    c.coverUri = cover["uri"].asString();
    if (c.coverUri.empty()) c.coverUri = cover["itemsUri"][size_t(0)].asString();
    if (c.coverUri.empty()) c.coverUri = p["ogImage"].asString();
    std::string owner = p["owner"]["name"].asString();
    if (owner.empty()) owner = p["owner"]["login"].asString();
    c.sub = owner.empty() ? "Плейлист" : "Плейлист · " + owner;
    return !c.id.empty() && !c.owner.empty();
}

bool YandexApi::searchAll(const std::string &text, SearchAll &out) {
    HttpResult r = httpGet(std::string(API) + "/search?type=all&nocorrect=false&page=0&text=" + urlEncode(text), headers());
    if (!checkHttp(r, lastError)) return false;
    Json root = Json::parse(r.body); // держим корень живым: ссылки ниже указывают внутрь
    const Json &res = root["result"];
    const Json &best = res["best"];
    std::string bt = best["type"].asString();
    if (bt == "track" && parseTrack(best["result"], out.bestTrack)) out.bestKind = 4;
    else if (bt == "artist" && parseArtist(best["result"], out.best)) out.bestKind = Card::Artist;
    else if (bt == "album" && parseAlbum(best["result"], out.best)) out.bestKind = Card::Album;
    else if (bt == "playlist" && parsePlaylistCard(best["result"], out.best)) out.bestKind = Card::PlaylistCard;

    const Json &tr = res["tracks"]["results"];
    for (size_t i = 0; i < tr.size(); i++) {
        Track t;
        if (parseTrack(tr[i], t)) out.tracks.push_back(t);
    }
    const Json &ar = res["artists"]["results"];
    for (size_t i = 0; i < ar.size(); i++) {
        Card c;
        if (parseArtist(ar[i], c)) out.artists.push_back(c);
    }
    const Json &al = res["albums"]["results"];
    for (size_t i = 0; i < al.size(); i++) {
        Card c;
        if (parseAlbum(al[i], c)) out.albums.push_back(c);
    }
    const Json &pl = res["playlists"]["results"];
    for (size_t i = 0; i < pl.size(); i++) {
        Card c;
        if (parsePlaylistCard(pl[i], c)) out.playlists.push_back(c);
    }
    return true;
}

bool YandexApi::albumTracks(const std::string &albumId, std::vector<Track> &out) {
    HttpResult r = httpGet(std::string(API) + "/albums/" + albumId + "/with-tracks", headers());
    if (!checkHttp(r, lastError)) return false;
    Json root = Json::parse(r.body);
    const Json &album = root["result"];
    const Json &volumes = album["volumes"]; // диски альбома
    for (size_t v = 0; v < volumes.size(); v++)
        for (size_t i = 0; i < volumes[v].size(); i++) {
            Track t;
            if (!parseTrack(volumes[v][i], t)) continue;
            if (t.coverUri.empty()) t.coverUri = album["coverUri"].asString();
            if (t.albumId.empty()) t.albumId = albumId;
            out.push_back(t);
        }
    if (out.empty()) { lastError = "В альбоме нет доступных треков"; return false; }
    return true;
}

bool YandexApi::artistPage(const std::string &artistId, std::vector<Track> &popular, std::vector<Card> &albums,
                           std::vector<Card> &similar) {
    HttpResult r = httpGet(std::string(API) + "/artists/" + artistId + "/brief-info", headers());
    if (!checkHttp(r, lastError)) return false;
    Json root = Json::parse(r.body);
    const Json &res = root["result"];
    const Json &pt = res["popularTracks"];
    for (size_t i = 0; i < pt.size(); i++) {
        Track t;
        if (parseTrack(pt[i], t)) popular.push_back(t);
    }
    // brief-info отдаёт только первые несколько альбомов — полный список берём из direct-albums
    // постранично (свежие сверху). Не вышло — остаёмся с тем, что было в brief-info.
    // Группа альбома: синглы отдельно; сборники и альбомы, где исполнитель не основной, — в «С участием».
    auto groupOf = [&](const Json &a, bool own) -> std::string {
        std::string type = a["type"].asString();
        if (!own || type == "compilation") return "also";
        const Json &ar = a["artists"];
        bool main = ar.size() == 0;
        for (size_t k = 0; k < ar.size(); k++)
            if (ar[k]["id"].asString() == artistId) main = true;
        if (!main) return "also";
        return type == "single" ? "single" : "album";
    };
    // Все страницы списка альбомов; false — запрос не прошёл (тогда берём то, что было в brief-info).
    auto pages = [&](const char *what, bool own, std::vector<Card> &out) {
        bool ok = false;
        for (int page = 0; page < 20; page++) {
            HttpResult d = httpGet(std::string(API) + "/artists/" + artistId + "/" + what + "?page=" + std::to_string(page) +
                                       "&page-size=100&sort-by=year&sort-order=desc",
                                   headers());
            if (d.code != 200) break;
            Json dj = Json::parse(d.body);
            const Json &list = dj["result"]["albums"];
            for (size_t i = 0; i < list.size(); i++) {
                Card c;
                if (parseAlbum(list[i], c)) { c.group = groupOf(list[i], own); out.push_back(c); }
            }
            ok = true;
            const Json &pager = dj["result"]["pager"];
            long long total = pager["total"].asInt(0), per = pager["perPage"].asInt(100);
            if (list.size() == 0 || (page + 1) * per >= total) break;
        }
        return ok && !out.empty();
    };
    std::vector<Card> direct, also;
    bool haveDirect = pages("direct-albums", true, direct);
    bool haveAlso = pages("also-albums", false, also);
    std::set<std::string> seen;
    auto add = [&](const Card &c) { if (seen.insert(c.id).second) albums.push_back(c); };
    auto addBrief = [&](const char *key, bool own) {
        const Json &al = res[key];
        for (size_t i = 0; i < al.size(); i++) {
            Card c;
            if (parseAlbum(al[i], c)) { c.group = groupOf(al[i], own); add(c); }
        }
    };
    if (haveDirect) for (const Card &c : direct) add(c);
    else addBrief("albums", true);
    if (haveAlso) for (const Card &c : also) add(c);
    else addBrief("alsoAlbums", false);
    const Json &sa = res["similarArtists"];
    for (size_t i = 0; i < sa.size(); i++) {
        Card c;
        if (parseArtist(sa[i], c)) similar.push_back(c);
    }
    return true;
}

bool YandexApi::likedAlbums(std::vector<Card> &out) {
    HttpResult r = httpGet(std::string(API) + "/users/" + uid + "/likes/albums?rich=true", headers());
    if (!checkHttp(r, lastError)) return false;
    Json root = Json::parse(r.body);
    const Json &list = root["result"];
    for (size_t i = 0; i < list.size(); i++) {
        Card c;
        const Json &a = list[i]["album"].isNull() ? list[i] : list[i]["album"];
        if (parseAlbum(a, c)) out.push_back(c);
    }
    return true;
}
