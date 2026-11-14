// YaMu-nx — клиент Яндекс Музыки для Nintendo Switch (Horizon OS).
// v0.6: скачивание для офлайна, старт без автовоспроизведения, библиотека, лайки, интерфейс на SDL2.
#include <algorithm>
#include <arpa/inet.h>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>
#include <switch.h>
#include <curl/curl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <SDL2/SDL_image.h>
#include "json.hpp"
#include "connect.hpp"
#include "crashlog.hpp"
#include "soundcloud.hpp"
#include "http.hpp"
#include "player.hpp"
#include "ui.hpp"
#include "worker.hpp"
#include "sclogin.hpp"

static const char *VERSION = "0.2.0"; // локальные сборки для отладки — "developer"
static const char *TOKEN_PATH = "sdmc:/switch/yamu-nx/token.txt";
static const u64 AUTO_SKIP_MS = 2500; // пауза перед пропуском неиграющего трека
static const int COVER_BIG = 400, COVER_SMALL = 100;
static const int UP_NEXT_ROWS = 3;
static const size_t COVER_CACHE_MAX = 120;

// Списки: геометрия строк
static const int LIST_TOP = 150, LIST_BOTTOM = Ui::H - 70;
// Экран плеера: правая колонка и полоса прогресса
static const int PLAYER_RX = 530, PLAYER_RW = Ui::W - PLAYER_RX - 70, BAR_Y = 250;
static const int LIB_ROW_H = 72, LIST_ROW_H = 64;

struct Button { SDL_Rect rc; int action; };
enum Action {
    ActNone, ActPrev, ActPlay, ActNext, ActNewWave, ActRetry, ActLike, ActLibrary, ActBack, ActShuffle,
    ActDownload, ActDownloadAll, ActDelete, ActSeekBar, ActSearch, ActActivateSel, ActToList, ActPlayer, ActExit, ActShuffleMode, ActRepeat, ActHome, ActSettings, ActQueue, ActSeekBack, ActSeekFwd, ActLyrics, ActLoginNew, ActLoginCancel,
    ActOpenAlbum,         // в плеере: тап по названию — альбом трека
    ActTabYa, ActTabSc,   // главная: вкладка «Яндекс» / «SoundCloud»
    ActArtistBase = 450,  // в плеере: тап по исполнителю (+его номер в треке)
    ActHomeBase = 500,    // плитка главного экрана (+индекс)
    ActSettingBase = 600, // строка настроек (+индекс)
    ActRowBase = 100,  // строка «Дальше» (+1..+3)
    ActItemBase = 1000, // строка списка (+индекс)
    ActLyricBase = 100000, // строка текста песни (+индекс) — перемотка к ней
    ActKbdBase = 200000,   // клавиша экранной клавиатуры поиска (+индекс)
    ActRichBase = 300000   // элемент экрана поиска / исполнителя (+индекс)
};
enum Screen { ScrHome, ScrPlayer, ScrLibrary, ScrList, ScrSettings, ScrQueue, ScrLyrics, ScrTiles, ScrHomeSet, ScrRich };
enum Source { SrcWave, SrcList };

struct CoverTex { SDL_Texture *tex; u64 used; SDL_Color avg; }; // avg — средний цвет (для фона плеера)

// Средний цвет картинки — считаем один раз при загрузке обложки.
static SDL_Color avgColor(SDL_Surface *s) {
    SDL_Color c{40, 40, 44, 255};
    if (!s) return c;
    SDL_Surface *rgba = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_RGBA32, 0);
    if (!rgba) return c;
    u64 sum[3] = {0, 0, 0}, n = 0;
    const u8 *px = (const u8 *)rgba->pixels;
    for (int y = 0; y < rgba->h; y += 2)
        for (int x = 0; x < rgba->w; x += 2, n++)
            for (int k = 0; k < 3; k++) sum[k] += px[y * rgba->pitch + x * 4 + k];
    SDL_FreeSurface(rgba);
    if (n) c = {(u8)(sum[0] / n), (u8)(sum[1] / n), (u8)(sum[2] / n), 255};
    return c;
}

struct ListView {
    int sel = 0, scroll = 0;
    void clamp(int count, int visible) {
        if (count <= 0) { sel = scroll = 0; return; }
        sel = std::max(0, std::min(sel, count - 1));
        if (sel < scroll) scroll = sel;
        if (sel >= scroll + visible) scroll = sel - visible + 1;
        scroll = std::max(0, std::min(scroll, std::max(0, count - visible)));
    }
};

struct App {
    Ui ui;
    Player player;
    Worker worker;
    Screen screen = ScrPlayer;
    u64 frame = 0;

    bool started = false, ready = false, waveLoading = false;
    std::string login, startupError;
    bool hasPlus = false;

    // Что играет
    Source source = SrcWave;
    std::string sourceTitle = "Моя волна";
    std::vector<Track> queue;
    int current = -1;
    int pendingIndex = -1; // ждём подгрузку волны, чтобы включить этот трек
    unsigned gen = 0;      // поколение запроса ссылки: старые ответы игнорируем
    bool resolving = false;
    int bitrate = 0;
    std::string codec, error;
    u64 skipAt = 0;

    // Библиотека
    std::vector<Playlist> playlists;
    std::set<std::string> liked;
    bool libLoaded = false, libLoading = false;
    std::string libError;
    ListView libView;

    // Открытый плейлист
    Playlist openList;
    // SoundCloud: вход в аккаунт с телефона, имя вошедшего, волна по похожим
    ScLoginServer scLogin;
    std::string scVerifier, scUser;
    bool scWave = false, scWaveLoading = false;
    u64 scLogoutAt = 0; // первое нажатие «выйти» — ждём подтверждения
    std::vector<Track> listTracks;
    bool listLoading = false;
    bool listMore = false;   // «Мне нравится» ещё догружается порциями
    unsigned listGen = 0;    // какое открытие списка сейчас на экране (старые порции отбрасываем)
    std::string listError;
    ListView listView;

    std::string toast;
    u64 toastUntil = 0;

    // Офлайн: скачанные треки и очередь скачивания
    struct OfflineEntry { Track track; std::string file, cover; };
    std::vector<OfflineEntry> offline;
    bool offlineMode = false; // нет сети — работаем только со скачанным
    Worker downloader;        // отдельный поток, чтобы скачивание не мешало слушать
    Worker coverWorker;       // и для обложек: поиск и ссылки на треки не ждут картинки
    std::set<std::string> dlQueued;
    int dlTotal = 0, dlDone = 0, dlFailed = 0;

    int touchX = 0; // где коснулись — для перемотки тапом по полосе
    double scrubFrac = -1; // ведём пальцем по полосе прогресса: куда перемотаем (доля трека), -1 — не ведём
    SDL_Rect seekBar{0, 0, 0, 0}; // полоса прогресса на текущем экране (w = 0 — её нет)
    SDL_Rect volBar{0, 0, 0, 0};  // ползунок громкости в плеере
    bool volDrag = false;         // ведём пальцем по ползунку громкости
    std::string lastQuery; // последний поисковый запрос (подставляется в клавиатуру)
    // Своя экранная клавиатура поиска: у системной кириллица есть, только если консоль на русском.
    // sc — искать в SoundCloud (в нём же ссылки soundcloud.com открываются напрямую).
    struct Kbd { bool active = false, ru = true, sc = false; int row = 1, col = 0; std::string text; } kbd;

    // Передача музыки фоновому модулю и обратно
    struct Takeover { bool wave = false; std::string source; std::vector<Track> tracks; int current = -1; double pos = 0; bool paused = false; int repeat = 0; };
    bool hasTakeover = false; // забрали музыку у модуля, ждём входа в аккаунт
    Takeover takeover;
    double pendingSeek = -1;  // перемотать, как только трек начнёт играть
    bool pendingPause = false; // и/или сразу поставить на паузу
    bool handedOff = false;   // музыка сейчас у модуля
    u64 focusGraceUntil = 0;  // после системной клавиатуры смена фокуса — не уход из приложения
    bool quit = false;        // выход по тапу на «Выход»
    bool moduleAlive = false; // фоновый модуль отвечает — тогда заранее готовим ему треки

    // Шафл и повтор
    enum Repeat { RepeatOff, RepeatAll, RepeatOne };
    bool shuffle = false;
    Repeat repeat = RepeatOff;
    std::vector<Track> unshuffled; // исходный порядок, чтобы вернуть его при выключении шафла

    // Настройки (switch/yamu-nx/settings.json)
    int seekStep = 10;     // шаг перемотки, секунд
    bool seekOnLR = false; // перемотка на L/R, треки — на крестовине
    int volume = 100;      // громкость приложения, %
    bool saveResume = true; // последний трек и место в нём (last.json) — отдельно от автосохранения
    // Автосохранение: общий выключатель и что именно сохранять
    bool autoSave = true;
    bool saveLyrics = true; // открытые тексты песен — на карту, чтобы читать без сети
    bool saveLikes = false; // лайкнул — трек сам скачивается в «Скачанное»
    bool wantResume() const { return saveResume; }
    int crossfade = 0;      // плавный переход между треками, секунд (0 — выкл)
    std::string xfId, xfUrl, xfCodec; // следующий трек, заранее подготовленный для перехода
    std::string scWarmId;             // следующий трек SoundCloud, для которого уже готовы адрес и плейлист
    u64 scWarmAt = 0;
    int xfBitrate = 0;

    // Ynison («Яндекс Коннект»): Свич как устройство в Яндекс Музыке
    Connect connect;
    bool connectOn = true;       // настройка
    std::string deviceId;        // наш постоянный id устройства (в settings.json)
    std::string ynActiveId;      // какое устройство сейчас играет
    bool ynActive = false;       // играем мы — шлём своё состояние
    std::string ynLastTrack;     // что последним отправили: трек, пауза, позиция
    bool ynLastPaused = true;
    double ynLastPos = 0;
    u64 ynLastSent = 0;
    bool ynRemote = false;       // сейчас выполняем команду с телефона — не перехватываем управление
    struct YnPending { std::string id; double pos = 0; bool paused = false; } ynPending; // ждём очередь с телефона

    // Вход по коду: показываем код, ждём, пока его введут на ya.ru/device
    struct LoginFlow {
        bool active = false, waiting = false, polling = false;
        std::string userCode, url, deviceCode, error;
        int interval = 5;
        u64 nextPoll = 0, expiresAt = 0;
    } loginFlow;

    // Главный экран, настройки, очередь
    int homeSel = 0;
    int homeTab = 0;         // вкладка главной: 0 — Яндекс Музыка, 1 — SoundCloud
    bool sideFocus = false;  // выбор в боковом меню главной (а не на плитках)
    int sideSel = 0;         // пункт меню: 0 — Яндекс, 1 — SoundCloud, 2 — Настройки
    bool listAutoPlay = false; // открытый список сразу играть, когда загрузится (волна SoundCloud с главной)
    ListView setView, queueView;
    // Оформление и главная (settings.json)
    int accent = 0, theme = 0; // индексы в col::ACCENTS (ACCENT_COUNT — свой цвет) и col::Theme
    int accentR = 255, accentG = 219, accentB = 77; // свой цвет акцента
    long long settingsMtime = 0; // когда settings.json последний раз писали мы (иначе — оверлей)
    int playerBg = 0;          // 0 — обычный, 1 — размытая обложка, 2 — цвет обложки
    bool bigText = false;
    std::vector<int> tileOrder{0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    std::set<int> tileHidden{6, 7, 8, 9};
    ListView tilesView;
    // Главный экран
    int homeLayout = 0;      // всегда 0 (сетка); поле осталось ради старых файлов настроек
    int bottomRow = 0;       // ряд снизу: 0 — нет, 1 — недавно играло, 2 — мои плейлисты
    bool greeting = true;    // «Добрый вечер, логин» вместо «Главная»
    bool showClock = true;   // часы и заряд внизу справа
    bool tileCompact = false;
    int tileStyle = 0;       // 0 — тёмные, 1 — цвет акцента, 2 — контур
    bool squareTiles = false;
    ListView homeSetView;
    std::vector<Track> recent; // недавно играло (recent.json), свежие первыми
    struct HomeItem { int kind, id; SDL_Rect rc; }; // kind: 0 — плитка, 1 — недавний трек, 2 — плейлист, 3 — закладка
    std::vector<HomeItem> homeItems; // что на главной сейчас и где (для навигации и касаний)
    int rowScroll = 0, rowScroll2 = 0;
    std::string waveMood, waveDiversity; // настроение для следующей новой волны
    bool waveTuned = false;               // включали станцию — обычная волна вернёт «любое»
    // Ещё настройки главной
    int columns = 3;         // столбцов в сетке
    bool subtitles = true;   // вторая строка на плитках
    bool nowCover = false;   // «Сейчас играет»: обложка во всю плитку
    bool anim = true;        // анимации выбора и эквалайзер
    int homeBg = 0;          // фон главной: 0 — обычный, 1 — размытая обложка, 2 — цвет обложки
    int bottomRow2 = 0;      // второй ряд снизу (как bottomRow)
    std::string customName;  // имя в приветствии (пусто — логин)
    bool showDate = false;   // дата под приветствием
    std::vector<Playlist> bookmarks; // закреплённые на главной плейлисты (bookmarks.json)

    // Поиск как в Яндекс Музыке, страницы исполнителя и «Мои альбомы» — стек страниц (назад — на предыдущую)
    struct RichSection {
        std::string title;
        std::vector<Track> tracks; // треки строками
        std::vector<Card> cards;   // или карточки (исполнители, альбомы, плейлисты)
        bool grid = false;         // карточки сеткой (иначе — рядом с прокруткой вбок)
        bool allTracks = false;    // строка «Все треки ›» под треками
        float sx = 0;              // прокрутка ряда карточек вбок, пикселей (свайпом или за выбором)
        int followed = -1;         // за каким выбором уже прокрутили ряд (дальше можно листать пальцем)
    };
    struct RichPage {
        int type = 0; // 0 — поиск, 1 — исполнитель, 2 — мои альбомы
        std::string title, query, artistId, error;
        bool loading = true;
        std::vector<RichSection> sections;
        int sel = 0, shownSel = -1; // shownSel — к какому выбору уже прокрутили (листать пальцем можно дальше)
        float scroll = 0;
    };
    std::vector<RichPage> rich;
    struct RichItem { int section, index; SDL_Rect rc; }; // index -1 — «Все треки»
    std::vector<RichItem> richItems;
    struct RichRow { int section; SDL_Rect rc; }; // ряды карточек (y — в координатах содержимого) для свайпа вбок
    std::vector<RichRow> richRows;
    int richOff = 0;      // сдвиг содержимого страницы на экране (RICH_TOP - прокрутка)
    int richHDrag = -1;   // какой ряд сейчас тянут пальцем вбок
    bool listFromRich = false;   // список открыт со страницы поиска — назад туда
    bool playerFromRich = false; // трек включён со страницы поиска
    bool listFromHome = false; // список открыт с главной — B вернёт туда
    bool richFromPlayer = false; // страница исполнителя открыта из плеера — B вернёт туда
    bool listFromPlayer = false; // альбом открыт из плеера
    int metaWant = -1;           // ждём данные трека: -2 — открыть альбом, 0.. — исполнителя с этим номером

    // Текст песни текущего трека
    struct LyricLine { double t; std::string s; }; // t < 0 — без таймкода
    struct LyricRow { int line; std::string s; };  // строка после переноса по ширине
    std::string lyricsId;  // для какого трека загружен (или грузится) текст
    bool lyricsLoading = false, lyricsSynced = false;
    std::string lyricsError;
    std::vector<LyricLine> lyrics;
    std::vector<LyricRow> lyricRows; // пусто — ещё не разложили по ширине
    float lyricsY = 0;            // прокрутка, пикселей
    u64 lyricsManualUntil = 0;     // листали сами — пока не следуем за песней

    std::map<std::string, CoverTex> covers;
    std::set<std::string> coverPending;
    std::vector<Button> buttons;
};

static u64 nowMs() { return armTicksToNs(armGetSystemTick()) / 1000000ULL; }

static std::string fmtTime(double sec) {
    int s = sec < 0 ? 0 : (int)sec;
    char b[16];
    snprintf(b, sizeof(b), "%d:%02d", s / 60, s % 60);
    return b;
}

static std::string tracksWord(int n) {
    int m10 = n % 10, m100 = n % 100;
    const char *w = (m10 == 1 && m100 != 11) ? "трек" : (m10 >= 2 && m10 <= 4 && (m100 < 12 || m100 > 14)) ? "трека" : "треков";
    return std::to_string(n) + " " + w;
}

static void showToast(App &app, const std::string &s) {
    app.toast = s;
    app.toastUntil = nowMs() + 2000;
}

// ---------- скачанное ----------

static const char *OFFLINE_KIND = "@offline"; // «плейлист» скачанного
static const char *SEARCH_KIND = "@search";   // «плейлист» результатов поиска

static std::string jsonEsc(const std::string &s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if ((unsigned char)c < 0x20) o += ' ';
        else o += c;
    }
    return o;
}

static App::OfflineEntry *findOffline(App &app, const std::string &id) {
    for (auto &e : app.offline)
        if (e.track.id == id) return &e;
    return nullptr;
}

static void saveOffline(App &app) {
    std::string path = std::string(OFFLINE_DIR) + "/index.json";
    FILE *f = fopen((path + ".tmp").c_str(), "w");
    if (!f) return;
    fputs("[", f);
    for (size_t i = 0; i < app.offline.size(); i++) {
        const auto &e = app.offline[i];
        fprintf(f, "%s{\"id\":\"%s\",\"album\":\"%s\",\"title\":\"%s\",\"artists\":\"%s\",\"dur\":%lld,"
                   "\"coverUri\":\"%s\",\"file\":\"%s\",\"cover\":\"%s\"}",
                i ? ",\n" : "", jsonEsc(e.track.id).c_str(), jsonEsc(e.track.albumId).c_str(),
                jsonEsc(e.track.title).c_str(), jsonEsc(e.track.artists).c_str(), e.track.durationMs,
                jsonEsc(e.track.coverUri).c_str(), jsonEsc(e.file).c_str(), jsonEsc(e.cover).c_str());
    }
    fputs("]\n", f);
    fclose(f);
    remove(path.c_str());
    rename((path + ".tmp").c_str(), path.c_str());
}

// «Недавно играло»: последние треки (свежие первыми), на карте — чтобы помнить между запусками.
static const char *RECENT_PATH = "sdmc:/switch/yamu-nx/recent.json";
static const size_t RECENT_MAX = 15;

static void saveRecent(App &app) {
    FILE *f = fopen(RECENT_PATH, "w");
    if (!f) return;
    fputs("[", f);
    for (size_t i = 0; i < app.recent.size(); i++) {
        const Track &t = app.recent[i];
        fprintf(f, "%s{\"id\":\"%s\",\"album\":\"%s\",\"title\":\"%s\",\"artists\":\"%s\",\"dur\":%lld,\"coverUri\":\"%s\"}",
                i ? ",\n" : "", jsonEsc(t.id).c_str(), jsonEsc(t.albumId).c_str(), jsonEsc(t.title).c_str(),
                jsonEsc(t.artists).c_str(), t.durationMs, jsonEsc(t.coverUri).c_str());
    }
    fputs("]\n", f);
    fclose(f);
}

static void loadRecent(App &app) {
    FILE *f = fopen(RECENT_PATH, "r");
    if (!f) return;
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    fclose(f);
    Json arr = Json::parse(text);
    for (size_t i = 0; i < arr.size() && app.recent.size() < RECENT_MAX; i++) {
        Track t;
        t.id = arr[i]["id"].asString();
        t.albumId = arr[i]["album"].asString();
        t.title = arr[i]["title"].asString();
        t.artists = arr[i]["artists"].asString();
        t.durationMs = arr[i]["dur"].asInt();
        t.coverUri = arr[i]["coverUri"].asString();
        if (!t.id.empty()) app.recent.push_back(t);
    }
}

static void addRecent(App &app, const Track &t) {
    if (!app.recent.empty() && app.recent[0].id == t.id) return;
    app.recent.erase(std::remove_if(app.recent.begin(), app.recent.end(), [&](const Track &r) { return r.id == t.id; }),
                     app.recent.end());
    app.recent.insert(app.recent.begin(), t);
    if (app.recent.size() > RECENT_MAX) app.recent.resize(RECENT_MAX);
    saveRecent(app);
}

static void loadOffline(App &app) {
    mkdir("sdmc:/switch/yamu-nx", 0777);
    mkdir(OFFLINE_DIR, 0777);
    // Недокачанные файлы от прошлого запуска не нужны.
    if (DIR *d = opendir(OFFLINE_DIR)) {
        while (dirent *e = readdir(d)) {
            std::string n = e->d_name;
            if (n.size() > 5 && n.compare(n.size() - 5, 5, ".part") == 0) remove((std::string(OFFLINE_DIR) + "/" + n).c_str());
        }
        closedir(d);
    }
    FILE *f = fopen((std::string(OFFLINE_DIR) + "/index.json").c_str(), "r");
    if (!f) return;
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    fclose(f);
    Json arr = Json::parse(text);
    for (size_t i = 0; i < arr.size(); i++) {
        const Json &j = arr[i];
        App::OfflineEntry e;
        e.track.id = j["id"].asString();
        e.track.albumId = j["album"].asString();
        e.track.title = j["title"].asString();
        e.track.artists = j["artists"].asString();
        e.track.durationMs = j["dur"].asInt();
        e.track.coverUri = j["coverUri"].asString();
        e.file = j["file"].asString();
        e.cover = j["cover"].asString();
        struct stat st;
        if (!e.track.id.empty() && stat(e.file.c_str(), &st) == 0) app.offline.push_back(e); // файл мог пропасть
    }
}

static std::vector<Track> offlineTracks(App &app) {
    std::vector<Track> v;
    for (auto it = app.offline.rbegin(); it != app.offline.rend(); ++it) v.push_back(it->track); // свежие сверху
    return v;
}

static void downloadTracks(App &app, const std::vector<Track> &tracks) {
    if (app.offlineMode) { showToast(app, "Нет сети — скачать не получится"); return; }
    int added = 0;
    for (const Track &t : tracks) {
        if (findOffline(app, t.id) || app.dlQueued.count(t.id)) continue;
        app.dlQueued.insert(t.id);
        Job j{Job::Download};
        j.track = t;
        app.downloader.post(j);
        added++;
    }
    if (!added) {
        showToast(app, tracks.size() == 1 ? "Уже скачано" : "Всё уже скачано");
        return;
    }
    if (app.dlDone >= app.dlTotal) app.dlTotal = app.dlDone = app.dlFailed = 0; // новая пачка
    app.dlTotal += added;
    showToast(app, added == 1 ? "Скачиваю трек" : "Скачиваю " + tracksWord(added));
}

static void deleteOffline(App &app, const std::string &id) {
    for (auto it = app.offline.begin(); it != app.offline.end(); ++it) {
        if (it->track.id != id) continue;
        remove(it->file.c_str());
        if (!it->cover.empty()) remove(it->cover.c_str());
        app.offline.erase(it);
        saveOffline(app);
        showToast(app, "Удалено с карты");
        return;
    }
}

// Обложка: скачанная с карты, если есть, иначе из сети.
static std::string coverFor(App &app, const Track &t, int size) {
    if (App::OfflineEntry *e = findOffline(app, t.id))
        if (!e->cover.empty()) return e->cover;
    return app.offlineMode ? std::string() : t.coverUrl(size);
}

// ---------- логика ----------

static void applyPending(App &app);
static void prepareInModule(App &app, const Track &t);

static void requestCover(App &app, const std::string &url) {
    if (url.empty() || app.covers.count(url) || app.coverPending.count(url)) return;
    if (url.compare(0, 5, "sdmc:") == 0) { // с карты грузим сразу, это быстро
        SDL_Surface *s = IMG_Load(url.c_str());
        SDL_Color avg = avgColor(s);
        app.covers[url] = {s ? app.ui.textureFrom(s) : nullptr, app.frame, avg};
        return;
    }
    app.coverPending.insert(url);
    Job j{Job::Cover};
    j.url = url;
    app.coverWorker.post(j);
}

static void feedback(App &app, const char *type, const Track *t = nullptr, double played = 0) {
    if (app.source != SrcWave) return; // отметки нужны только волне
    Job j{Job::Feedback};
    j.feedbackType = type;
    if (t) j.track = *t;
    j.played = played;
    app.worker.post(j);
}

static void loadMoreWave(App &app, bool fresh) {
    if (app.waveLoading) return;
    app.waveLoading = true;
    Job j{Job::LoadWave};
    j.fresh = fresh;
    if (!fresh && !app.queue.empty()) j.lastTrackId = app.queue.back().id;
    if (fresh) { j.mood = app.waveMood; j.diversity = app.waveDiversity; }
    app.worker.post(j);
}

// Волна SoundCloud: очередь подходит к концу — дописываем похожие на последний трек.
static void loadMoreScWave(App &app) {
    if (!app.scWave || app.scWaveLoading || app.queue.empty()) return;
    app.scWaveLoading = true;
    Job j{Job::ScRelated};
    j.track = app.queue.back();
    app.worker.post(j);
}

static void playIndex(App &app, int idx) {
    if (idx >= 0 && idx < (int)app.queue.size()) crashLog("трек %s", app.queue[idx].id.c_str());
    app.player.stop();
    app.skipAt = 0;
    app.error.clear();
    app.bitrate = 0;
    app.codec.clear();
    if (idx < 0) idx = 0;
    if (app.source == SrcWave && idx >= (int)app.queue.size() - UP_NEXT_ROWS - 1) loadMoreWave(app, false);
    if (app.scWave && idx >= (int)app.queue.size() - UP_NEXT_ROWS - 1) loadMoreScWave(app);
    if (idx >= (int)app.queue.size() && app.source == SrcList && app.repeat == App::RepeatAll && !app.queue.empty())
        idx = 0; // повтор всего плейлиста — по кругу
    if (idx >= (int)app.queue.size()) {
        if (app.source == SrcWave) app.pendingIndex = idx;
        else { app.resolving = false; showToast(app, "Плейлист закончился"); }
        return;
    }

    app.current = idx;
    addRecent(app, app.queue[idx]);
    app.pendingIndex = -1;
    app.gen++;
    app.worker.cancel(Job::Resolve);
    // Скачанный трек играем с карты — без сети и без запроса ссылки.
    if (App::OfflineEntry *e = findOffline(app, app.queue[idx].id)) {
        app.resolving = false;
        app.codec = "с карты";
        app.player.play(e->file);
        applyPending(app);
        prepareInModule(app, app.queue[idx]);
        feedback(app, "trackStarted", &app.queue[idx]);
        return;
    }
    if (app.offlineMode) { // без сети нескачанное не сыграть
        app.error = "Нет сети, а трек не скачан";
        app.skipAt = nowMs() + AUTO_SKIP_MS;
        return;
    }
    app.resolving = true;
    Job j{Job::Resolve};
    j.index = idx;
    j.gen = app.gen;
    j.track = app.queue[idx];
    app.worker.post(j);
}

// SoundCloud: пока играет трек, заранее узнаём адрес следующего и качаем его плейлист — переключение
// без ~1 с запросов через прокси. Подписи адресов живут недолго, поэтому раз в 2 минуты освежаем.
static void warmNextSc(App &app) {
    if (app.offlineMode || app.handedOff || app.resolving || app.player.state() != Player::Playing) return;
    if (app.current < 0 || app.current >= (int)app.queue.size()) return;
    int next = app.current + 1;
    if (next >= (int)app.queue.size()) {
        if (app.source == SrcList && app.repeat == App::RepeatAll && !app.queue.empty()) next = 0;
        else return;
    }
    const Track &nt = app.queue[next];
    if (!isSoundCloudId(nt.id) || findOffline(app, nt.id)) return;
    if (app.scWarmId == nt.id && nowMs() - app.scWarmAt < 120000) return;
    if (app.scWarmId != nt.id && app.player.position() < 3) return; // только что включили — может, листают дальше
    app.scWarmId = nt.id;
    app.scWarmAt = nowMs();
    Job j{Job::ScWarm};
    j.track = nt;
    app.worker.post(j);
}

// Плавный переход: за ~20 с до конца готовим ссылку на следующий трек, за crossfade секунд — запускаем его
// поверх текущего. Не вышло подготовить вовремя — обычная смена трека по окончании.
static void maybeCrossfade(App &app) {
    if (!app.crossfade || app.repeat == App::RepeatOne || app.resolving || app.handedOff) return;
    if (app.current < 0 || app.current >= (int)app.queue.size()) return;
    if (app.player.state() != Player::Playing) return;
    int next = app.current + 1;
    if (next >= (int)app.queue.size()) {
        if (app.source == SrcList && app.repeat == App::RepeatAll && !app.queue.empty()) next = 0;
        else return;
    }
    const Track &cur = app.queue[app.current], &nt = app.queue[next];
    double dur = cur.durationMs / 1000.0, pos = app.player.position();
    if (dur < app.crossfade * 2 + 10) return; // слишком короткий трек

    if (app.xfId != nt.id) {
        if (pos < dur - app.crossfade - 20) return;
        app.xfId = nt.id;
        app.xfUrl.clear();
        if (App::OfflineEntry *e = findOffline(app, nt.id)) {
            app.xfUrl = e->file;
            app.xfCodec = "с карты";
            app.xfBitrate = 0;
        } else if (!app.offlineMode) {
            Job j{Job::Resolve};
            j.index = next;
            j.gen = app.gen;
            j.track = nt;
            j.preload = true;
            app.worker.post(j);
        }
        return;
    }
    if (app.xfUrl.empty() || pos < dur - app.crossfade) return;

    feedback(app, "trackFinished", &cur, pos);
    std::string url = app.xfUrl;
    app.xfUrl.clear();
    app.current = next;
    addRecent(app, app.queue[next]);
    app.gen++;
    app.skipAt = 0;
    app.error.clear();
    app.bitrate = app.xfBitrate;
    app.codec = app.xfCodec;
    app.pendingSeek = -1;
    const Track &t = app.queue[next];
    app.player.crossfade(url, app.xfBitrate ? (size_t)(t.durationMs / 1000.0 * app.xfBitrate * 1000 / 8) : 0, app.crossfade);
    prepareInModule(app, t);
    feedback(app, "trackStarted", &t);
    if (app.source == SrcWave && next >= (int)app.queue.size() - UP_NEXT_ROWS - 1) loadMoreWave(app, false);
    if (app.scWave && next >= (int)app.queue.size() - UP_NEXT_ROWS - 1) loadMoreScWave(app);
}

