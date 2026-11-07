#include "ui.hpp"
#include <switch.h>
#include <SDL2/SDL_image.h>

static const size_t TEXT_CACHE_MAX = 300;

namespace col {
SDL_Color bg, panel, white, gray, dim, yellow, red, black;

// Акценты: первый — фирменный жёлтый Яндекса.
const Accent ACCENTS[] = {
    {"Жёлтый", {255, 219, 77, 255}},   {"Оранжевый", {255, 149, 64, 255}}, {"Красный", {240, 72, 72, 255}},
    {"Розовый", {244, 110, 170, 255}}, {"Фиолетовый", {155, 110, 255, 255}}, {"Синий", {80, 145, 255, 255}},
    {"Бирюзовый", {48, 200, 190, 255}}, {"Зелёный", {96, 205, 110, 255}},
};
const int ACCENT_COUNT = sizeof(ACCENTS) / sizeof(ACCENTS[0]);
const char *const THEME_NAMES[] = {"Тёмная", "Чёрная (OLED)", "Светлая"};

void apply(int theme, int accent, SDL_Color custom) {
    switch (theme) {
        case ThemeBlack:
            bg = {0, 0, 0, 255};
            panel = {24, 24, 27, 255};
            white = {245, 245, 245, 255};
            gray = {145, 145, 150, 255};
            dim = {80, 80, 86, 255};
            red = {255, 92, 92, 255};
            break;
        case ThemeLight:
            bg = {244, 244, 246, 255};
            panel = {224, 224, 229, 255};
            white = {24, 24, 27, 255};
            gray = {100, 100, 108, 255};
            dim = {165, 165, 172, 255};
            red = {210, 50, 50, 255};
            break;
        default:
            bg = {18, 18, 20, 255};
            panel = {32, 32, 36, 255};
            white = {245, 245, 245, 255};
            gray = {150, 150, 155, 255};
            dim = {90, 90, 96, 255};
            red = {255, 92, 92, 255};
            break;
    }
    if (accent == ACCENT_COUNT) yellow = {custom.r, custom.g, custom.b, 255}; // свой цвет
    else yellow = ACCENTS[accent < 0 || accent > ACCENT_COUNT ? 0 : accent].c;

    // На светлом акценте — чёрный текст, на тёмном — белый.
    int lum = (yellow.r * 299 + yellow.g * 587 + yellow.b * 114) / 1000;
    black = lum > 150 ? SDL_Color{0, 0, 0, 255} : SDL_Color{255, 255, 255, 255};
}

struct Init { Init() { apply(ThemeDark, 0); } } init; // палитра по умолчанию до загрузки настроек
}

bool Ui::init() {
    if (SDL_Init(SDL_INIT_VIDEO) < 0) return false;
    if (TTF_Init() < 0) return false;
    IMG_Init(IMG_INIT_JPG | IMG_INIT_PNG | IMG_INIT_WEBP);

    if (R_FAILED(plInitialize(PlServiceType_User))) return false;
    PlFontData std, ext;
    if (R_FAILED(plGetSharedFontByType(&std, PlSharedFontType_Standard))) return false;
    stdData_ = std.address;
    stdSize_ = std.size;
    if (R_SUCCEEDED(plGetSharedFontByType(&ext, PlSharedFontType_NintendoExt))) {
        extData_ = ext.address;
        extSize_ = ext.size;
    }

    win_ = SDL_CreateWindow("YaMu-nx", 0, 0, W, H, 0);
    if (!win_) return false;
    r_ = SDL_CreateRenderer(win_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!r_) return false;
    SDL_RenderSetLogicalSize(r_, W, H); // в доке 1080p — масштабируем
    SDL_SetRenderDrawBlendMode(r_, SDL_BLENDMODE_BLEND);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    return true;
}

void Ui::shutdown() {
    for (auto &kv : cache_) SDL_DestroyTexture(kv.second.tex);
    cache_.clear();
    for (auto &kv : fonts_) TTF_CloseFont(kv.second);
    for (auto &kv : extFonts_) TTF_CloseFont(kv.second);
    if (r_) SDL_DestroyRenderer(r_);
    if (win_) SDL_DestroyWindow(win_);
    plExit();
    IMG_Quit();
    TTF_Quit();
    SDL_Quit();
}

