// Отрисовка через SDL2: системный шрифт Свича (кириллица + значки кнопок), кэш текста, фигуры.
#pragma once
#include <list>
#include <map>
#include <string>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

// Палитра меняется настройками (тема и акцент), поэтому это переменные, а не константы.
// Имена исторические: white — основной текст, gray — второстепенный, yellow — акцент,
// black — текст и значки поверх акцента.
namespace col {
extern SDL_Color bg, panel, white, gray, dim, yellow, red, black;

enum Theme { ThemeDark, ThemeBlack, ThemeLight, THEME_COUNT };
struct Accent { const char *name; SDL_Color c; };
extern const Accent ACCENTS[];
extern const int ACCENT_COUNT;
extern const char *const THEME_NAMES[];

// accent == ACCENT_COUNT — свой цвет custom.
void apply(int theme, int accent, SDL_Color custom = {255, 219, 77, 255});
}

enum class Align { Left, Center, Right };

// Значки кнопок из шрифта Nintendo Extended.
namespace glyph {
constexpr const char *A = "";
constexpr const char *B = "";
constexpr const char *X = "";
constexpr const char *Y = "";
constexpr const char *L = "";
constexpr const char *R = "";
constexpr const char *ZL = "";
constexpr const char *ZR = "";
constexpr const char *Plus = "";
constexpr const char *Minus = "";
}

class Ui {
public:
    static constexpr int W = 1280, H = 720;

    bool init();
    void shutdown();
    void begin();   // очистить кадр
    void present(); // показать кадр

    // Текст; maxW > 0 — обрезать с «…». Возвращает ширину нарисованного.
    int text(const std::string &s, int x, int y, int size, SDL_Color c, Align a = Align::Left, int maxW = 0,
             bool ext = false);
    int textWidth(const std::string &s, int size, bool ext = false);

    void rect(int x, int y, int w, int h, SDL_Color c);
    void roundRect(int x, int y, int w, int h, int r, SDL_Color c);
    void circle(int cx, int cy, int r, SDL_Color c);
    void triangle(float x1, float y1, float x2, float y2, float x3, float y3, SDL_Color c);
    void heart(int cx, int cy, int size, SDL_Color c);
    void line(float x1, float y1, float x2, float y2, float width, SDL_Color c); // толстая линия
    void downArrow(int cx, int cy, int size, SDL_Color c); // значок «скачано»
    void image(SDL_Texture *t, int x, int y, int w, int h);

    // Множитель размера всего текста (настройка «Размер текста»).
    void setTextScale(float s);
    SDL_Texture *textureFrom(SDL_Surface *s); // забирает surface
    SDL_Renderer *renderer() const { return r_; }

private:
    TTF_Font *font(int size, bool ext);
    std::string fit(const std::string &s, int size, int maxW, bool ext);

    SDL_Window *win_ = nullptr;
    SDL_Renderer *r_ = nullptr;
    const void *stdData_ = nullptr, *extData_ = nullptr;
    size_t stdSize_ = 0, extSize_ = 0;
    std::map<int, TTF_Font *> fonts_, extFonts_;

    struct Cached { SDL_Texture *tex; int w, h; std::list<std::string>::iterator lru; };
    std::map<std::string, Cached> cache_;
    std::map<std::string, std::string> fitCache_;
    std::list<std::string> lru_;
    float textScale_ = 1.0f;
};
