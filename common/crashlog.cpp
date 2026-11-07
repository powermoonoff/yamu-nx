#include "crashlog.hpp"
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <malloc.h>
#include <new>
#include <sys/stat.h>
#include <switch.h>

extern "C" {
extern char __start__; // начало нашего кода (switch.ld): адреса в журнале — от него, как в отчётах Atmosphère
FILE *__real_fopen(const char *path, const char *mode);
int __real_fclose(FILE *f);
void __real_abort(void) __attribute__((noreturn));
}

static const char *g_path = nullptr;
static const char *g_who = "";
// Запас памяти на запись о нехватке: fopen сам берёт память из кучи, а её в этот момент нет.
static void *g_reserve = nullptr;

void heapKb(unsigned &usedKb, unsigned &freeKb) {
    struct mallinfo mi = mallinfo();
    usedKb = (unsigned)(mi.uordblks / 1024);
    freeKb = (unsigned)(mi.fordblks / 1024);
}

// Файл открываем только на время строки: открытый постоянно журнал не отдаётся по MTP.
// __real_fopen/__real_fclose — мимо обёрток с замком сокетов, чтобы в аварии не ждать замок.
static void vlog(const char *fmt, va_list ap) {
    if (!g_path) return;
    FILE *f = __real_fopen(g_path, "a");
    if (!f) return;
    double t = armTicksToNs(armGetSystemTick()) / 1e9;
    fprintf(f, "[%8.2f] %s ", t, g_who);
    vfprintf(f, fmt, ap);
    fputc('\n', f);
    __real_fclose(f);
}

void crashLog(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(fmt, ap);
    va_end(ap);
}

// Нет памяти: operator new зовёт этот обработчик (исключений нет — дальше был бы тихий abort).
static void onNoMemory() {
    free(g_reserve);
    g_reserve = nullptr;
    unsigned used = 0, freeKb = 0;
    heapKb(used, freeKb);
    crashLog("[fatal] нет памяти: занято %u КБ, свободно %u КБ (из них кусками)", used, freeKb);
    std::set_new_handler(nullptr);
    __real_abort();
}

void crashLogInit(const char *path, const char *who) {
    g_who = who;
    g_path = path;
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > 64 * 1024) { // разросся — начинаем заново
        if (FILE *f = __real_fopen(path, "w")) __real_fclose(f);
    }
    g_reserve = malloc(64 * 1024);
    std::set_new_handler(onNoMemory);
    unsigned used = 0, freeKb = 0;
    heapKb(used, freeKb);
    crashLog("===== старт, куча: занято %u КБ", used);
}

extern "C" void __wrap_abort(void) __attribute__((noreturn));
extern "C" void __wrap_abort(void) {
    free(g_reserve);
    g_reserve = nullptr;
    unsigned used = 0, freeKb = 0;
    heapKb(used, freeKb);
    uintptr_t ret = (uintptr_t)__builtin_return_address(0), base = (uintptr_t)&__start__;
    crashLog("[fatal] abort() из +0x%lx, куча: занято %u КБ, свободно %u КБ", (unsigned long)(ret - base), used, freeKb);
    __real_abort();
}
