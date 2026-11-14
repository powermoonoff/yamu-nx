#include "sclogin.hpp"
#include "crashlog.hpp"
#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

static const int PORT = 47130;

static std::string urlDecode(const std::string &s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '+') o += ' ';
        else if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            o += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else o += s[i];
    }
    return o;
}

// Значение параметра из адреса или строки запроса (name=…&…).
static std::string param(const std::string &s, const std::string &name) {
    size_t p = 0;
    while ((p = s.find(name + "=", p)) != std::string::npos) {
        if (p == 0 || s[p - 1] == '?' || s[p - 1] == '&' || s[p - 1] == '#') {
            size_t b = p + name.size() + 1, e = s.find_first_of("&#", b);
            return s.substr(b, e == std::string::npos ? std::string::npos : e - b);
        }
        p += name.size();
    }
    return "";
}

static std::string htmlEsc(const std::string &s) {
    std::string o;
    for (char c : s) {
        if (c == '&') o += "&amp;";
        else if (c == '"') o += "&quot;";
        else if (c == '<') o += "&lt;";
        else if (c == '>') o += "&gt;";
        else o += c;
    }
    return o;
}

static const char *STYLE =
    "<meta name=viewport content='width=device-width,initial-scale=1'><meta charset=utf-8>"
    "<style>body{font-family:sans-serif;background:#16161a;color:#eee;max-width:560px;margin:auto;padding:20px;line-height:1.45}"
    "a.b,button{display:block;width:100%;box-sizing:border-box;text-align:center;background:#ff5500;color:#fff;border:0;"
    "border-radius:10px;padding:14px;font-size:18px;text-decoration:none;margin:12px 0}"
    "input{width:100%;box-sizing:border-box;padding:12px;font-size:16px;border-radius:8px;border:1px solid #555;"
    "background:#222;color:#eee}small{color:#999}</style>";

std::string ScLoginServer::start(const std::string &authUrl, const std::string &state, std::string &err) {
    stop();
    {
        std::lock_guard<std::mutex> lk(m_);
        authUrl_ = authUrl;
        state_ = state;
        code_.clear();
    }
    // Адрес Свича в домашней сети.
    u32 ip = 0;
    if (R_FAILED(nifmInitialize(NifmServiceType_User)) || R_FAILED(nifmGetCurrentIpAddress(&ip)) || ip == 0) {
        err = "Свич не подключён к Wi-Fi";
        return "";
    }
    nifmExit();
    lfd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd_ < 0) { err = "Не создался сокет"; return ""; }
    int one = 1;
    setsockopt(lfd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(PORT);
    if (bind(lfd_, (sockaddr *)&a, sizeof(a)) != 0 || listen(lfd_, 4) != 0) {
        close(lfd_);
        lfd_ = -1;
        err = "Порт " + std::to_string(PORT) + " занят";
        return "";
    }
    fcntl(lfd_, F_SETFL, fcntl(lfd_, F_GETFL, 0) | O_NONBLOCK); // accept под общим замком — только неблокирующий
    in_addr ia;
    ia.s_addr = ip;
    address_ = std::string("http://") + inet_ntoa(ia) + ":" + std::to_string(PORT);
    quit_ = false;
    if (R_FAILED(threadCreate(&thread_, entry, this, nullptr, 0x10000, 0x2C, -2)) || R_FAILED(threadStart(&thread_))) {
        close(lfd_);
        lfd_ = -1;
        err = "Не создался поток";
        return "";
    }
    running_ = true;
    crashLog("[sc] страница входа: %s", address_.c_str());
    return address_;
}

void ScLoginServer::stop() {
    if (!running_) return;
    quit_ = true;
    threadWaitForExit(&thread_);
    threadClose(&thread_);
    close(lfd_);
    lfd_ = -1;
    running_ = false;
}

bool ScLoginServer::takeCode(std::string &code) {
    std::lock_guard<std::mutex> lk(m_);
    if (code_.empty()) return false;
    code = code_;
    code_.clear();
    return true;
}

void ScLoginServer::entry(void *arg) { static_cast<ScLoginServer *>(arg)->loop(); }

void ScLoginServer::loop() {
    while (!quit_) {
        pollfd p{lfd_, POLLIN, 0};
        if (poll(&p, 1, 300) <= 0) continue;
        int fd = accept(lfd_, nullptr, nullptr);
        if (fd < 0) continue;
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) & ~O_NONBLOCK);
        timeval tv{5, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        handle(fd);
        close(fd);
    }
}

void ScLoginServer::handle(int fd) {
    std::string req;
    char buf[2048];
    while (req.find("\r\n\r\n") == std::string::npos && req.size() < 16384) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        req.append(buf, n);
    }
    size_t sp1 = req.find(' '), sp2 = sp1 == std::string::npos ? std::string::npos : req.find(' ', sp1 + 1);
    std::string path = sp2 == std::string::npos ? "/" : req.substr(sp1 + 1, sp2 - sp1 - 1);

    std::string authUrl, state;
    {
        std::lock_guard<std::mutex> lk(m_);
        authUrl = authUrl_;
        state = state_;
    }
    int status = 200;
    std::string body = std::string("<!doctype html><html><head><title>YaMu-nx</title>") + STYLE + "</head><body>";
    if (path.compare(0, 5, "/code") == 0) {
        // Вставили адрес после входа (или только код).
        std::string pasted = urlDecode(param(path, "u"));
        std::string code = param(pasted, "code"), st = param(pasted, "state");
        if (code.empty() && pasted.find_first_of("/?&=: ") == std::string::npos) code = pasted; // вставили сам код
        code = urlDecode(code);
        if (!code.empty() && (st.empty() || urlDecode(st) == state)) {
            {
                std::lock_guard<std::mutex> lk(m_);
                code_ = code;
            }
            body += "<h2>Готово ✅</h2><p>Код передан на Свич. Можно закрыть эту страницу и вернуться к приставке.</p>";
        } else {
            body += "<h2>Не нашёл код</h2><p>Вставьте адрес целиком — тот, что открылся после входа в SoundCloud "
                    "(в нём есть <b>code=</b>).</p><a class=b href='/'>Назад</a>";
        }
    } else if (path == "/" || path.compare(0, 2, "/?") == 0) {
        body += "<h2>Вход в SoundCloud</h2>"
                "<p><b>1.</b> Войдите в SoundCloud и разрешите доступ:</p>"
                "<a class=b href=\"" + htmlEsc(authUrl) + "\" target=_blank>Войти в SoundCloud</a>"
                "<p><b>2.</b> После входа откроется пустая страница или ошибка — это нормально. "
                "Скопируйте её адрес из адресной строки и вставьте сюда:</p>"
                "<form action=/code method=get><input name=u placeholder='http://localhost/…?code=…' autocomplete=off>"
                "<button type=submit>Отправить на Свич</button></form>"
                "<small>Страница работает, пока на Свиче открыт вход. Пароль от SoundCloud сюда не попадает.</small>";
    } else {
        status = 404;
        body += "<p>Нет такой страницы. <a href='/'>На главную</a></p>";
    }
    body += "</body></html>";
    std::string resp = "HTTP/1.1 " + std::string(status == 200 ? "200 OK" : "404 Not Found") +
                       "\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: " +
                       std::to_string(body.size()) + "\r\n\r\n" + body;
    size_t off = 0;
    while (off < resp.size()) {
        ssize_t n = send(fd, resp.data() + off, resp.size() - off, 0);
        if (n <= 0) break;
        off += n;
    }
}
