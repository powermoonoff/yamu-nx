#include "ws.hpp"
#include "http.hpp"
#include <cstring>
#include <poll.h>
#include <switch.h>
#include <curl/curl.h>
#include <mbedtls/base64.h>

static const u64 MS = 1000000ULL;

bool WebSocket::sendAll(const char *data, size_t len) {
    CURL *c = (CURL *)curl_;
    size_t off = 0;
    for (int idle = 0; off < len && idle < 500;) {
        size_t n = 0;
        CURLcode rc = curl_easy_send(c, data + off, len - off, &n);
        if (rc == CURLE_AGAIN) { svcSleepThread(10 * MS); idle++; continue; }
        if (rc != CURLE_OK) return false;
        off += n;
        idle = 0;
    }
    return off == len;
}

int WebSocket::fill(int timeoutMs) {
    char buf[8192];
    for (int pass = 0; pass < 2; pass++) {
        size_t n = 0;
        CURLcode rc = curl_easy_recv((CURL *)curl_, buf, sizeof(buf), &n);
        if (rc == CURLE_OK) {
            if (n == 0) return -1; // сервер закрыл соединение
            in_.append(buf, n);
            return 1;
        }
        if (rc != CURLE_AGAIN) return -1;
        if (pass == 1 || timeoutMs <= 0) return 0;
        // TLS мог уже расшифровать данные в своём буфере, поэтому poll — только как ожидание.
        pollfd p{fd_, POLLIN, 0};
        if (poll(&p, 1, timeoutMs) < 0) return -1;
    }
    return 0;
}

