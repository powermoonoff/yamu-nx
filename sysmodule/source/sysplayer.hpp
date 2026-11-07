// Плеер системного модуля: mp3 (mpg123) и AAC (FAAD2), из сети или с карты.
// Каждый трек — «дека» (загрузка + декодер в свою очередь PCM); поток вывода берёт из деки и при
// плавном переходе смешивает две. Памяти у модулей мало: окно сети ~1 МБ, звука вперёд — 2 с.
#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <switch.h>

class SysDeck;

class SysPlayer {
public:
    enum State { Idle, Buffering, Playing, Paused, Finished, Error };

    bool init();
    void shutdown();
    // src — https-ссылка или файл на карте (sdmc:...). codec: "mp3", "aac"/"he-aac" (поток ADTS), "m4a" (MP4 с карты).
    // startSec > 0 — начать с середины: m4a — точно по таблице кадров, остальное — по оценке байта
    // (по битрейту kbps или по размеру файла и длине трека durSec).
    void play(const std::string &src, const std::string &codec, double startSec = 0, int kbps = 0, double durSec = 0);
    // Плавный переход: новый трек нарастает, текущий затихает за seconds секунд.
    void crossfade(const std::string &src, const std::string &codec, int kbps, double durSec, int seconds);
    void stop();
    void setPaused(bool p);
    bool paused() const { return paused_; }
    // Громкость 0..100 % (как в приложении).
    void setVolume(int percent);
    int volume() const { return volume_; }

    State state() const;
    double position() const;
    std::string error() const;
    // Начало трека уже есть — можно играть без паузы (для бесшовной передачи из приложения).
    bool ready() const;

private:
    static constexpr int NUM_BUFS = 4;
    static constexpr size_t BUF_FRAMES = 4096;
    static constexpr size_t BUF_BYTES = BUF_FRAMES * 2 * sizeof(s16);

    static void outEntry(void *arg);
    void outLoop();
    AudioOutBuffer *takeBuffer();
    void submit(AudioOutBuffer *b, size_t frames);
    void flushOut();
    std::shared_ptr<SysDeck> active() const;

    AudioOutBuffer bufs_[NUM_BUFS];
    std::vector<AudioOutBuffer *> free_;
    int queued_ = 0;
    std::vector<s16> mixA_, mixB_;

    Thread outThread_;
    bool outRunning_ = false;
    std::atomic<bool> quit_{false}, paused_{false}, flushReq_{false};

    mutable std::mutex m_;
    std::shared_ptr<SysDeck> active_, fading_; // fading_ — затихающий трек при переходе
    u64 cfTotal_ = 0, cfPos_ = 0;              // длина и ход перехода, в кадрах
    std::atomic<State> state_{Idle};
    int volume_ = 100;
};