static void seekTo(App &app, double sec) {
    if (app.current < 0 || app.current >= (int)app.queue.size() || app.resolving) return;
    Player::State st = app.player.state();
    if (st == Player::Idle || st == Player::Error || st == Player::Finished) return;
    double dur = app.queue[app.current].durationMs / 1000.0;
    if (dur > 1 && sec > dur - 1) sec = dur - 1;
    app.player.seek(sec < 0 ? 0 : sec);
}

static void skipCurrent(App &app) {
    if (app.current >= 0 && app.current < (int)app.queue.size())
        feedback(app, "skip", &app.queue[app.current], app.player.position());
    playIndex(app, app.current + 1);
}

// Шафл: текущий трек на месте, всё после него — вперемешку; выключение возвращает исходный порядок.
static void toggleShuffle(App &app) {
    if (app.source == SrcWave) { showToast(app, "В «Моей волне» порядок задаёт волна"); return; }
    app.shuffle = !app.shuffle;
    if (app.current < 0 || app.current >= (int)app.queue.size()) {
        showToast(app, app.shuffle ? "Перемешивание включено" : "Перемешивание выключено");
        return;
    }
    std::string curId = app.queue[app.current].id;
    if (app.shuffle) {
        app.unshuffled = app.queue;
        std::mt19937 rng((unsigned)armGetSystemTick());
        std::shuffle(app.queue.begin() + app.current + 1, app.queue.end(), rng);
    } else if (!app.unshuffled.empty()) {
        app.queue = app.unshuffled;
        for (int i = 0; i < (int)app.queue.size(); i++)
            if (app.queue[i].id == curId) { app.current = i; break; }
    }
    showToast(app, app.shuffle ? "Перемешивание включено" : "Перемешивание выключено");
}

// Повтор по кругу: выкл → весь плейлист → один трек → выкл (в волне — только один трек).
static void toggleRepeat(App &app) {
    if (app.repeat == App::RepeatOff) app.repeat = app.source == SrcWave ? App::RepeatOne : App::RepeatAll;
    else if (app.repeat == App::RepeatAll) app.repeat = App::RepeatOne;
    else app.repeat = App::RepeatOff;
    showToast(app, app.repeat == App::RepeatOff ? "Повтор выключен"
                   : app.repeat == App::RepeatAll ? "Повтор плейлиста" : "Повтор трека");
}
// Волна: preset 0 — как настроена в аккаунте, 1 — бодрое, 2 — спокойное, 3 — незнакомое.
// Станции меняют настроение волны в аккаунте; обычная волна после станции возвращает «любое».
static void startWave(App &app, int preset = 0) {
    static const char *titles[] = {"Моя волна", "Волна: бодрое", "Волна: спокойное", "Волна: незнакомое"};
    static const char *moods[] = {"all", "active", "calm", "all"};
    static const char *divs[] = {"default", "default", "default", "discover"};
    preset = std::max(0, std::min(3, preset));
    if (preset == 0 && !app.waveTuned) { app.waveMood.clear(); app.waveDiversity.clear(); }
    else { app.waveMood = moods[preset]; app.waveDiversity = divs[preset]; }
    app.waveTuned = preset != 0;
    app.playerFromRich = false;
    app.player.stop();
    app.worker.cancel(Job::Resolve);
    app.source = SrcWave;
    app.sourceTitle = titles[preset];
    app.shuffle = false;
    if (app.repeat == App::RepeatAll) app.repeat = App::RepeatOff;
    app.scWave = false;
    app.queue.clear();
    app.current = -1;
    app.waveLoading = false;
    loadMoreWave(app, true);
}

static void playList(App &app, int idx, bool shuffle) {
    app.playerFromRich = false;
    if (app.listTracks.empty()) return;
    app.source = SrcList;
    app.sourceTitle = app.openList.title;
    app.queue = app.listTracks;
    app.scWave = app.openList.kind == "@sc" && app.openList.owner == "@wave"; // волна SoundCloud — дописывается сама
    app.unshuffled = app.listTracks;
    std::mt19937 rng((unsigned)armGetSystemTick());
    if (shuffle) { // «Перемешать» — весь список вперемешку и шафл включается
        app.shuffle = true;
        std::shuffle(app.queue.begin(), app.queue.end(), rng);
        idx = 0;
    } else if (app.shuffle && idx >= 0 && idx < (int)app.queue.size()) {
        // Шафл включён: выбранный трек первым, остальное вперемешку.
        std::swap(app.queue[0], app.queue[idx]);
        std::shuffle(app.queue.begin() + 1, app.queue.end(), rng);
        idx = 0;
    }
    app.current = -1;
    playIndex(app, idx);
    app.screen = ScrPlayer;
}

static void openLibrary(App &app) {
    app.screen = ScrLibrary;
    if (!app.libLoaded && !app.libLoading && !app.offlineMode) {
        app.libLoading = true;
        app.libError.clear();
        app.worker.post(Job{Job::Library});
    }
}

static void openList(App &app, const Playlist &pl) {
    app.listFromHome = false;
    app.listAutoPlay = false;
    app.listFromPlayer = false;
    app.listFromRich = false;
    app.openList = pl;
    app.listTracks.clear();
    app.listError.clear();
    app.listView = ListView();
    app.screen = ScrList;
    app.listGen++;
    app.listMore = false;
    app.worker.cancel(Job::ListMore); // порции прошлого открытия больше не нужны
    if (pl.kind == OFFLINE_KIND) { // скачанное — сразу с карты
        app.listLoading = false;
        app.listTracks = offlineTracks(app);
        if (app.listTracks.empty()) app.listError = "Пока ничего не скачано — нажмите ZL на треке или в плейлисте";
        return;
    }
    app.listLoading = true;
    if (pl.kind == SEARCH_KIND) {
        Job s{Job::Search};
        s.query = app.lastQuery;
        app.worker.post(s);
        return;
    }
    Job j{Job::OpenList};
    j.playlist = pl;
    j.gen = app.listGen;
    app.worker.post(j);
}

// ---------- своя клавиатура поиска (русская и английская раскладки) ----------

// Ряды букв; последний ряд — служебные клавиши (KEY_*), они общие для обеих раскладок.
static const char *const KBD_RU[] = {"1234567890", "йцукенгшщзхъ", "фывапролджэ", "ячсмитьбюё"};
static const char *const KBD_EN[] = {"1234567890", "qwertyuiop", "asdfghjkl_", "zxcvbnm-./"}; // «_» и «/» — для ссылок и ников
static const int KBD_LETTER_ROWS = 4;
enum KbdKey { KEY_LANG, KEY_SVC, KEY_SYS, KEY_SPACE, KEY_BACK, KEY_OK, KEY_COUNT };
static const char *const KBD_KEY_LABEL[] = {"", "", "Системная", "Пробел", "Стереть", "Найти"};
static const int KBD_KEY_UNITS[] = {2, 3, 3, 4, 3, 3}; // ширина в «клавишах»

// Буквы ряда как отдельные UTF-8 символы.
static std::vector<std::string> kbdRow(const App &app, int row) {
    std::vector<std::string> out;
    const char *s = (app.kbd.ru ? KBD_RU : KBD_EN)[row];
    while (*s) {
        int n = (*s & 0x80) == 0 ? 1 : (*s & 0xE0) == 0xC0 ? 2 : (*s & 0xF0) == 0xE0 ? 3 : 4;
        out.emplace_back(s, n);
        s += n;
    }
    return out;
}
static int kbdRowLen(const App &app, int row) {
    return row < KBD_LETTER_ROWS ? (int)kbdRow(app, row).size() : KEY_COUNT;
}

static void popUtf8(std::string &s) {
    while (!s.empty() && ((unsigned char)s.back() & 0xC0) == 0x80) s.pop_back();
    if (!s.empty()) s.pop_back();
}

static void openRichSearch(App &app, const std::string &q);
static void openScSearch(App &app, const std::string &q);
static void systemSearch(App &app);

static void kbdSubmit(App &app) {
    std::string q = app.kbd.text;
    while (!q.empty() && q.back() == ' ') q.pop_back();
    while (!q.empty() && q.front() == ' ') q.erase(q.begin());
    if (q.empty()) { showToast(app, "Введите, что искать"); return; }
    app.kbd.active = false;
    app.lastQuery = q;
    if (app.kbd.sc) openScSearch(app, q);
    else openRichSearch(app, q);
}

// Нажатие клавиши: row/col — как в раскладке (служебные — в последнем ряду).
static void kbdPress(App &app, int row, int col) {
    App::Kbd &k = app.kbd;
    if (row < KBD_LETTER_ROWS) {
        std::vector<std::string> keys = kbdRow(app, row);
        if (col >= 0 && col < (int)keys.size() && k.text.size() < 100) k.text += keys[col];
        return;
    }
    switch (col) {
        case KEY_LANG: k.ru = !k.ru; break;
        case KEY_SVC: k.sc = !k.sc; break;
        case KEY_SYS: systemSearch(app); break;
        case KEY_SPACE: if (!k.text.empty() && k.text.back() != ' ' && k.text.size() < 100) k.text += ' '; break;
        case KEY_BACK: popUtf8(k.text); break;
        case KEY_OK: kbdSubmit(app); break;
    }
}

// Переход между рядами разной длины: к клавише, ближайшей по горизонтали.
static void kbdMove(App &app, int dx, int dy) {
    App::Kbd &k = app.kbd;
    const int rows = KBD_LETTER_ROWS + 1;
    if (dy) {
        auto centre = [&](int row, int col) {
            if (row < KBD_LETTER_ROWS) return (col + 0.5f) / kbdRowLen(app, row);
            int units = 0, before = 0;
            for (int i = 0; i < KEY_COUNT; i++) { if (i < col) before += KBD_KEY_UNITS[i]; units += KBD_KEY_UNITS[i]; }
            return (before + KBD_KEY_UNITS[col] / 2.0f) / units;
        };
        float x = centre(k.row, k.col);
        k.row = (k.row + dy + rows) % rows;
        int best = 0;
        float bd = 2;
        for (int c = 0; c < kbdRowLen(app, k.row); c++) {
            float d = std::fabs(centre(k.row, c) - x);
            if (d < bd) { bd = d; best = c; }
        }
        k.col = best;
    }
    if (dx) {
        int n = kbdRowLen(app, k.row);
        k.col = (k.col + dx + n) % n;
    }
}

// Поиск: своя клавиатура с русской раскладкой, результаты — как в Яндекс Музыке.
static bool hasScTab();

static void openSearch(App &app) {
    if (app.offlineMode) { showToast(app, "Нет сети — поиск недоступен"); return; }
    app.kbd.active = true;
    app.kbd.text = app.lastQuery;
    if (app.screen == ScrHome && hasScTab()) app.kbd.sc = app.homeTab == 1; // с вкладки — поиск в её сервисе
    app.kbd.row = 1;
    app.kbd.col = 0;
}

// Системная клавиатура Свича (для других языков и набора с телефона/клавиатуры).
static void systemSearch(App &app) {
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0))) {
        showToast(app, "Клавиатура недоступна — запустите приложение через удержание R");
        return;
    }
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetHeaderText(&kbd, "Поиск в Яндекс Музыке");
    swkbdConfigSetGuideText(&kbd, "Трек или исполнитель");
    swkbdConfigSetOkButtonText(&kbd, "Найти");
    swkbdConfigSetInitialText(&kbd, app.kbd.text.c_str());
    swkbdConfigSetStringLenMax(&kbd, 100);
    char out[512] = {0};
    Result rc = swkbdShow(&kbd, out, sizeof(out));
    swkbdClose(&kbd);
    // Клавиатура — отдельный апплет поверх нас: система шлёт «потерян фокус / вернулся».
    // Это не HOME, музыку фону отдавать не нужно.
    app.focusGraceUntil = nowMs() + 2000;
    if (R_FAILED(rc)) return; // отмена — остаёмся в своей клавиатуре
    app.kbd.text = out;
    kbdSubmit(app);
}

// ---------- поиск, исполнители, альбомы ----------

// Новая страница поверх предыдущих (назад — на предыдущую).
static void pushRich(App &app, const App::RichPage &p) {
    app.rich.push_back(p);
    app.richItems.clear();
    app.richRows.clear();
    app.screen = ScrRich;
}

static void openRichSearch(App &app, const std::string &q) {
    app.rich.clear();
    app.richFromPlayer = false;
    App::RichPage p;
    p.type = 0;
    p.title = "Поиск: " + q;
    p.query = q;
    pushRich(app, p);
    Job j{Job::SearchAll};
    j.query = q;
    app.worker.post(j);
}

// SoundCloud: поиск по словам или ссылка soundcloud.com/… (трек, плейлист, автор, закрытая /s-…).
static void openScSearch(App &app, const std::string &q) {
    if (!SoundCloud::configured()) {
        showToast(app, "Нет ключей SoundCloud: файл switch/yamu-nx/soundcloud.txt");
        return;
    }
    app.rich.clear();
    app.richFromPlayer = false;
    bool link = q.find("soundcloud.com") != std::string::npos || q.find("snd.sc") != std::string::npos;
    App::RichPage p;
    p.type = link ? 4 : 3; // 3 — поиск SoundCloud, 4 — открытая ссылка
    p.title = link ? "SoundCloud: ссылка" : "SoundCloud: " + q;
    p.query = q;
    pushRich(app, p);
    Job j{link ? Job::ScResolve : Job::ScSearch};
    j.query = q;
    app.worker.post(j);
}

// Главная SoundCloud (плитка): «Моё» — лайки, волна, лента; ниже — мои плейлисты и свежее от подписок.
static void openScHome(App &app) {
    if (!SoundCloud::configured()) { showToast(app, "Нет ключей SoundCloud: файл switch/yamu-nx/soundcloud.txt"); return; }
    app.rich.clear();
    app.richFromPlayer = false;
    App::RichPage p;
    p.type = 5;
    p.title = app.scUser.empty() ? "SoundCloud" : "SoundCloud · " + app.scUser;
    if (app.scUser.empty()) {
        p.loading = false;
        p.error = "Войдите в аккаунт: Настройки → SoundCloud";
        pushRich(app, p);
        return;
    }
    App::RichSection mine;
    mine.title = "Моё";
    const std::pair<const char *, const char *> items[] = {{"@wave", "Волна"}, {"@likes", "Мне нравится"}, {"@feed", "Лента подписок"}};
    for (auto &it : items) {
        Card c;
        c.kind = Card::PlaylistCard;
        c.id = "@sc";
        c.owner = it.first;
        c.title = it.second;
        c.sub = "SoundCloud";
        mine.cards.push_back(c);
    }
    p.sections.push_back(mine);
    pushRich(app, p);
    app.worker.post(Job{Job::ScHome});
}

static void openArtist(App &app, const Card &c) {
    App::RichPage p;
    p.type = 1;
    p.title = c.title;
    p.artistId = c.id;
    pushRich(app, p);
    Job j{Job::Artist};
    j.artistId = c.id;
    app.worker.post(j);
}

static void openMyAlbums(App &app) {
    app.rich.clear();
    app.richFromPlayer = false;
    App::RichPage p;
    p.type = 2;
    p.title = "Мои альбомы";
    pushRich(app, p);
    app.worker.post(Job{Job::LikedAlbums});
}

// Карточка: исполнитель — его страница, альбом и плейлист — список треков.
static void openCard(App &app, const Card &c) {
    if (c.kind == Card::Artist) { openArtist(app, c); return; }
    Playlist pl;
    pl.kind = c.kind == Card::Album ? "@album" : c.id;
    pl.owner = c.kind == Card::Album ? c.id : c.owner;
    pl.title = c.title;
    pl.coverUri = c.coverUri;
    pl.trackCount = c.trackCount;
    bool fromRich = app.screen == ScrRich && !app.rich.empty();
    openList(app, pl);
    app.listFromRich = fromRich;
}

// Из плеера: страница исполнителя (k — номер исполнителя в треке) или альбом трека.
// У треков из старых записей (недавние, скачанное, восстановленная очередь) нет id исполнителей —
// тогда сначала спрашиваем трек у API (Job::TrackMeta), а переходим, когда ответ придёт.
static void openFromPlayer(App &app, const Track &t, int want) {
    if (app.offlineMode) { showToast(app, "Нет сети — страницы исполнителей и альбомов недоступны"); return; }
    if (want == -2 && isSoundCloud(t)) { showToast(app, "У треков SoundCloud нет альбомов"); return; }
    bool needMeta = want == -2 ? t.albumId.empty() : want >= (int)t.artistIds.size() || t.artistIds[want].empty();
    if (needMeta) {
        if (app.metaWant != -1) return; // уже спросили
        app.metaWant = want;
        Job j{Job::TrackMeta};
        j.track = t;
        app.worker.post(j);
        showToast(app, "Открываю…");
        return;
    }
    if (want == -2) {
        Card c;
        c.kind = Card::Album;
        c.id = t.albumId;
        c.title = t.albumTitle.empty() ? "Альбом" : t.albumTitle;
        c.coverUri = t.coverUri;
        openCard(app, c);
        app.listFromPlayer = true;
    } else {
        app.rich.clear();
        Card c;
        c.kind = Card::Artist;
        c.id = t.artistIds[want];
        c.title = want < (int)t.artistNames.size() ? t.artistNames[want] : t.artists;
        openArtist(app, c);
        app.richFromPlayer = true;
    }
}

static void activateRich(App &app, int idx) {
    if (app.rich.empty() || idx < 0 || idx >= (int)app.richItems.size()) return;
    App::RichPage &page = app.rich.back();
    const App::RichItem it = app.richItems[idx];
    page.sel = idx;
    if (it.section < 0 || it.section >= (int)page.sections.size()) return;
    const App::RichSection &sec = page.sections[it.section];
    if (it.index < 0) { // «Все треки» — полный список найденного
        app.lastQuery = page.query;
        Playlist p;
        p.kind = SEARCH_KIND;
        p.title = "Поиск: " + page.query;
        openList(app, p);
        app.listFromRich = true;
        return;
    }
    if (!sec.tracks.empty()) { // трек — играем треки этого блока с него
        app.scWave = false;
        app.queue = sec.tracks;
        app.unshuffled.clear();
        app.shuffle = false;
        app.source = SrcList;
        app.sourceTitle = page.title;
        app.openList = Playlist();
        playIndex(app, it.index);
        app.playerFromRich = true;
        app.screen = ScrPlayer;
        return;
    }
    if (it.index < (int)sec.cards.size()) openCard(app, sec.cards[it.index]);
}

static void toggleLike(App &app) {
    if (app.current < 0 || app.current >= (int)app.queue.size()) return;
    const Track &t = app.queue[app.current];
    if (isSoundCloud(t) && app.scUser.empty()) { showToast(app, "Для лайков войдите в SoundCloud: Настройки → SoundCloud"); return; }
    bool like = !app.liked.count(t.id);
    if (like) app.liked.insert(t.id); // сразу показываем, откатим при ошибке
    else app.liked.erase(t.id);
    Job j{Job::Like};
    j.track = t;
    j.like = like;
    app.worker.post(j);
    if (like) feedback(app, "like", &t, app.player.position());
}

// Элементы библиотеки: волна, «Мне нравится», «Скачанное», дальше плейлисты.
// Без сети остаётся только «Скачанное».
enum LibKind { LibSearch, LibWave, LibLikes, LibOffline, LibAlbums, LibPlaylist };
static const int LIB_FIXED = 5;

static int libraryCount(const App &app) { return app.offlineMode ? 1 : LIB_FIXED + (int)app.playlists.size(); }

static LibKind libKind(const App &app, int idx) {
    if (app.offlineMode) return LibOffline;
    return idx == 0 ? LibSearch : idx == 1 ? LibWave : idx == 2 ? LibLikes : idx == 3 ? LibOffline : idx == 4 ? LibAlbums : LibPlaylist;
}

static Playlist offlinePlaylist(const App &app) {
    Playlist p;
    p.kind = OFFLINE_KIND;
    p.title = "Скачанное";
    p.trackCount = (int)app.offline.size();
    return p;
}

static void openSearch(App &app);

static void activateLibrary(App &app, int idx) {
    switch (libKind(app, idx)) {
        case LibSearch: openSearch(app); break;
        case LibWave: startWave(app); app.screen = ScrPlayer; break;
        case LibLikes: {
            Playlist likes;
            likes.title = "Мне нравится";
            likes.trackCount = (int)app.liked.size();
            openList(app, likes);
            break;
        }
        case LibAlbums: openMyAlbums(app); break;
        case LibOffline: openList(app, offlinePlaylist(app)); break;
        case LibPlaylist:
            if (idx - LIB_FIXED < (int)app.playlists.size()) openList(app, app.playlists[idx - LIB_FIXED]);
            break;
    }
}

// Y в списке плейлистов — закрепить на главной (или снять).
static void toggleBookmark(App &app, const Playlist &p);
static void bookmarkLibrary(App &app, int idx) {
    switch (libKind(app, idx)) {
        case LibLikes: {
            Playlist likes;
            likes.title = "Мне нравится";
            likes.trackCount = (int)app.liked.size();
            toggleBookmark(app, likes);
            break;
        }
        case LibOffline: toggleBookmark(app, offlinePlaylist(app)); break;
        case LibPlaylist:
            if (idx - LIB_FIXED < (int)app.playlists.size()) toggleBookmark(app, app.playlists[idx - LIB_FIXED]);
            break;
        default: showToast(app, "Закрепить можно плейлист"); break;
    }
}

// ---------- связь с фоновым модулем ----------

static const char *HANDOFF_PATH = "sdmc:/switch/yamu-nx/handoff.json";
static const u64 SYS_TID = 0x4200000000594D53ULL;

// Одна команда модулю — одна строка ответа; пусто, если модуль не ответил.
static std::string ymCall(const std::string &cmd, int timeoutMs = 500) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return "";
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(47123);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    connect(fd, (sockaddr *)&a, sizeof(a));
    pollfd p{fd, POLLOUT, 0};
    int soErr = 1;
    socklen_t len = sizeof(soErr);
    if (poll(&p, 1, 300) <= 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &soErr, &len) != 0 || soErr != 0) {
        close(fd);
        return "";
    }
    linger lg{1, 0}; // закрываем сразу, без TIME_WAIT
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) & ~O_NONBLOCK);
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    std::string out = cmd + "\n";
    send(fd, out.data(), out.size(), 0);
    std::string line;
    char buf[256];
    while (line.find('\n') == std::string::npos) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        line.append(buf, n);
    }
    close(fd);
    size_t nl = line.find('\n');
    return nl == std::string::npos ? line : line.substr(0, nl);
}

// Модуль не запущен — запускаем (у пользователя он и так стартует с консолью, но вдруг выключен).
static bool ensureModule() {
    if (ymCall("PING") == "PONG") return true;
    NcmProgramLocation loc{};
    loc.program_id = SYS_TID;
    loc.storageID = NcmStorageId_None;
    u64 pid = 0;
    if (R_FAILED(pmshellLaunchProgram(0, &loc, &pid))) return false;
    for (int i = 0; i < 30; i++) {
        svcSleepThread(100000000ULL);
        if (ymCall("PING") == "PONG") return true;
    }
    return false;
}

static void applyPending(App &app) {
    if (app.pendingSeek >= 0) app.player.seek(app.pendingSeek);
    if (app.pendingPause) app.player.togglePause();
    app.pendingSeek = -1;
    app.pendingPause = false;
}

static bool isPlaying(App &app) {
    Player::State st = app.player.state();
    return app.current >= 0 && app.current < (int)app.queue.size() &&
           (st == Player::Playing || st == Player::Buffering || app.resolving);
}

// «Последний трек»: пишется при выходе, при запуске показывается на паузе.
static const char *LAST_PATH = "sdmc:/switch/yamu-nx/last.json";

// Снимок очереди, трека и позиции — общий формат для передачи модулю и для last.json.
// maxTracks — сколько треков отдать (модулю мало памяти, ему — 60 вокруг текущего).
static bool writeSnapshot(App &app, const char *path, double pos, bool paused, int maxTracks = 300) {
    if (app.current < 0 || app.current >= (int)app.queue.size()) return false;
    int from = 0, to = (int)app.queue.size();
    if (app.source == SrcWave) { from = app.current; to = std::min(to, from + 30); }
    else if (to > maxTracks) { from = std::max(0, app.current - maxTracks / 6); to = std::min(to, from + maxTracks); }
    FILE *f = fopen(path, "w");
    if (!f) return false;
    fprintf(f, "{\"wave\":%s,\"scWave\":%s,\"source\":\"%s\",\"current\":%d,\"pos\":%.1f,\"paused\":%s,\"repeat\":%d,\"tracks\":[",
            app.source == SrcWave ? "true" : "false", app.scWave ? "true" : "false", jsonEsc(app.sourceTitle).c_str(), app.current - from, pos,
            paused ? "true" : "false", (int)app.repeat);
    for (int i = from; i < to; i++) {
        const Track &t = app.queue[i];
        fprintf(f, "%s{\"id\":\"%s\",\"album\":\"%s\",\"title\":\"%s\",\"artists\":\"%s\",\"dur\":%lld,\"coverUri\":\"%s\",\"liked\":%s}",
                i > from ? "," : "", jsonEsc(t.id).c_str(), jsonEsc(t.albumId).c_str(), jsonEsc(t.title).c_str(),
                jsonEsc(t.artists).c_str(), t.durationMs, jsonEsc(t.coverUri).c_str(), app.liked.count(t.id) ? "true" : "false");
    }
    fputs("]}", f);
    fclose(f);
    return true;
}

// Передать играющую музыку модулю.
// Выход через «+»: модуль готовит трек на паузе с запасом позиции, приложение доигрывает до этой
// точки и только тогда замолкает — стык без паузы и без повтора.
// HOME: Свич сразу глушит приложение, ждать бессмысленно — модуль подхватывает, как только готов.
static bool handOff(App &app, bool homeButton) {
    if (!isPlaying(app) || app.handedOff) return false;
    // Фоновый модуль играет только Яндекс: SoundCloud остаётся в приложении (в HOME звучит, пока не запустили игру).

    double pos = app.player.position();
    const double lead = homeButton ? 0.3 : 3.0;
    if (app.wantResume()) writeSnapshot(app, LAST_PATH, pos, true); // на случай, если модуль не подхватит
    if (!writeSnapshot(app, HANDOFF_PATH, pos + lead, true, 60)) return false;
    if (!ensureModule() || ymCall("HANDOFF") != "OK") return false; // своя музыка остаётся при нас
    app.moduleAlive = true;
    u64 t0 = nowMs();
    while (nowMs() - t0 < 5000) { // ждём, пока модуль скачает начало
        if (ymCall("STATUS", 300).find("\"ready\":true") != std::string::npos) break;
        svcSleepThread(50000000ULL);
    }
    if (!homeButton) // доигрываем до точки, с которой начнёт модуль (учитывая ~0,3 с буфера вывода)
        while (app.player.position() < pos + lead + 0.3 && nowMs() - t0 < 7000) svcSleepThread(10000000ULL);
    ymCall("RESUME");
    app.player.stop();
    app.resolving = false;
    app.handedOff = true;
    return true;
}

// Попросить модуль заранее найти ссылку на трек — чтобы передача в фон была почти мгновенной.
static void prepareInModule(App &app, const Track &t) {
    if (app.moduleAlive) ymCall("PREPARE " + t.id, 200);
}
static bool readSnapshot(App &app, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    fclose(f);
    Json j = Json::parse(text);
    App::Takeover t;
    t.wave = j["wave"].asBool();
    t.source = j["source"].asString("Моя волна");
    t.current = (int)j["current"].asInt(-1);
    t.pos = j["pos"].type == Json::Number ? j["pos"].num : 0;
    t.paused = j["paused"].asBool();
    t.repeat = (int)j["repeat"].asInt(0);
    const Json &arr = j["tracks"];
    for (size_t i = 0; i < arr.size(); i++) {
        Track tr;
        tr.id = arr[i]["id"].asString();
        tr.albumId = arr[i]["album"].asString();
        tr.title = arr[i]["title"].asString();
        tr.artists = arr[i]["artists"].asString();
        tr.durationMs = arr[i]["dur"].asInt();
        tr.coverUri = arr[i]["coverUri"].asString();
        if (!tr.id.empty()) t.tracks.push_back(tr);
    }
    if (t.tracks.empty() || t.current < 0 || t.current >= (int)t.tracks.size()) return false;
    app.takeover = t;
    app.hasTakeover = true;
    return true;
}

static void loadSettings(App &app);

// Забрать музыку у модуля (если он что-то играет). Применяется после входа в аккаунт.
static bool fetchTakeover(App &app) {
    std::string reply = ymCall("RELEASE", 4000);
    app.moduleAlive = !reply.empty();
    if (app.moduleAlive) { // громкость могли поменять в оверлее — модуль записал её в настройки
        loadSettings(app);
        app.player.setVolume(app.volume);
    }
    if (reply != "OK") return false;
    app.handedOff = false;
    return readSnapshot(app, HANDOFF_PATH);
}

// Восстановить музыку: от модуля — играть дальше; «последний трек» — показать на паузе,
// A продолжит с того же места (сами ничего не включаем).
static void applyTakeover(App &app) {
    if (!app.hasTakeover) return;
    app.hasTakeover = false;
    App::Takeover &t = app.takeover;
    app.source = t.wave ? SrcWave : SrcList;
    app.sourceTitle = t.source;
    app.scWave = false;
    app.queue = t.tracks;
    app.repeat = (App::Repeat)std::max(0, std::min(2, t.repeat));
    app.shuffle = false; // исходный порядок не сохраняем — очередь уже в нужном виде
    app.screen = ScrPlayer;
    if (t.paused) {
        app.player.stop();
        app.current = t.current;
        app.pendingSeek = t.pos > 1 ? t.pos : -1;
        showToast(app, "Нажмите A, чтобы продолжить");
        return;
    }
    app.current = -1;
    app.pendingSeek = t.pos > 1 ? t.pos : -1;
    playIndex(app, t.current);
    showToast(app, "Продолжаю с того же места");
}
// ---------- настройки ----------