void Ui::begin() {
    SDL_SetRenderDrawColor(r_, col::bg.r, col::bg.g, col::bg.b, 255);
    SDL_RenderClear(r_);
}

void Ui::present() { SDL_RenderPresent(r_); }

TTF_Font *Ui::font(int size, bool ext) {
    size = (int)(size * textScale_ + 0.5f); // настройка «Размер текста»
    auto &m = ext ? extFonts_ : fonts_;
    auto it = m.find(size);
    if (it != m.end()) return it->second;
    const void *data = ext ? extData_ : stdData_;
    size_t len = ext ? extSize_ : stdSize_;
    if (!data) return nullptr;
    TTF_Font *f = TTF_OpenFontRW(SDL_RWFromConstMem(data, (int)len), 1, size);
    m[size] = f;
    return f;
}

int Ui::textWidth(const std::string &s, int size, bool ext) {
    TTF_Font *f = font(size, ext);
    int w = 0, h = 0;
    if (f && !s.empty()) TTF_SizeUTF8(f, s.c_str(), &w, &h);
    return w;
}

// Обрезает строку по границам UTF-8, пока она не влезет в maxW вместе с «…».
std::string Ui::fit(const std::string &s, int size, int maxW, bool ext) {
    if (maxW <= 0) return s;
    std::string key = std::to_string(size) + "|" + std::to_string(maxW) + "|" + (ext ? "1|" : "0|") + s;
    auto it = fitCache_.find(key);
    if (it != fitCache_.end()) return it->second;
    if (fitCache_.size() > 500) fitCache_.clear();

    std::string out = s;
    if (textWidth(s, size, ext) > maxW) {
        static const std::string ell = "…";
        std::string head = s;
        while (!head.empty()) {
            do head.pop_back(); // убираем по одному символу UTF-8
            while (!head.empty() && (head.back() & 0xC0) == 0x80);
            if (!head.empty() && (unsigned char)head.back() >= 0xC0) head.pop_back();
            std::string t = head;
            while (!t.empty() && t.back() == ' ') t.pop_back();
            if (textWidth(t + ell, size, ext) <= maxW) { out = t + ell; break; }
        }
        if (head.empty()) out = ell;
    }
    fitCache_[key] = out;
    return out;
}

int Ui::text(const std::string &s0, int x, int y, int size, SDL_Color c, Align a, int maxW, bool ext) {
    if (s0.empty()) return 0;
    std::string s = fit(s0, size, maxW, ext);
    char keyBuf[48];
    snprintf(keyBuf, sizeof(keyBuf), "%d|%d|%02x%02x%02x%02x|", size, ext ? 1 : 0, c.r, c.g, c.b, c.a);
    std::string key = keyBuf + s;

    auto it = cache_.find(key);
    if (it == cache_.end()) {
        TTF_Font *f = font(size, ext);
        if (!f) return 0;
        SDL_Surface *surf = TTF_RenderUTF8_Blended(f, s.c_str(), c);
        if (!surf) return 0;
        Cached e{SDL_CreateTextureFromSurface(r_, surf), surf->w, surf->h, {}};
        SDL_FreeSurface(surf);
        lru_.push_front(key);
        e.lru = lru_.begin();
        it = cache_.emplace(key, e).first;
        if (cache_.size() > TEXT_CACHE_MAX) { // выкидываем давно не нужное
            auto old = cache_.find(lru_.back());
            SDL_DestroyTexture(old->second.tex);
            cache_.erase(old);
            lru_.pop_back();
        }
    } else {
        lru_.splice(lru_.begin(), lru_, it->second.lru);
    }

    const Cached &e = it->second;
    int dx = a == Align::Left ? x : a == Align::Center ? x - e.w / 2 : x - e.w;
    SDL_Rect dst{dx, y, e.w, e.h};
    SDL_RenderCopy(r_, e.tex, nullptr, &dst);
    return e.w;
}

void Ui::rect(int x, int y, int w, int h, SDL_Color c) {
    SDL_SetRenderDrawColor(r_, c.r, c.g, c.b, c.a);
    SDL_Rect rc{x, y, w, h};
    SDL_RenderFillRect(r_, &rc);
}

