#include "connect.hpp"
#include "json.hpp"
#include "ws.hpp"
#include <algorithm>
#include <cstdio>
#include <ctime>

static const u64 MS = 1000000ULL;
static const char *REDIRECT_URL = "wss://ynison.music.yandex.ru/redirector.YnisonRedirectService/GetRedirectToYnison";
static const char *STATE_PATH = "/ynison_state.YnisonStateService/PutYnisonState";
static const char *APP_NAME = "YaMu-nx";

static u64 nowMs() { return armTicksToNs(armGetSystemTick()) / MS; }

static std::string esc(const std::string &s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o;
}

static std::string q(const std::string &s) { return "\"" + esc(s) + "\""; }

static std::string readToken(const std::string &path) {
    FILE *f = fopen(path.c_str(), "r");
    if (!f) return "";
    char buf[512] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    std::string t(buf, n);
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) t.pop_back();
    while (!t.empty() && (t[0] == ' ' || (unsigned char)t[0] == 0xEF || (unsigned char)t[0] == 0xBB || (unsigned char)t[0] == 0xBF)) t.erase(0, 1);
    return t;
}

std::string Connect::newDeviceId() {
    u8 b[8];
    randomGet(b, sizeof(b));
    char s[17];
    for (int i = 0; i < 8; i++) snprintf(s + i * 2, 3, "%02x", b[i]);
    return s;
}

void Connect::start(const char *tokenPath, const std::string &deviceId, const std::string &title, const std::string &version) {
    if (running_) return;
    tokenPath_ = tokenPath;
    deviceId_ = deviceId;
    title_ = title;
    appVersion_ = version;
    stop_ = false;
    threadCreate(&thread_, entry, this, nullptr, 0x40000, 0x2C, -2);
    threadStart(&thread_);
    running_ = true;
}

void Connect::stop() {
    if (!running_) return;
    stop_ = true;
    threadWaitForExit(&thread_);
    threadClose(&thread_);
    running_ = false;
    online_ = false;
    std::lock_guard<std::mutex> lk(m_);
    in_.clear();
    out_.clear();
}

bool Connect::poll(std::string &msg) {
    std::lock_guard<std::mutex> lk(m_);
    if (in_.empty()) return false;
    msg = std::move(in_.front());
    in_.pop_front();
    return true;
}

void Connect::send(const std::string &json) {
    if (!online_) return; // без связи копить нечего: при подключении отправим состояние заново
    std::lock_guard<std::mutex> lk(m_);
    out_.push_back(json);
}

std::string Connect::lastError() const {
    std::lock_guard<std::mutex> lk(m_);
    return error_;
}

void Connect::setError(const std::string &e) {
    std::lock_guard<std::mutex> lk(m_);
    error_ = e;
}

// Версия изменения: Ynison сравнивает их, чтобы понять, чьё состояние новее. Всё — строками.
std::string Connect::version() const {
    long long ns = (long long)time(nullptr) * 1000000000LL + (long long)(armTicksToNs(armGetSystemTick()) % 1000000000ULL);
    return "{\"device_id\":" + q(deviceId_) + ",\"version\":\"" + std::to_string(ns) + "\",\"timestamp_ms\":\"0\"}";
}

std::string Connect::envelope(const std::string &body) const {
    u8 r[16];
    randomGet(r, sizeof(r));
    char rid[40];
    snprintf(rid, sizeof(rid), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", r[0], r[1], r[2],
             r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);
    return "{" + body + ",\"rid\":\"" + rid + "\",\"player_action_timestamp_ms\":\"" +
           std::to_string((long long)time(nullptr) * 1000) + "\",\"activity_interception_type\":\"DO_NOT_INTERCEPT_BY_DEFAULT\"}";
}

static std::string statusJson(double pos, double dur, bool paused, const std::string &ver) {
    return "{\"progress_ms\":\"" + std::to_string((long long)(pos * 1000)) + "\",\"duration_ms\":\"" +
           std::to_string((long long)(dur * 1000)) + "\",\"paused\":" + (paused ? "true" : "false") +
           ",\"playback_speed\":1,\"version\":" + ver + "}";
}