static const char *SETTINGS_PATH = "sdmc:/switch/yamu-nx/settings.json";
static const int SEEK_STEPS[] = {5, 10, 15, 30};
static const int CROSSFADES[] = {0, 2, 4, 6, 8, 10}; // плавный переход, секунд (0 — выкл)
static const char *const PLAYER_BGS[] = {"обычный", "размытая обложка", "цвет обложки"};

static const char *const BOTTOM_ROWS[] = {"нет", "недавно играло", "мои плейлисты"};
static const char *const TILE_STYLES[] = {"тёмные", "цвет акцента", "контур"};

// Настройки → SoundCloud: войти (страница для телефона), отменить вход или выйти (нажать дважды).
static void scAccountAction(App &app) {
    if (!SoundCloud::configured()) { showToast(app, "Нет ключей SoundCloud: файл switch/yamu-nx/soundcloud.txt"); return; }
    if (app.scLogin.running()) {
        app.scLogin.stop();
        showToast(app, "Вход в SoundCloud отменён");
        return;
    }
    if (!app.scUser.empty()) {
        if (app.scLogoutAt && nowMs() - app.scLogoutAt < 3000) {
            app.scLogoutAt = 0;
            Job j{Job::ScLogin}; // пустой код — выход
            app.worker.post(j);
            return;
        }
        app.scLogoutAt = nowMs();
        showToast(app, "Нажмите ещё раз, чтобы выйти из SoundCloud");
        return;
    }
    if (app.offlineMode) { showToast(app, "Нет сети"); return; }
    app.scVerifier = SoundCloud::newVerifier();
    std::string state = SoundCloud::newVerifier().substr(0, 20), err;
    std::string addr = app.scLogin.start(SoundCloud::loginUrl(app.scVerifier, state), state, err);
    if (addr.empty()) { showToast(app, "Вход в SoundCloud: " + err); return; }
    showToast(app, "Откройте на телефоне (та же Wi-Fi): " + addr);
}

enum SettingRow {
    SetSeekStep, SetSeekKeys, SetCrossfade, SetConnect, SetSaveResume, SetAccount, SetAutoSave, SetSaveLyrics,
    SetSaveLikes, SetAccent, SetTheme, SetPlayerBg, SetTextSize, SetHomeTiles, SetHomeScreen, SetLayout, SetBottomRow,
    SetGreeting, SetClock, SetTileSize, SetTileStyle, SetCorners, SetColumns, SetSubtitles, SetNowCover, SetHomeBg, SetAnim,
    SetBottomRow2, SetName, SetDate, SetAccentR, SetAccentG, SetAccentB, SetProxy, SetScAccount
};
// Порядок на экране: заголовки разделов (row = -1) и строки. Подпункты автосохранения — с отступом.
struct SetLine { int row; const char *header; };
static const SetLine SET_LINES[] = {
    {-1, "Воспроизведение"}, {SetSeekStep, nullptr}, {SetSeekKeys, nullptr}, {SetCrossfade, nullptr},
    {-1, "Оформление"}, {SetAccent, nullptr}, {SetAccentR, nullptr}, {SetAccentG, nullptr}, {SetAccentB, nullptr}, {SetTheme, nullptr}, {SetPlayerBg, nullptr}, {SetTextSize, nullptr},
    {SetHomeScreen, nullptr},
    {-1, "Аккаунт и связь"}, {SetAccount, nullptr}, {SetConnect, nullptr}, {SetScAccount, nullptr}, {SetProxy, nullptr},
    {-1, "Сохранение"}, {SetSaveResume, nullptr}, {SetAutoSave, nullptr}, {SetSaveLyrics, nullptr}, {SetSaveLikes, nullptr},
};
static const int SET_LINE_COUNT = sizeof(SET_LINES) / sizeof(SET_LINES[0]);

// Экран «Главный экран» (Оформление → Главный экран ›).
static const SetLine HOME_LINES[] = {
    {-1, "Раскладка"}, {SetColumns, nullptr}, {SetHomeTiles, nullptr},
    {SetBottomRow, nullptr}, {SetBottomRow2, nullptr},
    {-1, "Плитки"}, {SetTileSize, nullptr}, {SetTileStyle, nullptr}, {SetCorners, nullptr}, {SetSubtitles, nullptr},
    {SetNowCover, nullptr}, {SetAnim, nullptr},
    {-1, "Шапка и фон"}, {SetGreeting, nullptr}, {SetName, nullptr}, {SetDate, nullptr}, {SetClock, nullptr},
    {SetHomeBg, nullptr},
};
static const int HOME_LINE_COUNT = sizeof(HOME_LINES) / sizeof(HOME_LINES[0]);

// Какой список настроек на экране: основной или «Главный экран».
struct SetTable { const SetLine *lines; int count; };
static SetTable setTable(Screen s) {
    return s == ScrHomeSet ? SetTable{HOME_LINES, HOME_LINE_COUNT} : SetTable{SET_LINES, SET_LINE_COUNT};
}
static int setRowCount(const SetTable &t) {
    int n = 0;
    for (int i = 0; i < t.count; i++) n += t.lines[i].row >= 0;
    return n;
}
// Строки по порядку (без заголовков): выбор в настройках — индекс в этом списке.
static int settingAt(const SetTable &t, int pos) {
    for (int i = 0, k = 0; i < t.count; i++)
        if (t.lines[i].row >= 0 && k++ == pos) return t.lines[i].row;
    return SetSeekStep;
}
static ListView &curSetView(App &app) { return app.screen == ScrHomeSet ? app.homeSetView : app.setView; }
static int curSetting(App &app) { return settingAt(setTable(app.screen), curSetView(app).sel); }

// Плитки главной: порядок и скрытые (настройки скрыть нельзя — иначе до них не добраться).
enum HomeTile {
    TileNow, TileWave, TileLikes, TileLists, TileOffline, TileSettings,
    TileContinue, TileWaveUp, TileWaveCalm, TileWaveNew, TileSoundCloud, // новые плитки — изначально скрыты
    TILE_COUNT
};
static const int TILE_FIRST_NEW = TileContinue;
static const char *const TILE_NAMES[] = {"Сейчас играет", "Моя волна", "Мне нравится", "Плейлисты", "Скачанное", "Настройки",
                                         "Продолжить", "Волна: бодрое", "Волна: спокойное", "Волна: незнакомое", "SoundCloud"};

// Плитки вкладки SoundCloud — свой постоянный набор (в «Плитках главной» не настраиваются).
enum ScTile { TileScWave = 100, TileScLikes, TileScFeed, TileScLists, TileScAccount };
static const char *tileName(int id) {
    switch (id) {
        case TileScWave: return "Волна";
        case TileScLikes: return "Мне нравится";
        case TileScFeed: return "Лента";
        case TileScLists: return "Плейлисты";
        case TileScAccount: return "Аккаунт";
    }
    return id >= 0 && id < TILE_COUNT ? TILE_NAMES[id] : "";
}

// Вкладки на главной — только если SoundCloud настроен (есть ключи).
static bool hasScTab() {
    static int known = -1; // configured() читает файл с карты — а спрашиваем каждый кадр
    if (known < 0) known = SoundCloud::configured() ? 1 : 0;
    return known == 1;
}

static std::vector<int> homeTiles(const App &app) {
    if (app.homeTab == 1 && hasScTab()) return {TileNow, TileScWave, TileScLikes, TileScFeed, TileScLists, TileScAccount};
    std::vector<int> out;
    for (int t : app.tileOrder)
        if (!app.tileHidden.count(t) && t != TileSettings && !(t == TileSoundCloud && hasScTab())) // настройки и SoundCloud — в боковом меню
            out.push_back(t);
    return out;
}

// Оформление вступает в силу сразу.
static void applyLook(App &app) {
    col::apply(app.theme, app.accent, SDL_Color{(u8)app.accentR, (u8)app.accentG, (u8)app.accentB, 255});
    app.ui.setTextScale(app.bigText ? 1.15f : 1.0f);
}

static void saveSettings(App &app) {
    FILE *f = fopen(SETTINGS_PATH, "w");
    if (!f) return;
    auto b = [](bool v) { return v ? "true" : "false"; };
    std::string order, hidden;
    for (int t : app.tileOrder) order += (order.empty() ? "" : ",") + std::to_string(t);
    for (int t : app.tileHidden) hidden += (hidden.empty() ? "" : ",") + std::to_string(t);
    fprintf(f, "{\"seekStep\":%d,\"seekOnLR\":%s,\"volume\":%d,\"crossfade\":%d,\"connect\":%s,\"deviceId\":\"%s\","
               "\"autoSave\":%s,\"saveResume\":%s,\"saveLyrics\":%s,\"saveLikes\":%s,"
               "\"accent\":%d,\"theme\":%d,\"playerBg\":%d,\"bigText\":%s,\"tiles\":\"%s\",\"hiddenTiles\":\"%s\","
               "\"homeLayout\":%d,\"bottomRow\":%d,\"greeting\":%s,\"clock\":%s,\"tileCompact\":%s,\"tileStyle\":%d,"
               "\"squareTiles\":%s,\"columns\":%d,\"subtitles\":%s,\"nowCover\":%s,\"homeBg\":%d,"
               "\"anim\":%s,\"bottomRow2\":%d,\"name\":\"%s\",\"date\":%s,\"accentR\":%d,\"accentG\":%d,\"accentB\":%d}\n",
            app.seekStep, b(app.seekOnLR), app.volume, app.crossfade, b(app.connectOn), app.deviceId.c_str(),
            b(app.autoSave), b(app.saveResume), b(app.saveLyrics), b(app.saveLikes), app.accent, app.theme, app.playerBg,
            b(app.bigText), order.c_str(), hidden.c_str(), app.homeLayout, app.bottomRow, b(app.greeting), b(app.showClock),
            b(app.tileCompact), app.tileStyle, b(app.squareTiles), app.columns, b(app.subtitles), b(app.nowCover), app.homeBg,
            b(app.anim), app.bottomRow2, jsonEsc(app.customName).c_str(), b(app.showDate), app.accentR, app.accentG, app.accentB);
    fclose(f);
    struct stat st;
    if (stat(SETTINGS_PATH, &st) == 0) app.settingsMtime = st.st_mtime;
}

// Оверлей тоже меняет оформление (тема, акцент, углы) прямо в settings.json — подхватываем на лету.
static void watchSettings(App &app) {
    static u64 lastCheck = 0;
    if (nowMs() - lastCheck < 1500) return;
    lastCheck = nowMs();
    struct stat st;
    if (stat(SETTINGS_PATH, &st) != 0 || st.st_mtime == app.settingsMtime) return;
    app.settingsMtime = st.st_mtime;
    FILE *f = fopen(SETTINGS_PATH, "r");
    if (!f) return;
    std::string text;
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    fclose(f);
    Json j = Json::parse(text);
    if (j.isNull()) return;
    app.theme = std::max(0, std::min(col::THEME_COUNT - 1, (int)j["theme"].asInt(app.theme)));
    app.accent = std::max(0, std::min(col::ACCENT_COUNT, (int)j["accent"].asInt(app.accent)));
    app.accentR = std::max(0, std::min(255, (int)j["accentR"].asInt(app.accentR)));
    app.accentG = std::max(0, std::min(255, (int)j["accentG"].asInt(app.accentG)));
    app.accentB = std::max(0, std::min(255, (int)j["accentB"].asInt(app.accentB)));
    if (j["squareTiles"].type == Json::Bool) app.squareTiles = j["squareTiles"].asBool();
    applyLook(app);
}

static void loadSettings(App &app) {
    FILE *f = fopen(SETTINGS_PATH, "r");
    if (!f) return;
    char buf[4096] = {0};
    fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    struct stat st;
    if (stat(SETTINGS_PATH, &st) == 0) app.settingsMtime = st.st_mtime;
    Json j = Json::parse(buf);
    app.seekStep = (int)j["seekStep"].asInt(10);
    app.seekOnLR = j["seekOnLR"].asBool();
    app.volume = std::max(0, std::min(100, (int)j["volume"].asInt(100) / 5 * 5));
    auto flag = [&](const char *k, bool def) { return j[k].type == Json::Bool ? j[k].asBool() : def; };
    app.autoSave = flag("autoSave", true);
    app.saveResume = flag("saveResume", true);
    app.saveLyrics = flag("saveLyrics", true);
    app.saveLikes = flag("saveLikes", false);
    app.crossfade = std::max(0, std::min(10, (int)j["crossfade"].asInt(0)));
    app.connectOn = flag("connect", true);
    app.deviceId = j["deviceId"].asString();
    app.accent = std::max(0, std::min(col::ACCENT_COUNT, (int)j["accent"].asInt(0)));
    app.accentR = std::max(0, std::min(255, (int)j["accentR"].asInt(255)));
    app.accentG = std::max(0, std::min(255, (int)j["accentG"].asInt(219)));
    app.accentB = std::max(0, std::min(255, (int)j["accentB"].asInt(77)));
    app.theme = std::max(0, std::min(col::THEME_COUNT - 1, (int)j["theme"].asInt(0)));
    app.playerBg = std::max(0, std::min(2, (int)j["playerBg"].asInt(0)));
    app.bigText = flag("bigText", false);
    app.homeLayout = 0; // раскладки «большая Сейчас играет» и «список» убраны — всегда сетка
    app.bottomRow = std::max(0, std::min(2, (int)j["bottomRow"].asInt(0)));
    app.greeting = flag("greeting", true);
    app.showClock = flag("clock", true);
    app.tileCompact = flag("tileCompact", false);
    app.tileStyle = std::max(0, std::min(2, (int)j["tileStyle"].asInt(0)));
    app.squareTiles = flag("squareTiles", false);
    app.columns = std::max(2, std::min(4, (int)j["columns"].asInt(3)));
    app.subtitles = flag("subtitles", true);
    app.nowCover = flag("nowCover", false);
    app.homeBg = std::max(0, std::min(2, (int)j["homeBg"].asInt(0)));
    app.anim = flag("anim", true);
    app.bottomRow2 = std::max(0, std::min(2, (int)j["bottomRow2"].asInt(0)));
    app.customName = j["name"].asString();
    app.showDate = flag("date", false);
    // Плитки: порядок «0,1,…» — берём только известные и без повторов, недостающие добавляем в конец.
    std::vector<int> order;
    std::string s = j["tiles"].asString();
    for (size_t p = 0; p < s.size();) {
        int t = atoi(s.c_str() + p);
        if (t >= 0 && t < TILE_COUNT && std::find(order.begin(), order.end(), t) == order.end()) order.push_back(t);
        size_t c = s.find(',', p);
        if (c == std::string::npos) break;
        p = c + 1;
    }
    std::vector<int> added; // плитки, которых не было в сохранённом порядке (появились в новой версии)
    for (int t = 0; t < TILE_COUNT; t++)
        if (std::find(order.begin(), order.end(), t) == order.end()) { order.push_back(t); added.push_back(t); }
    app.tileOrder = order;
    app.tileHidden.clear();
    for (int t : added) if (t >= TILE_FIRST_NEW) app.tileHidden.insert(t); // новые — скрыты, включаются в «Плитках»
    s = j["hiddenTiles"].asString();
    for (size_t p = 0; p < s.size();) {
        int t = atoi(s.c_str() + p);
        if (t >= 0 && t < TILE_COUNT && t != TileSettings) app.tileHidden.insert(t);
        size_t c = s.find(',', p);
        if (c == std::string::npos) break;
        p = c + 1;
    }
    applyLook(app);
}

static void startLogin(App &app);

// Имя для приветствия — с системной клавиатуры; пустое — снова логин.
static void editName(App &app) {
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0))) { showToast(app, "Клавиатура недоступна"); return; }
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetHeaderText(&kbd, "Как к вам обращаться");
    swkbdConfigSetGuideText(&kbd, "Пусто — по логину");
    swkbdConfigSetInitialText(&kbd, app.customName.c_str());
    swkbdConfigSetStringLenMax(&kbd, 24);
    char out[128] = {0};
    Result rc = swkbdShow(&kbd, out, sizeof(out));
    swkbdClose(&kbd);
    app.focusGraceUntil = nowMs() + 2000; // клавиатура — отдельный апплет, это не уход в HOME
    if (R_FAILED(rc)) return;
    std::string name = out;
    while (!name.empty() && name.back() == ' ') name.pop_back();
    app.customName = name;
}

// Закладки: плейлисты, закреплённые плитками на главной (bookmarks.json).
static const char *BOOKMARKS_PATH = "sdmc:/switch/yamu-nx/bookmarks.json";
static const size_t BOOKMARKS_MAX = 6;

static void saveBookmarks(App &app) {
    FILE *f = fopen(BOOKMARKS_PATH, "w");
    if (!f) return;
    fputs("[", f);
    for (size_t i = 0; i < app.bookmarks.size(); i++) {
        const Playlist &p = app.bookmarks[i];
        fprintf(f, "%s{\"kind\":\"%s\",\"owner\":\"%s\",\"title\":\"%s\",\"cover\":\"%s\",\"n\":%d}", i ? ",\n" : "",
                jsonEsc(p.kind).c_str(), jsonEsc(p.owner).c_str(), jsonEsc(p.title).c_str(), jsonEsc(p.coverUri).c_str(),
                p.trackCount);
    }
    fputs("]\n", f);
    fclose(f);
}

static void loadBookmarks(App &app) {
    FILE *f = fopen(BOOKMARKS_PATH, "r");
    if (!f) return;
    std::string text;
    char buf[2048];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    fclose(f);
    Json arr = Json::parse(text);
    for (size_t i = 0; i < arr.size() && app.bookmarks.size() < BOOKMARKS_MAX; i++) {
        Playlist p;
        p.kind = arr[i]["kind"].asString();
        p.owner = arr[i]["owner"].asString();
        p.title = arr[i]["title"].asString();
        p.coverUri = arr[i]["cover"].asString();
        p.trackCount = (int)arr[i]["n"].asInt(0);
        if (!p.title.empty()) app.bookmarks.push_back(p);
    }
}

// Закрепить плейлист на главной или снять (Y в списке плейлистов).
static void toggleBookmark(App &app, const Playlist &p) {
    auto same = [&](const Playlist &b) { return b.kind == p.kind && b.owner == p.owner; };
    auto it = std::find_if(app.bookmarks.begin(), app.bookmarks.end(), same);
    if (it != app.bookmarks.end()) {
        app.bookmarks.erase(it);
        showToast(app, "Снято с главной");
    } else if (app.bookmarks.size() >= BOOKMARKS_MAX) {
        showToast(app, "На главной уже " + std::to_string(BOOKMARKS_MAX) + " закладок — снимите одну");
        return;
    } else {
        app.bookmarks.push_back(p);
        showToast(app, "Закреплено на главной");
    }
    saveBookmarks(app);
}

// Сдвинуть значение по кругу в списке из n вариантов.
static int cycle(int v, int dir, int n) { return ((v + dir) % n + n) % n; }

// Изменить настройку: dir = -1/+1 (←/→), row — какая (SettingRow).
static void changeSetting(App &app, int row, int dir) {
    bool sub = row == SetSaveLyrics || row == SetSaveLikes;
    if (sub && !app.autoSave) { showToast(app, "Сначала включите автосохранение"); return; }
    switch (row) {
        case SetSeekStep: {
            int n = sizeof(SEEK_STEPS) / sizeof(SEEK_STEPS[0]), i = 0;
            while (i < n && SEEK_STEPS[i] != app.seekStep) i++;
            app.seekStep = SEEK_STEPS[cycle(i < n ? i : 1, dir, n)];
            break;
        }
        case SetSeekKeys: app.seekOnLR = !app.seekOnLR; break;
        case SetCrossfade: {
            int n = sizeof(CROSSFADES) / sizeof(CROSSFADES[0]), i = 0;
            while (i < n && CROSSFADES[i] != app.crossfade) i++;
            app.crossfade = CROSSFADES[cycle(i < n ? i : 0, dir, n)];
            break;
        }
        case SetConnect:
            app.connectOn = !app.connectOn;
            if (!app.connectOn) { app.connect.stop(); app.ynActive = false; }
            else if (app.ready && !app.offlineMode) app.connect.start(TOKEN_PATH, app.deviceId, "Nintendo Switch", VERSION);
            break;
        case SetAccount: startLogin(app); return;
        case SetProxy: showToast(app, "Прокси задаётся в файле switch/yamu-nx/proxy.txt (см. README)"); return;
        case SetScAccount: scAccountAction(app); return;
        case SetAutoSave: app.autoSave = !app.autoSave; break;
        case SetSaveResume: app.saveResume = !app.saveResume; break;
        case SetSaveLyrics: app.saveLyrics = !app.saveLyrics; break;
        case SetSaveLikes: app.saveLikes = !app.saveLikes; break;
        case SetAccent: app.accent = cycle(app.accent, dir, col::ACCENT_COUNT + 1); applyLook(app); break;
        case SetAccentR: case SetAccentG: case SetAccentB: { // свой цвет: шаг 15, сразу включаем его
            int &c = row == SetAccentR ? app.accentR : row == SetAccentG ? app.accentG : app.accentB;
            c = std::max(0, std::min(255, c + dir * 15));
            app.accent = col::ACCENT_COUNT;
            applyLook(app);
            break;
        }
        case SetTheme: app.theme = cycle(app.theme, dir, col::THEME_COUNT); applyLook(app); break;
        case SetPlayerBg: app.playerBg = cycle(app.playerBg, dir, 3); break;
        case SetTextSize: app.bigText = !app.bigText; applyLook(app); break;
        case SetHomeTiles: app.tilesView = ListView(); app.screen = ScrTiles; return;
        case SetHomeScreen: app.homeSetView = ListView(); app.screen = ScrHomeSet; return;

        case SetBottomRow: app.bottomRow = cycle(app.bottomRow, dir, 3); app.rowScroll = 0; break;
        case SetGreeting: app.greeting = !app.greeting; break;
        case SetClock: app.showClock = !app.showClock; break;
        case SetTileSize: app.tileCompact = !app.tileCompact; break;
        case SetTileStyle: app.tileStyle = cycle(app.tileStyle, dir, 3); break;
        case SetCorners: app.squareTiles = !app.squareTiles; break;
        case SetColumns: app.columns = 2 + cycle(app.columns - 2, dir, 3); break;
        case SetSubtitles: app.subtitles = !app.subtitles; break;
        case SetNowCover: app.nowCover = !app.nowCover; break;
        case SetHomeBg: app.homeBg = cycle(app.homeBg, dir, 3); break;
        case SetAnim: app.anim = !app.anim; break;
        case SetBottomRow2: app.bottomRow2 = cycle(app.bottomRow2, dir, 3); break;
        case SetName: editName(app); break;
        case SetDate: app.showDate = !app.showDate; break;
    }
    if (!app.wantResume()) remove(LAST_PATH); // не запоминаем — и старое не подхватим
    saveSettings(app);
}

// Громкость приложения: шаг 5 %, сохраняется вместе с настройками.
static void setAppVolume(App &app, int v, bool save = true) {
    v = std::max(0, std::min(100, (v + 2) / 5 * 5));
    if (v == app.volume) return;
    app.volume = v;
    app.player.setVolume(v);
    if (save) saveSettings(app);
}

static std::string settingValue(const App &app, int row) {
    auto onOff = [&](bool v) { return std::string(v ? "вкл" : "выкл"); };
    switch (row) {
        case SetSeekStep: return std::to_string(app.seekStep) + " с";
        case SetSeekKeys: return app.seekOnLR ? "L / R (треки на крестовине)" : "Крестовина и стик (треки на L / R)";
        case SetCrossfade: return app.crossfade ? std::to_string(app.crossfade) + " с" : "выкл";
        case SetConnect:
            if (!app.connectOn) return "выкл";
            if (app.connect.online()) return app.ynActive ? "вкл · играет здесь" : "вкл · на связи";
            return app.connect.running() && !app.connect.lastError().empty() ? "вкл · ошибка (внизу)" : "вкл · подключаюсь";
        case SetAccount: return app.login.empty() ? "войти по коду" : app.login + " · войти заново";
        case SetAutoSave: return onOff(app.autoSave);
        case SetSaveResume: return onOff(app.saveResume);
        case SetSaveLyrics: return onOff(app.saveLyrics);
        case SetSaveLikes: return onOff(app.saveLikes);
        case SetAccent: return app.accent >= col::ACCENT_COUNT ? "Свой" : col::ACCENTS[app.accent].name;
        case SetProxy: return httpProxyInfo();
        case SetScAccount:
            if (app.scLogin.running()) return "на телефоне: " + app.scLogin.address();
            if (!app.scUser.empty()) return app.scUser + "  ·  выйти";
            return SoundCloud::configured() ? "войти  ›" : "нет ключей";
        case SetAccentR: return std::to_string(app.accentR);
        case SetAccentG: return std::to_string(app.accentG);
        case SetAccentB: return std::to_string(app.accentB);
        case SetTheme: return col::THEME_NAMES[app.theme];
        case SetPlayerBg: return PLAYER_BGS[app.playerBg];
        case SetTextSize: return app.bigText ? "крупный" : "обычный";
        case SetHomeScreen: return "сетка, " + std::to_string(app.columns) + " столбца  ›";

        case SetBottomRow: return BOTTOM_ROWS[app.bottomRow];
        case SetGreeting: return onOff(app.greeting);
        case SetClock: return onOff(app.showClock);
        case SetTileSize: return app.tileCompact ? "компактные" : "крупные";
        case SetTileStyle: return TILE_STYLES[app.tileStyle];
        case SetCorners: return app.squareTiles ? "прямые" : "скруглённые";
        case SetColumns: return std::to_string(app.columns);
        case SetSubtitles: return onOff(app.subtitles);
        case SetNowCover: return app.nowCover ? "во всю плитку" : "маленькая слева";
        case SetHomeBg: return PLAYER_BGS[app.homeBg];
        case SetAnim: return onOff(app.anim);
        case SetBottomRow2: return BOTTOM_ROWS[app.bottomRow2];
        case SetName: return app.customName.empty() ? "как логин" : app.customName;
        case SetDate: return onOff(app.showDate);
        default: { // SetHomeTiles
            int n = (int)homeTiles(app).size();
            return std::to_string(n) + " из " + std::to_string(TILE_COUNT) + "  ›";
        }
    }
}

static const char *settingTitle(int row) {
    switch (row) {
        case SetSeekStep: return "Шаг перемотки";
        case SetSeekKeys: return "Кнопки перемотки";
        case SetCrossfade: return "Плавный переход между треками";
        case SetConnect: return "Управление с телефона (Коннект)";
        case SetAccount: return "Аккаунт";
        case SetAutoSave: return "Автосохранение";
        case SetSaveResume: return "Последний трек и место в нём";
        case SetSaveLyrics: return "Тексты песен (читать без сети)";
        case SetSaveLikes: return "Лайкнутые треки → «Скачанное»";
        case SetAccent: return "Цвет акцента";
        case SetProxy: return "Прокси";
        case SetScAccount: return "SoundCloud";
        case SetAccentR: return "Свой цвет: красный";
        case SetAccentG: return "Свой цвет: зелёный";
        case SetAccentB: return "Свой цвет: синий";
        case SetTheme: return "Тема";
        case SetPlayerBg: return "Фон плеера";
        case SetTextSize: return "Размер текста";
        case SetHomeScreen: return "Главный экран";
        case SetLayout: return "Раскладка";
        case SetBottomRow: return "Ряд снизу";
        case SetGreeting: return "Приветствие вместо «Главная»";
        case SetClock: return "Часы и заряд батареи";
        case SetTileSize: return "Размер плиток";
        case SetTileStyle: return "Стиль плиток";
        case SetCorners: return "Углы";
        case SetColumns: return "Столбцов в сетке";
        case SetSubtitles: return "Подписи на плитках";
        case SetNowCover: return "Обложка «Сейчас играет»";
        case SetHomeBg: return "Фон главной";
        case SetAnim: return "Анимации";
        case SetBottomRow2: return "Второй ряд снизу";
        case SetName: return "Имя в приветствии";
        case SetDate: return "Дата под приветствием";
        default: return "Плитки главной";
    }
}

// ---------- главный экран ----------

// Экран «Плитки главной»: pos — строка в списке (порядок tileOrder).
static void toggleTile(App &app, int pos) {
    if (pos < 0 || pos >= (int)app.tileOrder.size()) return;
    app.tilesView.sel = pos;
    int t = app.tileOrder[pos];
    if (t == TileSettings) { showToast(app, "Настройки на главной нужны всегда"); return; }
    if (app.tileHidden.count(t)) app.tileHidden.erase(t);
    else app.tileHidden.insert(t);
    saveSettings(app);
}

static void moveTile(App &app, int dir) {
    int pos = app.tilesView.sel, to = pos + dir;
    if (to < 0 || to >= (int)app.tileOrder.size()) return;
    std::swap(app.tileOrder[pos], app.tileOrder[to]);
    app.tilesView.sel = to;
    saveSettings(app);
}

static void doAction(App &app, int act);
static void playRecent(App &app, int idx);
static bool hasCurrent(const App &app);

