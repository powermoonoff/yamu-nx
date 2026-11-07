// Клиент неофициального API Яндекс Музыки (api.music.yandex.net).
#pragma once
#include <string>
#include <vector>

struct Track {
    std::string id;      // "123456" — id трека без альбома
    std::string albumId;
    std::string title;
    std::string artists; // через запятую
    std::vector<std::string> artistIds, artistNames; // по отдельности (для перехода к исполнителю); могут быть пусты у старых записей
    std::string albumTitle;
    long long durationMs = 0;
    std::string coverUri; // avatars.yandex.net/.../%% — %% заменить на размер

    std::string coverUrl(int size) const;
};

struct Playlist {
    std::string kind, owner, title, coverUri;
    int trackCount = 0;

    std::string coverUrl(int size) const {
        Track t;
        t.coverUri = coverUri;
        return t.coverUrl(size);
    }
};

// Вход по коду (OAuth device flow): приложение показывает код, пользователь вводит его на ya.ru/device.
struct DeviceCode {
    std::string deviceCode, userCode, url;
    int interval = 5, expiresIn = 300;
};

// Исполнитель, альбом или плейлист — карточка в поиске и на странице исполнителя.
struct Card {
    enum Kind { Artist = 1, Album, PlaylistCard };
    int kind = Album;
    std::string id, owner, title, sub, coverUri; // id — исполнителя/альбома или kind плейлиста; owner — uid владельца плейлиста
    int trackCount = 0;
    std::string group; // у альбомов исполнителя: "album", "single" или "also" (не его альбом, сборник)

    std::string coverUrl(int size) const {
        Track t;
        t.coverUri = coverUri;
        return t.coverUrl(size);
    }
};

// Поиск сразу по всему, как в приложении Яндекс Музыки.
struct SearchAll {
    int bestKind = 0; // 0 — нет, 4 — трек, иначе Card::Kind
    Card best;
    Track bestTrack;
    std::vector<Track> tracks;
    std::vector<Card> artists, albums, playlists;
};

struct Json;

class YandexApi {
public:
    // Библиотека пользователя.
    bool playlists(std::vector<Playlist> &out);
    bool playlistTracks(const Playlist &pl, std::vector<Track> &out);
    bool likedIds(std::vector<std::string> &out);
    bool tracksByIds(const std::vector<std::string> &ids, std::vector<Track> &out);
    bool setLike(const Track &t, bool like);
    // Поиск треков (несколько страниц выдачи подряд).
    bool searchTracks(const std::string &text, std::vector<Track> &out, int pages = 2);
    bool searchAll(const std::string &text, SearchAll &out);
    bool albumTracks(const std::string &albumId, std::vector<Track> &out);
    // Страница исполнителя: популярные треки, альбомы, похожие исполнители.
    bool artistPage(const std::string &artistId, std::vector<Track> &popular, std::vector<Card> &albums,
                    std::vector<Card> &similar);
    bool likedAlbums(std::vector<Card> &out);

    bool loadToken(const char *path);
    static bool deviceCodeStart(const std::string &deviceId, const std::string &name, DeviceCode &out, std::string &err);
    // 1 — токен получен, 0 — ещё ждём подтверждения, -1 — ошибка (в err).
    static int deviceCodePoll(const std::string &deviceCode, std::string &token, std::string &err);
    // Проверка аккаунта. Заполняет login/uid/hasPlus, при ошибке — lastError.
    bool accountStatus();
    // Очередная порция «Моей волны». lastTrackId — последний сыгранный, чтобы волна шла дальше.
    bool waveTracks(std::vector<Track> &out, const std::string &lastTrackId = "");
    // Настроение волны: mood — all/active/calm/fun/sad, diversity — default/favorite/discover/popular.
    bool waveSettings(const std::string &mood, const std::string &diversity);
    // Сообщить волне о событии: radioStarted / trackStarted / trackFinished / skip.
    void waveFeedback(const std::string &type, const Track *track = nullptr, double playedSec = 0);
    // Прямая ссылка на аудио лучшего доступного качества (mp3, иначе AAC).
    bool trackUrl(const std::string &trackId, std::string &url, int &bitrate, std::string &codec);
    // Текст песни: LRC с таймкодами (synced = true) или обычный текст.
    bool lyrics(const std::string &trackId, std::string &text, bool &synced);

    std::string login, uid, lastError;
    bool mp3Only = false; // только mp3
    bool noMp4 = false;   // системный модуль: только mp3 и AAC-поток (ADTS) без HE-AAC
    bool hasPlus = false;

private:
    static bool parseTrack(const Json &t, Track &tr);
    static bool parseArtist(const Json &a, Card &c);
    static bool parseAlbum(const Json &a, Card &c);
    bool parsePlaylistCard(const Json &p, Card &c) const;
    bool fileInfoUrl(const std::string &trackId, std::string &url, int &bitrate, std::string &codec);
    bool downloadInfoUrl(const std::string &trackId, std::string &url, int &bitrate, std::string &codec);
    std::vector<std::string> headers() const;
    std::string token_, batchId_;
};
