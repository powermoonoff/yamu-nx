// Вход в SoundCloud с телефона: Свич поднимает в домашней сети маленькую страницу
// (http://<адрес Свича>:47130). На ней — кнопка входа в SoundCloud и поле, куда вставляется адрес,
// на который SoundCloud переадресовал после входа. Из него берётся код, приложение меняет его на токен.
#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <switch.h>

class ScLoginServer {
public:
    // authUrl — адрес страницы входа SoundCloud; state — проверка, что код пришёл от нашего входа.
    // Возвращает адрес страницы для телефона или пусто (и err).
    std::string start(const std::string &authUrl, const std::string &state, std::string &err);
    void stop();
    bool running() const { return running_; }
    // Пришёл код от телефона (забирает его).
    bool takeCode(std::string &code);
    const std::string &address() const { return address_; }

private:
    static void entry(void *arg);
    void loop();
    void handle(int fd);

    Thread thread_{};
    int lfd_ = -1;
    std::atomic<bool> running_{false}, quit_{false};
    std::mutex m_;
    std::string authUrl_, state_, code_, address_;
};