static void activateTile(App &app, int tile) {
    if (app.offlineMode && (tile == TileWave || tile == TileLikes || tile == TileLists || tile == TileSoundCloud ||
                            (tile >= TileScWave && tile != TileScAccount))) {
        showToast(app, "Нет сети — доступно только скачанное");
        return;
    }
    if (tile >= TileScWave) { // вкладка SoundCloud
        if (tile == TileScAccount) { // в настройки, сразу на строку «SoundCloud»
            SetTable t = setTable(ScrSettings);
            for (int pos = 0; pos < setRowCount(t); pos++)
                if (settingAt(t, pos) == SetScAccount) app.setView.sel = pos;
            app.screen = ScrSettings;
            return;
        }
        if (app.scUser.empty()) { showToast(app, "Сначала войдите в SoundCloud: плитка «Аккаунт»"); return; }
        if (tile == TileScLists) { openScHome(app); return; }
        Playlist pl;
        pl.kind = "@sc";
        pl.owner = tile == TileScWave ? "@wave" : tile == TileScLikes ? "@likes" : "@feed";
        pl.title = tile == TileScWave ? "Волна SoundCloud" : tile == TileScLikes ? "Мне нравится · SoundCloud" : "Лента подписок";
        openList(app, pl);
        app.listFromHome = true;
        app.listAutoPlay = tile == TileScWave; // волну — сразу играть
        return;
    }
    switch (tile) {
        case TileNow:
            if (app.current >= 0 && app.current < (int)app.queue.size()) app.screen = ScrPlayer;
            else showToast(app, "Ничего не играет — выберите волну или плейлист");
            break;
        case TileWave: startWave(app); app.screen = ScrPlayer; break;
        case TileLikes: {
            Playlist likes;
            likes.title = "Мне нравится";
            openList(app, likes);
            app.listFromHome = true;
            break;
        }
        case TileLists: openLibrary(app); break;
        case TileOffline: openList(app, offlinePlaylist(app)); app.listFromHome = true; break;
        case TileSettings: app.screen = ScrSettings; break;
        case TileSoundCloud: openScHome(app); break;
        case TileWaveUp: case TileWaveCalm: case TileWaveNew:
            startWave(app, tile - TileWaveUp + 1);
            app.screen = ScrPlayer;
            break;
        case TileContinue: // трек на паузе — продолжить; иначе последний из «Недавно играло»
            if (hasCurrent(app) && !isPlaying(app)) {
                doAction(app, ActPlay);
                app.screen = ScrPlayer;
            } else if (hasCurrent(app)) {
                app.screen = ScrPlayer;
            } else if (!app.recent.empty()) {
                playRecent(app, 0);
            } else {
                showToast(app, "Пока нечего продолжать");
            }
            break;
    }
}

// LRC: «[мм:сс.хх] строка», таймкодов на строке может быть несколько. Обычный текст — построчно без времени.
static void parseLyrics(App &app, const std::string &text, bool synced) {
    app.lyrics.clear();
    app.lyricRows.clear();
    size_t p = 0;
    while (p <= text.size()) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        std::string ln = text.substr(p, e - p);
        p = e + 1;
        if (!ln.empty() && ln.back() == '\r') ln.pop_back();
        if (!synced) { app.lyrics.push_back({-1, ln}); continue; }
        std::vector<double> times;
        size_t q = 0;
        while (q < ln.size() && ln[q] == '[') {
            size_t c = ln.find(']', q);
            if (c == std::string::npos) break;
            int mm = 0;
            double ss = 0;
            if (sscanf(ln.c_str() + q + 1, "%d:%lf", &mm, &ss) == 2) times.push_back(mm * 60 + ss);
            q = c + 1;
        }
        std::string s = ln.substr(q);
        while (!s.empty() && s[0] == ' ') s.erase(0, 1);
        for (double t : times) app.lyrics.push_back({t, s});
    }
    if (synced) std::stable_sort(app.lyrics.begin(), app.lyrics.end(),
                                 [](const App::LyricLine &a, const App::LyricLine &b) { return a.t < b.t; });
    while (!app.lyrics.empty() && app.lyrics.back().s.empty()) app.lyrics.pop_back();
    while (!app.lyrics.empty() && app.lyrics.front().s.empty() && app.lyrics.front().t <= 0) app.lyrics.erase(app.lyrics.begin());
    app.lyricsSynced = synced && !app.lyrics.empty();
}

// Запросить текст для текущего трека, если ещё не запрашивали.
static void requestLyrics(App &app) {
    if (app.current < 0 || app.current >= (int)app.queue.size()) return;
    const Track &t = app.queue[app.current];
    if (t.id == app.lyricsId) return;
    app.lyricsId = t.id;
    app.lyrics.clear();
    app.lyricRows.clear();
    app.lyricsError.clear();
    app.lyricsSynced = false;
    app.lyricsLoading = true;
    app.lyricsY = 0;
    if (isSoundCloud(t)) { app.lyricsLoading = false; app.lyricsError = "У треков SoundCloud нет текстов"; return; }
    app.lyricsManualUntil = 0;
    Job j{Job::Lyrics};
    j.track = t;
    j.cache = app.autoSave && app.saveLyrics;
    app.worker.post(j);
}

// Строка, которая поётся сейчас (-1 — ещё до первой).
static int currentLyric(App &app) {
    if (!app.lyricsSynced) return -1;
    double pos = app.player.position() + 0.25; // чуть заранее — глаз успевает за голосом
    int cur = -1;
    for (int i = 0; i < (int)app.lyrics.size() && app.lyrics[i].t <= pos; i++) cur = i;
    return cur;
}

// «Недавно играло»: очередь — вся история, с выбранного трека.
static void playRecent(App &app, int idx) {
    if (idx < 0 || idx >= (int)app.recent.size()) return;
    app.playerFromRich = false;
    std::vector<Track> tracks = app.recent; // копия: playIndex сам двигает историю
    app.scWave = false;
    app.queue = tracks;
    app.unshuffled.clear();
    app.shuffle = false;
    app.source = SrcList;
    app.sourceTitle = "Недавно играло";
    app.openList = Playlist();
    playIndex(app, idx);
    app.screen = ScrPlayer;
}

// Элемент главной (плитка или обложка из ряда снизу) — по номеру в homeItems.
static void activateHome(App &app, int idx) {
    if (idx < 0 || idx >= (int)app.homeItems.size()) return;
    app.homeSel = idx;
    const App::HomeItem &it = app.homeItems[idx];
    if (it.kind == 0) { activateTile(app, it.id); return; }
    if (it.kind == 1) { playRecent(app, it.id); return; }
    if (it.kind == 3) { // закладка
        if (it.id < 0 || it.id >= (int)app.bookmarks.size()) return;
        const Playlist &p = app.bookmarks[it.id];
        if (app.offlineMode && p.kind != OFFLINE_KIND) { showToast(app, "Нет сети — доступно только скачанное"); return; }
        openList(app, p.kind == OFFLINE_KIND ? offlinePlaylist(app) : p);
        app.listFromHome = true;
        return;
    }
    if (app.offlineMode) { showToast(app, "Нет сети — доступно только скачанное"); return; }
    if (it.id < 0) {
        Playlist likes;
        likes.title = "Мне нравится";
        openList(app, likes);
    } else if (it.id < (int)app.playlists.size()) {
        openList(app, app.playlists[it.id]);
    }
    app.listFromHome = true;
}

// Назад: плеер → список (если включали из него) или главная; списки и настройки → выше.
static void goBack(App &app) {
    switch (app.screen) {
        case ScrPlayer:
            if (app.playerFromRich && !app.rich.empty()) { app.screen = ScrRich; break; }
            app.screen = app.source == SrcList && !app.openList.title.empty() ? ScrList : ScrHome;
            break;
        case ScrList:
            app.screen = app.listFromPlayer && hasCurrent(app) ? ScrPlayer : app.listFromRich && !app.rich.empty() ? ScrRich : app.listFromHome ? ScrHome : ScrLibrary;
            break;
        case ScrRich:
            if (!app.rich.empty()) app.rich.pop_back();
            app.richItems.clear();
            app.screen = !app.rich.empty() ? ScrRich : app.richFromPlayer && hasCurrent(app) ? ScrPlayer : ScrHome;
            if (app.rich.empty()) app.richFromPlayer = false;
            break;
        case ScrQueue: app.screen = ScrPlayer; break;
        case ScrTiles: app.screen = ScrHomeSet; break;
        case ScrHomeSet: app.screen = ScrSettings; break;
        case ScrLyrics: app.screen = ScrPlayer; break;
        default: app.screen = ScrHome; break;
    }
}

// ---------- Ynison: управление с телефона ----------

static bool hasCurrent(const App &app) { return app.current >= 0 && app.current < (int)app.queue.size(); }

// Отправить полное состояние (очередь + статус); activate — заодно объявить себя играющим устройством.
static void ynSendState(App &app, bool activate) {
    if (!app.connect.online() || !hasCurrent(app) || isSoundCloud(app.queue[app.current])) return; // SoundCloud Яндексу не показываем
    // Очередь целиком бывает огромной — шлём окно вокруг текущего трека.
    int from = std::max(0, app.current - 50), to = std::min((int)app.queue.size(), app.current + 150);
    std::vector<Track> part(app.queue.begin() + from, app.queue.begin() + to);
    double pos = app.player.position(), dur = app.queue[app.current].durationMs / 1000.0;
    bool paused = !isPlaying(app);
    app.connect.send(app.connect.playerState(part, app.current - from, pos, dur, paused, app.repeat));
    if (activate) app.connect.send(app.connect.activate());
    app.ynActive = app.ynActive || activate;
    app.ynLastTrack = app.queue[app.current].id;
    app.ynLastPaused = paused;
    app.ynLastPos = pos;
    app.ynLastSent = nowMs();
}

// Включить трек по команде с телефона: позиция и пауза — как прислали.
static void ynPlay(App &app, int idx, double pos, bool paused) {
    app.ynRemote = true;
    app.pendingSeek = pos > 1 ? pos : -1;
    app.pendingPause = paused;
    playIndex(app, idx);
    app.ynRemote = false;
}

// Пришла очередь с телефона: ставим её и включаем нужный трек.
static void ynApplyQueue(App &app, const std::vector<Track> &tracks) {
    int idx = -1;
    for (int i = 0; i < (int)tracks.size(); i++)
        if (tracks[i].id == app.ynPending.id) { idx = i; break; }
    if (idx < 0 || !app.ynActive) return;
    app.scWave = false;
    app.queue = tracks;
    app.unshuffled.clear();
    app.shuffle = false;
    app.source = SrcList;
    app.sourceTitle = "С другого устройства";
    app.openList = Playlist();
    ynPlay(app, idx, app.ynPending.pos, app.ynPending.paused);
    if (app.screen == ScrHome) app.screen = ScrPlayer;
}

static void ynHandle(App &app, const std::string &msg) {
    Json j = Json::parse(msg);
    if (j["_connected"].asBool()) {
        app.ynActive = false;
        if (isPlaying(app)) ynSendState(app, true); // играем — пусть телефон это видит
        return;
    }
    if (j["_lost"].asBool()) return;
    if (!j["error"].isNull()) return;

    if (j["active_device_id_optional"].type == Json::String) app.ynActiveId = j["active_device_id_optional"].asString();
    bool wasActive = app.ynActive;
    app.ynActive = !app.ynActiveId.empty() && app.ynActiveId == app.deviceId;
    if (wasActive && !app.ynActive && isPlaying(app)) { // музыку забрали на другое устройство
        app.player.togglePause();
        app.ynLastPaused = true;
        showToast(app, "Музыка перешла на другое устройство");
    }
    if (!wasActive && app.ynActive) showToast(app, "Управление с телефона: играет здесь");

    const Json &ps = j["player_state"];
    if (!app.ynActive || ps.isNull()) return;
    const Json &qu = ps["player_queue"], &st = ps["status"];
    // Наше же изменение вернулось эхом — ничего делать не нужно.
    bool qMine = qu.isNull() || qu["version"]["device_id"].asString() == app.deviceId;
    bool sMine = st.isNull() || st["version"]["device_id"].asString() == app.deviceId;
    if (qMine && sMine) return;

    const Json &list = qu["playable_list"];
    int idx = (int)qu["current_playable_index"].asInt(-1);
    if (qu.isNull() || idx < 0 || idx >= (int)list.size()) return;
    std::vector<std::string> ids;
    for (size_t i = 0; i < list.size(); i++) {
        std::string id = list[i]["playable_id"].asString();
        size_t colon = id.find(':');
        if (colon != std::string::npos) id = id.substr(0, colon);
        ids.push_back(id);
    }
    double pos = st["progress_ms"].asInt(0) / 1000.0;
    bool paused = st["paused"].asBool(true);

    // Тот же список, что у нас? Тогда это пауза, перемотка или другой трек из него.
    bool same = ids.size() <= app.queue.size();
    int offset = 0;
    if (same && hasCurrent(app)) { // мы шлём окно очереди — ищем, где оно начинается
        auto it = std::find_if(app.queue.begin(), app.queue.end(), [&](const Track &t) { return t.id == ids[0]; });
        offset = it == app.queue.end() ? -1 : (int)(it - app.queue.begin());
        same = offset >= 0 && offset + ids.size() <= app.queue.size();
        for (size_t i = 0; same && i < ids.size(); i++) same = app.queue[offset + i].id == ids[i];
    } else {
        same = false;
    }
    if (!same) {
        app.ynPending = {ids[idx], pos, paused};
        Job job{Job::ConnectQueue};
        job.ids = ids;
        app.worker.cancel(Job::ConnectQueue);
        app.worker.post(job);
        return;
    }
    int target = offset + idx;
    Player::State ps0 = app.player.state();
    bool idle = ps0 == Player::Idle || ps0 == Player::Error || ps0 == Player::Finished;
    if (target != app.current || (idle && !app.resolving && !paused)) {
        ynPlay(app, target, pos, paused);
    } else {
        bool localPaused = ps0 == Player::Paused;
        if (paused != localPaused && !app.resolving) app.player.togglePause();
        if (!st.isNull() && std::abs(pos - app.player.position()) > 3 && !app.resolving) seekTo(app, pos);
    }
    app.ynLastTrack = app.queue[app.current].id;
    app.ynLastPaused = paused;
    app.ynLastPos = pos;
    app.ynLastSent = nowMs();
}

// Каждый кадр: следим за своим плеером и сообщаем Яндексу, что изменилось.
static void ynTick(App &app) {
    std::string msg;
    while (app.connect.poll(msg)) ynHandle(app, msg);
    if (!app.connect.online() || !hasCurrent(app) || app.handedOff) return;

    const std::string &id = app.queue[app.current].id;
    bool paused = !isPlaying(app);
    double pos = app.player.position(), dur = app.queue[app.current].durationMs / 1000.0;
    // Включили музыку здесь, а играло другое устройство, — забираем управление.
    if (!app.ynActive) {
        if (!paused && (app.ynLastPaused || id != app.ynLastTrack)) ynSendState(app, true);
        else { app.ynLastPaused = paused; app.ynLastTrack = id; }
        return;
    }
    if (id != app.ynLastTrack) { ynSendState(app, false); return; }
    double expect = app.ynLastPos + (app.ynLastPaused ? 0 : (nowMs() - app.ynLastSent) / 1000.0);
    bool jumped = std::abs(pos - expect) > 2.5;
    if (paused != app.ynLastPaused || jumped || nowMs() - app.ynLastSent > 5000) {
        app.connect.send(app.connect.playingStatus(pos, dur, paused));
        app.ynLastPaused = paused;
        app.ynLastPos = pos;
        app.ynLastSent = nowMs();
    }
}

// ---------- вход по коду ----------

static void startLogin(App &app) {
    if (app.deviceId.empty()) { app.deviceId = Connect::newDeviceId(); saveSettings(app); }
    app.loginFlow = App::LoginFlow();
    app.loginFlow.active = true;
    app.loginFlow.waiting = true;
    Job j{Job::LoginStart};
    j.code = app.deviceId;
    app.worker.post(j);
}

// Каждый кадр: пора спросить, ввёл ли пользователь код.
static void loginTick(App &app) {
    App::LoginFlow &lf = app.loginFlow;
    if (!lf.active || lf.waiting || lf.polling || lf.deviceCode.empty() || !lf.error.empty()) return;
    if (nowMs() >= lf.expiresAt) { startLogin(app); return; } // код устарел — берём новый
    if (nowMs() < lf.nextPoll) return;
    lf.polling = true;
    Job j{Job::LoginPoll};
    j.code = lf.deviceCode;
    app.worker.post(j);
}

static void handleResult(App &app, JobResult &r) {
    // Музыка у фонового модуля — запоздавшие ответы не должны включить её и здесь.
    if (app.handedOff && (r.kind == Job::Resolve || r.kind == Job::LoadWave)) {
        if (r.kind == Job::LoadWave) app.waveLoading = false;
        return;
    }
    switch (r.kind) {
        case Job::Startup:
            app.started = true;
            if (!r.ok) {
                // Нет сети, но есть скачанное — открываем его вместо ошибки.
                if (r.error.compare(0, 8, "Network:") == 0 && !app.offline.empty()) {
                    app.offlineMode = true;
                    app.ready = true;
                    app.screen = ScrHome;
                    showToast(app, "Нет сети — доступно скачанное");
                    applyTakeover(app);
                } else if (r.error.find("token") != std::string::npos || r.error.find("Token") != std::string::npos) {
                    startLogin(app); // токена нет или он больше не действует — входим по коду
                } else {
                    app.startupError = r.error;
                }
                break;
            }
            app.login = r.login;
            app.hasPlus = r.hasPlus;
            app.ready = true;
            // Ничего не включаем сами: открываем главную, пользователь выбирает.
            // Исключение — музыка, которую мы только что забрали у фонового модуля.
            app.screen = ScrHome;
            applyTakeover(app);
            app.libLoading = true;
            app.worker.post(Job{Job::Library});
            if (app.deviceId.empty()) { app.deviceId = Connect::newDeviceId(); saveSettings(app); }
            if (app.connectOn) app.connect.start(TOKEN_PATH, app.deviceId, "Nintendo Switch", VERSION);
            break;

        case Job::ScSearch:
        case Job::ScResolve: { // SoundCloud: треки (и плейлисты при поиске) — на страницу запроса
            int type = r.kind == Job::ScSearch ? 3 : 4;
            App::RichPage *page = nullptr;
            for (auto &p : app.rich) if (p.type == type && p.query == r.query) page = &p;
            if (!page) break;
            page->loading = false;
            page->sections.clear();
            if (!r.ok) { page->error = r.error; break; }
            if (type == 4 && !r.lyrics.empty()) page->title = "SoundCloud: " + r.lyrics; // название найденного
            if (!r.tracks.empty()) {
                App::RichSection sec;
                sec.title = type == 3 ? "Треки" : "";
                sec.tracks = r.tracks;
                page->sections.push_back(sec);
            }
            if (!r.cards.empty()) {
                App::RichSection sec;
                sec.title = "Плейлисты";
                sec.cards = r.cards;
                page->sections.push_back(sec);
            }
            if (page->sections.empty()) page->error = "Ничего не нашлось по запросу «" + r.query + "»";
            break;
        }

        case Job::ScLogin:
            if (r.query.empty()) { // выход
                app.scUser.clear();
                showToast(app, "Вышли из SoundCloud");
                break;
            }
            if (!r.ok) { showToast(app, r.error); break; } // страница входа остаётся — можно повторить
            app.scLogin.stop();
            app.scUser = r.login.empty() ? "вход выполнен" : r.login;
            showToast(app, "SoundCloud: вошли как " + app.scUser);
            break;

        case Job::ScHome: { // главная SoundCloud: «Моё» уже на странице, дописываем плейлисты и ленту
            app.liked.insert(r.likedIds.begin(), r.likedIds.end()); // свежие лайки SoundCloud — отметки в плеере
            App::RichPage *page = nullptr;
            for (auto &p : app.rich) if (p.type == 5) page = &p;
            if (!page) break;
            page->loading = false;
            if (!r.ok) { page->error = r.error; break; }
            if (!r.cards.empty()) {
                App::RichSection sec;
                sec.title = "Мои плейлисты";
                sec.cards = r.cards;
                page->sections.push_back(sec);
            }
            if (!r.tracks.empty()) {
                App::RichSection sec;
                sec.title = "Свежее от подписок";
                sec.tracks.assign(r.tracks.begin(), r.tracks.begin() + std::min<size_t>(20, r.tracks.size()));
                page->sections.push_back(sec);
            }
            break;
        }

        case Job::ScRelated: { // волна SoundCloud: дописываем похожие, без повторов
            app.scWaveLoading = false;
            if (!app.scWave || !r.ok) break;
            std::set<std::string> have;
            for (auto &t : app.queue) have.insert(t.id);
            int added = 0;
            for (auto &t : r.tracks)
                if (have.insert(t.id).second) { app.queue.push_back(t); app.unshuffled.push_back(t); added++; }
            if (!added) crashLog("[sc] волна: похожих новых нет");
            break;
        }

        case Job::SearchAll: { // результаты — на страницу этого запроса, если она ещё открыта
            App::RichPage *page = nullptr;
            for (auto &p : app.rich) if (p.type == 0 && p.query == r.query) page = &p;
            if (!page) break;
            page->loading = false;
            page->sections.clear();
            if (!r.ok) { page->error = r.error; break; }
            const SearchAll &s = r.search;
            if (s.bestKind) {
                App::RichSection sec;
                sec.title = "Лучший результат";
                if (s.bestKind == 4) sec.tracks.push_back(s.bestTrack);
                else sec.cards.push_back(s.best);
                page->sections.push_back(sec);
            }
            if (!s.tracks.empty()) {
                App::RichSection sec;
                sec.title = "Треки";
                sec.tracks.assign(s.tracks.begin(), s.tracks.begin() + std::min<size_t>(5, s.tracks.size()));
                sec.allTracks = true;
                page->sections.push_back(sec);
            }
            const std::pair<const char *, const std::vector<Card> *> groups[] = {
                {"Исполнители", &s.artists}, {"Альбомы", &s.albums}, {"Плейлисты", &s.playlists}};
            for (auto &g : groups) {
                if (g.second->empty()) continue;
                App::RichSection sec;
                sec.title = g.first;
                sec.cards = *g.second;
                page->sections.push_back(sec);
            }
            if (page->sections.empty()) page->error = "Ничего не нашлось";
            break;
        }

        case Job::Artist: {
            App::RichPage *page = nullptr;
            for (auto &p : app.rich) if (p.type == 1 && p.artistId == r.artistId) page = &p;
            if (!page) break;
            page->loading = false;
            page->sections.clear();
            if (!r.ok) { page->error = r.error; break; }
            if (!r.tracks.empty()) {
                App::RichSection sec;
                bool sc = isSoundCloudId(r.artistId); // у автора SoundCloud — все его треки
                sec.title = sc ? "Треки" : "Популярные треки";
                sec.tracks.assign(r.tracks.begin(), r.tracks.begin() + (sc ? r.tracks.size() : std::min<size_t>(10, r.tracks.size())));
                page->sections.push_back(sec);
            }
            // Альбомы исполнителя, синглы и чужие альбомы (сборники, совместные) — отдельными рядами.
            for (auto g : {std::make_pair("album", "Альбомы"), std::make_pair("single", "Синглы и EP"),
                           std::make_pair("also", "С участием")}) {
                App::RichSection sec;
                sec.title = g.second;
                for (const Card &c : r.cards)
                    if (c.group == g.first || (c.group.empty() && std::string(g.first) == "album")) sec.cards.push_back(c);
                if (!sec.cards.empty()) page->sections.push_back(sec);
            }
            if (!r.cards2.empty()) {
                App::RichSection sec;
                sec.title = "Похожие исполнители";
                sec.cards = r.cards2;
                page->sections.push_back(sec);
            }
            if (page->sections.empty()) page->error = "У исполнителя пока ничего нет";
            break;
        }

        case Job::TrackMeta: { // данные трека пришли — дописываем их в очередь и переходим, куда просили
            int want = app.metaWant;
            app.metaWant = -1;
            if (!r.ok) { showToast(app, "Не получилось открыть: " + r.error); break; }
            for (Track &t : app.queue)
                if (t.id == r.track.id) {
                    if (t.albumId.empty()) t.albumId = r.track.albumId;
                    t.albumTitle = r.track.albumTitle;
                    t.artistIds = r.track.artistIds;
                    t.artistNames = r.track.artistNames;
                }
            if (app.screen != ScrPlayer || want == -1) break; // пока ждали, ушли с плеера
            if (want >= 0 && want >= (int)r.track.artistIds.size()) want = 0;
            if (want >= 0 && r.track.artistIds.empty()) { showToast(app, "У трека нет исполнителя"); break; }
            if (want == -2 && r.track.albumId.empty()) { showToast(app, "У трека нет альбома"); break; }
            openFromPlayer(app, r.track, want);
            break;
        }

        case Job::LikedAlbums: {
            App::RichPage *page = nullptr;
            for (auto &p : app.rich) if (p.type == 2) page = &p;
            if (!page) break;
            page->loading = false;
            page->sections.clear();
            if (!r.ok) { page->error = r.error; break; }
            if (r.cards.empty()) { page->error = "Пока нет альбомов — отметьте их лайком в приложении Яндекс Музыки"; break; }
            App::RichSection sec;
            sec.cards = r.cards;
            sec.grid = true;
            page->sections.push_back(sec);
            break;
        }

        case Job::LoginStart: {
            App::LoginFlow &lf = app.loginFlow;
            lf.waiting = false;
            if (!r.ok) { lf.error = r.error; break; }
            lf.userCode = r.device.userCode;
            lf.url = r.device.url;
            lf.deviceCode = r.device.deviceCode;
            lf.interval = std::max(2, r.device.interval);
            lf.nextPoll = nowMs() + lf.interval * 1000ULL;
            lf.expiresAt = nowMs() + r.device.expiresIn * 1000ULL;
            break;
        }

        case Job::LoginPoll: {
            App::LoginFlow &lf = app.loginFlow;
            lf.polling = false;
            if (!lf.active) break;
            if (r.pending) { lf.nextPoll = nowMs() + lf.interval * 1000ULL; break; }
            if (!r.ok) { lf.error = r.error; break; }
            lf.active = false; // токен уже записан — входим заново
            showToast(app, "Вход выполнен");
            app.started = app.ready = false;
            app.startupError.clear();
            app.worker.post(Job{Job::Startup});
            break;
        }

        case Job::ConnectQueue: {
            if (!r.ok) { showToast(app, "Не получилось открыть очередь с телефона: " + r.error); break; }
            // Порядок — как прислали: сервер отдаёт треки как попало.
            std::map<std::string, Track> byId;
            for (const Track &t : r.tracks) byId[t.id] = t;
            std::vector<Track> ordered;
            for (const std::string &id : r.likedIds)
                if (byId.count(id)) ordered.push_back(byId[id]);
            ynApplyQueue(app, ordered);
            break;
        }

        case Job::Search:
            if (app.openList.kind != SEARCH_KIND || r.query != app.lastQuery) break; // устаревший ответ
            app.listLoading = false;
            if (!r.ok) { app.listError = "Поиск: " + r.error; break; }
            app.listTracks = r.tracks;
            if (app.listTracks.empty()) app.listError = "Ничего не нашлось по запросу «" + r.query + "»";
            break;

        case Job::Download:
            app.dlQueued.erase(r.track.id);
            app.dlDone++;
            if (!r.ok) {
                app.dlFailed++;
                showToast(app, "Не скачалось: " + r.error);
                break;
            }
            app.offline.push_back({r.track, r.filePath, r.coverPath});
            saveOffline(app);
            if (app.screen == ScrList && app.openList.kind == OFFLINE_KIND) app.listTracks = offlineTracks(app);
            if (app.dlDone >= app.dlTotal)
                showToast(app, app.dlFailed ? "Скачано, но " + std::to_string(app.dlFailed) + " не удалось" : "Всё скачано");
            break;

        case Job::LoadWave:
            app.waveLoading = false;
            if (app.source != SrcWave) break; // пока грузилось, включили плейлист
            if (!r.ok) { app.error = "Волна: " + r.error; break; }
            if (r.fresh) { app.queue = r.tracks; app.current = -1; playIndex(app, 0); break; }
            app.queue.insert(app.queue.end(), r.tracks.begin(), r.tracks.end());
            if (app.pendingIndex >= 0) playIndex(app, app.pendingIndex);
            break;

        case Job::Resolve: {
            if (r.preload) { // ссылка на следующий трек для плавного перехода
                if (r.ok && r.index < (int)app.queue.size() && app.queue[r.index].id == app.xfId) {
                    app.xfUrl = r.url;
                    app.xfBitrate = r.bitrate;
                    app.xfCodec = r.codec;
                }
                break;
            }
            if (r.gen != app.gen || r.index >= (int)app.queue.size()) break; // трек уже переключили
            app.resolving = false;
            const Track &t = app.queue[r.index];
            if (!r.ok) {
                app.error = r.error;
                app.skipAt = nowMs() + AUTO_SKIP_MS;
                break;
            }
            app.bitrate = r.bitrate;
            app.codec = r.codec;
            app.player.play(r.url, (size_t)(t.durationMs / 1000.0 * r.bitrate * 1000 / 8));
            applyPending(app);
            prepareInModule(app, t);
            feedback(app, "trackStarted", &t);
            break;
        }

        case Job::Library:
            app.libLoading = false;
            if (!r.ok) { app.libError = r.error; break; }
            app.libLoaded = true;
            app.playlists = r.playlists;
            app.liked = std::set<std::string>(r.likedIds.begin(), r.likedIds.end());
            break;

        case Job::OpenList:
            if (r.gen != app.listGen || r.playlist.kind != app.openList.kind || r.playlist.owner != app.openList.owner) break;
            app.listLoading = false;
            if (!r.ok) { app.listError = r.error; break; }
            app.listTracks = r.tracks;
            app.listMore = r.more;
            if (app.openList.kind.empty()) app.liked.insert(r.likedIds.begin(), r.likedIds.end());
            if (app.openList.kind == "@sc" && app.openList.owner == "@likes") // лайки SoundCloud — отмечаем в плеере
                for (auto &t : r.tracks) app.liked.insert(t.id);
            if (app.listTracks.empty()) app.listError = "Здесь пока пусто";
            if (app.listAutoPlay && !app.listTracks.empty() && app.screen == ScrList) { // волна SoundCloud с главной
                playList(app, 0, false);
                app.screen = ScrPlayer;
            }
            app.listAutoPlay = false;
            break;

        case Job::ListMore: // следующая порция «Мне нравится»: дописываем в конец, выбор не трогаем
            if (r.gen != app.listGen || app.screen != ScrList || !app.openList.kind.empty()) break;
            app.listMore = r.ok && r.more;
            if (!r.ok) { showToast(app, "Не все лайки загрузились: " + r.error); break; }
            app.listTracks.insert(app.listTracks.end(), r.tracks.begin(), r.tracks.end());
            break;

        case Job::Like:
            if (!r.ok) {
                if (r.like) app.liked.erase(r.trackId);
                else app.liked.insert(r.trackId);
                showToast(app, "Не получилось: " + r.error);
            } else {
                showToast(app, r.like ? "Добавлено в «Мне нравится»" : "Убрано из «Мне нравится»");
                if (r.like && app.autoSave && app.saveLikes && !findOffline(app, r.trackId)) {
                    for (const Track &t : app.queue)
                        if (t.id == r.trackId) { downloadTracks(app, {t}); break; }
                }
            }
            break;

        case Job::Cover:
            app.coverPending.erase(r.url);
            if (r.surface) {
                SDL_Color avg = avgColor(r.surface);
                app.covers[r.url] = {app.ui.textureFrom(r.surface), app.frame, avg};
            }
            if (app.covers.size() > COVER_CACHE_MAX) { // выкидываем давно не показанные
                std::vector<std::pair<u64, std::string>> byAge;
                for (auto &kv : app.covers) byAge.push_back({kv.second.used, kv.first});
                std::sort(byAge.begin(), byAge.end());
                for (size_t i = 0; i < byAge.size() / 3; i++) {
                    SDL_DestroyTexture(app.covers[byAge[i].second].tex);
                    app.covers.erase(byAge[i].second);
                }
            }
            break;

        case Job::Feedback:
            break;

        case Job::Lyrics:
            if (r.trackId != app.lyricsId) break; // трек уже сменился
            app.lyricsLoading = false;
            if (r.ok) parseLyrics(app, r.lyrics, r.synced);
            if (app.lyrics.empty()) app.lyricsError = r.ok || r.error.empty() ? "У этого трека нет текста" : r.error;
            break;
    }
}