void Ui::roundRect(int x, int y, int w, int h, int rad, SDL_Color c) {
    if (rad * 2 > h) rad = h / 2;
    if (rad * 2 > w) rad = w / 2;
    SDL_SetRenderDrawColor(r_, c.r, c.g, c.b, c.a);
    SDL_Rect mid{x, y + rad, w, h - 2 * rad};
    SDL_RenderFillRect(r_, &mid);
    for (int i = 0; i < rad; i++) { // скругления построчно
        float dy = rad - i - 0.5f;
        int dx = (int)(rad - SDL_sqrtf((float)rad * rad - dy * dy) + 0.5f);
        SDL_RenderDrawLine(r_, x + dx, y + i, x + w - 1 - dx, y + i);
        SDL_RenderDrawLine(r_, x + dx, y + h - 1 - i, x + w - 1 - dx, y + h - 1 - i);
    }
}

void Ui::circle(int cx, int cy, int rad, SDL_Color c) { roundRect(cx - rad, cy - rad, rad * 2, rad * 2, rad, c); }

void Ui::triangle(float x1, float y1, float x2, float y2, float x3, float y3, SDL_Color c) {
    SDL_Vertex v[3] = {{{x1, y1}, c, {0, 0}}, {{x2, y2}, c, {0, 0}}, {{x3, y3}, c, {0, 0}}};
    SDL_RenderGeometry(r_, nullptr, v, 3, nullptr, 0);
}

// Линия заданной толщины — прямоугольник из двух треугольников.
void Ui::line(float x1, float y1, float x2, float y2, float width, SDL_Color c) {
    float dx = x2 - x1, dy = y2 - y1, len = SDL_sqrtf(dx * dx + dy * dy);
    if (len < 0.01f) return;
    float nx = -dy / len * width / 2, ny = dx / len * width / 2;
    SDL_Vertex v[4] = {{{x1 + nx, y1 + ny}, c, {0, 0}}, {{x1 - nx, y1 - ny}, c, {0, 0}},
                       {{x2 - nx, y2 - ny}, c, {0, 0}}, {{x2 + nx, y2 + ny}, c, {0, 0}}};
    int idx[6] = {0, 1, 2, 0, 2, 3};
    SDL_RenderGeometry(r_, nullptr, v, 4, idx, 6);
}

// Сердечко из двух кругов и треугольника; size — ширина.
void Ui::heart(int cx, int cy, int size, SDL_Color c) {
    int r = size / 4;
    circle(cx - r, cy - r / 2, r, c);
    circle(cx + r, cy - r / 2, r, c);
    float top = cy - r / 2.0f + r * 0.35f;
    triangle(cx - 2.0f * r + 0.6f, top, cx + 2.0f * r - 0.6f, top, cx, cy + 1.45f * r, c);
}

// Стрелка вниз с полочкой: size — высота.
void Ui::downArrow(int cx, int cy, int size, SDL_Color c) {
    int s = size;
    rect(cx - s / 10, cy - s / 2, s / 5 + 1, s / 2, c);
    triangle(cx - s * 0.38f, cy - s * 0.05f, cx + s * 0.38f, cy - s * 0.05f, cx, cy + s * 0.32f, c);
    rect(cx - s * 2 / 5, cy + s * 2 / 5, s * 4 / 5, s / 10 + 1, c);
}

void Ui::image(SDL_Texture *t, int x, int y, int w, int h) {
    SDL_Rect dst{x, y, w, h};
    SDL_RenderCopy(r_, t, nullptr, &dst);
}

SDL_Texture *Ui::textureFrom(SDL_Surface *s) {
    if (!s) return nullptr;
    SDL_Texture *t = SDL_CreateTextureFromSurface(r_, s);
    SDL_FreeSurface(s);
    return t;
}

void Ui::setTextScale(float s) {
    if (s == textScale_) return;
    textScale_ = s;
    // Готовые надписи и обрезки посчитаны под старый размер — выбрасываем.
    for (auto &kv : cache_) SDL_DestroyTexture(kv.second.tex);
    cache_.clear();
    lru_.clear();
    fitCache_.clear();
}
