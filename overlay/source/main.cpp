// Оверлей YaMu-nx для Tesla/Ultrahand: управление фоновым плеером yamu-sys поверх игр.
// Вид (вариант А без обложки): своя тёмная рамка, название, перемотка, кнопки, громкость, карточки меню.
#define TESLA_INIT_IMPL
#include <tesla.hpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include "json.hpp"

static const u64 SYS_TID = 0x4200000000594D53ULL;
static const int PORT = 47123;

// Подключиться к модулю; дальше сокет блокирующий с таймаутом. -1 — модуль не отвечает.
static int ymConnect(int timeoutMs) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    connect(fd, (sockaddr *)&a, sizeof(a));
    pollfd p{fd, POLLOUT, 0};
    if (poll(&p, 1, timeoutMs) <= 0) { close(fd); return -1; }
    int soErr = 0;
    socklen_t len = sizeof(soErr);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &soErr, &len);
    if (soErr) { close(fd); return -1; }

    linger lg{1, 0}; // закрываем сразу, без TIME_WAIT — бережём сетевую память модуля
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
    // Дальше — блокирующий режим с таймаутом: так команда уходит и приходит целиком.
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) & ~O_NONBLOCK);
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return fd;
}

static bool sendAll(int fd, const std::string &s) {
    for (size_t off = 0; off < s.size();) {
        ssize_t n = send(fd, s.data() + off, s.size() - off, 0);
        if (n <= 0) return false;
        off += n;
    }
    return true;
}

// Одна команда — одно соединение; пустая строка, если модуль не ответил.
static std::string ymCall(const std::string &cmd, int timeoutMs = 400) {
    int fd = ymConnect(timeoutMs);
    if (fd < 0) return "";
    if (!sendAll(fd, cmd + "\n")) { close(fd); return ""; }
    std::string line;
    char buf[1024];
    while (line.find('\n') == std::string::npos && line.size() < 64 * 1024) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        line.append(buf, n);
    }
    close(fd);
    size_t nl = line.find('\n');
    return nl == std::string::npos ? line : line.substr(0, nl);
}

static bool launchModule() {
    if (ymCall("PING") == "PONG") return true;
    // Процесс уже есть, но не отвечает — второй экземпляр только помешает, ждём его.
    u64 runningPid = 0;
    if (R_SUCCEEDED(pmdmntGetProcessId(&runningPid, SYS_TID)) && runningPid) {
        for (int i = 0; i < 20; i++) {
            svcSleepThread(150000000ULL);
            if (ymCall("PING") == "PONG") return true;
        }
        return false;
    }
    NcmProgramLocation loc{};
    loc.program_id = SYS_TID;
    loc.storageID = NcmStorageId_None;
    u64 pid = 0;
    if (R_FAILED(pmshellLaunchProgram(0, &loc, &pid))) return false;
    for (int i = 0; i < 30; i++) { // ждём, пока модуль поднимет сервер
        svcSleepThread(100000000ULL);
        if (ymCall("PING") == "PONG") return true;
    }
    return false;
}

static std::string fmtTime(int s) {
    char b[16];
    snprintf(b, sizeof(b), "%d:%02d", s / 60, s % 60);
    return b;
}

struct Status {
    bool online = false;
    std::string state, source, title, artists, error, id;
    int pos = 0, dur = 0, vol = 100;
    bool liked = false, hasTrack = false, shuffle = false, wave = false;
    u64 tick = 0; // когда получили — чтобы полоса шла плавно между опросами
};

static Status g_status;
static std::string g_note; // сообщение для пользователя (ошибка запуска и т.п.)

static void refreshStatus() {
    std::string r = ymCall("STATUS");
    Status s;
    if (!r.empty()) {
        Json j = Json::parse(r);
        s.online = true;
        s.state = j["state"].asString();
        s.source = j["source"].asString();
        s.title = j["title"].asString();
        s.artists = j["artists"].asString();
        s.error = j["error"].asString();
        s.id = j["id"].asString();
        s.pos = (int)j["pos"].asInt();
        s.dur = (int)j["dur"].asInt();
        s.vol = (int)j["vol"].asInt(100);
        s.liked = j["liked"].asBool();
        s.shuffle = j["shuffle"].asBool();
        s.wave = j["wave"].asBool();
        s.hasTrack = !s.title.empty();
    }
    s.tick = armGetSystemTick();
    g_status = s;
}

// Позиция «сейчас»: последняя известная плюс прошедшее время, если играет.
static int livePos() {
    const Status &s = g_status;
    int p = s.pos;
    if (s.state == "playing") p += (int)(armTicksToNs(armGetSystemTick() - s.tick) / 1000000000ULL);
    return s.dur > 0 && p > s.dur ? s.dur : p;
}

static void ymSend(const std::string &cmd) {
    if (!g_status.online && !launchModule()) { g_note = "Модуль не запустился"; return; }
    g_note.clear();
    ymCall(cmd);
    svcSleepThread(150000000ULL);
    refreshStatus();
}

// ---------- рисование ----------

// Палитра и углы — как выбраны в приложении (settings.json: тема, акцент, «Углы»). WHITE — основной текст.
static tsl::Color PANEL{0x2, 0x2, 0x2, 0xF}, GRAY{0x9, 0x9, 0x9, 0xF}, DIM{0x5, 0x5, 0x5, 0xF},
    WHITE{0xF, 0xF, 0xF, 0xF}, BG{0x1, 0x1, 0x1, 0xF};
static const tsl::Color BLACK{0x0, 0x0, 0x0, 0xF}, PURE_WHITE{0xF, 0xF, 0xF, 0xF};
static bool g_square = false;
static tsl::Color YELLOW{0xF, 0xD, 0x4, 0xF}, ON_ACCENT{0x0, 0x0, 0x0, 0xF};