static void doAction(App &app, int act) {
    if (act >= ActKbdBase && act < ActRichBase) { // клавиша экранной клавиатуры (ряд * 100 + клавиша)
        int k = act - ActKbdBase;
        if (k == 99) app.kbd.active = false; // «Закрыть»
        else if (app.kbd.active) { app.kbd.row = k / 100; app.kbd.col = k % 100; kbdPress(app, k / 100, k % 100); }
        return;
    }
    if (act >= ActArtistBase && act < ActArtistBase + 40) { // тап по исполнителю в плеере
        if (hasCurrent(app)) openFromPlayer(app, app.queue[app.current], act - ActArtistBase);
        return;
    }
    if (act == ActExit) { app.quit = true; return; }
    if (act == ActLoginNew) { startLogin(app); return; }
    if (act == ActLoginCancel) { if (app.ready) app.loginFlow.active = false; return; }
    if (act == ActRetry) {
        app.started = false;
        app.startupError.clear();
        app.worker.post(Job{Job::Startup});
        return;
    }
    if (!app.ready) return;
    switch (act) {
        case ActPlay: {
            // Трек выбран, но не играет (восстановлен после выхода) — запускаем с запомненного места.
            Player::State st = app.player.state();
            bool idle = st == Player::Idle || st == Player::Error || st == Player::Finished;
            if (idle && !app.resolving && app.current >= 0 && app.current < (int)app.queue.size())
                playIndex(app, app.current);
            else
                app.player.togglePause();
            break;
        }
        case ActNext: skipCurrent(app); break;
        case ActPrev:
            // Как в плеерах: после 3 секунд — сначала, иначе предыдущий трек.
            if (app.player.position() > 3 || app.current <= 0) playIndex(app, app.current);
            else playIndex(app, app.current - 1);
            break;
        case ActNewWave: startWave(app); break;
        case ActLike: toggleLike(app); break;
        case ActLibrary: openLibrary(app); break;
        case ActBack: goBack(app); break;
        case ActHome: app.screen = ScrHome; break;
        case ActSettings: app.screen = ScrSettings; break;
        case ActQueue:
            app.queueView = ListView();
            app.screen = ScrQueue;
            break;
        case ActLyrics:
            if (app.screen == ScrLyrics) { app.screen = ScrPlayer; break; }
            if (app.current < 0 || app.current >= (int)app.queue.size()) break;
            if (app.lyrics.empty() && !app.lyricsLoading) app.lyricsId.clear(); // в прошлый раз не вышло — пробуем снова
            requestLyrics(app);
            app.lyricsManualUntil = 0;
            app.screen = ScrLyrics;
            break;
        case ActSeekBack: seekTo(app, app.player.position() - app.seekStep); break;
        case ActSeekFwd: seekTo(app, app.player.position() + app.seekStep); break;
        case ActShuffle: playList(app, 0, true); break;
        case ActSearch: openSearch(app); break;
        case ActTabYa: case ActTabSc: { // раздел главной: Яндекс / SoundCloud
            int tab = act == ActTabSc ? 1 : 0;
            if (app.homeTab != tab) app.homeSel = 0;
            app.homeTab = tab;
            if (app.sideFocus) app.sideSel = tab;
            break;
        }
        case ActOpenAlbum: if (hasCurrent(app)) openFromPlayer(app, app.queue[app.current], -2); break;
        case ActActivateSel: // тап по «Открыть/Играть» — как A на выбранной строке
            if (app.screen == ScrHome) activateHome(app, app.homeSel);
            else if (app.screen == ScrSettings || app.screen == ScrHomeSet) changeSetting(app, curSetting(app), 1);
            else if (app.screen == ScrTiles) toggleTile(app, app.tilesView.sel);
            else if (app.screen == ScrQueue) doAction(app, ActItemBase + app.queueView.sel);
            else if (app.screen == ScrLibrary) activateLibrary(app, app.libView.sel);
            else if (app.screen == ScrList && app.listView.sel < (int)app.listTracks.size()) playList(app, app.listView.sel, false);
            break;
        case ActToList:
            if (app.source == SrcList && !app.openList.title.empty()) app.screen = ScrList;
            break;
        case ActPlayer: app.screen = ScrPlayer; break;
        case ActShuffleMode: toggleShuffle(app); break;
        case ActRepeat: toggleRepeat(app); break;
        case ActSeekBar:
            if (app.current >= 0 && app.current < (int)app.queue.size()) {
                double frac = (double)(app.touchX - PLAYER_RX) / PLAYER_RW;
                frac = frac < 0 ? 0 : frac > 1 ? 1 : frac;
                seekTo(app, frac * app.queue[app.current].durationMs / 1000.0);
            }
            break;
        case ActDownload:
            if (app.current >= 0 && app.current < (int)app.queue.size()) downloadTracks(app, {app.queue[app.current]});
            break;
        case ActDownloadAll:
            if (app.openList.kind != OFFLINE_KIND) downloadTracks(app, app.listTracks);
            break;
        case ActDelete:
            if (app.screen == ScrList && app.openList.kind == OFFLINE_KIND && app.listView.sel < (int)app.listTracks.size()) {
                deleteOffline(app, app.listTracks[app.listView.sel].id);
                app.listTracks = offlineTracks(app);
                if (app.listTracks.empty()) app.listError = "Пока ничего не скачано — нажмите ZL на треке или в плейлисте";
            }
            break;
        default:
            if (act >= ActRichBase) { activateRich(app, act - ActRichBase); break; }
            if (act >= ActLyricBase) { // тап по строке текста — перемотка к ней
                int i = act - ActLyricBase;
                if (i < (int)app.lyrics.size() && app.lyrics[i].t >= 0) {
                    seekTo(app, app.lyrics[i].t);
                    app.lyricsManualUntil = 0;
                }
            } else if (act >= ActItemBase && app.screen == ScrQueue) { // очередь: строка 0 — текущий трек
                int idx = act - ActItemBase;
                app.queueView.sel = idx;
                if (idx > 0) playIndex(app, app.current + idx);
                app.screen = ScrPlayer;
            } else if (act >= ActItemBase) {
                int idx = act - ActItemBase;
                if (app.screen == ScrLibrary) { app.libView.sel = idx; activateLibrary(app, idx); }
                else if (app.screen == ScrList) { app.listView.sel = idx; playList(app, idx, false); }
            } else if (act >= ActSettingBase && act < ActItemBase) {
                curSetView(app).sel = act - ActSettingBase;
                changeSetting(app, curSetting(app), 1);
            } else if (act >= ActHomeBase && act < ActSettingBase) {
                int pos = act - ActHomeBase;
                if (app.screen == ScrTiles) toggleTile(app, pos);
                else activateHome(app, pos);
            } else if (act >= ActRowBase) {
                playIndex(app, app.current + (act - ActRowBase));
            }
    }
}

// ---------- отрисовка ----------

static SDL_Texture *coverTex(App &app, const std::string &url) {
    if (url.empty()) return nullptr;
    auto it = app.covers.find(url);
    if (it == app.covers.end()) { requestCover(app, url); return nullptr; }
    it->second.used = app.frame;
    return it->second.tex;
}

static void drawCover(App &app, const std::string &url, int x, int y, int size) {
    Ui &ui = app.ui;
    SDL_Texture *tex = coverTex(app, url);
    if (tex) {
        ui.image(tex, x, y, size, size);
    } else {
        ui.roundRect(x, y, size, size, std::max(4, size / 16), col::panel);
        ui.circle(x + size / 2, y + size / 2, std::max(3, size / 8), col::dim);
    }
}

static void addButton(App &app, int x, int y, int w, int h, int act) { app.buttons.push_back({{x, y, w, h}, act}); }

// Подсказка внизу; act — что сделать по тапу (подсказки работают как сенсорные кнопки).
static void drawHint(App &app, int &x, int y, const char *g, const char *label, int act = ActNone) {
    Ui &ui = app.ui;
    int x0 = x;
    x += ui.text(g, x, y - 2, 26, col::white, Align::Left, 0, true) + 8;
    x += ui.text(label, x, y, 20, col::gray) + 20;
    if (act != ActNone) addButton(app, x0 - 8, y - 14, x - x0, 48, act);
}

static void drawPlayer(App &app) {
    Ui &ui = app.ui;
    const Track &t = app.queue[app.current];

    // Обложка
    const int cx = 70, cy = 110;
    drawCover(app, coverFor(app, t, COVER_BIG), cx, cy, COVER_BIG);
    addButton(app, cx, cy, COVER_BIG, COVER_BIG, ActPlay);

    // Название, исполнитель, лайк
    const int rx = PLAYER_RX, rw = PLAYER_RW;
    bool isLiked = app.liked.count(t.id) > 0;
    int titleW = ui.text(t.title, rx, 120, 42, col::white, Align::Left, rw - 70);
    int artistsW = ui.text(t.artists, rx, 178, 26, col::gray, Align::Left, rw - 70);
    // Тап по названию — альбом трека, по имени исполнителя — его страница (каждое имя — своя кнопка).
    addButton(app, rx - 8, 116, titleW + 16, 54, ActOpenAlbum);
    if (t.artistNames.size() > 1) {
        int x = rx;
        for (size_t k = 0; k < t.artistNames.size() && x < rx + artistsW; k++) {
            int w = ui.textWidth(t.artistNames[k], 26);
            addButton(app, x - 4, 174, std::min(w, rx + artistsW - x) + 8, 42, ActArtistBase + (int)k);
            x += w + ui.textWidth(", ", 26);
        }
    } else {
        addButton(app, rx - 8, 174, artistsW + 16, 42, ActArtistBase);
    }
    ui.heart(rx + rw - 22, 152, 40, isLiked ? col::yellow : col::panel);
    addButton(app, rx + rw - 50, 120, 60, 60, ActLike);
    bool saved = findOffline(app, t.id) != nullptr, saving = app.dlQueued.count(t.id) > 0;
    ui.downArrow(rx + rw - 22, 205, 30, saved ? col::yellow : saving ? col::white : col::panel);
    addButton(app, rx + rw - 50, 185, 60, 45, ActDownload);

    // Прогресс
    double pos = app.player.position(), dur = t.durationMs / 1000.0;
    if (app.player.state() == Player::Idle && !app.resolving && app.pendingSeek >= 0) pos = app.pendingSeek; // на паузе после восстановления
    if (app.scrubFrac >= 0) pos = app.scrubFrac * dur; // палец на полосе — показываем, куда перемотаем
    double frac = dur > 0 ? pos / dur : 0;
    if (frac > 1) frac = 1;
    double dl = app.player.downloaded();
    const int by = BAR_Y, bh = 6;
    app.seekBar = {rx, by, rw, bh};
    addButton(app, rx - 10, by - 22, rw + 20, 46, ActSeekBar); // тап по полосе — перемотка
    ui.roundRect(rx, by, rw, bh, 3, col::panel);
    if (dl > 0) ui.roundRect(rx, by, (int)(rw * dl), bh, 3, col::dim);
    if (frac > 0) ui.roundRect(rx, by, (int)(rw * frac), bh, 3, col::yellow);
    ui.circle(rx + (int)(rw * frac), by + bh / 2, (app.scrubFrac >= 0 ? 13 : 9), col::white);
    ui.text(fmtTime(pos), rx, by + 18, 20, col::gray);
    ui.text(fmtTime(dur), rx + rw, by + 18, 20, col::gray, Align::Right);

    // Громкость — под обложкой: значок, ползунок, проценты.
    {
        const int vy = cy + COVER_BIG + 42, vx = cx + 44, vw = COVER_BIG - 44 - 70;
        SDL_Color ic = app.volume == 0 ? col::dim : col::gray;
        ui.rect(cx + 2, vy - 6, 9, 12, ic);
        ui.triangle(cx + 6, vy, cx + 22, vy - 14, cx + 22, vy + 14, ic);
        if (app.volume == 0) ui.line(cx + 27, vy - 8, cx + 37, vy + 8, 3, col::dim);
        else ui.line(cx + 28, vy - 6, cx + 28, vy + 6, 3, ic);
        float f = app.volume / 100.0f;
        ui.roundRect(vx, vy - 3, vw, 6, 3, col::panel);
        if (f > 0) ui.roundRect(vx, vy - 3, (int)(vw * f), 6, 3, app.volDrag ? col::yellow : col::white);
        ui.circle(vx + (int)(vw * f), vy, app.volDrag ? 12 : 8, col::white);
        ui.text(std::to_string(app.volume) + " %", cx + COVER_BIG, vy - 12, 20, col::gray, Align::Right);
        app.volBar = {vx, vy, vw, 6};
    }

    // Кнопки управления
    const int ctrlY = 350, mid = rx + rw / 2;
    Player::State st = app.player.state();
    bool playing = st == Player::Playing || st == Player::Buffering;
    ui.circle(mid - 130, ctrlY, 34, col::panel);
    ui.rect(mid - 146, ctrlY - 14, 5, 28, col::white);
    ui.triangle(mid - 114, ctrlY - 14, mid - 114, ctrlY + 14, mid - 138, ctrlY, col::white);
    addButton(app, mid - 170, ctrlY - 40, 80, 80, ActPrev);
    ui.circle(mid, ctrlY, 48, col::yellow);
    if (playing) {
        ui.rect(mid - 15, ctrlY - 18, 10, 36, col::black);
        ui.rect(mid + 5, ctrlY - 18, 10, 36, col::black);
    } else {
        ui.triangle(mid - 12, ctrlY - 20, mid - 12, ctrlY + 20, mid + 22, ctrlY, col::black);
    }
    addButton(app, mid - 50, ctrlY - 50, 100, 100, ActPlay);
    ui.circle(mid + 130, ctrlY, 34, col::panel);
    ui.triangle(mid + 114, ctrlY - 14, mid + 114, ctrlY + 14, mid + 138, ctrlY, col::white);
    ui.rect(mid + 141, ctrlY - 14, 5, 28, col::white);
    addButton(app, mid + 90, ctrlY - 40, 80, 80, ActNext);

    // Шафл (слева) и повтор (справа); включённые — жёлтые.
    {
        const float sx = mid - 215, ry = ctrlY;
        SDL_Color sc = app.shuffle ? col::yellow : app.source == SrcWave ? col::dim : col::gray;
        ui.line(sx - 14, ry - 9, sx + 8, ry + 9, 3.5f, sc);
        ui.line(sx - 14, ry + 9, sx + 8, ry - 9, 3.5f, sc);
        ui.triangle(sx + 6, ry + 2, sx + 6, ry + 16, sx + 16, ry + 9, sc);
        ui.triangle(sx + 6, ry - 16, sx + 6, ry - 2, sx + 16, ry - 9, sc);
        addButton(app, (int)sx - 32, (int)ry - 32, 64, 64, ActShuffleMode);

        const float px = mid + 215;
        SDL_Color pc = app.repeat != App::RepeatOff ? col::yellow : col::gray;
        ui.line(px - 15, ry - 8, px + 8, ry - 8, 3.5f, pc);
        ui.line(px + 15, ry - 8, px + 15, ry + 8, 3.5f, pc);
        ui.line(px + 15, ry + 8, px - 15, ry + 8, 3.5f, pc);
        ui.line(px - 15, ry + 8, px - 15, ry - 8, 3.5f, pc);
        ui.triangle(px + 4, ry - 15, px + 4, ry - 1, px + 13, ry - 8, pc);
        if (app.repeat == App::RepeatOne) {
            ui.circle((int)px + 17, (int)ry - 16, 9, col::yellow);
            ui.text("1", (int)px + 17, (int)ry - 27, 15, col::black, Align::Center);
        }
        addButton(app, (int)px - 32, (int)ry - 32, 64, 64, ActRepeat);
    }
    // Статус
    std::string status;
    if (app.resolving) status = "Загрузка трека…";
    else if (st == Player::Buffering) status = "Буферизация…";
    else if (st == Player::Paused) status = "Пауза";
    else if (st == Player::Error) status = "Ошибка воспроизведения";
    if (app.bitrate) status += (status.empty() ? "" : "   ·   ") + app.codec + " " + std::to_string(app.bitrate) + " kbps";
    else if (!app.codec.empty()) status += (status.empty() ? "" : "   ·   ") + app.codec;
    ui.text(status, mid, 418, 20, col::gray, Align::Center, rw);
    if (!app.error.empty())
        ui.text(app.error + (app.skipAt ? " — пропускаю" : ""), mid, 446, 20, col::red, Align::Center, rw);

    // Далее: один следующий трек и кнопка «Очередь» со всем списком.
    const int qy = 492;
    ui.text("Далее", rx, qy, 22, col::yellow);
    {
        const char *label = "Очередь";
        int bw = ui.textWidth(label, 20) + 56, bx = rx + rw - bw;
        ui.roundRect(bx, qy - 6, bw, 38, 19, col::panel);
        for (int i = 0; i < 3; i++) ui.rect(bx + 16, qy + 4 + i * 8, 16, 3, col::yellow); // значок списка
        ui.text(label, bx + 40, qy + 1, 20, col::white);
        addButton(app, bx - 6, qy - 12, bw + 12, 50, ActQueue);
        // «Текст» — левее «Очереди».
        const char *tl = "Текст";
        int tw = ui.textWidth(tl, 20) + 56, tx = bx - 14 - tw;
        ui.roundRect(tx, qy - 6, tw, 38, 19, col::panel);
        ui.text("“", tx + 18, qy - 2, 30, col::yellow); // значок кавычек
        ui.text(tl, tx + 40, qy + 1, 20, col::white);
        addButton(app, tx - 6, qy - 12, tw + 12, 50, ActLyrics);
    }
    int ny = qy + 40, k = app.current + 1;
    if (k < (int)app.queue.size()) {
        const Track &n = app.queue[k];
        drawCover(app, coverFor(app, n, COVER_SMALL), rx, ny, 48);
        ui.text(n.title, rx + 62, ny + 2, 22, col::white, Align::Left, rw - 62);
        ui.text(n.artists, rx + 62, ny + 28, 17, col::gray, Align::Left, rw - 62);
        addButton(app, rx, ny - 4, rw, 56, ActRowBase + 1);
    } else if (app.source == SrcWave && app.waveLoading) {
        ui.text("Подгружаю волну…", rx, ny + 12, 20, col::dim);
    } else {
        ui.text(app.repeat == App::RepeatAll ? "Плейлист начнётся сначала" : "Это последний трек", rx, ny + 12, 20, col::dim);
    }
}

// Общая отрисовка строки списка; возвращает true, если строка видима.
static void drawRow(App &app, int y, int h, bool selected, bool playing, const std::string &cover,
                    const std::string &title, const std::string &sub, const std::string &right, int act,
                    bool liked = false, bool saved = false) {
    Ui &ui = app.ui;
    const int x = 70, w = Ui::W - 140;
    if (selected) ui.roundRect(x - 12, y - 4, w + 24, h, 10, col::panel);
    int thumb = h - 16;
    drawCover(app, cover, x, y + 4, thumb);
    int tx = x + thumb + 18, tw = w - thumb - 18 - (right.empty() ? 0 : 90);
    if (liked) { ui.heart(tx + 9, y + 20, 18, col::yellow); tx += 26; tw -= 26; }
    ui.text(title, tx, y + 6, 23, playing ? col::yellow : col::white, Align::Left, tw);
    ui.text(sub, tx, y + 34, 18, col::gray, Align::Left, tw);
    if (!right.empty()) ui.text(right, x + w, y + h / 2 - 14, 20, col::gray, Align::Right);
    if (saved) ui.downArrow(x + w - 80, y + h / 2 - 2, 18, col::yellow);
    addButton(app, x - 12, y - 4, w + 24, h, act);
}

static void drawLibrary(App &app) {
    Ui &ui = app.ui;
    ui.text("Библиотека", 70, 92, 22, col::gray);
    const int rowH = LIB_ROW_H, visible = (LIST_BOTTOM - LIST_TOP) / rowH;
    int count = libraryCount(app);
    app.libView.clamp(count, visible);

    for (int i = app.libView.scroll; i < count && i < app.libView.scroll + visible; i++) {
        int y = LIST_TOP + (i - app.libView.scroll) * rowH;
        bool sel = i == app.libView.sel;
        switch (libKind(app, i)) {
            case LibSearch:
                drawRow(app, y, rowH - 6, sel, false, "", "Поиск",
                        app.lastQuery.empty() ? "Треки и исполнители" : "Последний запрос: " + app.lastQuery, "", ActItemBase + i);
                ui.circle(70 + (rowH - 22) / 2 - 3, y + 4 + (rowH - 22) / 2 - 3, 10, col::yellow); // лупа
                ui.circle(70 + (rowH - 22) / 2 - 3, y + 4 + (rowH - 22) / 2 - 3, 6, col::panel);
                ui.triangle(70 + (rowH - 22) / 2 + 3, y + 4 + (rowH - 22) / 2 + 1, 70 + (rowH - 22) / 2 + 7, y + 4 + (rowH - 22) / 2 - 3,
                            70 + (rowH - 22) / 2 + 13, y + 4 + (rowH - 22) / 2 + 9, col::yellow);
                break;
            case LibWave:
                drawRow(app, y, rowH - 6, sel, app.source == SrcWave && app.current >= 0, "", "Моя волна",
                        "Бесконечный поток под ваш вкус", "", ActItemBase + i);
                break;
            case LibLikes:
                drawRow(app, y, rowH - 6, sel, app.source == SrcList && app.sourceTitle == "Мне нравится", "",
                        "Мне нравится", app.libLoaded ? tracksWord((int)app.liked.size()) : "…", "", ActItemBase + i);
                break;
            case LibOffline:
                drawRow(app, y, rowH - 6, sel, app.source == SrcList && app.sourceTitle == "Скачанное", "", "Скачанное",
                        app.offline.empty() ? "Слушайте без интернета" : tracksWord((int)app.offline.size()) + " на карте",
                        "", ActItemBase + i);
                ui.downArrow(70 + (rowH - 22) / 2, y + 4 + (rowH - 22) / 2, 26, col::yellow); // вместо обложки
                break;
            case LibAlbums:
                drawRow(app, y, rowH - 6, sel, false, "", "Мои альбомы", "Альбомы, которые вы отметили", "", ActItemBase + i);
                ui.roundRect(70 + 14, y + 12, rowH - 40, rowH - 40, 6, col::yellow); // значок: пластинка
                ui.circle(70 + 14 + (rowH - 40) / 2, y + 12 + (rowH - 40) / 2, 6, sel ? col::panel : col::bg);
                break;
        case LibPlaylist: {
                const Playlist &p = app.playlists[i - LIB_FIXED];
                drawRow(app, y, rowH - 6, sel, app.source == SrcList && app.sourceTitle == p.title,
                        p.coverUrl(COVER_SMALL), p.title, tracksWord(p.trackCount), "", ActItemBase + i);
                break;
            }
        }
    }
    if (app.offlineMode) ui.text("Нет сети — показано только скачанное", Ui::W / 2, LIST_BOTTOM - 30, 20, col::gray, Align::Center);
    if (app.libLoading) ui.text("Загружаю плейлисты…", Ui::W / 2, LIST_BOTTOM - 30, 20, col::dim, Align::Center);
    if (!app.libError.empty()) ui.text(app.libError, Ui::W / 2, LIST_BOTTOM - 30, 20, col::red, Align::Center, 1100);
}

static void drawList(App &app) {
    Ui &ui = app.ui;
    std::string info = !app.listLoading ? tracksWord((int)app.listTracks.size()) + (app.listMore ? " · гружу ещё…" : "")
                       : app.openList.kind == SEARCH_KIND ? "Ищу «" + app.lastQuery + "»…" : "Загружаю…";
    ui.text(info, 70, 92, 22, col::gray);
    if (!app.listTracks.empty()) {
        // Кнопки справа налево: «Перемешать», «Скачать всё» (не для «Скачанного»).
        int bx = Ui::W - 70;
        auto pill = [&](const char *g, const char *label, SDL_Color bg, int act) {
            int w = ui.textWidth(label, 20) + 70;
            ui.roundRect(bx - w, 86, w, 40, 20, bg);
            // На акценте — цвет «поверх акцента», на панели — обычный текст (иначе в светлой теме не видно).
            SDL_Color fg = bg.r == col::yellow.r && bg.g == col::yellow.g && bg.b == col::yellow.b ? col::black : col::white;
            ui.text(g, bx - w + 14, 89, 26, fg, Align::Left, 0, true);
            ui.text(label, bx - 22, 94, 20, fg, Align::Right);
            addButton(app, bx - w, 86, w, 40, act);
            bx -= w + 12;
        };
        pill(glyph::Y, "Перемешать", col::yellow, ActShuffle);
        if (app.openList.kind != OFFLINE_KIND && !app.offlineMode) pill(glyph::ZL, "Скачать всё", col::panel, ActDownloadAll);
    }

    const int rowH = LIST_ROW_H, visible = (LIST_BOTTOM - LIST_TOP) / rowH;
    int count = (int)app.listTracks.size();
    app.listView.clamp(count, visible);
    std::string playingId =
        app.current >= 0 && app.current < (int)app.queue.size() ? app.queue[app.current].id : std::string();
    for (int i = app.listView.scroll; i < count && i < app.listView.scroll + visible; i++) {
        const Track &t = app.listTracks[i];
        int y = LIST_TOP + (i - app.listView.scroll) * rowH;
        drawRow(app, y, rowH - 6, i == app.listView.sel, t.id == playingId, coverFor(app, t, COVER_SMALL), t.title, t.artists,
                fmtTime(t.durationMs / 1000.0), ActItemBase + i, app.liked.count(t.id) > 0,
                app.openList.kind != OFFLINE_KIND && findOffline(app, t.id) != nullptr);
    }
    if (!app.listError.empty()) ui.text(app.listError, Ui::W / 2, 330, 24, col::gray, Align::Center, 1100);
    // полоса прокрутки
    if (count > visible) {
        int trackH = LIST_BOTTOM - LIST_TOP, barH = std::max(30, trackH * visible / count);
        int barY = LIST_TOP + (trackH - barH) * app.listView.scroll / std::max(1, count - visible);
        ui.roundRect(Ui::W - 40, barY, 5, barH, 2, col::dim);
    }
}

// Мини-плеер внизу экранов библиотеки.
static void drawMiniPlayer(App &app) {
    if (app.current < 0 || app.current >= (int)app.queue.size()) return;
    Ui &ui = app.ui;
    const Track &t = app.queue[app.current];
    int y = 82, x = Ui::W / 2 - 40;
    std::string s = t.title + " — " + t.artists;
    if (app.player.state() == Player::Paused) {
        ui.rect(x - 26, y + 15, 4, 16, col::yellow);
        ui.rect(x - 18, y + 15, 4, 16, col::yellow);
    } else {
        ui.triangle(x - 26, y + 14, x - 26, y + 32, x - 12, y + 23, col::yellow);
    }
    ui.text(s, x, y + 10, 20, col::white, Align::Left, 300);
}

// ---------- главная, настройки, очередь ----------

static const int SIDE_W = 290; // боковое меню главной: Яндекс, SoundCloud, Настройки
static const int HOME_X = SIDE_W + 34, HOME_R = 70, HOME_TOP = 104, HOME_BOTTOM = LIST_BOTTOM, HOME_GAP = 20;
static const int ROW_H = 186, ROW_ITEM = 124, ROW_STEP = 160; // ряд снизу: высота, обложка, шаг

static float animTime() { return armTicksToNs(armGetSystemTick()) / 1e9f; }

