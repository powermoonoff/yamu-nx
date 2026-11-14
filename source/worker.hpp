// Фоновый поток для всех сетевых запросов к API, чтобы интерфейс не замирал.
// YandexApi трогается только из этого потока.
#pragma once
#include <deque>
#include <mutex>
#include <string>
#include <vector>
#include <switch.h>
#include "yandex.hpp"
#include "soundcloud.hpp"

struct SDL_Surface;

// Скачанные треки, обложки и index.json лежат здесь.
constexpr const char *OFFLINE_DIR = "sdmc:/switch/yamu-nx/offline";
// Кэш текстов песен: <id>.lrc (с таймкодами) или <id>.txt.
constexpr const char *LYRICS_DIR = "sdmc:/switch/yamu-nx/lyrics";

struct Job {
    enum Kind { Startup, LoadWave, Resolve, Feedback, Cover, Library, OpenList, Like, Download, Search, Lyrics, ConnectQueue, LoginStart, LoginPoll, SearchAll, Artist, LikedAlbums, ListMore, TrackMeta, ScSearch, ScResolve, ScLogin, ScHome, ScRelated, ScWarm } kind;
    // LoadWave
    bool fresh = false;
    std::string lastTrackId;
    std::string mood, diversity; // настроение волны перед новой волной (пусто — не трогать)
    // Resolve / Feedback
    int index = -1;
    unsigned gen = 0;
    Track track;
    std::string feedbackType;
    double played = 0;
    // Cover
    std::string url;
    // OpenList: плейлист (пустой kind — «Мне нравится»); Like
    Playlist playlist;
    bool like = false;
    // Search
    std::string query;
    // Lyrics: сохранить загруженный текст на карту
    bool cache = true;
    // Resolve: ссылка заранее, для плавного перехода (трек ещё не играет)
    bool preload = false;
    // ConnectQueue: очередь, присланная с другого устройства (id треков)
    std::vector<std::string> ids;
    // LoginStart: id устройства; LoginPoll: device_code
    std::string code;
    // Artist: id исполнителя
    std::string artistId;
};

struct JobResult {
    Job::Kind kind;
    bool ok = false;
    std::string error;
    // Startup
    std::string login;
    bool hasPlus = false;
    // Startup / LoadWave
    bool fresh = false;
    std::vector<Track> tracks;
    // Resolve
    int index = -1;
    unsigned gen = 0;
    std::string url, codec;
    int bitrate = 0;
    bool preload = false;
    // Library
    std::vector<Playlist> playlists;
    std::vector<std::string> likedIds;
    // OpenList / ListMore: more — следом придут ещё треки этого списка (ListMore)
    Playlist playlist;
    bool more = false;
    // Like
    std::string trackId;
    bool like = false;
    // Search: запрос, на который пришли tracks
    std::string query;
    // Download: скачанный трек и пути к файлам на карте
    Track track;
    std::string filePath, coverPath;
    // SearchAll: найденное; Artist: популярные треки — в tracks, альбомы — в cards, похожие — в cards2;
    // LikedAlbums: альбомы — в cards
    ::SearchAll search;
    std::vector<Card> cards, cards2;
    std::string artistId;
    // LoginStart: код для ввода; LoginPoll: токен (pending — ещё ждём)
    DeviceCode device;
    std::string token;
    bool pending = false;
    // Cover: декодированная картинка (текстуру создаёт главный поток)
    SDL_Surface *surface = nullptr;
    // Lyrics: текст песни (trackId — для какого трека)
    std::string lyrics;
    bool synced = false;
};

class Worker {
public:
    void start(const char *tokenPath);
    void stop();
    void post(const Job &job);
    // Выкинуть ещё не начатые задачи этого вида (например, устаревшие Resolve).
    void cancel(Job::Kind kind);
    bool poll(JobResult &out);
    size_t pending(Job::Kind kind);

private:
    static void entry(void *arg);
    void loop();
    void downloadSoundCloud(const Track &t, JobResult &r);
    JobResult run(const Job &job);

    YandexApi api_;
    SoundCloud sc_; // треки с id "sc:…" — через официальное API SoundCloud
    std::string tokenPath_;
    Thread thread_;
    bool running_ = false;
    volatile bool stop_ = false;
    std::mutex m_;
    std::deque<Job> jobs_;
    std::deque<JobResult> results_;
};
