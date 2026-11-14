#pragma once
#include <functional>
#include <mutex>
#include <string>
#include <vector>

struct HttpResult {
    long code = -1;      // HTTP-код, -1 при сетевой ошибке
    std::string body;
    std::string error;
    std::string finalUrl; // адрес после всех переадресаций (для относительных ссылок в плейлистах HLS)
};

// Колбэк для потоковой загрузки: вернуть false, чтобы прервать.
using HttpSink = std::function<bool(const char *data, size_t len)>;
// Вызывается, когда пришёл заголовок Content-Length.
using HttpLength = std::function<void(size_t len)>;

HttpResult httpGet(const std::string &url, const std::vector<std::string> &headers, const HttpSink &sink = nullptr,
                   const HttpLength &onLength = nullptr);
// Запрос без перехода по переадресации: finalUrl — куда переадресовали (пусто, если не переадресовали).
HttpResult httpRedirect(const std::string &url, const std::vector<std::string> &headers);
// Закрыть соединения через прокси, которые держит текущий поток (вызывать в конце потока).
void httpThreadDone();
// Общий замок на всё, что выделяет или освобождает дескрипторы libnx (сокеты и файлы делят одну таблицу,
// и она портится, если её меняют из нескольких потоков). Рекурсивный: под ним можно вызывать socket/close.
// socket, socketpair, accept, close, fopen, fclose берут его сами — их подменяет линковщик
// (-Wl,--wrap=...), так что и curl со своим служебным socketpair его не обойдёт.
std::recursive_mutex &socketMutex();

// Текущее время по часам сервера (из заголовка Date последнего ответа), -1 если ещё не знаем.
// Нужно системному модулю: служба времени ему недоступна.
long long httpServerTime();

HttpResult httpPost(const std::string &url, const std::vector<std::string> &headers, const std::string &form);
HttpResult httpDelete(const std::string &url, const std::vector<std::string> &headers);

// Открывать и закрывать сокеты этого curl-хендла под общим замком (см. socketMutex).
void httpUseSharedSockets(void *curl);

// Прокси для приложения (см. http.cpp): файл switch/yamu-nx/proxy.txt.
constexpr const char *PROXY_PATH = "sdmc:/switch/yamu-nx/proxy.txt";
// Через какой прокси идти на этот адрес; пусто — напрямую.
std::string httpProxyFor(const std::string &url);
// Для настроек: «выкл», «только SoundCloud» или «весь трафик» (с адресом без пароля).
std::string httpProxyInfo();