static const char *SETTINGS_PATH = "sdmc:/switch/yamu-nx/settings.json"; // общие с приложением
static const char *PREFS_PATH = "sdmc:/switch/yamu-nx/overlay.json";      // только оверлея

// Те же цвета, что col::ACCENTS в приложении; ACCENT_COUNT — свой цвет (accentR/G/B).
static const u8 ACCENT_RGB[][3] = {{255, 219, 77}, {255, 149, 64}, {240, 72, 72}, {244, 110, 170},
                                   {155, 110, 255}, {80, 145, 255}, {48, 200, 190}, {96, 205, 110}};
static const char *const ACCENT_NAMES[] = {"Жёлтый", "Оранжевый", "Красный", "Розовый", "Фиолетовый",
                                           "Синий", "Бирюзовый", "Зелёный", "Свой"};
static const int ACCENT_COUNT = 8;
static const char *const THEME_NAMES[] = {"Тёмная", "Чёрная (OLED)", "Светлая"};
static int g_theme = 0, g_accent = 0, g_rgb[3] = {255, 219, 77};

static std::string readFile(const char *path) {
    std::string s;
    if (FILE *f = fopen(path, "r")) {
        char buf[1024];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && s.size() < 64 * 1024) s.append(buf, n);
        fclose(f);
    }
    return s;
}

static void applyLook();
static void loadLook() {
    Json j = Json::parse(readFile(SETTINGS_PATH));
    g_accent = std::clamp((int)j["accent"].asInt(0), 0, ACCENT_COUNT);
    g_theme = std::clamp((int)j["theme"].asInt(0), 0, 2);
    g_square = j["squareTiles"].asBool();
    g_rgb[0] = std::clamp((int)j["accentR"].asInt(255), 0, 255);
    g_rgb[1] = std::clamp((int)j["accentG"].asInt(219), 0, 255);
    g_rgb[2] = std::clamp((int)j["accentB"].asInt(77), 0, 255);
    applyLook();
}

// Цвета из g_theme / g_accent / g_rgb (без чтения файла).
static void applyLook() {
    int theme = g_theme;
    if (theme == 2) { // светлая
        BG = {0xF, 0xF, 0xF, 0xF}; PANEL = {0xD, 0xD, 0xE, 0xF}; WHITE = {0x1, 0x1, 0x2, 0xF};
        GRAY = {0x6, 0x6, 0x7, 0xF}; DIM = {0xA, 0xA, 0xB, 0xF};
    } else {          // тёмная и чёрная (OLED)
        BG = theme == 1 ? tsl::Color{0x0, 0x0, 0x0, 0xF} : tsl::Color{0x1, 0x1, 0x1, 0xF};
        PANEL = theme == 1 ? tsl::Color{0x1, 0x1, 0x2, 0xF} : tsl::Color{0x2, 0x2, 0x2, 0xF};
        WHITE = {0xF, 0xF, 0xF, 0xF}; GRAY = {0x9, 0x9, 0x9, 0xF}; DIM = {0x5, 0x5, 0x5, 0xF};
    }
    int c[3];
    for (int k = 0; k < 3; k++) c[k] = g_accent == ACCENT_COUNT ? g_rgb[k] : ACCENT_RGB[g_accent][k];
    YELLOW = {(u8)(c[0] >> 4), (u8)(c[1] >> 4), (u8)(c[2] >> 4), 0xF};
    ON_ACCENT = (c[0] * 299 + c[1] * 587 + c[2] * 114) / 1000 > 150 ? BLACK : PURE_WHITE;
}

