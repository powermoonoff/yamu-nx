#include "http.hpp"
#include "crashlog.hpp"
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

// ---------- прокси ----------
// switch/yamu-nx/proxy.txt: первая строка — адрес прокси (socks5h://хост:порт, http://хост:порт,
// можно с логином: socks5h://логин:пароль@хост:порт). Вторая строка «все» (или «all») — через прокси
// весь трафик; иначе только SoundCloud. Системный прокси Свича из настроек Wi-Fi на homebrew не действует.
struct ProxyConf {
    bool loaded = false, all = false;
    std::string url;
};
ProxyConf g_proxy;
std::mutex g_proxyM;

std::string trimmed(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || (unsigned char)s[i] == 0xEF || (unsigned char)s[i] == 0xBB ||
                            (unsigned char)s[i] == 0xBF)) // пробелы и BOM от Блокнота
        i++;
    return s.substr(i);
}

const ProxyConf &proxyConf() {
    std::lock_guard<std::mutex> lk(g_proxyM);
    if (!g_proxy.loaded) {
        g_proxy.loaded = true;
        if (FILE *f = fopen(PROXY_PATH, "r")) {
            char line[512];
            int n = 0;
            while (fgets(line, sizeof(line), f) && n < 2) {
                std::string s = trimmed(line);
                if (s.empty() || s[0] == '#') continue;
                if (n == 0) g_proxy.url = s;
                else g_proxy.all = s == "все" || s == "всё" || s == "all" || s == "ALL";
                n++;
            }
            fclose(f);
        }
        // Без схемы — считаем HTTP-прокси (так понимает curl), но обычно это socks5h://.
    }
    return g_proxy;
}

