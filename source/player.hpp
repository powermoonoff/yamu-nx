// Потоковый плеер: curl качает в память, FFmpeg декодирует (mp3/AAC), audout играет.
// Каждый трек — своя «дека» (загрузка + декодер); поток вывода смешивает две деки при плавном переходе.
#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <switch.h>

class Deck;

class Player {
public:
    enum State { Idle, Buffering, Playing, Paused, Finished, Error };

    bool init();
    void shutdown();
    // expectedBytes — примерный размер файла, пока сервер не прислал Content-Length.
    void play(const std::string &url, size_t expectedBytes = 0);
    // Плавный переход: новый трек нарастает, текущий затихает за seconds секунд.
    void crossfade(const std::string &url, size_t expectedBytes, int seconds);
    void stop();
    void togglePause();
    // Громкость приложения 0..100 % (отдельно от системной).
    void setVolume(int percent);
    // Перемотка на позицию в секундах (выполнится в потоке декодера).
    void seek(double sec);

    State state() const;
    double position() const;
    // Сколько скачано: 0..1, или -1 если размер неизвестен.
    double downloaded() const;
    std::string error() const;

private:
    static constexpr int NUM_BUFS = 4;
    static constexpr size_t BUF_FRAMES = 4096;
    static constexpr size_t BUF_BYTES = BUF_FRAMES * 2 * sizeof(s16);

    static void outEntry(void *arg);
    void outLoop();
    AudioOutBuffer *takeBuffer();
    void submit(AudioOutBuffer *b, size_t frames);
    void flushOut();
    std::shared_ptr<Deck> active() const;

    AudioOutBuffer bufs_[NUM_BUFS];
    std::vector<AudioOutBuffer *> free_;
    int queued_ = 0;
    std::vector<s16> mixA_, mixB_;

    Thread outThread_;
    bool outRunning_ = false;
    std::atomic<bool> quit_{false}, paused_{false}, flushReq_{false};

    mutable std::mutex m_;
    std::shared_ptr<Deck> active_, fading_; // fading_ — затихающий трек при переходе
    u64 cfTotal_ = 0, cfPos_ = 0;           // длина и ход перехода, в кадрах
    std::atomic<State> state_{Idle};
};