// Запись JSON (парсер в common только читает).
static void dumpJson(const Json &j, std::string &o) {
    switch (j.type) {
        case Json::Null: o += "null"; break;
        case Json::Bool: o += j.b ? "true" : "false"; break;
        case Json::Number: {
            char b[32];
            if (j.num == (double)(long long)j.num) snprintf(b, sizeof(b), "%lld", (long long)j.num);
            else snprintf(b, sizeof(b), "%.6g", j.num);
            o += b;
            break;
        }
        case Json::String:
            o += '"';
            for (unsigned char ch : j.str) {
                if (ch == '"' || ch == '\\') { o += '\\'; o += (char)ch; }
                else if (ch < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", ch); o += b; }
                else o += (char)ch;
            }
            o += '"';
            break;
        case Json::Array:
            o += '[';
            for (size_t i = 0; i < j.arr.size(); i++) { if (i) o += ','; dumpJson(j.arr[i], o); }
            o += ']';
            break;
        case Json::Object: {
            o += '{';
            bool first = true;
            for (const auto &kv : j.obj) {
                if (!first) o += ',';
                first = false;
                Json key;
                key.type = Json::String;
                key.str = kv.first;
                dumpJson(key, o);
                o += ':';
                dumpJson(kv.second, o);
            }
            o += '}';
            break;
        }
    }
}

// Оформление из оверлея — в общий settings.json; остальные настройки приложения не трогаем.
// Приложение, если открыто, подхватит изменения само (следит за файлом).
static void saveLook() {
    Json j = Json::parse(readFile(SETTINGS_PATH));
    if (j.type != Json::Object) { j = Json(); j.type = Json::Object; }
    auto num = [&](const char *k, int v) { Json n; n.type = Json::Number; n.num = v; j.obj[k] = n; };
    num("theme", g_theme);
    num("accent", g_accent);
    num("accentR", g_rgb[0]);
    num("accentG", g_rgb[1]);
    num("accentB", g_rgb[2]);
    Json sq;
    sq.type = Json::Bool;
    sq.b = g_square;
    j.obj["squareTiles"] = sq;
    std::string out;
    dumpJson(j, out);
    // Цвета меняем сразу из памяти: раньше перечитывали файл, и если запись не удалась,
    // значение тут же откатывалось — казалось, что кнопки не работают.
    applyLook();
    FILE *f = fopen(SETTINGS_PATH, "w");
    bool ok = f && fwrite(out.data(), 1, out.size(), f) == out.size();
    if (f) { fputc('\n', f); ok = fclose(f) == 0 && ok; }
    g_note = ok ? "" : "Не удалось сохранить settings.json";
}

// Что показывать на главном экране оверлея и насколько плотно.
struct Prefs {
    bool seek = true, controls = true, volume = true, wave = true, lists = true, offline = true, power = true;
    bool compact = false;
};
static Prefs g_prefs;
static bool g_rebuildMain = false; // настройки поменялись — пересобрать главный экран

static void loadPrefs() {
    Json j = Json::parse(readFile(PREFS_PATH));
    auto f = [&](const char *k, bool def) { return j[k].type == Json::Bool ? j[k].asBool() : def; };
    g_prefs.seek = f("seek", true);
    g_prefs.controls = f("controls", true);
    g_prefs.volume = f("volume", true);
    g_prefs.wave = f("wave", true);
    g_prefs.lists = f("lists", true);
    g_prefs.offline = f("offline", true);
    g_prefs.power = f("power", true);
    g_prefs.compact = f("compact", false);
    static bool loaded = false;
    static Prefs last;
    if (loaded && memcmp(&last, &g_prefs, sizeof(Prefs)) != 0) g_rebuildMain = true; // поменяли в другом месте
    last = g_prefs;
    loaded = true;
}

static void savePrefs() {
    auto b = [](bool v) { return v ? "true" : "false"; };
    if (FILE *f = fopen(PREFS_PATH, "w")) {
        fprintf(f, "{\"seek\":%s,\"controls\":%s,\"volume\":%s,\"wave\":%s,\"lists\":%s,\"offline\":%s,\"power\":%s,\"compact\":%s}\n",
                b(g_prefs.seek), b(g_prefs.controls), b(g_prefs.volume), b(g_prefs.wave), b(g_prefs.lists),
                b(g_prefs.offline), b(g_prefs.power), b(g_prefs.compact));
        fclose(f);
    }
    g_rebuildMain = true;
}
// Высоты строк: обычные и компактные.
static int rowH(int normal, int compact) { return g_prefs.compact ? compact : normal; }

// Треугольник с вертикальным основанием в x: остриём вправо (right) или влево.
static void tri(tsl::gfx::Renderer *r, s32 x, s32 y, s32 h, bool right, tsl::Color c) {
    s32 half = h / 2, w = h * 7 / 8;
    for (s32 i = 0; i < h; i++) {
        s32 d = i < half ? i : h - 1 - i;
        s32 len = (d + 1) * w / (half + 1);
        r->drawRect(right ? x : x - len, y + i, len, 1, r->a(c));
    }
}

// Треугольник остриём вниз: основание шириной w сверху в (x, y).
static void triDown(tsl::gfx::Renderer *r, s32 x, s32 y, s32 w, tsl::Color c) {
    for (s32 i = 0; i < w / 2; i++) r->drawRect(x + i, y + i, w - 2 * i, 1, r->a(c));
}

static void line(tsl::gfx::Renderer *r, s32 x0, s32 y0, s32 x1, s32 y1, s32 th, tsl::Color c) {
    s32 steps = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
    for (s32 i = 0; i <= steps; i++) {
        s32 x = x0 + (x1 - x0) * i / (steps ? steps : 1), y = y0 + (y1 - y0) * i / (steps ? steps : 1);
        r->drawRect(x - th / 2, y - th / 2, th, th, r->a(c));
    }
}

static void roundRect(tsl::gfx::Renderer *r, s32 x, s32 y, s32 w, s32 h, s32 rad, tsl::Color c) {
    if (g_square) rad = std::min<s32>(rad, 2); // «Углы: прямые»
    r->drawRect(x + rad, y, w - 2 * rad, h, r->a(c));
    r->drawRect(x, y + rad, w, h - 2 * rad, r->a(c));
    r->drawCircle(x + rad, y + rad, rad, true, r->a(c));
    r->drawCircle(x + w - rad - 1, y + rad, rad, true, r->a(c));
    r->drawCircle(x + rad, y + h - rad - 1, rad, true, r->a(c));
    r->drawCircle(x + w - rad - 1, y + h - rad - 1, rad, true, r->a(c));
}

// Жёлтая рамка выбранного элемента — со срезанными углами, чтобы не была квадратной.
static void outline(tsl::gfx::Renderer *r, s32 x, s32 y, s32 w, s32 h) {
    const s32 t = 2, cut = 4;
    r->drawRect(x + cut, y, w - 2 * cut, t, r->a(YELLOW));
    r->drawRect(x + cut, y + h - t, w - 2 * cut, t, r->a(YELLOW));
    r->drawRect(x, y + cut, t, h - 2 * cut, r->a(YELLOW));
    r->drawRect(x + w - t, y + cut, t, h - 2 * cut, r->a(YELLOW));
    line(r, x + 1, y + cut, x + cut, y + 1, t, YELLOW);
    line(r, x + w - cut - 1, y + 1, x + w - 2, y + cut, t, YELLOW);
    line(r, x + 1, y + h - cut - 1, x + cut, y + h - 2, t, YELLOW);
    line(r, x + w - cut - 1, y + h - 2, x + w - 2, y + h - cut - 1, t, YELLOW);
}

static s32 textWidth(tsl::gfx::Renderer *r, const std::string &s, float size) {
    return (s32)r->drawString(s.c_str(), false, 0, 0, size, tsl::style::color::ColorTransparent).first;
}

// Свои элементы: жёлтая рамка вместо мигающей синей, без тёмной подложки Tesla.
class YaElement : public tsl::elm::Element {
public:
    void layout(u16, u16, u16, u16) override {}
    tsl::elm::Element *requestFocus(tsl::elm::Element *, tsl::FocusDirection) override { return this; }
    void drawFocusBackground(tsl::gfx::Renderer *) override {}
    void drawHighlight(tsl::gfx::Renderer *r) override { outline(r, getX() - 4, getY() + 1, getWidth() + 8, getHeight() - 2); }
};

// Рамка оверлея как в наброске: тёмный фон, маленький жёлтый заголовок, справа — подпись.
class YaFrame : public tsl::elm::Element {
public:
    YaFrame(const std::string &title, std::function<std::string()> subtitle) : title_(title), subtitle_(subtitle) {}
    ~YaFrame() { delete content_; }
    void setContent(tsl::elm::Element *e) {
        delete content_;
        content_ = e;
        if (e) { e->setParent(this); invalidate(); }
    }
    void draw(tsl::gfx::Renderer *r) override {
        r->fillScreen(r->a(BG));
        r->drawString(title_.c_str(), false, 24, 44, 24, r->a(YELLOW));
        std::string sub = subtitle_ ? subtitle_() : "";
        if (!sub.empty()) {
            s32 w = std::min<s32>(textWidth(r, sub, 16), 240);
            r->drawString(sub.c_str(), false, tsl::cfg::FramebufferWidth - 24 - w, 42, 16, r->a(GRAY), 240);
        }
        r->drawRect(16, tsl::cfg::FramebufferHeight - 50, tsl::cfg::FramebufferWidth - 32, 1, r->a(DIM));
        r->drawString(" Назад      OK", false, 28, tsl::cfg::FramebufferHeight - 18, 18, r->a(GRAY));
        if (content_) content_->frame(r);
    }
    void layout(u16 x, u16 y, u16 w, u16 h) override {
        setBoundaries(x, y, w, h);
        if (content_) {
            content_->setBoundaries(x + 28, y + 70, w - 56, h - 70 - 56);
            content_->invalidate();
        }
    }
    tsl::elm::Element *requestFocus(tsl::elm::Element *old, tsl::FocusDirection dir) override {
        return content_ ? content_->requestFocus(old, dir) : nullptr;
    }
    bool onTouch(tsl::elm::TouchEvent e, s32 cx, s32 cy, s32 px, s32 py, s32 ix, s32 iy) override {
        if (!content_ || !content_->inBounds(cx, cy)) return false;
        return content_->onTouch(e, cx, cy, px, py, ix, iy);
    }

private:
    std::string title_;
    std::function<std::string()> subtitle_;
    tsl::elm::Element *content_ = nullptr;
};

// Пункт меню: тёмная карточка со значком, текстом и значением справа.
enum class Icon { None, Wave, Playlist, Download, Power, Heart, Gear };

static void drawIcon(tsl::gfx::Renderer *r, Icon icon, s32 cx, s32 cy, tsl::Color c) {
    switch (icon) {
        case Icon::Wave: { // эквалайзер
            static const s32 hs[] = {8, 16, 11, 5};
            for (int i = 0; i < 4; i++) r->drawRect(cx - 9 + i * 5, cy + 8 - hs[i], 3, hs[i], r->a(c));
            break;
        }
        case Icon::Playlist:
            for (int i = 0; i < 3; i++) r->drawRect(cx - 9, cy - 7 + i * 6, i == 2 ? 11 : 18, 3, r->a(c));
            break;
        case Icon::Download:
            r->drawRect(cx - 2, cy - 9, 4, 9, r->a(c));
            triDown(r, cx - 7, cy, 14, c);
            r->drawRect(cx - 9, cy + 8, 18, 2, r->a(c));
            break;
        case Icon::Power:
            r->drawCircle(cx, cy + 1, 8, false, r->a(c));
            r->drawCircle(cx, cy + 1, 7, false, r->a(c));
            r->drawRect(cx - 3, cy - 10, 6, 7, r->a(PANEL)); // разрыв кольца — цветом карточки
            r->drawRect(cx - 1, cy - 10, 2, 9, r->a(c));
            break;
        case Icon::Heart: {
            std::string h = "♥";
            r->drawString(h.c_str(), false, cx - textWidth(r, h, 18) / 2, cy + 7, 18, r->a(c));
            break;
        }
        case Icon::Gear: // шестерёнка: зубцы, кольцо, дырка
            for (int i = 0; i < 8; i++) {
                float a = i * 3.14159f / 4;
                r->drawCircle(cx + (s32)(9 * cosf(a)), cy + (s32)(9 * sinf(a)), 3, true, r->a(c));
            }
            r->drawCircle(cx, cy, 8, true, r->a(c));
            r->drawCircle(cx, cy, 3, true, r->a(PANEL));
            break;
        case Icon::None: break;
    }
}

class MenuItem : public YaElement {
public:
    MenuItem(Icon icon, const std::string &text, const std::string &value = "") : icon_(icon), text_(text), value_(value) {}
    void draw(tsl::gfx::Renderer *r) override {
        s32 x = getX(), y = getY() + 5, w = getWidth(), h = getHeight() - 10;
        roundRect(r, x, y, w, h, 9, PANEL);
        s32 tx = x + 16;
        if (icon_ != Icon::None) {
            drawIcon(r, icon_, x + 30, y + h / 2, icon_ == Icon::Wave ? YELLOW : WHITE);
            tx = x + 58;
        }
        s32 vw = value_.empty() ? 0 : textWidth(r, value_, 17) + 16;
        r->drawString(text_.c_str(), false, tx, y + h / 2 + 8, 21, r->a(WHITE), w - (tx - x) - vw - 8);
        if (!value_.empty()) r->drawString(value_.c_str(), false, x + w - vw, y + h / 2 + 7, 17, r->a(GRAY));
    }
    bool onTouch(tsl::elm::TouchEvent e, s32 cx, s32 cy, s32, s32, s32, s32) override {
        if (e != tsl::elm::TouchEvent::Release || !inBounds(cx, cy)) return false;
        return onClick(HidNpadButton_A);
    }

private:
    Icon icon_;
    std::string text_, value_;
};

// Длинное название плавно прокручивается: пауза в начале, затем едет по кругу (элемент сам обрезает края).
static void drawMarquee(tsl::gfx::Renderer *r, const std::string &text, s32 x, s32 baseline, s32 w, float size, tsl::Color c) {
    static std::string last;
    static u64 since = 0;
    if (text != last) { last = text; since = armGetSystemTick(); }
    s32 tw = textWidth(r, text, size);
    if (tw <= w) {
        r->drawString(text.c_str(), false, x, baseline, size, r->a(c));
        return;
    }
    const s32 gap = 48, cycle = tw + gap;
    const double pause = 2.0, speed = 40.0; // секунды в начале, пикселей в секунду
    double t = armTicksToNs(armGetSystemTick() - since) / 1e9;
    double run = cycle / speed;
    double phase = fmod(t, pause + run);
    s32 off = phase < pause ? 0 : (s32)((phase - pause) * speed);
    r->drawString(text.c_str(), false, x - off, baseline, size, r->a(c));
    r->drawString(text.c_str(), false, x - off + cycle, baseline, size, r->a(c));
}

// Ярлык источника, название (бегущей строкой, если длинное) и исполнитель.
static void drawNow(tsl::gfx::Renderer *r, s32 x, s32 y, s32 w, s32 h) {
    const Status &s = g_status;
    if (!s.online) {
        r->drawString("Модуль выключен", false, x, y + 34, 22, r->a(WHITE), w);
        r->drawString("Включите «Моя волна» или «Скачанное»", false, x, y + 60, 15, r->a(GRAY), w);
    } else if (!s.hasTrack) {
        r->drawString("Ничего не играет", false, x, y + 34, 22, r->a(WHITE), w);
    } else if (g_prefs.compact) { // компактно: без плашки источника, название и исполнитель мельче
        drawMarquee(r, s.title, x, y + 30, w, 22, WHITE);
        r->drawString(s.artists.c_str(), false, x, y + 54, 15, r->a(GRAY), w);
    } else {
        if (!s.source.empty()) { // жёлтая плашка «Моя волна» / «Скачанное» / «Плейлист»
            s32 pw = std::min<s32>(textWidth(r, s.source, 14) + 20, w);
            roundRect(r, x, y + 2, pw, 22, 11, YELLOW);
            r->drawString(s.source.c_str(), false, x + 10, y + 19, 14, r->a(ON_ACCENT), pw - 20);
        }
        drawMarquee(r, s.title, x, y + 56, w, 26, WHITE);
        r->drawString(s.artists.c_str(), false, x, y + 82, 17, r->a(GRAY), w);
    }
    std::string err = !g_note.empty() ? g_note : s.error;
    if (!err.empty()) r->drawString(err.c_str(), false, x, y + (g_prefs.compact ? 70 : 100), 13, r->a({0xF, 0x5, 0x5, 0xF}), w);
}

// Полоса перемотки: ←/→ — на 10 секунд, тап — в точку, A — пауза.
class SeekBar : public YaElement {
public:
    void draw(tsl::gfx::Renderer *r) override {
        const Status &s = g_status;
        s32 x = getX() + 6, w = getWidth() - 12, by = getY() + 18;
        float f = s.dur > 0 ? std::min(1.0f, (float)livePos() / s.dur) : 0;
        roundRect(r, x, by, w, 6, 3, DIM);
        if (f > 0) roundRect(r, x, by, std::max<s32>(4, (s32)(w * f)), 6, 3, YELLOW);
        if (s.hasTrack) r->drawCircle(x + (s32)(w * f), by + 3, 8, true, r->a(WHITE));
        r->drawString(fmtTime(livePos()).c_str(), false, x, by + 32, 16, r->a(GRAY));
        std::string d = fmtTime(s.dur);
        r->drawString(d.c_str(), false, x + w - textWidth(r, d, 16), by + 32, 16, r->a(GRAY));
        std::string st = s.state == "paused" ? "пауза" : s.state == "buffering" ? "загрузка…" : "";
        if (!st.empty()) r->drawString(st.c_str(), false, x + w / 2 - textWidth(r, st, 16) / 2, by + 32, 16, r->a(GRAY));
    }
    bool onClick(u64 keys) override {
        if (keys & HidNpadButton_AnyLeft) { ymSend("SEEKREL -10"); return true; }
        if (keys & HidNpadButton_AnyRight) { ymSend("SEEKREL 10"); return true; }
        if (keys & HidNpadButton_A) { ymSend("TOGGLE"); return true; }
        return false;
    }
    bool onTouch(tsl::elm::TouchEvent e, s32 cx, s32 cy, s32, s32, s32, s32) override {
        if (e != tsl::elm::TouchEvent::Release || !inBounds(cx, cy) || g_status.dur <= 0) return false;
        float f = std::clamp((float)(cx - getX() - 6) / (getWidth() - 12), 0.0f, 1.0f);
        ymSend("SEEK " + std::to_string((int)(f * g_status.dur)));
        return true;
    }
};

// Кнопки: перемешать, назад, пауза, дальше, лайк. ←/→ — выбор, A — нажать.
class Controls : public YaElement {
public:
    void drawHighlight(tsl::gfx::Renderer *) override {} // выбранную кнопку обводим сами
    void draw(tsl::gfx::Renderer *r) override {
        const Status &s = g_status;
        s32 cy = getY() + getHeight() / 2;
        for (int i = 0; i < 5; i++) {
            s32 cx = getX() + getWidth() * (2 * i + 1) / 10;
            bool sel = i == sel_ && m_focused;
            s32 rad = i == 2 ? 32 : i == 0 || i == 4 ? 0 : 24; // перемешать и лайк — без кружка, как в наброске
            if (sel) r->drawCircle(cx, cy, (rad ? rad : 24) + 5, true, r->a(YELLOW));
            if (i == 2) r->drawCircle(cx, cy, rad, true, r->a(sel ? WHITE : YELLOW));
            else if (rad) r->drawCircle(cx, cy, rad, true, r->a(PANEL));
            else if (sel) r->drawCircle(cx, cy, 24, true, r->a(PANEL));
            switch (i) {
                case 0: { // перемешать: два перекрещенных пути со стрелками
                    tsl::Color c = s.wave ? DIM : s.shuffle ? YELLOW : GRAY;
                    line(r, cx - 13, cy - 8, cx + 8, cy + 8, 3, c);
                    line(r, cx - 13, cy + 8, cx + 8, cy - 8, 3, c);
                    tri(r, cx + 7, cy - 13, 11, true, c);
                    tri(r, cx + 7, cy + 2, 11, true, c);
                    break;
                }
                case 1: r->drawRect(cx - 11, cy - 10, 4, 20, r->a(WHITE)); tri(r, cx + 9, cy - 10, 20, false, WHITE); break;
                case 2:
                    if (s.state == "playing" || s.state == "buffering") {
                        r->drawRect(cx - 10, cy - 13, 7, 26, r->a(sel ? BG : ON_ACCENT));
                        r->drawRect(cx + 3, cy - 13, 7, 26, r->a(sel ? BG : ON_ACCENT));
                    } else {
                        tri(r, cx - 8, cy - 14, 28, true, sel ? BG : ON_ACCENT);
                    }
                    break;
                case 3: tri(r, cx - 9, cy - 10, 20, true, WHITE); r->drawRect(cx + 7, cy - 10, 4, 20, r->a(WHITE)); break;
                case 4: {
                    std::string heart = "♥";
                    r->drawString(heart.c_str(), false, cx - textWidth(r, heart, 28) / 2, cy + 10, 28, r->a(s.liked ? YELLOW : GRAY));
                    break;
                }
            }
        }
    }
    bool onClick(u64 keys) override {
        if (keys & HidNpadButton_AnyLeft) { if (sel_ > 0) sel_--; return true; }
        if (keys & HidNpadButton_AnyRight) { if (sel_ < 4) sel_++; return true; }
        if (keys & HidNpadButton_A) { press(sel_); return true; }
        return false;
    }
    bool onTouch(tsl::elm::TouchEvent e, s32 cx, s32 cy, s32, s32, s32, s32) override {
        if (e != tsl::elm::TouchEvent::Release || !inBounds(cx, cy)) return false;
        sel_ = std::clamp((int)((cx - getX()) * 5 / getWidth()), 0, 4);
        press(sel_);
        return true;
    }

private:
    void press(int i) {
        static const char *cmds[] = {"SHUFFLE", "PREV", "TOGGLE", "NEXT", "LIKE"};
        ymSend(cmds[i]);
    }
    int sel_ = 2;
};

// Громкость: ←/→ — по 5 %, тап — в точку.
class Volume : public YaElement {
public:
    void draw(tsl::gfx::Renderer *r) override {
        s32 x = getX() + 6, cy = getY() + getHeight() / 2;
        int vol = g_status.vol;
        tsl::Color ic = vol == 0 ? DIM : GRAY;
        r->drawRect(x, cy - 4, 6, 8, r->a(ic)); // динамик
        tri(r, x + 14, cy - 8, 16, false, ic);
        if (vol == 0) line(r, x + 19, cy - 5, x + 27, cy + 5, 2, DIM);
        s32 bx = barX(), bw = barW();
        float f = vol / 100.0f;
        roundRect(r, bx, cy - 3, bw, 6, 3, DIM);
        if (f > 0) roundRect(r, bx, cy - 3, std::max<s32>(4, (s32)(bw * f)), 6, 3, WHITE);
        r->drawCircle(bx + (s32)(bw * f), cy, 8, true, r->a(WHITE));
        std::string p = std::to_string(vol) + " %";
        r->drawString(p.c_str(), false, getX() + getWidth() - 6 - textWidth(r, p, 17), cy + 7, 17, r->a(GRAY));
    }
    bool onClick(u64 keys) override {
        if (keys & HidNpadButton_AnyLeft) { set(g_status.vol - 5); return true; }
        if (keys & HidNpadButton_AnyRight) { set(g_status.vol + 5); return true; }
        return false;
    }
    bool onTouch(tsl::elm::TouchEvent e, s32 cx, s32 cy, s32, s32, s32, s32) override {
        if (e != tsl::elm::TouchEvent::Release || !inBounds(cx, cy)) return false;
        float f = std::clamp((float)(cx - barX()) / barW(), 0.0f, 1.0f);
        set((int)(f * 100 + 2.5f) / 5 * 5);
        return true;
    }

private:
    s32 barX() { return getX() + 46; }
    s32 barW() { return getWidth() - 46 - 76; }
    void set(int v) {
        v = std::clamp(v, 0, 100);
        g_status.vol = v; // сразу показываем, не дожидаясь модуля
        if (!g_status.online && !launchModule()) { g_note = "Модуль не запустился"; return; }
        ymCall("VOLUME " + std::to_string(v));
    }
};

// ---------- плейлисты ----------

class LibraryGui : public tsl::Gui {
    // Возврат откладываем до update(): goBack() внутри обработчика сразу удаляет этот экран,
    // а Tesla ещё перебирает его элементы в List::onTouch — тап по плейлисту ронял оверлей.
    bool back_ = false;
public:
    void update() override {
        if (back_) { back_ = false; tsl::goBack(); }
    }
    tsl::elm::Element *createUI() override {
        auto frame = new YaFrame("Плейлисты", [] { return std::string("A — играть, Y — вперемешку"); });
        auto list = new tsl::elm::List();
        std::string r = ymCall("LIBRARY", 5000);
        Json arr = Json::parse(r);
        if (arr.size() == 0)
            list->addItem(new MenuItem(Icon::None, r.empty() ? "Модуль не отвечает" : "Библиотека ещё грузится — откройте снова"), 70);
        for (size_t i = 0; i < arr.size(); i++) {
            const Json &p = arr[i];
            std::string owner = p["o"].asString(), kind = p["k"].asString();
            auto item = new MenuItem(kind == "-" ? Icon::Heart : Icon::Playlist, p["t"].asString(), std::to_string(p["n"].asInt()));
            item->setClickListener([this, owner, kind](u64 keys) {
                if (!(keys & (HidNpadButton_A | HidNpadButton_Y))) return false;
                bool shuffle = keys & HidNpadButton_Y;
                ymCall("PLAYLIST " + owner + " " + kind + (shuffle ? " 1" : " 0"));
                back_ = true;
                return true;
            });
            list->addItem(item, rowH(70, 54));
        }
        frame->setContent(list);
        return frame;
    }
};

// ---------- настройки оверлея ----------

// Подзаголовок раздела: не выбирается.
class Header : public tsl::elm::Element {
public:
    explicit Header(const std::string &t) : text_(t) {}
    void draw(tsl::gfx::Renderer *r) override {
        r->drawString(text_.c_str(), false, getX() + 4, getY() + getHeight() - 10, 16, r->a(YELLOW));
    }
    void layout(u16, u16, u16, u16) override {}
    tsl::elm::Element *requestFocus(tsl::elm::Element *, tsl::FocusDirection) override { return nullptr; }

private:
    std::string text_;
};

// Строка настройки: ←/→ или A меняют значение, оно справа (у выбранной — акцентом).
class OptItem : public YaElement {
public:
    OptItem(const std::string &text, std::function<std::string()> value, std::function<void(int)> change)
        : text_(text), value_(value), change_(change) {}
    void draw(tsl::gfx::Renderer *r) override {
        s32 x = getX(), y = getY() + 4, w = getWidth(), h = getHeight() - 8;
        roundRect(r, x, y, w, h, 9, PANEL);
        std::string v = value_();
        bool swatch = v.rfind("#", 0) == 0; // «#» в начале — показать образец акцента
        if (swatch) v = v.substr(1);
        std::string shown = m_focused ? "‹ " + v + " ›" : v;
        s32 vw = textWidth(r, shown, 17);
        r->drawString(text_.c_str(), false, x + 16, y + h / 2 + 7, 19, r->a(WHITE), w - vw - 48);
        r->drawString(shown.c_str(), false, x + w - 14 - vw, y + h / 2 + 6, 17, r->a(m_focused ? YELLOW : GRAY));
        if (swatch) r->drawCircle(x + w - 14 - vw - 14, y + h / 2, 7, true, r->a(YELLOW));
    }
    bool onClick(u64 keys) override {
        if (keys & HidNpadButton_AnyLeft) { apply(-1); return true; }
        if (keys & (HidNpadButton_AnyRight | HidNpadButton_A)) { apply(1); return true; }
        return false;
    }
    bool onTouch(tsl::elm::TouchEvent e, s32 cx, s32 cy, s32, s32, s32, s32) override {
        if (e != tsl::elm::TouchEvent::Release || !inBounds(cx, cy)) return false;
        apply(cx < getX() + getWidth() / 2 ? -1 : 1); // левая половина — назад, правая — вперёд
        return true;
    }
    void apply(int d) {
        std::string before = value_();
        change_(d);
    }

private:
    std::string text_;
    std::function<std::string()> value_;
    std::function<void(int)> change_;
};

static int cycleInt(int v, int dir, int n) { return ((v + dir) % n + n) % n; }

class SettingsGui : public tsl::Gui {
public:
    tsl::elm::Element *createUI() override {
        auto frame = new YaFrame("Настройки", [] { return g_note.empty() ? std::string("←/→ — изменить") : g_note; });
        auto list = new tsl::elm::List();
        const int h = 52;

        list->addItem(new Header("Оформление (общее с приложением)"), 36);
        list->addItem(new OptItem("Тема", [] { return std::string(THEME_NAMES[g_theme]); },
                                  [](int d) { g_theme = cycleInt(g_theme, d, 3); saveLook(); }), h);
        list->addItem(new OptItem("Цвет акцента", [] { return "#" + std::string(ACCENT_NAMES[g_accent]); },
                                  [](int d) { g_accent = cycleInt(g_accent, d, ACCENT_COUNT + 1); saveLook(); }), h);
        static const char *const RGB_NAMES[] = {"Свой цвет: красный", "Свой цвет: зелёный", "Свой цвет: синий"};
        for (int k = 0; k < 3; k++)
            list->addItem(new OptItem(RGB_NAMES[k], [k] { return std::to_string(g_rgb[k]); },
                                      [k](int d) {
                                          g_rgb[k] = std::clamp(g_rgb[k] + d * 15, 0, 255);
                                          g_accent = ACCENT_COUNT; // крутим свой цвет — сразу его и включаем
                                          saveLook();
                                      }), h);
        list->addItem(new OptItem("Углы", [] { return std::string(g_square ? "прямые" : "скруглённые"); },
                                  [](int) { g_square = !g_square; saveLook(); }), h);

        list->addItem(new Header("Главный экран оверлея"), 36);
        list->addItem(new OptItem("Компактный вид", [] { return std::string(g_prefs.compact ? "вкл" : "выкл"); },
                                  [](int) { g_prefs.compact = !g_prefs.compact; savePrefs(); }), h);
        struct Block { const char *name; bool *flag; };
        static const Block blocks[] = {
            {"Перемотка", &g_prefs.seek},        {"Кнопки управления", &g_prefs.controls},
            {"Громкость", &g_prefs.volume},      {"«Моя волна»", &g_prefs.wave},
            {"Плейлисты", &g_prefs.lists},       {"Скачанное", &g_prefs.offline},
            {"«Выключить фоновый плеер»", &g_prefs.power},
        };
        for (const Block &b : blocks) {
            bool *flag = b.flag;
            list->addItem(new OptItem(b.name, [flag] { return std::string(*flag ? "показывать" : "скрыть"); },
                                      [flag](int) { *flag = !*flag; savePrefs(); }), h);
        }
        frame->setContent(list);
        return frame;
    }
};

// ---------- главный экран ----------

class MainGui : public tsl::Gui {
public:
    tsl::elm::Element *createUI() override {
        // Компактно плашки источника нет — пишем его справа в шапке.
        frame_ = new YaFrame("YaMu-nx", [] { return g_prefs.compact && g_status.hasTrack ? g_status.source : std::string(); });
        frame_->setContent(buildList());
        return frame_;
    }

    void update() override {
        if (g_rebuildMain) { // вернулись из настроек — собираем экран заново с новыми блоками
            g_rebuildMain = false;
            removeFocus();
            frame_->setContent(buildList());
            restoreFocus();
        }
        if (++frames_ % 60 == 0) { // раз в секунду
            refreshStatus();
        }
    }

private:
    tsl::elm::Element *buildList() {
        auto list = new tsl::elm::List();
        const int item = rowH(70, 54);
        list->addItem(new tsl::elm::CustomDrawer(drawNow), rowH(106, 76));
        if (g_prefs.seek) list->addItem(new SeekBar(), rowH(60, 52));
        if (g_prefs.controls) list->addItem(new Controls(), rowH(88, 76));
        if (g_prefs.volume) list->addItem(new Volume(), rowH(56, 46));

        if (g_prefs.wave) {
            auto wave = new MenuItem(Icon::Wave, "Моя волна");
            wave->setClickListener([](u64 k) { if (!(k & HidNpadButton_A)) return false; ymSend("WAVE"); return true; });
            list->addItem(wave, item);
        }
        if (g_prefs.lists) {
            auto lib = new MenuItem(Icon::Playlist, "Плейлисты", "›");
            lib->setClickListener([](u64 k) {
                if (!(k & HidNpadButton_A)) return false;
                if (!g_status.online && !launchModule()) { g_note = "Модуль не запустился"; return true; }
                tsl::changeTo<LibraryGui>();
                return true;
            });
            list->addItem(lib, item);
        }
        if (g_prefs.offline) {
            auto offline = new MenuItem(Icon::Download, "Скачанное", "Y — вперемешку");
            offline->setClickListener([](u64 k) {
                if (!(k & (HidNpadButton_A | HidNpadButton_Y))) return false;
                ymSend(k & HidNpadButton_Y ? "OFFLINE 1" : "OFFLINE 0");
                return true;
            });
            list->addItem(offline, item);
        }
        if (g_prefs.power) {
            auto off = new MenuItem(Icon::Power, "Выключить фоновый плеер");
            off->setClickListener([](u64 k) {
                if (!(k & HidNpadButton_A)) return false;
                ymCall("EXIT");
                svcSleepThread(300000000ULL);
                refreshStatus();
                return true;
            });
            list->addItem(off, item);
        }
        auto set = new MenuItem(Icon::Gear, "Настройки", "›");
        set->setClickListener([](u64 k) {
            if (!(k & HidNpadButton_A)) return false;
            tsl::changeTo<SettingsGui>();
            return true;
        });
        list->addItem(set, item);
        return list;
    }

    YaFrame *frame_ = nullptr;
    u64 frames_ = 0;
};

class YaOverlay : public tsl::Overlay {
public:
    void initServices() override {
        static const SocketInitConfig cfg = {
            // Слишком маленькие буферы ломали отправку — команды доходили пустыми.
            .tcp_tx_buf_size = 0x4000,
            .tcp_rx_buf_size = 0x8000,
            .tcp_tx_buf_max_size = 0x8000,
            .tcp_rx_buf_max_size = 0x10000,
            .udp_tx_buf_size = 0x2400,
            .udp_rx_buf_size = 0xA500,
            .sb_efficiency = 2,
            .num_bsd_sessions = 1,
            .bsd_service_type = BsdServiceType_Auto,
        };
        socketInitialize(&cfg);
        pmshellInitialize();
        pmdmntInitialize();
        fsdevMountSdmc(); // Tesla сама карту не подключает: без этого fopen("sdmc:/...") не работал вовсе
        loadPrefs(); // до первого экрана: он строится по этим настройкам
        loadLook();
    }
    void exitServices() override {
        fsdevUnmountDevice("sdmc");
        pmdmntExit();
        pmshellExit();
        socketExit();
    }
    void onShow() override {
        loadPrefs();
        loadLook();
        refreshStatus();
    }
    std::unique_ptr<tsl::Gui> loadInitialGui() override { return initially<MainGui>(); }
};

int main(int argc, char **argv) { return tsl::loop<YaOverlay>(argc, argv); }