std::string Connect::playerState(const std::vector<Track> &queue, int index, double pos, double dur, bool paused,
                                 int repeat) const {
    std::string list;
    for (const Track &t : queue) {
        if (!list.empty()) list += ",";
        list += "{\"playable_id\":" + q(t.id) + ",\"album_id_optional\":" + q(t.albumId) +
                ",\"playable_type\":\"TRACK\",\"from\":\"yamu-nx\",\"title\":" + q(t.title) +
                ",\"cover_url_optional\":" + q(t.coverUri) + "}";
    }
    const char *rep = repeat == 2 ? "ONE" : repeat == 1 ? "ALL" : "NONE";
    std::string ver = version();
    std::string queueJson = "{\"current_playable_index\":" + std::to_string(index) +
                            ",\"entity_id\":\"\",\"entity_type\":\"VARIOUS\",\"playable_list\":[" + list +
                            "],\"options\":{\"repeat_mode\":\"" + rep +
                            "\"},\"entity_context\":\"BASED_ON_ENTITY_BY_DEFAULT\",\"version\":" + ver +
                            ",\"from_optional\":\"\"}";
    return envelope("\"update_player_state\":{\"player_state\":{\"status\":" + statusJson(pos, dur, paused, ver) +
                    ",\"player_queue\":" + queueJson + "}}");
}

std::string Connect::playingStatus(double pos, double dur, bool paused) const {
    // Без версии и служебных полей — так шлёт рабочий клиент (лишнее сервер встречает ошибкой 500).
    return "{\"update_playing_status\":{\"playing_status\":{\"progress_ms\":\"" + std::to_string((long long)(pos * 1000)) +
           "\",\"duration_ms\":\"" + std::to_string((long long)(dur * 1000)) + "\",\"paused\":" + (paused ? "true" : "false") +
           ",\"playback_speed\":1.0}}}";
}

std::string Connect::activate() const {
    return "{\"update_active_device\":{\"device_id_optional\":" + q(deviceId_) + "}}";
}

std::string Connect::protocolHeader(const std::string &ticket, const std::string &session) const {
    std::string info = "{\"app_name\":\"" + std::string(APP_NAME) + "\",\"type\":1}";
    std::string proto = "{\"Ynison-Device-Id\":" + q(deviceId_) + ",\"Ynison-Device-Info\":" + q(info);
    if (!ticket.empty()) proto += ",\"Ynison-Redirect-Ticket\":" + q(ticket) + ",\"Ynison-Session-Id\":" + q(session);
    proto += "}";
    return "Sec-WebSocket-Protocol: Bearer, v2, " + proto;
}

