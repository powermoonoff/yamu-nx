// Ynison — «Яндекс Коннект»: Свич виден как устройство в Яндекс Музыке, им можно управлять с телефона.
// Свой поток держит WebSocket к ynison.music.yandex.ru; сообщения в обе стороны — JSON-строки через очереди.
#pragma once
#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <vector>
#include <switch.h>
#include "yandex.hpp"

class Connect {
public:
    void start(const char *tokenPath, const std::string &deviceId, const std::string &title, const std::string &version);
    void stop();
    bool running() const { return running_; }
    bool online() const { return online_; }
    std::string lastError() const;
    const std::string &deviceId() const { return deviceId_; }

    // Входящие состояния от сервера. Служебные: {"_connected":true} и {"_lost":true}.
    bool poll(std::string &msg);
    void send(const std::string &json);

    // Сообщения протокола.
    std::string playerState(const std::vector<Track> &queue, int index, double posSec, double durSec, bool paused,
                            int repeat) const;
    std::string playingStatus(double posSec, double durSec, bool paused) const;
    std::string activate() const;

    static std::string newDeviceId();

private:
    static void entry(void *arg) { static_cast<Connect *>(arg)->loop(); }
    void loop();
    bool session(); // одно подключение до обрыва; false — не удалось подключиться
    std::string version() const;
    std::string envelope(const std::string &body) const;
    std::string protocolHeader(const std::string &ticket, const std::string &session) const;
    void setError(const std::string &e);

    std::string tokenPath_, deviceId_, title_, appVersion_;
    Thread thread_;
    std::atomic<bool> running_{false}, stop_{false}, online_{false};
    mutable std::mutex m_;
    std::deque<std::string> in_, out_;
    std::string error_;
    int goAwaySec_ = 0; // сервер попросил не подключаться столько секунд
};