bool WebSocket::connect(const std::string &url, const std::vector<std::string> &headers, std::string &err) {
    close();
    closeReason_.clear();
    if (url.compare(0, 6, "wss://") != 0) { err = "bad url"; return false; }
    std::string rest = url.substr(6);
    size_t slash = rest.find('/');
    std::string host = slash == std::string::npos ? rest : rest.substr(0, slash);
    std::string path = slash == std::string::npos ? "/" : rest.substr(slash);

    CURL *c = curl_easy_init();
    if (!c) { err = "curl_easy_init failed"; return false; }
    curl_ = c;
    curl_easy_setopt(c, CURLOPT_URL, ("https://" + host + "/").c_str());
    curl_easy_setopt(c, CURLOPT_CONNECT_ONLY, 1L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    // Только IPv4: у Свича нет IPv6, а у части серверов (SoundCloud за CloudFront) есть адреса IPv6 —
    // curl пытался соединиться по ним и упирался в тайм-аут.
    curl_easy_setopt(c, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    std::string proxy = httpProxyFor("https://" + host + "/"); // Коннект — через прокси, только если он «на всё»
    if (!proxy.empty()) curl_easy_setopt(c, CURLOPT_PROXY, proxy.c_str());
    httpUseSharedSockets(c);
    CURLcode rc = curl_easy_perform(c);
    if (rc != CURLE_OK) { err = curl_easy_strerror(rc); close(); return false; }
    long sock = -1;
    curl_easy_getinfo(c, CURLINFO_LASTSOCKET, &sock);
    fd_ = (int)sock;

    u8 key[16];
    randomGet(key, sizeof(key));
    unsigned char key64[32];
    size_t klen = 0;
    mbedtls_base64_encode(key64, sizeof(key64), &klen, key, sizeof(key));

    std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + host +
                      "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: " +
                      std::string((char *)key64, klen) + "\r\nUser-Agent: YaMu-nx\r\n";
    for (const auto &h : headers) req += h + "\r\n";
    req += "\r\n";
    if (!sendAll(req.data(), req.size())) { err = "handshake send failed"; close(); return false; }

    // Ответ на рукопожатие: ждём пустую строку, остаток — уже кадры.
    u64 deadline = armTicksToNs(armGetSystemTick()) / MS + 15000;
    size_t end;
    while ((end = in_.find("\r\n\r\n")) == std::string::npos) {
        if (armTicksToNs(armGetSystemTick()) / MS > deadline) { err = "handshake timeout"; close(); return false; }
        if (fill(200) < 0) { err = "handshake: connection closed"; close(); return false; }
    }
    std::string head = in_.substr(0, end);
    in_.erase(0, end + 4);
    sscanf(head.c_str(), "HTTP/1.1 %d", &status_);
    if (status_ != 101) {
        err = "HTTP " + std::to_string(status_);
        close();
        return false;
    }
    return true;
}

bool WebSocket::sendFrame(int opcode, const std::string &payload) {
    if (!curl_) return false;
    std::string f;
    f.push_back((char)(0x80 | opcode)); // FIN
    size_t n = payload.size();
    if (n < 126) {
        f.push_back((char)(0x80 | n)); // клиент обязан маскировать
    } else if (n < 65536) {
        f.push_back((char)(0x80 | 126));
        f.push_back((char)(n >> 8));
        f.push_back((char)n);
    } else {
        f.push_back((char)(0x80 | 127));
        for (int i = 7; i >= 0; i--) f.push_back((char)((u64)n >> (i * 8)));
    }
    u8 mask[4];
    randomGet(mask, sizeof(mask));
    f.append((char *)mask, 4);
    size_t off = f.size();
    f.append(payload);
    for (size_t i = 0; i < n; i++) f[off + i] ^= mask[i & 3];
    return sendAll(f.data(), f.size());
}

bool WebSocket::sendText(const std::string &text) { return sendFrame(0x1, text); }
bool WebSocket::ping() { return sendFrame(0x9, ""); }

int WebSocket::recv(std::string &msg, int timeoutMs) {
    if (!curl_) return -1;
    u64 deadline = armTicksToNs(armGetSystemTick()) / MS + timeoutMs;
    for (;;) {
        // Разбираем кадр, если он уже целиком в буфере.
        while (in_.size() >= 2) {
            const u8 *p = (const u8 *)in_.data();
            bool fin = p[0] & 0x80;
            int op = p[0] & 0x0F;
            bool masked = p[1] & 0x80;
            u64 len = p[1] & 0x7F;
            size_t hdr = 2;
            if (len == 126) {
                if (in_.size() < 4) break;
                len = (p[2] << 8) | p[3];
                hdr = 4;
            } else if (len == 127) {
                if (in_.size() < 10) break;
                len = 0;
                for (int i = 0; i < 8; i++) len = (len << 8) | p[2 + i];
                hdr = 10;
            }
            if (masked) hdr += 4;
            if (in_.size() < hdr + len) break;
            std::string payload = in_.substr(hdr, len);
            if (masked)
                for (size_t i = 0; i < len; i++) payload[i] ^= p[hdr - 4 + (i & 3)];
            in_.erase(0, hdr + len);

            if (op == 0x8) { // close: 2 байта кода и текст причины
                int code = payload.size() >= 2 ? ((u8)payload[0] << 8) | (u8)payload[1] : 0;
                std::string why = payload.size() > 2 ? payload.substr(2) : "";
                close();
                closeReason_ = "close " + std::to_string(code) + (why.empty() ? "" : " " + why);
                return -1;
            }
            if (op == 0x9) { sendFrame(0xA, payload); continue; } // ping → pong
            if (op == 0xA) continue;                         // pong
            partial_ += payload;                             // текст/бинарные данные и продолжения
            if (fin) {
                msg.swap(partial_);
                partial_.clear();
                return 1;
            }
        }
        long long left = (long long)deadline - (long long)(armTicksToNs(armGetSystemTick()) / MS);
        if (left <= 0) return 0;
        int r = fill(left > 100 ? 100 : (int)left);
        if (r < 0) {
            close();
            if (closeReason_.empty()) closeReason_ = "TCP закрыт";
            return -1;
        }
    }
}

void WebSocket::close() {
    if (curl_) curl_easy_cleanup((CURL *)curl_);
    curl_ = nullptr;
    fd_ = -1;
    in_.clear();
    partial_.clear();
}