bool soundCloudHost(const std::string &url) {
    size_t s = url.find("://");
    std::string host = url.substr(s == std::string::npos ? 0 : s + 3);
    host = host.substr(0, host.find_first_of("/:?"));
    for (const char *d : {"soundcloud.com", "sndcdn.com", "soundcloud.cloud"}) {
        size_t n = strlen(d);
        if (host.size() >= n && host.compare(host.size() - n, n, d) == 0 &&
            (host.size() == n || host[host.size() - n - 1] == '.'))
            return true;
    }
    return false;
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

// Постоянный хендл curl этого потока для запросов через прокси (см. perform). Деструкторы thread_local
// у потоков libnx не вызываются — закрывает его httpThreadDone().
struct Keep {
    CURL *c = nullptr;
};
thread_local Keep keep;

HttpResult perform(const std::string &url, const std::vector<std::string> &headers,
                   const std::string *form, const HttpSink &sink, const HttpLength &onLength, bool follow = true, const char *method = nullptr) {
    HttpResult res;
    // Через прокси каждое новое соединение — это рукопожатие с сервером прокси и TLS сквозь него,
    // секунды на запрос. Поэтому для таких запросов у каждого потока свой постоянный хендл curl:
    // он держит открытые соединения и сессии TLS, и следующие куски трека идут по ним же.
    std::string proxy = httpProxyFor(url);
    CURL *c;
    if (!proxy.empty() && keep.c) {
        c = keep.c;
        curl_easy_reset(c); // настройки — заново, соединения и сессии TLS остаются
    } else c = curl_easy_init();
    if (!c) { res.error = "curl_easy_init failed"; return res; }
    if (!proxy.empty()) keep.c = c;

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
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, follow ? 1L : 0L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    // Только IPv4: у Свича нет IPv6, а у части серверов (SoundCloud за CloudFront) есть адреса IPv6 —
    // curl пытался соединиться по ним и упирался в тайм-аут.
    curl_easy_setopt(c, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    if (!proxy.empty()) {
        curl_easy_setopt(c, CURLOPT_PROXY, proxy.c_str());
        curl_easy_setopt(c, CURLOPT_MAXCONNECTS, 3L);   // держим немного соединений с прокси
        curl_easy_setopt(c, CURLOPT_MAXAGE_CONN, 60L);  // простаивающие дольше минуты — закрываем
    }
    // Для потоков общий таймаут не ставим: трек качается всё время воспроизведения.
    if (!sink) curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
    if (form) {
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, form->c_str());
    }
    if (method) curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method); // DELETE и т. п.

    CURLcode rc = curl_easy_perform(c);
    if (rc == CURLE_OK) {
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &res.code);
        char *eff = nullptr;
        if (follow) {
            if (curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &eff) == CURLE_OK && eff) res.finalUrl = eff;
        } else if (curl_easy_getinfo(c, CURLINFO_REDIRECT_URL, &eff) == CURLE_OK && eff) res.finalUrl = eff;
    }
    else res.error = curl_easy_strerror(rc);

    if (!proxy.empty()) { // через прокси: по фазам, чтобы видеть, где теряется время
        double tc = 0, ts = 0, tf = 0, tt = 0;
        long nconn = 0;
        curl_easy_getinfo(c, CURLINFO_CONNECT_TIME, &tc);
        curl_easy_getinfo(c, CURLINFO_APPCONNECT_TIME, &ts);
        curl_easy_getinfo(c, CURLINFO_STARTTRANSFER_TIME, &tf);
        curl_easy_getinfo(c, CURLINFO_TOTAL_TIME, &tt);
        curl_easy_getinfo(c, CURLINFO_NUM_CONNECTS, &nconn);
        // В журнал — только ошибки и медленные запросы (ответ дольше 5 с), иначе куски треков его раздуют.
        if (rc != CURLE_OK || tf >= 5.0) {
        size_t hs = url.find("://");
        std::string host = url.substr(hs == std::string::npos ? 0 : hs + 3);
        host = host.substr(0, host.find_first_of("/?"));
        crashLog("[net] %s %s: прокси %.1f, TLS %.1f, ответ %.1f, всего %.1f с%s", host.c_str(),
                 rc == CURLE_OK ? std::to_string(res.code).c_str() : res.error.c_str(), tc, ts, tf, tt,
                 nconn == 0 ? " (соединение повторно)" : "");
        }
    }

    curl_slist_free_all(hdr);
    if (c != keep.c) curl_easy_cleanup(c);
    else curl_easy_setopt(c, CURLOPT_HTTPHEADER, nullptr); // список заголовков уже освобождён
    return res;
}
}

HttpResult httpGet(const std::string &url, const std::vector<std::string> &headers, const HttpSink &sink,
                   const HttpLength &onLength) {
    return perform(url, headers, nullptr, sink, onLength);
}

void httpThreadDone() {
    if (keep.c) curl_easy_cleanup(keep.c);
    keep.c = nullptr;
}

HttpResult httpRedirect(const std::string &url, const std::vector<std::string> &headers) {
    return perform(url, headers, nullptr, nullptr, nullptr, false);
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

HttpResult httpDelete(const std::string &url, const std::vector<std::string> &headers) {
    return perform(url, headers, nullptr, nullptr, nullptr, true, "DELETE");
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

std::string httpProxyFor(const std::string &url) {
    const ProxyConf &p = proxyConf();
    if (p.url.empty() || !(p.all || soundCloudHost(url))) return "";
    if (p.url.compare(0, 9, "trojan://") == 0) return "socks5h://127.0.0.1:9"; // не поддерживается — но и мимо прокси молча не идём
    return p.url;
}

std::string httpProxyInfo() {
    const ProxyConf &p = proxyConf();
    if (p.url.empty()) return "выкл";
    std::string where = p.all ? "весь трафик · " : "SoundCloud · ";
    if (p.url.compare(0, 9, "trojan://") == 0) return where + "Trojan не поддерживается — нужен socks5h:// или http://";
    std::string shown = p.url;
    size_t at = shown.rfind('@'), scheme = shown.find("://");
    if (at != std::string::npos) shown = shown.substr(0, scheme == std::string::npos ? 0 : scheme + 3) + shown.substr(at + 1); // без логина и пароля
    return where + shown;
}