bool Connect::session() {
    std::string token = readToken(tokenPath_);
    if (token.empty()) { setError("нет токена"); return false; }
    std::vector<std::string> base = {"Authorization: OAuth " + token, "Origin: https://music.yandex.ru"};

    // 1. Редиректор говорит, к какому серверу состояний идти.
    std::string host, ticket, sessionId, err;
    {
        WebSocket ws;
        std::vector<std::string> h = base;
        h.push_back(protocolHeader("", ""));
        if (!ws.connect(REDIRECT_URL, h, err)) { setError("редиректор: " + err); return false; }
        std::string msg;
        if (ws.recv(msg, 10000) != 1) { setError("редиректор не ответил"); return false; }
        Json j = Json::parse(msg);
        host = j["host"].asString();
        ticket = j["redirect_ticket"].asString();
        // session_id — длинное число: берём как есть из текста, double парсера его бы округлил.
        size_t p = msg.find("\"session_id\"");
        if (p != std::string::npos && (p = msg.find(':', p)) != std::string::npos) {
            size_t s = msg.find_first_not_of(" \"", p + 1), e = msg.find_first_not_of("0123456789", s);
            if (s != std::string::npos) sessionId = msg.substr(s, e == std::string::npos ? std::string::npos : e - s);
        }
        if (host.empty() || ticket.empty()) { setError("редиректор: " + msg.substr(0, 120)); return false; }
    }

    // 2. Сервер состояний: представляемся и дальше обмениваемся состояниями.
    WebSocket ws;
    std::vector<std::string> h = base;
    h.push_back(protocolHeader(ticket, sessionId));
    if (!ws.connect("wss://" + host + STATE_PATH, h, err)) { setError("сервер: " + err); return false; }

    std::string ver = version();
    std::string full =
        "\"update_full_state\":{\"player_state\":{\"status\":" + statusJson(0, 0, true, ver) +
        ",\"player_queue\":{\"current_playable_index\":-1,\"entity_id\":\"\",\"entity_type\":\"VARIOUS\","
        "\"playable_list\":[],\"options\":{\"repeat_mode\":\"NONE\"},\"entity_context\":\"BASED_ON_ENTITY_BY_DEFAULT\","
        "\"version\":" + ver + ",\"from_optional\":\"\"}},"
        "\"device\":{\"info\":{\"device_id\":" + q(deviceId_) + ",\"title\":" + q(title_) +
        ",\"type\":\"WEB\",\"app_name\":\"" + APP_NAME + "\",\"app_version\":" + q(appVersion_) +
        "},\"capabilities\":{\"can_be_player\":true,\"can_be_remote_controller\":false},"
        "\"is_shadow\":false},\"is_currently_active\":false}";
    if (!ws.sendText(envelope(full))) { setError("не отправилось начальное состояние"); return false; }
    ws.sendText("{\"update_session_params\":{\"mute_events_if_passive\":true}}");

    online_ = true;
    setError("");
    {
        std::lock_guard<std::mutex> lk(m_);
        in_.push_back("{\"_connected\":true}");
    }
    u64 lastPing = nowMs();
    std::string lastMsg; // последнее сообщение сервера — в нём бывает текст ошибки
    while (!stop_) {
        std::deque<std::string> out;
        {
            std::lock_guard<std::mutex> lk(m_);
            out.swap(out_);
        }
        bool ok = true;
        for (const auto &m : out)
            if (!ws.sendText(m)) { ok = false; break; }
        if (ok && nowMs() - lastPing > 25000) {
            ok = ws.ping();
            lastPing = nowMs();
        }
        std::string msg;
        int r = ok ? ws.recv(msg, 100) : -1;
        if (r < 0) {
            std::string why = ws.closeReason();
            Json e = Json::parse(lastMsg)["error"];
            if (!e.isNull()) why += " / " + e["message"].asString() + " " + e["details"]["ynison-error-code"].asString();
            // Сервер просит не возвращаться какое-то время — слушаемся, иначе он продлевает запрет.
            long long away = e["details"]["ynison-go-away-for-seconds"].asInt(0);
            if (away > 0) {
                goAwaySec_ = (int)std::min(away, 3600LL);
                why = "сервер просит подождать " + std::to_string((goAwaySec_ + 59) / 60) + " мин";
            }
            setError("связь оборвалась: " + (why.empty() ? std::string("?") : why));
            break;
        }
        if (r == 1) {
            lastMsg = msg.substr(0, 600);
            std::lock_guard<std::mutex> lk(m_);
            if (in_.size() > 50) in_.pop_front(); // главный поток не успевает — старые состояния не нужны
            in_.push_back(std::move(msg));
        }
    }
    online_ = false;
    std::lock_guard<std::mutex> lk(m_);
    out_.clear();
    in_.push_back("{\"_lost\":true}");
    return true;
}

void Connect::loop() {
    static const int DELAYS[] = {5, 15, 60, 300};
    int fails = 0;
    while (!stop_) {
        u64 started = nowMs();
        bool connected = session();
        if (stop_) break;
        // Связь жила долго — сразу переподключаемся; иначе ждём всё дольше.
        if (connected && nowMs() - started > 60000) fails = 0;
        int delay = DELAYS[fails < 3 ? fails : 3];
        if (goAwaySec_ > delay) delay = goAwaySec_;
        goAwaySec_ = 0;
        fails++;
        for (int i = 0; i < delay * 10 && !stop_; i++) svcSleepThread(100 * MS);
    }
}