static void drawTileIcon(Ui &ui, int tile, int x, int y, SDL_Color c, SDL_Color hole) {
    if (tile == TileScFeed) { // лента: три полосы
        for (int i = 0; i < 3; i++) ui.rect(x + 2, y + 2 + i * 16, 58, 9, c);
        return;
    }
    if (tile == TileScWave) tile = TileWave; // остальные плитки SoundCloud — с теми же значками
    else if (tile == TileScLikes) tile = TileLikes;
    else if (tile == TileScLists) tile = TileLists;
    else if (tile == TileScAccount) tile = TileSoundCloud;
    switch (tile) {
        case TileWave: case TileWaveUp: case TileWaveCalm: case TileWaveNew: // синусоида
            for (int i = 0; i < 24; i++) {
                float t0 = i / 24.0f, t1 = (i + 1) / 24.0f;
                ui.line(x + t0 * 64, y + 22 - 16 * SDL_sinf(t0 * 12.566f), x + t1 * 64, y + 22 - 16 * SDL_sinf(t1 * 12.566f), 5, c);
            }
            break;
        case TileLikes: ui.heart(x + 30, y + 24, 56, c); break;
        case TileLists:
            for (int i = 0; i < 3; i++) { ui.circle(x + 6, y + 8 + i * 16, 5, c); ui.rect(x + 18, y + 5 + i * 16, 44, 6, c); }
            break;
        case TileOffline: ui.downArrow(x + 30, y + 22, 44, c); break;
        case TileSoundCloud: // облако
            ui.circle(x + 18, y + 30, 12, c);
            ui.circle(x + 34, y + 22, 17, c);
            ui.circle(x + 50, y + 30, 12, c);
            ui.rect(x + 18, y + 30, 32, 12, c);
            break;
        case TileContinue: // круг со стрелкой и «играть» внутри
            for (int i = 0; i < 20; i++) {
                float a0 = 0.6f + i * 0.26f, a1 = a0 + 0.26f;
                ui.line(x + 30 + 22 * SDL_cosf(a0), y + 22 + 22 * SDL_sinf(a0), x + 30 + 22 * SDL_cosf(a1), y + 22 + 22 * SDL_sinf(a1), 5, c);
            }
            ui.triangle(x + 46, y + 4, x + 56, y + 16, x + 42, y + 18, c);
            ui.triangle(x + 25, y + 12, x + 25, y + 32, x + 40, y + 22, c);
            break;
        case TileSettings: // шестерёнка
            for (int i = 0; i < 8; i++) {
                float a = i * 0.785f;
                ui.line(x + 30 + 18 * SDL_cosf(a), y + 22 + 18 * SDL_sinf(a), x + 30 + 27 * SDL_cosf(a), y + 22 + 27 * SDL_sinf(a), 9, c);
            }
            ui.circle(x + 30, y + 22, 19, c);
            ui.circle(x + 30, y + 22, 8, hole);
            break;
    }
}

// Цвета плиток по настройке «Стиль плиток».
struct TileLook { SDL_Color bg, fg, sub, icon, ring; int rad; };
static TileLook tileLook(const App &app) {
    TileLook l{col::panel, col::white, col::gray, col::yellow, col::yellow, app.squareTiles ? 4 : 14};
    if (app.tileStyle == 1) l = {col::yellow, col::black, col::black, col::black, col::white, l.rad};
    else if (app.tileStyle == 2) l.bg = col::bg; // контур рисуется отдельно
    return l;
}

// Станции волны — свои цвета, чтобы их было видно среди плиток.
static bool stationColor(int id, SDL_Color &c) {
    if (id == TileWaveUp) c = {255, 149, 64, 255};
    else if (id == TileWaveCalm) c = {48, 200, 190, 255};
    else if (id == TileWaveNew) c = {155, 110, 255, 255};
    else if (id == TileScWave) c = {255, 110, 30, 255}; // оранжевый SoundCloud
    else return false;
    return true;
}

// Подложка плитки: фон, рамка выбора (с «дыханием», если включены анимации), контур.
static void tileBase(App &app, const TileLook &L, int x, int y, int w, int h, bool sel, const SDL_Color *fill = nullptr) {
    Ui &ui = app.ui;
    if (sel) {
        int grow = app.anim ? 4 + (int)(2 * (1 + SDL_sinf(animTime() * 4))) : 4;
        ui.roundRect(x - grow, y - grow, w + 2 * grow, h + 2 * grow, L.rad + grow, L.ring);
    }
    if (fill) {
        ui.roundRect(x, y, w, h, L.rad, *fill);
    } else if (app.tileStyle == 2) {
        ui.roundRect(x, y, w, h, L.rad, sel ? col::yellow : col::panel);
        ui.roundRect(x + 2, y + 2, w - 4, h - 4, std::max(2, L.rad - 2), col::bg);
    } else {
        ui.roundRect(x, y, w, h, L.rad, L.bg);
    }
}

// Столбики эквалайзера: живые, пока играет (если включены анимации).
static void drawEq(App &app, int x, int y, SDL_Color c, bool playing) {
    static const float speed[] = {7.0f, 5.3f, 8.6f, 6.1f};
    for (int i = 0; i < 4; i++) {
        int h = 6;
        if (playing && app.anim) h = 5 + (int)(12 * (0.5f + 0.5f * SDL_sinf(animTime() * speed[i] + i)));
        else if (playing) h = 8 + i % 2 * 6;
        app.ui.rect(x + i * 6, y + 18 - h, 4, h, c);
    }
}

// «Сейчас играет»: обложка во всю плитку, крупно (обложка сверху), средне (слева) или строкой.
static void drawNowTile(App &app, const TileLook &L, int x, int y, int w, int h) {
    Ui &ui = app.ui;
    bool has = hasCurrent(app);
    SDL_Color accent = app.tileStyle == 1 ? L.fg : col::yellow;
    if (!has) {
        if (h >= 120) ui.text("Сейчас играет", x + 22, y + 16, 20, accent);
        ui.text("Ничего не играет", x + 22, y + h / 2 - (h >= 120 ? 4 : 14), 24, L.fg, Align::Left, w - 44);
        if (h >= 120 && app.subtitles) ui.text("Выберите волну или плейлист", x + 22, y + h / 2 + 30, 18, L.sub, Align::Left, w - 44);
        return;
    }
    const Track &t = app.queue[app.current];
    Player::State st = app.player.state();
    bool playing = st == Player::Playing || st == Player::Buffering;
    double pos = app.player.position();
    if (st == Player::Idle && app.pendingSeek >= 0) pos = app.pendingSeek;
    std::string time = fmtTime(pos) + " / " + fmtTime(t.durationMs / 1000.0);
    auto playIcon = [&](int px, int py, SDL_Color c) {
        if (playing) { ui.rect(px, py, 5, 18, c); ui.rect(px + 9, py, 5, 18, c); }
        else ui.triangle(px, py - 1, px, py + 19, px + 16, py + 9, c);
    };

    if (app.nowCover && h >= 120) { // обложка во всю плитку, текст поверх затемнения
        int side = std::max(w, h);
        SDL_Rect clip{x, y, w, h};
        SDL_RenderSetClipRect(ui.renderer(), &clip);
        drawCover(app, coverFor(app, t, COVER_BIG), x + (w - side) / 2, y + (h - side) / 2, side);
        for (int i = 0; i < 12; i++) // снизу темнее — чтобы читался текст
            ui.rect(x, y + h - 120 + i * 10, w, 10, SDL_Color{0, 0, 0, (u8)(i * 18)});
        SDL_RenderSetClipRect(ui.renderer(), nullptr);
        SDL_Color white{245, 245, 245, 255}, light{210, 210, 215, 255};
        ui.text("Сейчас играет", x + 20, y + h - 104, 18, col::yellow);
        ui.text(t.title, x + 20, y + h - 80, 26, white, Align::Left, w - 40);
        ui.text(t.artists + "  ·  " + time, x + 20, y + h - 44, 18, light, Align::Left, w - 40);
        drawEq(app, x + w - 44, y + 16, col::yellow, playing);
        return;
    }
    if (w >= 400 && h >= 380) { // большая: обложка сверху во всю ширину
        int cs = std::min(w - 48, h - 160);
        drawCover(app, coverFor(app, t, COVER_BIG), x + (w - cs) / 2, y + 24, cs);
        int ty = y + 24 + cs + 16;
        ui.text(t.title, x + 24, ty, 30, L.fg, Align::Left, w - 48);
        ui.text(t.artists, x + 24, ty + 42, 20, L.sub, Align::Left, w - 48);
        playIcon(x + 24, ty + 84, accent);
        ui.text(time, x + 50, ty + 80, 18, L.sub);
        drawEq(app, x + w - 50, ty + 80, accent, playing);
        return;
    }
    if (h >= 120) { // средняя: обложка слева
        ui.text("Сейчас играет", x + 22, y + 16, 20, accent);
        drawEq(app, x + w - 46, y + 16, accent, playing);
        int cs = std::min({h - 76, w * 2 / 5, 140});
        drawCover(app, coverFor(app, t, COVER_SMALL), x + 22, y + 54, cs);
        int tx = x + 22 + cs + 16, tw = x + w - 18 - tx;
        ui.text(t.title, tx, y + 58, 24, L.fg, Align::Left, tw);
        if (app.subtitles) ui.text(t.artists, tx, y + 92, 18, L.sub, Align::Left, tw);
        if (y + 54 + cs - 24 > y + 120) {
            playIcon(tx, y + 54 + cs - 22, accent);
            ui.text(time, tx + 26, y + 54 + cs - 26, 18, L.sub);
        }
        return;
    }
    // строка
    int cs = h - 16;
    drawCover(app, coverFor(app, t, COVER_SMALL), x + 10, y + 8, cs);
    int tx = x + 10 + cs + 14;
    playIcon(tx, y + h / 2 - 9, accent);
    ui.text(t.title + " — " + t.artists, tx + 28, y + h / 2 - 13, 22, L.fg, Align::Left, w - (tx + 28 - x) - 150);
    ui.text(time, x + w - 18, y + h / 2 - 11, 18, L.sub, Align::Right);
}

// Закладка: обложка плейлиста и его название.
static void drawBookmarkTile(App &app, int b, int x, int y, int w, int h, bool sel, int item) {
    Ui &ui = app.ui;
    TileLook L = tileLook(app);
    tileBase(app, L, x, y, w, h, sel);
    addButton(app, x, y, w, h, ActHomeBase + item);
    const Playlist &p = app.bookmarks[b];
    bool likes = p.kind.empty(), offline = p.kind == OFFLINE_KIND;
    int cs = h < 100 ? h - 16 : std::min(h - 40, w * 2 / 5);
    int cx = x + (h < 100 ? 10 : 20), cy = y + (h - cs) / 2;
    if (likes || offline || p.coverUri.empty()) {
        ui.roundRect(cx, cy, cs, cs, 8, app.tileStyle == 1 ? col::black : col::bg);
        if (likes) ui.heart(cx + cs / 2, cy + cs / 2, cs / 2, col::yellow);
        else ui.downArrow(cx + cs / 2, cy + cs / 2, cs / 2, col::yellow);
    } else {
        drawCover(app, p.coverUrl(200), cx, cy, cs);
    }
    int tx = cx + cs + 16, tw = x + w - 16 - tx;
    ui.text(p.title, tx, y + h / 2 - (app.subtitles && h >= 100 ? 30 : 14), h >= 100 ? 24 : 22, L.fg, Align::Left, tw);
    if (app.subtitles && h >= 100)
        ui.text(likes ? "Мне нравится" : offline ? "Скачанное" : tracksWord(p.trackCount), tx, y + h / 2 + 4, 18, L.sub, Align::Left, tw);
}

static void drawTile(App &app, int id, int x, int y, int w, int h, bool sel, int item) {
    Ui &ui = app.ui;
    TileLook L = tileLook(app);
    SDL_Color station;
    bool isStation = stationColor(id, station);
    if (isStation) L.fg = L.sub = L.icon = SDL_Color{20, 20, 22, 255}; // на цветной плитке — тёмный текст
    tileBase(app, L, x, y, w, h, sel, isStation ? &station : nullptr);
    addButton(app, x, y, w, h, ActHomeBase + item);
    if (id == TileNow) { drawNowTile(app, L, x, y, w, h); return; }

    bool disabled = app.offlineMode && (id == TileWave || id == TileLikes || id == TileLists || isStation ||
                                         (id >= TileScWave && id != TileScAccount));
    SDL_Color fg = disabled ? col::dim : L.fg, icon = disabled ? col::dim : L.icon;
    SDL_Color hole = isStation ? station : app.tileStyle == 1 ? col::yellow : app.tileStyle == 2 ? col::bg : col::panel;
    std::string sub;
    if (id == TileWave) sub = disabled ? "Нужна сеть" : "Бесконечный поток под ваш вкус";
    else if (id == TileLikes) sub = disabled ? "Нужна сеть" : app.libLoaded ? tracksWord((int)app.liked.size()) : "…";
    else if (id == TileLists) sub = disabled ? "Нужна сеть" : app.libLoaded ? std::to_string(app.playlists.size()) + " плейлистов" : "…";
    else if (id == TileOffline) sub = app.offline.empty() ? "Слушайте без интернета" : tracksWord((int)app.offline.size()) + " на карте";
    else if (id == TileSettings) sub = "Перемотка, оформление";
    else if (id == TileWaveUp) sub = disabled ? "Нужна сеть" : "Энергичнее и громче";
    else if (id == TileWaveCalm) sub = disabled ? "Нужна сеть" : "Для фона и работы";
    else if (id == TileWaveNew) sub = disabled ? "Нужна сеть" : "Новое для вас";
    else if (id >= TileScWave) {
        bool in = !app.scUser.empty();
        if (id == TileScAccount) sub = app.scLogin.running() ? "Идёт вход…" : in ? app.scUser : "Войти в SoundCloud";
        else if (disabled) sub = "Нужна сеть";
        else if (!in) sub = "Нужен вход в аккаунт";
        else sub = id == TileScWave ? "Похожее на ваши лайки" : id == TileScLikes ? "Лайки SoundCloud"
                 : id == TileScFeed ? "Новое от подписок" : "Ваши и сохранённые";
    }
    else if (id == TileContinue) {
        if (hasCurrent(app)) sub = app.queue[app.current].title + " · " + fmtTime(app.player.state() == Player::Idle && app.pendingSeek >= 0 ? app.pendingSeek : app.player.position());
        else sub = app.recent.empty() ? "Пока нечего продолжать" : app.recent[0].title;
    }

    if (h < 100) { // строка списка: значок слева, справа подпись
        drawTileIcon(ui, id, x + 18, y + h / 2 - 22, icon, hole);
        ui.text(tileName(id), x + 104, y + h / 2 - 16, 26, fg, Align::Left, w / 2);
        if (app.subtitles) ui.text(sub, x + w - 22, y + h / 2 - 11, 18, L.sub, Align::Right, w / 2 - 110);
        return;
    }
    if (h < 150) { // низкая плитка (4 ряда): над названием иконке места нет — ставим её слева
        drawTileIcon(ui, id, x + 22, y + h / 2 - 22, icon, hole);
        int ty2 = app.subtitles ? y + h / 2 - 30 : y + h / 2 - 15;
        ui.text(tileName(id), x + 104, ty2, 24, fg, Align::Left, w - 124);
        if (app.subtitles) ui.text(sub, x + 104, ty2 + 34, 18, L.sub, Align::Left, w - 124);
        return;
    }
    drawTileIcon(ui, id, x + 24, y + (h >= 200 ? 32 : 20), icon, hole);
    int ty = y + h - (app.subtitles ? (h >= 200 ? 100 : 74) : (h >= 200 ? 62 : 48));
    ui.text(tileName(id), x + 24, ty, h >= 200 ? 28 : 24, fg, Align::Left, w - 48);
    if (app.subtitles) ui.text(sub, x + 24, ty + (h >= 200 ? 42 : 34), 18, L.sub, Align::Left, w - 48);
}

static int rowCount(const App &app, int mode) {
    int n = mode == 1 ? (int)app.recent.size() : mode == 2 ? (app.libLoaded ? (int)app.playlists.size() + 1 : 0) : 0;
    return std::min(n, 40); // номера кнопок главной — до ActSettingBase
}

// Ряд обложек на главной: mode 1 — недавно играло, 2 — мои плейлисты. scroll — своя прокрутка у каждого ряда.
static void drawCoverRow(App &app, int mode, int y, int &scroll) {
    Ui &ui = app.ui;
    bool recent = mode == 1;
    ui.text(recent ? "Недавно играло" : "Мои плейлисты", HOME_X, y, 22, col::gray);
    int n = rowCount(app, mode);
    if (n == 0) {
        ui.text(recent ? "Здесь появятся треки, которые вы слушали" : app.offlineMode ? "Нужна сеть" : "Загружаю…",
                HOME_X, y + 70, 20, col::dim);
        return;
    }
    const int visible = (Ui::W - HOME_X - HOME_R + HOME_GAP) / ROW_STEP;
    int first = (int)app.homeItems.size(), selInRow = app.homeSel - first;
    if (selInRow >= 0 && selInRow < n) { // выбор в этом ряду — держим его в видимой части
        if (selInRow < scroll) scroll = selInRow;
        if (selInRow >= scroll + visible) scroll = selInRow - visible + 1;
    }
    scroll = std::max(0, std::min(scroll, std::max(0, n - visible)));
    TileLook L = tileLook(app);
    for (int k = 0; k < n; k++) {
        int x = HOME_X + (k - scroll) * ROW_STEP, cy = y + 36;
        SDL_Rect rc{x, cy, ROW_ITEM, ROW_ITEM + 30};
        int id = recent ? k : k - 1; // в плейлистах первым — «Мне нравится»
        app.homeItems.push_back({recent ? 1 : 2, id, rc});
        int idx = (int)app.homeItems.size() - 1;
        if (k < scroll || k >= scroll + visible) continue;
        bool sel = idx == app.homeSel;
        if (sel) ui.roundRect(x - 4, cy - 4, ROW_ITEM + 8, ROW_ITEM + 8, L.rad + 4, L.ring);
        std::string title;
        if (recent) {
            const Track &t = app.recent[k];
            drawCover(app, coverFor(app, t, COVER_SMALL), x, cy, ROW_ITEM);
            title = t.title;
        } else if (id < 0) {
            ui.roundRect(x, cy, ROW_ITEM, ROW_ITEM, L.rad, col::panel);
            ui.heart(x + ROW_ITEM / 2, cy + ROW_ITEM / 2, 60, col::yellow);
            title = "Мне нравится";
        } else {
            const Playlist &p = app.playlists[id];
            drawCover(app, p.coverUrl(200), x, cy, ROW_ITEM);
            title = p.title;
        }
        ui.text(title, x, cy + ROW_ITEM + 6, 17, sel ? col::white : col::gray, Align::Left, ROW_ITEM);
        addButton(app, x, cy, ROW_ITEM, ROW_ITEM + 30, ActHomeBase + idx);
    }
}

// Пункты бокового меню: Яндекс, SoundCloud (если настроен), Настройки.
static std::vector<int> sideItems() {
    if (hasScTab()) return {0, 1, 2};
    return {0, 2};
}
static const SDL_Color SC_ORANGE{255, 110, 30, 255};

// Боковое меню главной: разделы сверху, общие настройки — внизу.
static void drawSidebar(App &app) {
    Ui &ui = app.ui;
    SDL_Color side{(u8)((col::bg.r + col::panel.r) / 2), (u8)((col::bg.g + col::panel.g) / 2),
                   (u8)((col::bg.b + col::panel.b) / 2), 255};
    ui.rect(0, 0, SIDE_W, HOME_BOTTOM, side);
    ui.text("YaMu-nx", 34, 36, 32, col::yellow);
    for (int item : sideItems()) {
        int y = item == 2 ? HOME_BOTTOM - 96 : 106 + item * 76;
        bool active = item == 2 ? app.screen == ScrSettings : app.homeTab == item;
        bool focus = app.sideFocus && app.sideSel == item;
        SDL_Color accent = item == 1 ? SC_ORANGE : col::yellow;
        if (focus) {
            ui.roundRect(14, y - 4, SIDE_W - 28, 68, 14, col::yellow);
            ui.roundRect(17, y - 1, SIDE_W - 34, 62, 12, col::panel);
        } else if (active) ui.roundRect(14, y - 4, SIDE_W - 28, 68, 14, col::panel);
        if (active) ui.rect(14, y + 12, 5, 36, accent);
        SDL_Color ic = active || focus ? accent : col::gray;
        int tile = item == 0 ? TileWave : item == 1 ? TileSoundCloud : TileSettings;
        { // значок поменьше: те же фигуры, что на плитках, в масштабе 0.6
            const float s = 0.6f;
            float bx = 1, by = 1; // в доке масштаб уже не 1 (1080p) — домножаем и возвращаем как было
            SDL_RenderGetScale(ui.renderer(), &bx, &by);
            SDL_RenderSetScale(ui.renderer(), bx * s, by * s);
            drawTileIcon(ui, tile, (int)(38 / s), (int)((y + 16) / s), ic, active || focus ? col::panel : side);
            SDL_RenderSetScale(ui.renderer(), bx, by);
        }
        ui.text(item == 0 ? "Яндекс" : item == 1 ? "SoundCloud" : "Настройки", 92, y + 14, 26,
                active || focus ? col::white : col::gray, Align::Left, SIDE_W - 104);
        addButton(app, 14, y - 4, SIDE_W - 28, 68, item == 0 ? ActTabYa : item == 1 ? ActTabSc : ActSettings);
    }
}

// Главная: плитки и закладки в выбранной раскладке, под ними — ряды обложек (если включены).
static void drawHome(App &app) {
    drawSidebar(app);
    std::vector<int> tiles = homeTiles(app);
    app.homeItems.clear();
    // Ячейки: плитки, затем закладки (kind 3).
    struct Cell { int kind, id; };
    std::vector<Cell> cells;
    for (int t : tiles) cells.push_back({0, t});
    if (app.homeTab == 0 || !hasScTab()) // закладки — на вкладке Яндекса
        for (int b = 0; b < (int)app.bookmarks.size(); b++) cells.push_back({3, b});

    int rows = (app.bottomRow ? 1 : 0) + (app.bottomRow2 ? 1 : 0);
    const int areaW = Ui::W - HOME_X - HOME_R;
    const int bottom = HOME_BOTTOM - rows * ROW_H;
    const int areaH = bottom - HOME_TOP;
    const int maxH = app.tileCompact ? 160 : 230;
    std::vector<SDL_Rect> rects;

    auto grid = [&](int x0, int y0, int w, int h, int cols, int from) {
        int n = (int)cells.size() - from;
        int rws = std::max(1, (n + cols - 1) / cols);
        int tw = (w - (cols - 1) * HOME_GAP) / cols;
        int th = std::min(maxH, (h - (rws - 1) * HOME_GAP) / rws);
        th = std::max(th, 60);
        for (int i = 0; i < n; i++)
            rects.push_back({x0 + (i % cols) * (tw + HOME_GAP), y0 + (i / cols) * (th + HOME_GAP), tw, th});
    };

    grid(HOME_X, HOME_TOP, areaW, areaH, app.columns, 0);

    // Не влезшие плитки не рисуем и не даём на них встать (раньше выбор уходил на невидимую плитку).
    for (size_t i = 0; i < cells.size(); i++)
        if (rects[i].y + rects[i].h <= bottom + 4) app.homeItems.push_back({cells[i].kind, cells[i].id, rects[i]});
    int cellCount = (int)app.homeItems.size();
    int total = cellCount + (app.bottomRow ? rowCount(app, app.bottomRow) : 0) + (app.bottomRow2 ? rowCount(app, app.bottomRow2) : 0);
    app.homeSel = std::max(0, std::min(app.homeSel, total - 1));
    for (int i = 0; i < cellCount; i++) {
        const App::HomeItem &it = app.homeItems[i];
        const SDL_Rect &r = it.rc;
        if (it.kind == 3) drawBookmarkTile(app, it.id, r.x, r.y, r.w, r.h, i == app.homeSel && !app.sideFocus, i);
        else drawTile(app, it.id, r.x, r.y, r.w, r.h, i == app.homeSel && !app.sideFocus, i);
    }
    int ry = bottom + 8;
    if (app.bottomRow) { drawCoverRow(app, app.bottomRow, ry, app.rowScroll); ry += ROW_H; }
    if (app.bottomRow2) drawCoverRow(app, app.bottomRow2, ry, app.rowScroll2);
}

// «Доброе утро / день / вечер / ночи» по часам консоли.
static std::string greeting(const App &app) {
    u64 now = 0;
    TimeCalendarTime cal{};
    TimeCalendarAdditionalInfo info{};
    int hour = 12;
    if (R_SUCCEEDED(timeGetCurrentTime(TimeType_Default, &now)) && R_SUCCEEDED(timeToCalendarTimeWithMyRule(now, &cal, &info)))
        hour = cal.hour;
    std::string g = hour >= 5 && hour < 12 ? "Доброе утро" : hour >= 12 && hour < 18 ? "Добрый день"
                  : hour >= 18 && hour < 23 ? "Добрый вечер" : "Доброй ночи";
    std::string name = !app.customName.empty() ? app.customName : app.login;
    return name.empty() ? g : g + ", " + name;
}

// «среда, 5 ноября» — под приветствием.
static std::string todayText() {
    static const char *days[] = {"воскресенье", "понедельник", "вторник", "среда", "четверг", "пятница", "суббота"};
    static const char *months[] = {"января", "февраля", "марта", "апреля", "мая", "июня",
                                   "июля", "августа", "сентября", "октября", "ноября", "декабря"};
    u64 now = 0;
    TimeCalendarTime cal{};
    TimeCalendarAdditionalInfo info{};
    if (R_FAILED(timeGetCurrentTime(TimeType_Default, &now)) || R_FAILED(timeToCalendarTimeWithMyRule(now, &cal, &info)))
        return "";
    return std::string(days[info.wday % 7]) + ", " + std::to_string(cal.day) + " " + months[(cal.month + 11) % 12];
}

// Часы и заряд — внизу справа, рядом с версией (опрос заряда раз в 10 секунд).
static int drawClock(App &app, int right, int y) {
    Ui &ui = app.ui;
    static u64 nextPoll = 0;
    static u32 battery = 0;
    static bool charging = false;
    if (nowMs() >= nextPoll) {
        nextPoll = nowMs() + 10000;
        psmGetBatteryChargePercentage(&battery);
        PsmChargerType ch = PsmChargerType_Unconnected;
        charging = R_SUCCEEDED(psmGetChargerType(&ch)) && ch != PsmChargerType_Unconnected;
    }
    u64 now = 0;
    TimeCalendarTime cal{};
    TimeCalendarAdditionalInfo info{};
    char clock[8] = "";
    if (R_SUCCEEDED(timeGetCurrentTime(TimeType_Default, &now)) && R_SUCCEEDED(timeToCalendarTimeWithMyRule(now, &cal, &info)))
        snprintf(clock, sizeof(clock), "%d:%02d", cal.hour, cal.minute);
    std::string pct = std::to_string(battery) + " %";
    int x = right - ui.textWidth(pct, 18);
    ui.text(pct, right, y, 18, col::gray, Align::Right);
    // Батарейка: корпус, носик, заливка по заряду
    int bx = x - 40, by = y + 3;
    SDL_Color bc = charging ? col::yellow : battery <= 15 ? col::red : col::gray;
    ui.roundRect(bx, by, 28, 15, 3, bc);
    ui.roundRect(bx + 2, by + 2, 24, 11, 2, col::bg);
    ui.rect(bx + 3, by + 3, std::max<int>(2, (int)(22 * std::min<u32>(battery, 100) / 100)), 9, bc);
    ui.rect(bx + 28, by + 5, 3, 5, bc);
    int cw = ui.textWidth(clock, 18);
    ui.text(clock, bx - 14, y, 18, col::gray, Align::Right);
    return right - (bx - 14 - cw); // сколько места заняли
}

// Настройки: разделы с заголовками, список прокручивается за выбранной строкой.
static void drawSettings(App &app) {
    Ui &ui = app.ui;
    // Основные настройки или «Главный экран» — один и тот же вид.
    SetTable t = setTable(app.screen);
    ListView &v = app.screen == ScrHomeSet ? app.homeSetView : app.setView;
    const int rows = setRowCount(t);
    const int x = 70, w = Ui::W - 140, rowH = 54, headH = 46, top = 96, bottom = LIST_BOTTOM - 20;
    v.sel = std::max(0, std::min(rows - 1, v.sel));
    // Строка выбранного пункта в SET_LINES
    int selLine = 0;
    for (int i = 0, k = 0; i < t.count; i++)
        if (t.lines[i].row >= 0 && k++ == v.sel) { selLine = i; break; }
    int &scroll = v.scroll;
    if (selLine < scroll) scroll = selLine;
    if (scroll > 0 && t.lines[scroll - 1].row < 0 && scroll - 1 <= selLine) scroll--; // заголовок раздела — вместе с первой строкой
    auto bottomOf = [&](int from, int to) { // низ строки to, если рисовать с from
        int y = top;
        for (int i = from; i <= to; i++) y += t.lines[i].row < 0 ? headH : rowH;
        return y;
    };
    while (scroll < selLine && bottomOf(scroll, selLine) > bottom) scroll++;

    int y = top;
    for (int i = scroll, pos = 0; i < t.count && y < bottom; i++) {
        const SetLine &ln = t.lines[i];
        if (ln.row < 0) {
            ui.text(ln.header, x, y + 16, 20, col::yellow);
            y += headH;
            continue;
        }
        pos = 0; // номер пункта по порядку — для выбора и касаний
        for (int j = 0; j < i; j++) if (t.lines[j].row >= 0) pos++;
        if (y + rowH > bottom + 6) break;
        int row = ln.row;
        bool sel = pos == v.sel;
        bool rgb = row == SetAccentR || row == SetAccentG || row == SetAccentB;
        bool sub = row == SetSaveLyrics || row == SetSaveLikes || rgb;
        bool off = rgb ? app.accent != col::ACCENT_COUNT : sub && !app.autoSave; // подпункты: автосохранение, свой цвет
        if (sel) ui.roundRect(x - 12, y - 2, w + 24, rowH - 6, 12, col::panel);
        ui.text(settingTitle(row), x + (sub ? 36 : 0), y + 12, sub ? 22 : 24, off ? col::dim : col::white);
        std::string v = row == SetHomeTiles || row == SetHomeScreen || row == SetProxy || row == SetScAccount ? settingValue(app, row) : "‹  " + settingValue(app, row) + "  ›";
        ui.text(v, x + w - 8, y + 14, 22, off ? col::dim : sel ? col::yellow : col::gray, Align::Right);
        if (row == SetAccent) ui.circle(x + w - 8 - ui.textWidth(v, 22) - 18, y + 26, 9, col::yellow); // образец цвета
        addButton(app, x - 12, y - 2, w + 24, rowH - 6, ActSettingBase + pos);
        y += rowH;
    }
    // Полоса прокрутки
    int total = bottomOf(0, t.count - 1) - top, view = bottom - top;
    if (total > view) {
        int barH = std::max(40, view * view / total), barY = top + (bottomOf(0, scroll) - top - (t.lines[scroll].row < 0 ? headH : rowH)) * view / total;
        ui.roundRect(Ui::W - 40, top, 4, view, 2, col::panel);
        ui.roundRect(Ui::W - 40, std::min(barY, bottom - barH), 4, barH, 2, col::gray);
    }
    std::string cerr = app.connectOn && app.screen == ScrSettings ? app.connect.lastError() : "";
    if (!cerr.empty()) ui.text("Коннект: " + cerr, x, LIST_BOTTOM - 10, 18, col::red, Align::Left, w);
}

