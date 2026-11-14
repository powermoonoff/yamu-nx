// Логика фонового плеера: очередь (волна или плейлист), переключение треков, лайки.
// Команды приходят из сервера и выполняются в своём потоке, чтобы сеть не блокировала ответы.
#pragma once
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include "sysplayer.hpp"
#include "yandex.hpp"
#include "soundcloud.hpp"

class Core {
public:
    bool init(const char *tokenPath);
    void shutdown();
    // Поставить команду в очередь (TOGGLE, NEXT, PREV, LIKE, WAVE, PLAYLIST owner kind shuffle, STOP).
    void command(const std::string &cmd);
    std::string statusJson();
    std::string libraryJson(); // ждёт загрузку библиотеки, если её ещё нет
    // Отдать текущую музыку приложению: записать состояние в handoff.json и замолчать.
    // true — было что отдать.
    bool releaseSync();

private:
    static void entry(void *arg);
    void loop();
    void exec(const std::string &cmd);
    void tick();
    void playIndex(int idx, double startSec = 0);
    void handoffFromFile();
    void releaseToFile();
    void seekTo(double sec);
    void startWave();
    void loadLibrary();
    // Скачанный трек на карте (по offline/index.json приложения) или пустая строка.
    std::string offlineFile(const std::string &id);
    void toggleShuffle();
    void playOffline(bool shuffle);
    void maybeCrossfade();

    YandexApi api_;
    SoundCloud sc_; // треки "sc:…" — mp3-поток через официальное API (свой токен с карты)
    // Адрес потока: Яндекс или SoundCloud — по id трека.
    bool streamUrl(const std::string &id, std::string &url, int &kbps, std::string &codec, std::string &err);
    SysPlayer player_;
    std::string tokenPath_;
    bool loggedIn_ = false;

    Thread thread_;
    volatile bool exit_ = false;

    std::mutex cmdM_;
    std::deque<std::string> cmds_;

    // Состояние под stateM_ (его читает сервер)
    mutable std::mutex stateM_;
    bool wave_ = true;
    bool scWave_ = false; // волна SoundCloud из приложения: в конце очереди дописываем похожие
    std::string sourceTitle_ = "Моя волна";
    std::vector<Track> queue_;
    int current_ = -1;
    std::set<std::string> liked_;
    std::vector<Playlist> playlists_;
    bool libLoaded_ = false;
    std::string error_;
    int failStreak_ = 0;
    int repeat_ = 0; // 0 — нет, 1 — плейлист, 2 — трек (приходит из приложения при передаче)
    bool shuffle_ = false;
    std::vector<Track> unshuffled_; // порядок до перемешивания
    int crossfade_ = 0;              // плавный переход, секунд (из настроек приложения)
    std::string xfId_, xfSrc_, xfCodec_; // следующий трек, заранее подготовленный для перехода
    int xfKbps_ = 0;
    std::string curSrc_, curCodec_; // откуда играет текущий трек и чем декодировать (для перемотки)
    int curKbps_ = 0;
    double curDur_ = 0;
    // Заранее найденная ссылка (PREPARE): трек, ссылка, битрейт, когда нашли.
    std::string prepId_, prepUrl_, prepCodec_;
    int prepKbps_ = 0;
    u64 prepTick_ = 0;
    volatile bool releaseDone_ = false, releaseHad_ = false;
};
