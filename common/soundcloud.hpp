// SoundCloud через официальное API (api.soundcloud.com) с ключами приложения пользователя.
// Ключи (Client ID и Client Secret) пользователь получает сам в кабинете разработчика SoundCloud
// и кладёт в switch/yamu-nx/soundcloud.txt — две строки. В коде и репозитории ключей нет.
//
// Треки SoundCloud живут в общей очереди как обычные Track: id = "sc:<urn>" (а для закрытых
// ссылок — "sc:<urn>?<secret_token>"), так приложение отличает их от треков Яндекса.
#pragma once
#include <ctime>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "yandex.hpp"

constexpr const char *SC_KEYS_PATH = "sdmc:/switch/yamu-nx/soundcloud.txt";
constexpr const char *SC_TOKEN_PATH = "sdmc:/switch/yamu-nx/soundcloud-token.json";
constexpr const char *SC_USER_PATH = "sdmc:/switch/yamu-nx/soundcloud-user.json"; // вход в аккаунт
// Переадресация после входа, если в soundcloud.txt нет третьей строки. Должна совпадать с указанной
// в настройках приложения на developers.soundcloud.com.
constexpr const char *SC_DEFAULT_REDIRECT = "http://localhost/yamu-nx";

inline bool isSoundCloud(const Track &t) { return t.id.compare(0, 3, "sc:") == 0; }
inline bool isSoundCloudId(const std::string &id) { return id.compare(0, 3, "sc:") == 0; }

// Что нашлось по ссылке soundcloud.com/…
struct ScResolved {
    enum Kind { None, Track_, Playlist_, User_ } kind = None;
    std::vector<::Track> tracks; // для трека — один, для плейлиста и автора — список
    std::string title;
};

class SoundCloud {
public:
    // Есть ли файл с ключами (без него SoundCloud в приложении выключен).
    static bool configured();

    bool searchTracks(const std::string &q, std::vector<Track> &out, int limit = 40);
    bool searchPlaylists(const std::string &q, std::vector<Card> &out, int limit = 20);
    // Треки плейлиста; ref — urn плейлиста, после «?» может идти secret_token.
    bool playlistTracks(const std::string &ref, std::vector<Track> &out);
    bool userTracks(const std::string &userUrn, std::vector<Track> &out);
    // Ссылка на трек, плейлист или автора (в том числе закрытая, с /s-…).
    bool resolve(const std::string &url, ScResolved &out);
    // Адрес потока для плеера: "hls:<адрес плейлиста>"; kbps — примерный битрейт.
    bool streamUrl(const std::string &trackId, std::string &url, int &kbps, std::string &codec);

    // ---- Вход в аккаунт (OAuth с PKCE) ----
    static std::string newVerifier();
    // Адрес страницы входа SoundCloud; verifier — случайная строка из newVerifier(), её же — в login().
    static std::string loginUrl(const std::string &verifier, const std::string &state);
    static std::string redirectAddress(); // куда SoundCloud переадресует после входа
    static bool loggedIn();
    static std::string userName();
    bool login(const std::string &code, const std::string &verifier);
    void logout();

    // ---- Своё (нужен вход) ----
    bool myLikes(std::vector<Track> &out, int maxPages = 10);
    bool setLike(const std::string &trackId, bool like);
    bool myFeed(std::vector<Track> &out);       // свежие треки тех, на кого подписан
    bool myPlaylists(std::vector<Card> &out);   // свои и лайкнутые плейлисты
    bool related(const std::string &trackId, std::vector<Track> &out); // похожие — для волны
    // Заранее, пока играет предыдущий: адрес потока (запомнится для streamUrl) и плейлист HLS
    // (заберёт плеер через hlsCacheTake). Переключение на этот трек — почти без ожидания.
    bool warm(const std::string &trackId);

    std::string lastError;
    size_t originalBytes = 0; // после streamUrl: размер оригинала, если отдали его (иначе 0)
    // Для фонового модуля: оригиналы (FLAC, WAV…) и AAC в fMP4 он не играет — только mp3.
    bool allowOriginal = true, preferMp3 = false;
    static constexpr size_t ORIGINAL_MAX = 50u << 20; // оригинал целиком в памяти — больше не берём

private:
    bool resolveStream(const std::string &trackId, std::string &url, int &kbps, std::string &codec);
    struct Resolved { std::string url, codec; int kbps = 0; size_t originalBytes = 0; time_t at = 0; };
    std::map<std::string, Resolved> resolved_; // адреса потоков: подписаны и живут недолго — держим 150 с
    std::set<std::string> noDownload_;         // urn треков, у которых скачивание оригинала закрыто
    bool ensureToken();
    bool requestToken(bool refresh);
    bool get(const std::string &pathAndQuery, std::string &body);
    bool parseTrack(const struct Json &j, Track &t, const std::string &secret = "");
    bool parsePlaylistCard(const struct Json &p, Card &c);
    bool pagedTracks(std::string next, std::vector<Track> &out, int maxPages);
    void saveToken();

    bool user_ = false;   // токен — от аккаунта пользователя (а не ключами приложения)
    long tokenCode_ = 0;  // HTTP-код последнего запроса токена
    std::string userName_;
    std::string clientId_, clientSecret_, access_, refresh_;
    long long expires_ = 0; // время (unix), когда токен перестанет действовать
    bool keysLoaded_ = false;
};

// Плейлист HLS (m3u8) → адреса частей по порядку; первой — EXT-X-MAP, если есть.
// durations — длительность каждой части в секундах (у начального куска EXT-X-MAP — 0), если нужна.
std::vector<std::string> hlsSegments(const std::string &playlist, const std::string &baseUrl,
                                     std::vector<double> *durations = nullptr);

// Плейлисты HLS, скачанные заранее (SoundCloud::warm): адрес → тело и итоговый адрес после переадресаций.
// Take отдаёт и забывает; устаревшее (подписи CDN недолговечны) не отдаёт.
void hlsCachePut(const std::string &url, const std::string &finalUrl, const std::string &body);
bool hlsCacheTake(const std::string &url, std::string &finalUrl, std::string &body);