// Плитки главной: A — показать или скрыть, L/R — переставить выше или ниже.
static void drawTiles(App &app) {
    Ui &ui = app.ui;
    const int x = 70, w = Ui::W - 140, rowH = 48;
    ui.text("Что показывать на главной и в каком порядке", x, 92, 22, col::gray);
    app.tilesView.sel = std::max(0, std::min((int)app.tileOrder.size() - 1, app.tilesView.sel));
    for (int pos = 0; pos < (int)app.tileOrder.size(); pos++) {
        int t = app.tileOrder[pos], y = 128 + pos * rowH;
        bool sel = pos == app.tilesView.sel, shown = !app.tileHidden.count(t);
        if (sel) ui.roundRect(x - 12, y - 4, w + 24, rowH - 4, 10, col::panel);
        // Галочка: жёлтый кружок — показана, пустой — скрыта
        ui.circle(x + 14, y + 18, 12, shown ? col::yellow : col::dim);
        if (!shown) ui.circle(x + 14, y + 18, 9, sel ? col::panel : col::bg);
        else { ui.line(x + 8, y + 18, x + 12, y + 23, 3, col::black); ui.line(x + 12, y + 23, x + 20, y + 12, 3, col::black); }
        ui.text(TILE_NAMES[t], x + 44, y + 4, 24, shown ? col::white : col::dim);
        ui.text(std::to_string(pos + 1), x + w - 8, y + 7, 20, col::gray, Align::Right);
        addButton(app, x - 12, y - 4, w + 24, rowH - 4, ActHomeBase + pos);
    }
}

static void drawQueue(App &app) {
    Ui &ui = app.ui;
    int count = app.current >= 0 ? (int)app.queue.size() - app.current : 0;
    ui.text(count > 1 ? tracksWord(count - 1) + " дальше" : "Дальше ничего нет", 70, 92, 22, col::gray);
    const int rowH = LIST_ROW_H, visible = (LIST_BOTTOM - LIST_TOP) / rowH;
    app.queueView.clamp(count, visible);
    for (int i = app.queueView.scroll; i < count && i < app.queueView.scroll + visible; i++) {
        const Track &t = app.queue[app.current + i];
        int y = LIST_TOP + (i - app.queueView.scroll) * rowH;
        drawRow(app, y, rowH - 6, i == app.queueView.sel, i == 0, coverFor(app, t, COVER_SMALL), t.title, t.artists,
                i == 0 ? "сейчас" : fmtTime(t.durationMs / 1000.0), ActItemBase + i, app.liked.count(t.id) > 0);
    }
}

// Фон из обложки играющего трека (плеер или главная): mode 1 — размытая обложка под затемнением,
// 2 — её цвет, плавно уходящий в фон.
static void drawCoverBg(App &app, int mode) {
    Ui &ui = app.ui;
    std::string url = coverFor(app, app.queue[app.current], COVER_SMALL);
    SDL_Texture *tex = coverTex(app, url);
    if (!tex) return;
    if (mode == 1) {
        // Маленькую обложку растягиваем на весь экран со сглаживанием — получается размытие.
        SDL_SetTextureScaleMode(tex, SDL_ScaleModeLinear);
        ui.image(tex, 0, (Ui::H - Ui::W) / 2, Ui::W, Ui::W);
        ui.rect(0, 0, Ui::W, Ui::H, SDL_Color{col::bg.r, col::bg.g, col::bg.b, 175});
    } else {
        SDL_Color a = app.covers[url].avg;
        for (int y = 0; y < Ui::H; y += 8) {
            float k = 0.6f * (1.0f - (float)y / Ui::H); // доля цвета обложки: сверху больше
            ui.rect(0, y, Ui::W, 8, SDL_Color{(u8)(a.r * k + col::bg.r * (1 - k)), (u8)(a.g * k + col::bg.g * (1 - k)),
                                              (u8)(a.b * k + col::bg.b * (1 - k)), 255});
        }
    }
}

// Текст песни: слева обложка и прогресс, справа строки; текущая — жёлтая и держится в середине.
static const int LYR_X = 430, LYR_W = Ui::W - LYR_X - 70, LYR_TOP = 100, LYR_BOTTOM = Ui::H - 70;
static const int LYR_ROW = 50, LYR_SIZE = 30, LYR_ANCHOR = 300;

// Раскладываем строки по ширине (переносы по словам) — один раз на текст.
static void layoutLyrics(App &app) {
    Ui &ui = app.ui;
    app.lyricRows.clear();
    for (int i = 0; i < (int)app.lyrics.size(); i++) {
        const std::string &s = app.lyrics[i].s;
        if (s.empty()) { app.lyricRows.push_back({i, app.lyricsSynced ? "· · ·" : ""}); continue; }
        std::string row;
        size_t p = 0;
        while (p < s.size()) {
            size_t e = s.find(' ', p);
            if (e == std::string::npos) e = s.size();
            std::string word = s.substr(p, e - p);
            p = e + 1;
            std::string cand = row.empty() ? word : row + " " + word;
            if (!row.empty() && ui.textWidth(cand, LYR_SIZE) > LYR_W) {
                app.lyricRows.push_back({i, row});
                row = word;
            } else {
                row = cand;
            }
        }
        if (!row.empty()) app.lyricRows.push_back({i, row});
    }
}

static float lyricsMaxScroll(const App &app) {
    int rows = (int)app.lyricRows.size();
    if (app.lyricsSynced) return std::max(0, rows - 1) * (float)LYR_ROW;
    return std::max(0.0f, rows * (float)LYR_ROW - (LYR_BOTTOM - LYR_TOP - 20));
}

static void drawLyrics(App &app) {
    Ui &ui = app.ui;
    const Track &t = app.queue[app.current];

    // Слева — что играет.
    const int cx = 70, cy = 110, cs = 300;
    drawCover(app, coverFor(app, t, COVER_BIG), cx, cy, cs);
    addButton(app, cx, cy, cs, cs, ActPlay);
    ui.text(t.title, cx, cy + cs + 22, 26, col::white, Align::Left, cs);
    ui.text(t.artists, cx, cy + cs + 58, 20, col::gray, Align::Left, cs);
    double pos = app.player.position(), dur = t.durationMs / 1000.0;
    if (app.scrubFrac >= 0) pos = app.scrubFrac * dur;
    double frac = dur > 0 ? std::min(1.0, pos / dur) : 0;
    const int by = cy + cs + 104;
    app.seekBar = {cx, by, cs, 5};
    ui.roundRect(cx, by, cs, 5, 2, col::panel);
    if (frac > 0) ui.roundRect(cx, by, (int)(cs * frac), 5, 2, col::yellow);
    if (app.scrubFrac >= 0) ui.circle(cx + (int)(cs * frac), by + 2, 11, col::white);
    ui.text(fmtTime(pos), cx, by + 14, 18, col::gray);
    ui.text(fmtTime(dur), cx + cs, by + 14, 18, col::gray, Align::Right);
    if (app.player.state() == Player::Paused) ui.text("Пауза", cx + cs / 2, by + 14, 18, col::gray, Align::Center);

    // Справа — текст.
    if (app.lyricsLoading) {
        ui.text("Загружаю текст…", LYR_X, LYR_ANCHOR, 26, col::gray);
        return;
    }
    if (app.lyrics.empty()) {
        std::string e = app.lyricsError.empty() ? "У этого трека нет текста" : app.lyricsError;
        if (app.offlineMode && e.compare(0, 8, "Network:") == 0) e = "Нет сети, а текст этого трека ещё не открывали";
        ui.text(e, LYR_X, LYR_ANCHOR, 24, col::gray, Align::Left, LYR_W);
        return;
    }
    if (app.lyricRows.empty()) layoutLyrics(app);

    int cur = currentLyric(app);
    if (app.lyricsSynced && nowMs() >= app.lyricsManualUntil) {
        int r = 0;
        while (r < (int)app.lyricRows.size() && app.lyricRows[r].line < std::max(0, cur)) r++;
        float target = r * (float)LYR_ROW;
        app.lyricsY += (target - app.lyricsY) * 0.12f; // плавно подъезжаем к текущей строке
        if (std::abs(target - app.lyricsY) < 0.5f) app.lyricsY = target;
    }
    app.lyricsY = std::max(0.0f, std::min(app.lyricsY, lyricsMaxScroll(app)));

    const int base = app.lyricsSynced ? LYR_ANCHOR : LYR_TOP + 10;
    for (int r = 0; r < (int)app.lyricRows.size(); r++) {
        int y = base + r * LYR_ROW - (int)app.lyricsY;
        if (y < LYR_TOP || y > LYR_BOTTOM - LYR_ROW) continue;
        const App::LyricRow &row = app.lyricRows[r];
        SDL_Color c = !app.lyricsSynced ? col::white : row.line == cur ? col::yellow : row.line < cur ? col::dim : col::gray;
        ui.text(row.s, LYR_X, y, LYR_SIZE, c, Align::Left, LYR_W);
        if (app.lyricsSynced) addButton(app, LYR_X - 10, y - 6, LYR_W + 20, LYR_ROW, ActLyricBase + row.line);
    }
    ui.text(app.lyricsSynced ? "Тап по строке — перемотка к ней" : "Текст без таймкодов", Ui::W - 70, 92, 18, col::dim, Align::Right);
}

// Экран поиска / исполнителя / «Мои альбомы»: блоки сверху вниз — треки строками, карточки рядом или сеткой.
static const int RICH_TOP = 96, RICH_BOTTOM = LIST_BOTTOM;
static const int CARD_W = 172, CARD_STEP = 196, CARD_H = 236, GRID_COLS = 5;

static void drawCard(App &app, const Card &c, int x, int y, int w, bool sel, int act) {
    Ui &ui = app.ui;
    if (sel) ui.roundRect(x - 5, y - 5, w + 10, w + 10, 14, col::yellow);
    std::string url = c.coverUrl(200);
    if (url.empty()) {
        ui.roundRect(x, y, w, w, 10, col::panel);
        if (c.kind == Card::Artist) { // силуэт вместо фото
            ui.circle(x + w / 2, y + w * 2 / 5, w / 6, col::dim);
            ui.roundRect(x + w / 4, y + w * 3 / 5, w / 2, w / 4, w / 8, col::dim);
        }
    } else {
        drawCover(app, url, x, y, w);
    }
    ui.text(c.title, x, y + w + 8, 19, sel ? col::white : col::white, Align::Left, w);
    ui.text(c.sub, x, y + w + 34, 15, col::gray, Align::Left, w);
    addButton(app, x, y, w, w + 52, act);
}

static void drawRich(App &app) {
    Ui &ui = app.ui;
    app.richItems.clear();
    app.richRows.clear();
    if (app.rich.empty()) return;
    App::RichPage &page = app.rich.back();
    if (page.loading) { ui.text(page.type == 0 || page.type == 3 ? "Ищу «" + page.query + "»…" : "Загружаю…", Ui::W / 2, 320, 26, col::gray, Align::Center); return; }
    if (!page.error.empty()) { ui.text(page.error, Ui::W / 2, 300, 24, col::gray, Align::Center, 1100); return; }

    // Сначала раскладываем всё в координатах содержимого (y от 0), потом сдвигаем на прокрутку.
    struct Draw { int section, index; SDL_Rect rc; };
    std::vector<int> headY(page.sections.size());
    const int x0 = 70, w = Ui::W - 140;
    int y = 0;
    for (int s = 0; s < (int)page.sections.size(); s++) {
        App::RichSection &sec = page.sections[s];
        headY[s] = y;
        if (!sec.title.empty()) y += 42;
        int first = (int)app.richItems.size();
        if (!sec.tracks.empty()) {
            for (int i = 0; i < (int)sec.tracks.size(); i++, y += 64) app.richItems.push_back({s, i, {x0, y, w, 58}});
            if (sec.allTracks) { app.richItems.push_back({s, -1, {x0, y, w, 46}}); y += 54; }
        } else if (sec.grid) {
            int cw = (w - (GRID_COLS - 1) * 24) / GRID_COLS;
            for (int i = 0; i < (int)sec.cards.size(); i++)
                app.richItems.push_back({s, i, {x0 + (i % GRID_COLS) * (cw + 24), y + (i / GRID_COLS) * (cw + 76), cw, cw + 52}});
            y += ((int)sec.cards.size() + GRID_COLS - 1) / GRID_COLS * (cw + 76);
        } else { // ряд карточек с прокруткой вбок: листается свайпом, кнопками — за выбором
            int selIn = page.sel - first, n = (int)sec.cards.size();
            if (selIn >= 0 && selIn < n && page.sel != sec.followed) {
                sec.followed = page.sel;
                float left = (float)selIn * CARD_STEP;
                if (left < sec.sx) sec.sx = left;
                if (left + CARD_W > sec.sx + w) sec.sx = left + CARD_W - w;
            }
            if (selIn < 0 || selIn >= n) sec.followed = -1; // вернёмся в ряд — снова покажем выбранную
            float maxSx = (float)std::max(0, n * CARD_STEP - (CARD_STEP - CARD_W) - w);
            sec.sx = std::max(0.0f, std::min(sec.sx, maxSx));
            for (int i = 0; i < n; i++)
                app.richItems.push_back({s, i, {x0 + i * CARD_STEP - (int)sec.sx, y, CARD_W, CARD_W + 52}});
            app.richRows.push_back({s, {0, y, Ui::W, CARD_W + 52}});
            // «Лучший результат» рисуется одной крупной карточкой высотой 146, а не рядом (236)
            bool best = s == 0 && page.type == 0 && sec.title == "Лучший результат";
            y += best ? 146 : CARD_H;
        }
        y += 18;
    }
    const int contentH = y;
    page.sel = std::max(0, std::min(page.sel, (int)app.richItems.size() - 1));
    // Прокрутка за выбором (заголовок блока — вместе с его первым элементом)
    if (!app.richItems.empty() && page.sel != page.shownSel) {
        page.shownSel = page.sel;
        const SDL_Rect &r = app.richItems[page.sel].rc;
        int sec = app.richItems[page.sel].section;
        int topWanted = (page.sel == 0 || app.richItems[page.sel - 1].section != sec) ? headY[sec] : r.y;
        if (topWanted - page.scroll < 0) page.scroll = (float)topWanted;
        if (r.y + r.h - page.scroll > RICH_BOTTOM - RICH_TOP) page.scroll = (float)(r.y + r.h - (RICH_BOTTOM - RICH_TOP));
    }
    page.scroll = std::max(0.0f, std::min(page.scroll, (float)std::max(0, contentH - (RICH_BOTTOM - RICH_TOP))));
    const int off = RICH_TOP - (int)page.scroll;
    app.richOff = off;

    SDL_Rect clip{0, RICH_TOP, Ui::W, RICH_BOTTOM - RICH_TOP};
    SDL_RenderSetClipRect(ui.renderer(), &clip);
    for (int s = 0; s < (int)page.sections.size(); s++)
        if (!page.sections[s].title.empty() && headY[s] + off > RICH_TOP - 40 && headY[s] + off < RICH_BOTTOM)
            ui.text(page.sections[s].title, x0, headY[s] + off + 6, 24, col::yellow);
    std::vector<Button> before = app.buttons;
    for (int i = 0; i < (int)app.richItems.size(); i++) {
        const App::RichItem &it = app.richItems[i];
        SDL_Rect rc = it.rc;
        rc.y += off;
        if (rc.y + rc.h < RICH_TOP || rc.y > RICH_BOTTOM || rc.x + rc.w < 0 || rc.x > Ui::W) continue;
        const App::RichSection &sec = page.sections[it.section];
        bool sel = i == page.sel;
        if (it.index < 0) {
            if (sel) ui.roundRect(rc.x - 12, rc.y, rc.w + 24, rc.h, 12, col::panel);
            ui.text("Все треки  ›", rc.x + 12, rc.y + 10, 22, col::yellow);
            addButton(app, rc.x - 12, rc.y, rc.w + 24, rc.h, ActRichBase + i);
        } else if (!sec.tracks.empty()) {
            const Track &t = sec.tracks[it.index];
            bool playing = hasCurrent(app) && app.queue[app.current].id == t.id;
            drawRow(app, rc.y, rc.h, sel, playing, coverFor(app, t, COVER_SMALL), t.title, t.artists,
                    fmtTime(t.durationMs / 1000.0), ActRichBase + i, app.liked.count(t.id) > 0, findOffline(app, t.id) != nullptr);
        } else {
            const Card &c = sec.cards[it.index];
            if (it.section == 0 && page.type == 0 && sec.title == "Лучший результат") { // крупная карточка лучшего
                if (sel) ui.roundRect(rc.x - 12, rc.y - 6, w + 24, 152, 14, col::panel);
                std::string url = c.coverUrl(200);
                if (url.empty()) ui.roundRect(rc.x, rc.y, 140, 140, 10, col::panel);
                else drawCover(app, url, rc.x, rc.y, 140);
                const char *kind = c.kind == Card::Artist ? "Исполнитель" : c.kind == Card::Album ? "Альбом" : "Плейлист";
                ui.text(kind, rc.x + 164, rc.y + 18, 18, col::yellow);
                ui.text(c.title, rc.x + 164, rc.y + 44, 32, col::white, Align::Left, w - 180);
                ui.text(c.kind == Card::Artist ? "Открыть страницу исполнителя" : c.sub, rc.x + 164, rc.y + 92, 19, col::gray, Align::Left, w - 180);
                addButton(app, rc.x - 12, rc.y - 6, w + 24, 152, ActRichBase + i);
            } else {
                drawCard(app, c, rc.x, rc.y, rc.w, sel, ActRichBase + i);
            }
        }
    }
    SDL_RenderSetClipRect(ui.renderer(), nullptr);
    // Кнопки вне видимой области не нужны (рисовали с обрезкой — касание там означало бы невидимое)
    for (size_t b = before.size(); b < app.buttons.size(); b++) {
        SDL_Rect &r = app.buttons[b].rc;
        int y0 = std::max(r.y, RICH_TOP), y1 = std::min(r.y + r.h, RICH_BOTTOM);
        r.y = y0;
        r.h = std::max(0, y1 - y0);
    }
}

// Вход по коду: крупный код и что с ним делать.
static void drawLogin(App &app) {
    Ui &ui = app.ui;
    const App::LoginFlow &lf = app.loginFlow;
    const int cx = Ui::W / 2;
    ui.text("Вход в Яндекс Музыку", cx, 130, 34, col::white, Align::Center);
    if (lf.waiting) {
        ui.text("Получаю код…", cx, 300, 26, col::gray, Align::Center);
        return;
    }
    if (!lf.error.empty() && lf.userCode.empty()) {
        ui.text("Не получилось получить код", cx, 280, 26, col::white, Align::Center);
        ui.text(lf.error, cx, 324, 20, col::red, Align::Center, 1100);
        return;
    }
    std::string url = lf.url;
    for (const char *p : {"https://", "http://"})
        if (url.compare(0, strlen(p), p) == 0) url = url.substr(strlen(p));
    ui.text("1. На телефоне или компьютере откройте " + url, cx, 200, 24, col::gray, Align::Center);
    ui.text("2. Войдите в Яндекс и введите код:", cx, 238, 24, col::gray, Align::Center);
    // Код крупно, с пробелом посередине — так его легче переписать.
    std::string code = lf.userCode;
    if (code.size() == 8) code = code.substr(0, 4) + "  " + code.substr(4);
    int w = ui.textWidth(code, 84) + 80;
    ui.roundRect(cx - w / 2, 300, w, 130, 24, col::panel);
    ui.text(code, cx, 318, 84, col::yellow, Align::Center);
    if (!lf.error.empty()) {
        ui.text(lf.error + " — нажмите A, чтобы получить новый код", cx, 470, 22, col::red, Align::Center, 1100);
    } else {
        int left = lf.expiresAt > nowMs() ? (int)((lf.expiresAt - nowMs()) / 1000) : 0;
        ui.text("Жду подтверждения…  код действует ещё " + fmtTime(left), cx, 470, 22, col::dim, Align::Center);
    }
}

// Клавиатура поиска поверх текущего экрана: поле ввода сверху, клавиши снизу.
static void drawKbd(App &app) {
    Ui &ui = app.ui;
    App::Kbd &k = app.kbd;
    SDL_Color shade = col::bg;
    shade.a = 225;
    ui.rect(0, 0, Ui::W, Ui::H - 58, shade);

    ui.text(k.sc ? "Поиск в SoundCloud" : "Поиск в Яндекс Музыке", 70, 36, 34, col::white);
    ui.text(k.ru ? "Русская раскладка" : "English", Ui::W - 70, 46, 20, col::gray, Align::Right);
    // Поле ввода с мигающим курсором
    const int fx = 70, fy = 100, fw = Ui::W - 140, fh = 72;
    ui.roundRect(fx, fy, fw, fh, 18, col::panel);
    int tw = 0;
    if (k.text.empty()) ui.text(k.sc ? "Трек, автор или ссылка soundcloud.com/…" : "Трек, исполнитель или альбом", fx + 28, fy + 20, 28, col::dim, Align::Left, fw - 56);
    else tw = ui.text(k.text, fx + 28, fy + 20, 28, col::white, Align::Left, fw - 70);
    if ((nowMs() / 500) % 2 == 0) ui.rect(fx + 30 + tw, fy + 18, 3, 36, col::yellow);

    const int keyH = 70, gap = 10, top = 214, unit = 84;
    for (int row = 0; row <= KBD_LETTER_ROWS; row++) {
        int y = top + row * (keyH + gap);
        if (row < KBD_LETTER_ROWS) {
            std::vector<std::string> keys = kbdRow(app, row);
            int n = (int)keys.size(), w = n * unit + (n - 1) * gap, x = (Ui::W - w) / 2;
            for (int c = 0; c < n; c++, x += unit + gap) {
                bool sel = k.row == row && k.col == c;
                ui.roundRect(x, y, unit, keyH, 14, sel ? col::yellow : col::panel);
                ui.text(keys[c], x + unit / 2, y + 16, 32, sel ? col::black : col::white, Align::Center);
                addButton(app, x, y, unit, keyH, ActKbdBase + row * 100 + c);
            }
        } else {
            // Служебный ряд — шириной с самый длинный ряд букв, клавиши делят её по KBD_KEY_UNITS.
            int units = 0;
            for (int i = 0; i < KEY_COUNT; i++) units += KBD_KEY_UNITS[i];
            int total = 12 * unit + 11 * gap, x = (Ui::W - total) / 2, space = total - (KEY_COUNT - 1) * gap;
            for (int c = 0; c < KEY_COUNT; c++) {
                int w = space * KBD_KEY_UNITS[c] / units;
                bool sel = k.row == row && k.col == c;
                SDL_Color bg = sel ? col::yellow : col::panel;
                SDL_Color fg = sel ? col::black : c == KEY_OK ? col::yellow : col::white; // «Найти» — акцентом
                ui.roundRect(x, y, w, keyH, 14, bg);
                std::string label = c == KEY_LANG ? (k.ru ? "EN" : "RU") : c == KEY_SVC ? (k.sc ? "Яндекс" : "SoundCloud") : KBD_KEY_LABEL[c];
                ui.text(label, x + w / 2, y + 20, 26, fg, Align::Center, w - 16);
                addButton(app, x, y, w, keyH, ActKbdBase + row * 100 + c);
                x += w + gap;
            }
        }
    }
}

