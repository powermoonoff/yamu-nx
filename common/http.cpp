#include "http.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <switch.h>
#include <sys/socket.h>
#include <unistd.h>
#include <curl/curl.h>

namespace {
struct Ctx {
    std::string *body;
    const HttpSink *sink;
    const HttpLength *onLength;
};

size_t writeCb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    Ctx *ctx = static_cast<Ctx *>(userdata);
    size_t n = size * nmemb;
    if (ctx->sink && *ctx->sink) return (*ctx->sink)(ptr, n) ? n : 0;
    ctx->body->append(ptr, n);
    return n;
}

// Время сервера: секунды эпохи и такт процессора, когда мы их узнали.
std::atomic<long long> g_serverEpoch{-1};
std::atomic<u64> g_serverTick{0};

// "date: Sun, 01 Nov 2026 10:42:00 GMT" -> секунды Unix.
long long parseHttpDate(const char *s, size_t n) {
    std::string v(s, n);
    static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int d = 0, y = 0, hh = 0, mm = 0, ss = 0;
    char mon[4] = {0};
    size_t comma = v.find(',');
    if (comma == std::string::npos) return -1;
    if (sscanf(v.c_str() + comma + 1, " %d %3s %d %d:%d:%d", &d, mon, &y, &hh, &mm, &ss) != 6) return -1;
    int m = -1;
    for (int i = 0; i < 12; i++)
        if (strncmp(mon, months[i], 3) == 0) m = i;
    if (m < 0 || y < 1970) return -1;
    static const int cum[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    long long days = (y - 1970) * 365LL + (y - 1969) / 4 + cum[m] + d - 1;
    if (m > 1 && (y % 4 == 0)) days++; // високосный (до 2100 этого достаточно)
    return days * 86400 + hh * 3600 + mm * 60 + ss;
}

size_t headerCb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    Ctx *ctx = static_cast<Ctx *>(userdata);
    size_t n = size * nmemb;
    static const char dateKey[] = "date:";
    if (n > sizeof(dateKey) && strncasecmp(ptr, dateKey, sizeof(dateKey) - 1) == 0) {
        long long t = parseHttpDate(ptr + sizeof(dateKey) - 1, n - (sizeof(dateKey) - 1));
        if (t > 0) {
            g_serverEpoch = t;
            g_serverTick = armGetSystemTick();
        }
    }
    static const char key[] = "content-length:";
    if (ctx->onLength && *ctx->onLength && n > sizeof(key) && strncasecmp(ptr, key, sizeof(key) - 1) == 0)
        (*ctx->onLength)(strtoull(ptr + sizeof(key) - 1, nullptr, 10));
    return n;
}

curl_socket_t openSocket(void *, curlsocktype, struct curl_sockaddr *a) {
    std::lock_guard<std::recursive_mutex> lk(socketMutex());
    return socket(a->family, a->socktype, a->protocol);
}

int closeSocket(void *, curl_socket_t fd) {
    std::lock_guard<std::recursive_mutex> lk(socketMutex());
    return close(fd);
}

HttpResult perform(const std::string &url, const std::vector<std::string> &headers,
                   const std::string *form, const HttpSink &sink, const HttpLength &onLength) {
    HttpResult res;
    CURL *c = curl_easy_init();
    if (!c) { res.error = "curl_easy_init failed"; return res; }

    struct curl_slist *hdr = nullptr;
    for (const auto &h : headers) hdr = curl_slist_append(hdr, h.c_str());
    Ctx ctx{&res.body, &sink, &onLength};

    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "Yandex-Music-API");
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, headerCb);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &ctx);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_OPENSOCKETFUNCTION, openSocket);
    curl_easy_setopt(c, CURLOPT_CLOSESOCKETFUNCTION, closeSocket);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    // Для потоков общий таймаут не ставим: трек качается всё время воспроизведения.
    if (!sink) curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
    if (form) {
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, form->c_str());
    }

    CURLcode rc = curl_easy_perform(c);
    if (rc == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &res.code);
    else res.error = curl_easy_strerror(rc);

    curl_slist_free_all(hdr);
    curl_easy_cleanup(c);
    return res;
}
}

HttpResult httpGet(const std::string &url, const std::vector<std::string> &headers, const HttpSink &sink,
                   const HttpLength &onLength) {
    return perform(url, headers, nullptr, sink, onLength);
}

std::recursive_mutex &socketMutex() {
    static std::recursive_mutex m;
    return m;
}

long long httpServerTime() {
    long long e = g_serverEpoch;
    if (e < 0) return -1;
    return e + (long long)(armTicksToNs(armGetSystemTick() - g_serverTick) / 1000000000ULL);
}

HttpResult httpPost(const std::string &url, const std::vector<std::string> &headers, const std::string &form) {
    return perform(url, headers, &form, nullptr, nullptr);
}

void httpUseSharedSockets(void *curl) {
    curl_easy_setopt((CURL *)curl, CURLOPT_OPENSOCKETFUNCTION, openSocket);
    curl_easy_setopt((CURL *)curl, CURLOPT_CLOSESOCKETFUNCTION, closeSocket);
}

// Подмена функций, выделяющих и освобождающих дескрипторы libnx (-Wl,--wrap=... в Makefile):
// всё это — под общим замком socketMutex(). Без этого curl (служебный socketpair в каждом запросе),
// файлы и наш accept из разных потоков портили общую таблицу, и модуль падал в _socketGetFd.
extern "C" {
int __real_socket(int domain, int type, int protocol);
int __real_socketpair(int domain, int type, int protocol, int sv[2]);
int __real_accept(int fd, struct sockaddr *addr, socklen_t *len);
int __real_close(int fd);
FILE *__real_fopen(const char *path, const char *mode);
int __real_fclose(FILE *f);

int __wrap_socket(int domain, int type, int protocol) {
    std::lock_guard<std::recursive_mutex> lk(socketMutex());
    return __real_socket(domain, type, protocol);
}
int __wrap_socketpair(int domain, int type, int protocol, int sv[2]) {
    std::lock_guard<std::recursive_mutex> lk(socketMutex());
    return __real_socketpair(domain, type, protocol, sv);
}
// Только для неблокирующего accept: блокирующий держал бы замок, пока ждёт.
int __wrap_accept(int fd, struct sockaddr *addr, socklen_t *len) {
    std::lock_guard<std::recursive_mutex> lk(socketMutex());
    return __real_accept(fd, addr, len);
}
int __wrap_close(int fd) {
    std::lock_guard<std::recursive_mutex> lk(socketMutex());
    return __real_close(fd);
}
FILE *__wrap_fopen(const char *path, const char *mode) {
    std::lock_guard<std::recursive_mutex> lk(socketMutex());
    return __real_fopen(path, mode);
}
int __wrap_fclose(FILE *f) {
    std::lock_guard<std::recursive_mutex> lk(socketMutex());
    return __real_fclose(f);
}
}