static void draw(App &app) {
    Ui &ui = app.ui;
    app.buttons.clear();
    app.seekBar = {0, 0, 0, 0};
    app.volBar = {0, 0, 0, 0};
    app.frame++;
    ui.begin();
    if (app.playerBg && (app.screen == ScrPlayer || app.screen == ScrLyrics || app.screen == ScrQueue) && hasCurrent(app))
        drawCoverBg(app, app.playerBg);
    else if (app.homeBg && app.screen == ScrHome && hasCurrent(app))
        drawCoverBg(app, app.homeBg);

    // Шапка
    std::string header = app.screen == ScrHome ? (app.greeting ? greeting(app) : "Главная") : app.screen == ScrPlayer ? app.sourceTitle
                       : app.screen == ScrLibrary ? "Плейлисты" : app.screen == ScrSettings ? "Настройки" : app.screen == ScrTiles ? "Плитки главной" : app.screen == ScrHomeSet ? "Главный экран" : app.screen == ScrRich && !app.rich.empty() ? app.rich.back().title
                       : app.screen == ScrQueue ? "Очередь" : app.screen == ScrLyrics ? "Текст песни" : app.openList.title;
    // На главной слева боковое меню — шапка начинается правее него.
    bool side = app.screen == ScrHome && app.started && app.startupError.empty() && !app.loginFlow.active;
    bool scTab = side && hasScTab() && app.homeTab == 1;
    int hx = side ? HOME_X : 70;
    ui.text(header, hx, 36, 34, col::white, Align::Left, side ? 740 - hx : 660);
    if (app.screen == ScrHome && app.showDate && app.started && !app.loginFlow.active) ui.text(todayText(), hx + 2, 76, 18, col::gray);
    if (scTab) { // на вкладке SoundCloud справа — аккаунт SoundCloud
        if (!app.scUser.empty()) ui.text(app.scUser, Ui::W - 70, 46, 20, col::gray, Align::Right, Ui::W - 70 - 910);
    } else if (!app.login.empty()) {
        int x = Ui::W - 70;
        if (app.hasPlus) {
            int w = ui.textWidth("Плюс", 18) + 20;
            ui.roundRect(x - w, 44, w, 28, 14, col::yellow);
            ui.text("Плюс", x - w / 2, 47, 18, col::black, Align::Center);
            x -= w + 14;
        }
        ui.text(app.login, x, 46, 20, col::gray, Align::Right, x - 910); // не залезать на «Поиск»
    }

    // Кнопка поиска в шапке — чтобы искать пальцем, не тянясь к «−».
    if (app.ready && !app.offlineMode) {
        const int sx = 760, sy = 40, sw = 132, sh = 36;
        ui.roundRect(sx, sy, sw, sh, sh / 2, col::panel);
        ui.circle(sx + 22, sy + 16, 8, col::yellow); // лупа
        ui.circle(sx + 22, sy + 16, 5, col::panel);
        ui.triangle(sx + 26, sy + 21, sx + 29, sy + 18, sx + 34, sy + 28, col::yellow);
        ui.text("Поиск", sx + 44, sy + 6, 20, col::white);
        addButton(app, sx - 6, sy - 6, sw + 12, sh + 12, ActSearch);
    }
    if (app.loginFlow.active) {
        drawLogin(app);
    } else if (!app.started) {
        ui.text("Подключаюсь к Яндекс Музыке…", Ui::W / 2, 330, 28, col::gray, Align::Center);
    } else if (!app.startupError.empty()) {
        ui.text("Не получилось войти", Ui::W / 2, 280, 34, col::white, Align::Center);
        ui.text(app.startupError, Ui::W / 2, 336, 22, col::red, Align::Center, 1100);
        ui.text("Токен лежит в " + std::string(TOKEN_PATH), Ui::W / 2, 376, 20, col::gray, Align::Center);
        int x = Ui::W / 2 - 80;
        drawHint(app, x, 430, glyph::A, "Повторить", ActRetry);
        addButton(app, Ui::W / 2 - 120, 410, 240, 60, ActRetry);
    } else if (app.screen == ScrHome) {
        drawHome(app);
    } else if (app.screen == ScrRich) {
        drawRich(app);
    } else if (app.screen == ScrSettings || app.screen == ScrHomeSet) {
        drawSettings(app);
    } else if (app.screen == ScrTiles) {
        drawTiles(app);
    } else if (app.screen == ScrQueue) {
        drawQueue(app);
    } else if (app.screen == ScrLyrics && app.current >= 0 && app.current < (int)app.queue.size()) {
        drawLyrics(app);
    } else if (app.screen == ScrLibrary) {
        drawLibrary(app);
        drawMiniPlayer(app);
    } else if (app.screen == ScrList) {
        drawList(app);
    } else if (app.current >= 0 && app.current < (int)app.queue.size()) {
        drawPlayer(app);
    } else if (app.source == SrcWave && app.waveLoading) {
        ui.text("Загружаю волну…", Ui::W / 2, 330, 26, col::gray, Align::Center);
    } else {
        ui.text(app.error.empty() ? "Ничего не играет" : app.error, Ui::W / 2, 300, 28,
                app.error.empty() ? col::white : col::red, Align::Center, 1100);
        int x = Ui::W / 2 - 150;
        drawHint(app, x, 360, glyph::X, "На главную", ActHome);
        addButton(app, Ui::W / 2 - 200, 340, 400, 60, ActHome);
    }

    // Прогресс скачивания — справа над подсказками, на любом экране.
    if (app.dlTotal > 0 && app.dlDone < app.dlTotal) {
        std::string s = "Скачиваю " + std::to_string(app.dlDone + 1) + " из " + std::to_string(app.dlTotal);
        int w = ui.textWidth(s, 18) + 52, y = Ui::H - 100;
        ui.roundRect(Ui::W - 30 - w, y, w, 30, 15, col::panel);
        ui.downArrow(Ui::W - 30 - w + 20, y + 15, 16, col::yellow);
        ui.text(s, Ui::W - 30 - 14, y + 4, 18, col::white, Align::Right);
    }

    // Клавиатура поиска — поверх всего; кнопки под ней не нажимаются.
    if (app.kbd.active) {
        app.buttons.clear();
        drawKbd(app);
    }

    // Всплывающее сообщение
    if (!app.toast.empty() && nowMs() < app.toastUntil) {
        int w = ui.textWidth(app.toast, 20) + 48;
        ui.roundRect(Ui::W / 2 - w / 2, Ui::H - 120, w, 44, 22, col::white);
        ui.text(app.toast, Ui::W / 2, Ui::H - 109, 20, col::bg, Align::Center);
    }

    // Подсказки
    ui.rect(0, Ui::H - 58, Ui::W, 1, col::panel);
    int x = 70;
    const int hy = Ui::H - 40;
    bool hasTrack = app.current >= 0 && app.current < (int)app.queue.size();
    if (app.kbd.active) {
        drawHint(app, x, hy, glyph::A, "Ввести", ActNone);
        drawHint(app, x, hy, glyph::B, "Стереть", ActKbdBase + KBD_LETTER_ROWS * 100 + KEY_BACK);
        drawHint(app, x, hy, glyph::Y, app.kbd.ru ? "English" : "Русский", ActKbdBase + KBD_LETTER_ROWS * 100 + KEY_LANG);
        drawHint(app, x, hy, glyph::X, "Пробел", ActKbdBase + KBD_LETTER_ROWS * 100 + KEY_SPACE);
        drawHint(app, x, hy, glyph::Plus, "Найти", ActKbdBase + KBD_LETTER_ROWS * 100 + KEY_OK);
        drawHint(app, x, hy, glyph::Minus, "Закрыть", ActKbdBase + 99);
    } else if (app.loginFlow.active) {
        drawHint(app, x, hy, glyph::A, "Новый код", ActLoginNew);
        if (app.ready) drawHint(app, x, hy, glyph::B, "Отмена", ActLoginCancel);
    } else if (app.screen == ScrHome) {
        drawHint(app, x, hy, glyph::A, "Открыть", ActActivateSel);
        drawHint(app, x, hy, glyph::Minus, "Поиск", ActSearch);
        if (hasScTab()) {
            drawHint(app, x, hy, glyph::L, "", ActTabYa);
            x -= 26;
            drawHint(app, x, hy, glyph::R, "Раздел", ActTabSc);
        }
        if (hasTrack) drawHint(app, x, hy, glyph::X, "Плеер", ActPlayer);
    } else if (app.screen == ScrTiles) {
        drawHint(app, x, hy, glyph::A, "Показать / скрыть", ActActivateSel);
        drawHint(app, x, hy, glyph::L, "", ActNone);
        x -= 26;
        drawHint(app, x, hy, glyph::R, "Выше / ниже", ActNone);
        drawHint(app, x, hy, glyph::B, "Назад", ActBack);
    } else if (app.screen == ScrRich) {
        drawHint(app, x, hy, glyph::A, "Открыть", ActNone);
        drawHint(app, x, hy, glyph::B, "Назад", ActBack);
        drawHint(app, x, hy, glyph::Minus, "Поиск", ActSearch);
        drawHint(app, x, hy, glyph::X, "Главная", ActHome);
    } else if (app.screen == ScrSettings || app.screen == ScrHomeSet) {
        drawHint(app, x, hy, glyph::A, "Изменить", ActActivateSel);
        drawHint(app, x, hy, glyph::B, "Назад", ActBack);
        drawHint(app, x, hy, glyph::X, "Главная", ActHome);
    } else if (app.screen == ScrLyrics) {
        drawHint(app, x, hy, glyph::A, "Пауза", ActPlay);
        drawHint(app, x, hy, glyph::B, "Плеер", ActBack);
        drawHint(app, x, hy, glyph::L, "", ActPrev);
        x -= 26;
        drawHint(app, x, hy, glyph::R, "Треки", ActNext);
        drawHint(app, x, hy, glyph::ZR, "Лайк", ActLike);
        drawHint(app, x, hy, glyph::X, "Главная", ActHome);
    } else if (app.screen == ScrPlayer) {
        drawHint(app, x, hy, glyph::A, "Пауза", ActPlay);
        drawHint(app, x, hy, glyph::B, app.source == SrcList && !app.openList.title.empty() ? "Список" : "Назад", ActBack);
        if (app.seekOnLR) {
            drawHint(app, x, hy, glyph::L, "", ActSeekBack);
            x -= 26;
            drawHint(app, x, hy, glyph::R, "Перемотка", ActSeekFwd);
        } else {
            drawHint(app, x, hy, glyph::L, "", ActPrev);
            x -= 26;
            drawHint(app, x, hy, glyph::R, "Треки", ActNext);
        }
        drawHint(app, x, hy, glyph::ZR, "Лайк", ActLike);
        drawHint(app, x, hy, glyph::ZL, "Скачать", ActDownload);
        drawHint(app, x, hy, glyph::X, "Главная", ActHome);
        drawHint(app, x, hy, glyph::Minus, "Поиск", ActSearch);
        drawHint(app, x, hy, glyph::Y, "Текст", ActLyrics);
    } else {
        bool offlineList = app.screen == ScrList && app.openList.kind == OFFLINE_KIND;
        drawHint(app, x, hy, glyph::A, app.screen == ScrLibrary ? "Открыть" : "Играть", ActActivateSel);
        drawHint(app, x, hy, glyph::B, "Назад", ActBack);
        if (app.screen == ScrList) drawHint(app, x, hy, glyph::Y, "Перемешать", ActShuffle);
        if (app.screen == ScrLibrary) drawHint(app, x, hy, glyph::Y, "На главную", ActNone);
        if (offlineList) {
            drawHint(app, x, hy, glyph::Minus, "Удалить", ActDelete);
        } else {
            if (app.screen == ScrList) drawHint(app, x, hy, glyph::ZL, "Скачать всё", ActDownloadAll);
            drawHint(app, x, hy, glyph::Minus, "Поиск", ActSearch);
        }
        drawHint(app, x, hy, glyph::X, "Главная", ActHome);
    }
    if (!app.kbd.active) drawHint(app, x, hy, glyph::Plus, "Выход", ActExit);
    std::string ver = std::string(isdigit((unsigned char)VERSION[0]) ? "v" : "") + VERSION;
    ui.text(ver, Ui::W - 30, Ui::H - 38, 16, col::dim, Align::Right);
    // Часы и заряд — левее версии, если подсказки до них не дотянулись.
    int clockRight = Ui::W - 30 - ui.textWidth(ver, 16) - 26;
    if (app.showClock && x < clockRight - 150) drawClock(app, clockRight, Ui::H - 40);

    ui.present();
}

// ---------- ввод ----------

// Вверх/вниз с автоповтором при удержании.
static int navDelta(PadState &pad) {
    static u64 heldSince = 0, lastRepeat = 0;
    u64 held = padGetButtons(&pad), down = padGetButtonsDown(&pad);
    const u64 UP = HidNpadButton_Up | HidNpadButton_StickLUp | HidNpadButton_StickRUp;
    const u64 DOWN = HidNpadButton_Down | HidNpadButton_StickLDown | HidNpadButton_StickRDown;
    int dir = (held & UP) ? -1 : (held & DOWN) ? 1 : 0;
    u64 now = nowMs();
    if (down & (UP | DOWN)) { heldSince = lastRepeat = now; return dir; }
    if (!dir) return 0;
    if (now - heldSince > 400 && now - lastRepeat > 70) { lastRepeat = now; return dir; }
    return 0;
}

// Влево/вправо с автоповтором (для перемотки).
static int seekDelta(PadState &pad) {
    static u64 heldSince = 0, lastRepeat = 0;
    u64 held = padGetButtons(&pad), down = padGetButtonsDown(&pad);
    const u64 LEFT = HidNpadButton_Left | HidNpadButton_StickLLeft;
    const u64 RIGHT = HidNpadButton_Right | HidNpadButton_StickLRight;
    int dir = (held & LEFT) ? -1 : (held & RIGHT) ? 1 : 0;
    u64 now = nowMs();
    if (down & (LEFT | RIGHT)) { heldSince = lastRepeat = now; return dir; }
    if (!dir) return 0;
    if (now - heldSince > 450 && now - lastRepeat > 250) { lastRepeat = now; return dir; }
    return 0;
}

// Правый стик вверх/вниз — громкость, с автоповтором при удержании.
static int volDelta(PadState &pad) {
    static u64 heldSince = 0, lastRepeat = 0;
    u64 held = padGetButtons(&pad), down = padGetButtonsDown(&pad);
    const u64 UP = HidNpadButton_StickRUp, DOWN = HidNpadButton_StickRDown;
    int dir = (held & UP) ? 1 : (held & DOWN) ? -1 : 0;
    u64 now = nowMs();
    if (down & (UP | DOWN)) { heldSince = lastRepeat = now; return dir; }
    if (!dir) return 0;
    if (now - heldSince > 400 && now - lastRepeat > 120) { lastRepeat = now; return dir; }
    return 0;
}

static void handleInput(App &app, PadState &pad, u64 down) {
    if (app.kbd.active) { // клавиатура поиска забирает все кнопки
        if (int d = navDelta(pad)) kbdMove(app, 0, d);
        if (int d = seekDelta(pad)) kbdMove(app, d, 0);
        if (down & HidNpadButton_A) kbdPress(app, app.kbd.row, app.kbd.col);
        if (down & HidNpadButton_B) popUtf8(app.kbd.text);
        if (down & HidNpadButton_X) kbdPress(app, KBD_LETTER_ROWS, KEY_SPACE);
        if (down & HidNpadButton_Y) app.kbd.ru = !app.kbd.ru;
        if (down & HidNpadButton_Plus) kbdSubmit(app);
        if (down & HidNpadButton_Minus) app.kbd.active = false;
        return;
    }
    if (app.loginFlow.active) { // экран входа: A — новый код, B — отмена (если уже был вход)
        if (down & HidNpadButton_A) doAction(app, ActLoginNew);
        if (down & HidNpadButton_B) doAction(app, ActLoginCancel);
        return;
    }
    bool hasTrack = app.current >= 0 && app.current < (int)app.queue.size();
    if (down & HidNpadButton_X) {
        if (app.screen != ScrHome) app.screen = ScrHome;
        else if (hasTrack) app.screen = ScrPlayer;
    }
    if (down & HidNpadButton_ZR) doAction(app, ActLike);
    if ((down & HidNpadButton_ZL) && app.screen == ScrPlayer) doAction(app, ActDownload);
    bool offlineList = app.screen == ScrList && app.openList.kind == OFFLINE_KIND;
    if ((down & HidNpadButton_Minus) && !offlineList) doAction(app, ActSearch);
    const u64 LEFT = HidNpadButton_AnyLeft, RIGHT = HidNpadButton_AnyRight;
    const u64 UP = HidNpadButton_AnyUp, DOWN = HidNpadButton_AnyDown;

    switch (app.screen) {
        case ScrHome: { // переход к ближайшему элементу в сторону нажатия — работает в любой раскладке
            int dx = (down & LEFT) ? -1 : (down & RIGHT) ? 1 : 0, dy = (down & UP) ? -1 : (down & DOWN) ? 1 : 0;
            if (hasScTab() && (down & HidNpadButton_L)) doAction(app, ActTabYa);
            if (hasScTab() && (down & HidNpadButton_R)) doAction(app, ActTabSc);
            if (app.sideFocus) { // боковое меню: вверх/вниз — раздел сразу, A на «Настройках» — открыть их
                std::vector<int> items = sideItems();
                int k = (int)(std::find(items.begin(), items.end(), app.sideSel) - items.begin());
                if (k >= (int)items.size()) k = 0;
                if (dy) k = std::max(0, std::min((int)items.size() - 1, k + dy));
                app.sideSel = items[k];
                if (dy && app.sideSel != 2 && app.homeTab != app.sideSel) {
                    app.homeTab = app.sideSel;
                    app.homeSel = 0;
                }
                if (dx > 0 || ((down & HidNpadButton_A) && app.sideSel != 2)) app.sideFocus = false;
                else if ((down & HidNpadButton_A) && app.sideSel == 2) doAction(app, ActSettings);
                return;
            }
            if ((dx || dy) && app.homeSel < (int)app.homeItems.size()) {
                const SDL_Rect &c = app.homeItems[app.homeSel].rc;
                float cx = c.x + c.w / 2.0f, cy = c.y + c.h / 2.0f, best = 1e9f;
                int pick = -1;
                for (int i = 0; i < (int)app.homeItems.size(); i++) {
                    if (i == app.homeSel) continue;
                    const SDL_Rect &r = app.homeItems[i].rc;
                    float ex = r.x + r.w / 2.0f - cx, ey = r.y + r.h / 2.0f - cy;
                    float along = ex * dx + ey * dy, across = std::fabs(ex * dy - ey * dx);
                    if (along <= 1) continue; // не в ту сторону
                    float score = along + across * 2.5f;
                    if (score < best) { best = score; pick = i; }
                }
                if (pick >= 0) app.homeSel = pick;
                else if (dx < 0) { // левее плиток ничего — в боковое меню, на текущий раздел
                    app.sideFocus = true;
                    app.sideSel = hasScTab() ? app.homeTab : 0;
                }
            }
            if (down & HidNpadButton_A) activateHome(app, app.homeSel);
            return;
        }
        case ScrRich: { // как на главной: к ближайшему элементу в сторону нажатия
            if (app.rich.empty()) { goBack(app); return; }
            App::RichPage &page = app.rich.back();
            int dx = (down & LEFT) ? -1 : (down & RIGHT) ? 1 : 0, dy = (down & UP) ? -1 : (down & DOWN) ? 1 : 0;
            if ((dx || dy) && page.sel < (int)app.richItems.size()) {
                const SDL_Rect &c = app.richItems[page.sel].rc;
                float cx = c.x + c.w / 2.0f, cy = c.y + c.h / 2.0f, best = 1e9f;
                int pick = -1;
                for (int i = 0; i < (int)app.richItems.size(); i++) {
                    if (i == page.sel) continue;
                    // В ряду карточек координата x уже сдвинута на прокрутку ряда — для выбора берём «логическую».
                    const App::RichItem &it = app.richItems[i];
                    const SDL_Rect &r = it.rc;
                    float ex = r.x + r.w / 2.0f - cx, ey = r.y + r.h / 2.0f - cy;
                    float along = ex * dx + ey * dy, across = std::fabs(ex * dy - ey * dx);
                    if (along <= 1) continue;
                    float score = along + across * 2.5f;
                    if (score < best) { best = score; pick = i; }
                }
                if (pick >= 0) page.sel = pick;
            }
            if (down & HidNpadButton_A) activateRich(app, page.sel);
            if (down & HidNpadButton_B) goBack(app);
            return;
        }
        case ScrTiles:
            app.tilesView.sel = std::max(0, std::min((int)app.tileOrder.size() - 1, app.tilesView.sel + navDelta(pad)));
            if (down & HidNpadButton_A) toggleTile(app, app.tilesView.sel);
            if (down & HidNpadButton_L) moveTile(app, -1);
            if (down & HidNpadButton_R) moveTile(app, 1);
            if (down & HidNpadButton_B) goBack(app);
            return;
        case ScrSettings:
        case ScrHomeSet: {
            ListView &v = curSetView(app);
            v.sel = std::max(0, std::min(setRowCount(setTable(app.screen)) - 1, v.sel + navDelta(pad)));
            if (down & LEFT) changeSetting(app, curSetting(app), -1);
            if (down & (RIGHT | HidNpadButton_A)) changeSetting(app, curSetting(app), 1);
            if (down & HidNpadButton_B) goBack(app);
            return;
        }
        case ScrPlayer:
            if (down & HidNpadButton_A) doAction(app, app.ready ? ActPlay : ActRetry);
            if (down & HidNpadButton_B) goBack(app);
            if (down & HidNpadButton_Y) doAction(app, ActLyrics);
            if (int d = volDelta(pad)) setAppVolume(app, app.volume + d * 5);
            if (down & HidNpadButton_Up) doAction(app, ActShuffleMode);
            if (down & HidNpadButton_Down) doAction(app, ActRepeat);
            if (app.seekOnLR) { // перемотка на L/R, треки — на крестовине и стике
                if (down & HidNpadButton_R) doAction(app, ActSeekFwd);
                if (down & HidNpadButton_L) doAction(app, ActSeekBack);
                if (down & (HidNpadButton_Right | HidNpadButton_StickLRight)) doAction(app, ActNext);
                if (down & (HidNpadButton_Left | HidNpadButton_StickLLeft)) doAction(app, ActPrev);
            } else {
                if (down & HidNpadButton_R) doAction(app, ActNext);
                if (down & HidNpadButton_L) doAction(app, ActPrev);
                if (int d = seekDelta(pad)) seekTo(app, app.player.position() + d * app.seekStep);
            }
            return;
        case ScrLyrics: {
            if (down & HidNpadButton_A) doAction(app, ActPlay);
            if (down & (HidNpadButton_B | HidNpadButton_Y)) goBack(app);
            if (down & HidNpadButton_R) doAction(app, ActNext);
            if (down & HidNpadButton_L) doAction(app, ActPrev);
            if (int d = seekDelta(pad)) seekTo(app, app.player.position() + d * app.seekStep);
            // Листаем сами — на 4 секунды перестаём следовать за песней.
            if (int d = navDelta(pad)) {
                app.lyricsY += d * LYR_ROW * 2;
                app.lyricsManualUntil = nowMs() + 4000;
            }
            return;
        }
        case ScrQueue:
            app.queueView.sel += navDelta(pad);
            if (down & HidNpadButton_A) doAction(app, ActItemBase + app.queueView.sel);
            if (down & HidNpadButton_B) goBack(app);
            if (down & HidNpadButton_R) doAction(app, ActNext);
            if (down & HidNpadButton_L) doAction(app, ActPrev);
            return;
        default:
            break;
    }

    // Экраны со списками
    if (down & HidNpadButton_B) doAction(app, ActBack);
    if (down & HidNpadButton_R) doAction(app, ActNext);
    if (down & HidNpadButton_L) doAction(app, ActPrev);
    ListView &v = app.screen == ScrLibrary ? app.libView : app.listView;
    v.sel += navDelta(pad);
    if (down & HidNpadButton_A) doAction(app, ActItemBase + v.sel);
    if (app.screen == ScrList && (down & HidNpadButton_Y)) doAction(app, ActShuffle);
    if (app.screen == ScrLibrary && (down & HidNpadButton_Y)) bookmarkLibrary(app, app.libView.sel);
    if (app.screen == ScrList && (down & HidNpadButton_ZL)) doAction(app, ActDownloadAll);
    if (app.screen == ScrList && (down & HidNpadButton_Minus)) doAction(app, ActDelete);
}
// ---------- main ----------

int main(int argc, char *argv[]) {
    crashLogInit("sdmc:/switch/yamu-nx/app.log", "[app]"); // почему закрылись — см. crashlog.hpp
    crashLog("версия %s", VERSION);
    socketInitializeDefault();
    curl_global_init(CURL_GLOBAL_DEFAULT);
    appletSetMediaPlaybackState(true); // не гасить экран, пока играет музыка

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    hidInitializeTouchScreen();

    App app;
    if (!app.ui.init()) {
        // Без графики — хотя бы текстом.
        consoleInit(NULL);
        printf("Graphics init failed: %s\nPress + to exit\n", SDL_GetError());
        while (appletMainLoop()) {
            padUpdate(&pad);
            if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
            consoleUpdate(NULL);
        }
        consoleExit(NULL);
        return 0;
    }
    if (!app.player.init()) app.startupError = "Не удалось открыть аудиовыход";
    loadOffline(app);
    loadRecent(app);
    loadBookmarks(app);
    loadSettings(app);
    app.player.setVolume(app.volume);
    pmshellInitialize();
    psmInitialize(); // заряд батареи для часов внизу
    // Модуль замолкает и отдаёт свой трек; если он ничего не играл — показываем последний трек на паузе.
    if (!fetchTakeover(app) && app.wantResume()) readSnapshot(app, LAST_PATH);
    // Не засыпать при HOME: успеваем отдать музыку фону, пока пользователь выбирает игру.
    appletSetFocusHandlingMode(AppletFocusHandlingMode_NoSuspend);
    app.worker.start(TOKEN_PATH);
    app.downloader.start(TOKEN_PATH);
    app.coverWorker.start(TOKEN_PATH);
    if (app.startupError.empty()) app.worker.post(Job{Job::Startup});
    else app.started = true;

    bool wasTouching = false, touchDragged = false;
    int touchStartX = 0, touchStartY = 0, touchLastX = 0, touchLastY = 0, dragAccum = 0;
    AppletFocusState lastFocus = AppletFocusState_InFocus;
    bool exitedByUser = false;
    if (SoundCloud::loggedIn()) {
        app.scUser = SoundCloud::userName();
        if (app.scUser.empty()) app.scUser = "вход выполнен";
    }
    u64 heapLoggedAt = nowMs();
    while (appletMainLoop()) {
        // Вход в SoundCloud: телефон прислал код — меняем его на токен.
        if (app.scLogin.running()) {
            std::string code;
            if (app.scLogin.takeCode(code)) {
                Job j{Job::ScLogin};
                j.query = code;
                j.url = app.scVerifier;
                app.worker.post(j);
                showToast(app, "SoundCloud: вхожу…");
            }
        }
        // HOME: отдаём музыку фону; вернулись — забираем обратно.
        AppletFocusState focus = appletGetFocusState();
        if (focus != lastFocus && nowMs() < app.focusGraceUntil) lastFocus = focus; // это была наша клавиатура
        if (focus != lastFocus) {
            crashLog("фокус: %s", focus == AppletFocusState_InFocus ? "вернулись" : "ушли (HOME или апплет)");
            if (focus != AppletFocusState_InFocus) {
                handOff(app, true);
            } else if (app.handedOff) {
                app.handedOff = false;
                if (app.ready && fetchTakeover(app)) applyTakeover(app);
            }
            lastFocus = focus;
        }
        if (focus != AppletFocusState_InFocus) { // не на экране — не рисуем и не опрашиваем кнопки
            JobResult r;
            while (app.worker.poll(r)) handleResult(app, r);
            while (app.downloader.poll(r)) handleResult(app, r);
            while (app.coverWorker.poll(r)) handleResult(app, r);
            svcSleepThread(50000000ULL);
            continue;
        }
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);
        if (((down & HidNpadButton_Plus) && !app.kbd.active) || app.quit) { // в клавиатуре «+» — «Найти»
            crashLog("выход: %s", app.quit ? "кнопка «Выход»" : "«+»");
            exitedByUser = true;
            break;
        }
        if (nowMs() - heapLoggedAt > 300000) { // раз в 5 минут — сколько памяти занято
            heapLoggedAt = nowMs();
            unsigned used = 0, freeKb = 0;
            heapKb(used, freeKb);
            crashLog("куча: занято %u КБ, очередь %d, обложек %d", used, (int)app.queue.size(), (int)app.covers.size());
        }
        handleInput(app, pad, down);

        // Тач: свайп по списку прокручивает его, короткое касание — нажатие (по отпусканию,
        // чтобы при прокрутке ничего случайно не включилось).
        HidTouchScreenState ts{};
        bool touching = hidGetTouchScreenStates(&ts, 1) && ts.count > 0;
        int tx = touching ? (int)ts.touches[0].x : 0, ty = touching ? (int)ts.touches[0].y : 0;
        if (touching && !wasTouching) {
            // Палец лёг на полосу прогресса в плеере — перемотка свайпом.
            const SDL_Rect &vb = app.volBar;
            if (vb.w > 0 && std::abs(ty - vb.y) <= 28 && tx >= vb.x - 24 && tx <= vb.x + vb.w + 24) {
                app.volDrag = true;
                setAppVolume(app, (int)((tx - vb.x) * 100.0 / vb.w), false);
            }
            const SDL_Rect &sb = app.seekBar;
            if (sb.w > 0 && app.ready && std::abs(ty - sb.y) <= 30 && tx >= sb.x - 20 && tx <= sb.x + sb.w + 20)
                app.scrubFrac = std::max(0.0, std::min(1.0, (double)(tx - sb.x) / sb.w));
            touchStartX = touchLastX = tx;
            touchStartY = touchLastY = ty;
            touchDragged = false;
            dragAccum = 0;
        } else if (touching) {
            if (app.volDrag && app.volBar.w > 0) setAppVolume(app, (int)((tx - app.volBar.x) * 100.0 / app.volBar.w), false);
            if (app.scrubFrac >= 0 && app.seekBar.w > 0)
                app.scrubFrac = std::max(0.0, std::min(1.0, (double)(tx - app.seekBar.x) / app.seekBar.w));
            if (!touchDragged && (std::abs(ty - touchStartY) > 14 || std::abs(tx - touchStartX) > 14)) {
                touchDragged = true;
                // Свайп вбок по ряду карточек (альбомы исполнителя и т. п.) листает ряд, а не страницу.
                app.richHDrag = -1;
                if (app.screen == ScrRich && !app.kbd.active && std::abs(tx - touchStartX) > std::abs(ty - touchStartY))
                    for (const App::RichRow &row : app.richRows) {
                        SDL_Rect rc = row.rc;
                        rc.y += app.richOff;
                        SDL_Point p{touchStartX, touchStartY};
                        if (SDL_PointInRect(&p, &rc)) { app.richHDrag = row.section; break; }
                    }
            }
            bool inList = !app.kbd.active && (app.screen == ScrList || app.screen == ScrLibrary || app.screen == ScrQueue) && touchStartY >= LIST_TOP - 10 &&
                          touchStartY <= LIST_BOTTOM;
            if (touchDragged && inList) {
                ListView &v = app.screen == ScrLibrary ? app.libView : app.screen == ScrQueue ? app.queueView : app.listView;
                int rowH = app.screen == ScrLibrary ? LIB_ROW_H : LIST_ROW_H;
                int visible = (LIST_BOTTOM - LIST_TOP) / rowH;
                int count = app.screen == ScrLibrary ? libraryCount(app)
                          : app.screen == ScrQueue    ? (app.current >= 0 ? (int)app.queue.size() - app.current : 0)
                                                      : (int)app.listTracks.size();
                dragAccum += touchLastY - ty; // палец вверх — список вверх
                while (dragAccum >= rowH) { v.scroll++; dragAccum -= rowH; }
                while (dragAccum <= -rowH) { v.scroll--; dragAccum += rowH; }
                v.scroll = std::max(0, std::min(v.scroll, std::max(0, count - visible)));
                // Выделение держим в видимой части, иначе clamp() вернёт прокрутку обратно.
                v.sel = std::max(v.scroll, std::min(v.sel, v.scroll + visible - 1));
            }
            if (touchDragged && !app.kbd.active && app.screen == ScrRich && !app.rich.empty()) {
                App::RichPage &page = app.rich.back();
                if (app.richHDrag >= 0 && app.richHDrag < (int)page.sections.size())
                    page.sections[app.richHDrag].sx += touchLastX - tx; // палец влево — ряд вправо
                else
                    page.scroll += touchLastY - ty;
            }
            if (touchDragged && app.screen == ScrLyrics && app.scrubFrac < 0) {
                app.lyricsY += touchLastY - ty;
                app.lyricsManualUntil = nowMs() + 4000;
            }
            touchLastX = tx;
            touchLastY = ty;
        } else if (wasTouching && app.volDrag) { // отпустили ползунок — запоминаем громкость
            app.volDrag = false;
            saveSettings(app);
        } else if (wasTouching && app.scrubFrac >= 0) { // отпустили полосу — перематываем туда
            if (app.current >= 0 && app.current < (int)app.queue.size())
                seekTo(app, app.scrubFrac * app.queue[app.current].durationMs / 1000.0);
            app.scrubFrac = -1;
            app.lyricsManualUntil = 0; // текст сразу едет к новому месту
        } else if (wasTouching && !touchDragged) {
            SDL_Point p{touchStartX, touchStartY};
            app.touchX = p.x;
            for (const Button &b : app.buttons)
                if (SDL_PointInRect(&p, &b.rc)) { doAction(app, b.action); break; }
        }
        wasTouching = touching;

        JobResult r;
        while (app.worker.poll(r)) handleResult(app, r);
        while (app.downloader.poll(r)) handleResult(app, r);
        while (app.coverWorker.poll(r)) handleResult(app, r);
        watchSettings(app);

        if (app.ready && app.current >= 0 && app.current < (int)app.queue.size()) {
            Player::State st = app.player.state();
            if (st == Player::Finished) {
                feedback(app, "trackFinished", &app.queue[app.current], app.player.position());
                playIndex(app, app.repeat == App::RepeatOne ? app.current : app.current + 1);
            } else if (st == Player::Error && !app.skipAt) {
                app.error = app.player.error();
                app.skipAt = nowMs() + AUTO_SKIP_MS;
            }
            if (app.skipAt && nowMs() >= app.skipAt) playIndex(app, app.current + 1);
            maybeCrossfade(app);
            warmNextSc(app);
        }
        if (app.ready) {
            ynTick(app);
        }
        loginTick(app);

        if (app.screen == ScrLyrics) requestLyrics(app); // трек сменился — подгружаем его текст
        draw(app); // vsync держит 60 к/с
    }

    // Выход по + или закрытие ради игры: играло — продолжится в фоне; не играло — запомним трек.
    if (!handOff(app, false) && !app.handedOff && app.wantResume()) writeSnapshot(app, LAST_PATH, app.player.position() > 0 ? app.player.position() : std::max(0.0, app.pendingSeek), true);
    pmshellExit();
    psmExit();
    appletSetFocusHandlingMode(AppletFocusHandlingMode_SuspendHomeSleep); // как было до нас — для загрузчика Homebrew
    app.player.shutdown();
    app.connect.stop();
    if (!exitedByUser) crashLog("выход: система закрыла приложение (appletMainLoop)");
    app.coverWorker.stop();
    app.downloader.stop(); // недокачанный файл останется .part и удалится при следующем запуске
    app.scLogin.stop();
    app.worker.stop();
    for (auto &kv : app.covers) SDL_DestroyTexture(kv.second.tex);
    app.ui.shutdown();
    appletSetMediaPlaybackState(false);
    curl_global_cleanup();
    socketExit();
    return 0;
}
