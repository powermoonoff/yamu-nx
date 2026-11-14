// yamu-sys — фоновый плеер Яндекс Музыки (системный модуль Atmosphère).
// Управляется текстовыми командами по TCP на 127.0.0.1:47123: одна строка запроса — одна строка ответа.
#include <arpa/inet.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <sys/stat.h>
#include <ctime>
#include <sys/time.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <switch.h>
#include <curl/curl.h>
#include "core.hpp"
#include "crashlog.hpp"
#include "http.hpp"
#include "probe.hpp"

static const char *TOKEN_PATH = "sdmc:/switch/yamu-nx/token.txt";
static const char *LOG_PATH = "sdmc:/switch/yamu-nx/sys.log";

// Журнал запуска: модуль не видно, поэтому пишем каждый шаг в файл.
void ymLog(const char *fmt, ...) {
    FILE *f = fopen(LOG_PATH, "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(f, "[%7.2f] ", armTicksToNs(armGetSystemTick()) / 1e9); // секунды с включения — чтобы мерить паузы
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}
static const int PORT = 47123;
static Result g_mountRc = 0;

extern "C" {
u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;
extern TimeServiceType __nx_time_service_type;
void __libnx_init_time(void);

// Своя куча: модулю никто не выделит память автоматически.
#define HEAP_SIZE (10 * 1024 * 1024)
static char g_heap[HEAP_SIZE];
void __libnx_initheap(void) {
    extern char *fake_heap_start, *fake_heap_end;
    fake_heap_start = g_heap;
    fake_heap_end = g_heap + HEAP_SIZE;
}

void __appInit(void) {
    if (R_FAILED(smInitialize())) diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_SM));
    if (R_SUCCEEDED(setsysInitialize())) {
        SetSysFirmwareVersion fw;
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
        setsysExit();
    }
    fsInitialize();
    g_mountRc = fsdevMountSdmc();
    struct stat st;
    if (stat(LOG_PATH, &st) == 0 && st.st_size > 64 * 1024) remove(LOG_PATH);
    ymLog("[init] fs ok");
    // Время нужно для подписи запросов. Модулю пользовательский time:u не положен — берём time:s.
    __nx_time_service_type = TimeServiceType_System;
    Result rc = timeInitialize();
    if (R_FAILED(rc)) {
        __nx_time_service_type = TimeServiceType_User;
        rc = timeInitialize();
    }
    ymLog("[init] time: 0x%x", rc);
    __libnx_init_time();
    ymLog("[init] now: %lld", (long long)time(nullptr));
    static const SocketInitConfig cfg = {
        .tcp_tx_buf_size = 0x4000,
        .tcp_rx_buf_size = 0x8000,
        .tcp_tx_buf_max_size = 0x10000,
        .tcp_rx_buf_max_size = 0x20000,
        .udp_tx_buf_size = 0x2400,
        .udp_rx_buf_size = 0xA500,
        .sb_efficiency = 4,
        .num_bsd_sessions = 3,
        .bsd_service_type = BsdServiceType_Auto,
    };
    rc = socketInitialize(&cfg);
    ymLog("[init] socket: 0x%x", rc);
    // sm не закрываем: audout и ssl запускаются позже, им нужен менеджер служб.
}

void __appExit(void) {
    socketExit();
    timeExit();
    fsdevUnmountAll();
    fsExit();
    smExit();
}
}

static const char *MODULE_VERSION = "0.2.0"; // локальные сборки для отладки — "developer"
static const u64 SYS_TID = 0x4200000000594D53ULL;

static void sendLine(int fd, const std::string &s) {
    std::string out = s + "\n";
    for (size_t off = 0; off < out.size();) {
        ssize_t n = send(fd, out.data() + off, out.size() - off, 0);
        if (n <= 0) { ymLog("[net] send n=%d errno=%d", (int)n, errno); return; }
        off += n;
    }
}

// Закрывать локальное соединение сразу (RST), без минутного TIME_WAIT: иначе частые опросы
// оверлея съедают сетевую память модуля, и ему нечем подключиться к Яндексу.
static void noLinger(int fd) {
    linger lg{1, 0};
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
}

// Блокирующий сокет с таймаутом приёма в секунду: читаем до перевода строки.
static std::string recvLine(int fd) {
    timeval tv{1, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    std::string line;
    char buf[256];
    while (line.size() < 1024 && line.find('\n') == std::string::npos) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            if (line.empty()) ymLog("[net] recv n=%d errno=%d", (int)n, errno);
            break;
        }
        line.append(buf, n);
    }
    size_t nl = line.find('\n');
    if (nl != std::string::npos) line.resize(nl);
    while (!line.empty() && line.back() == '\r') line.pop_back();
    return line;
}

// Открыть слушающий сокет (неблокирующий). attempts — сколько раз пробовать bind.
static int openListener(const sockaddr_in &addr, int attempts) {
    for (int i = 0; i < attempts; i++) {
        int s;
        {
            std::lock_guard<std::recursive_mutex> lk(socketMutex());
            s = socket(AF_INET, SOCK_STREAM, 0);
        }
        if (s >= 0 && bind(s, (const sockaddr *)&addr, sizeof(addr)) == 0 && listen(s, 8) == 0) {
            fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
            return s;
        }
        ymLog("[main] bind attempt %d errno=%d", i, errno);
        if (s >= 0) {
            std::lock_guard<std::recursive_mutex> lk(socketMutex());
            close(s);
        }
        svcSleepThread(1000000000ULL);
    }
    return -1;
}

// Не запущен ли уже другой экземпляр: спрашиваем менеджер процессов, чей PID числится за нашим ID.
static bool anotherInstance() {
    u64 me = 0, registered = 0;
    svcGetProcessId(&me, CUR_PROCESS_HANDLE);
    if (R_FAILED(pmdmntInitialize())) return false;
    Result rc = pmdmntGetProcessId(&registered, SYS_TID);
    pmdmntExit();
    ymLog("[main] pid me=%llu registered=%llu rc=0x%x", (unsigned long long)me, (unsigned long long)registered, rc);
    return R_SUCCEEDED(rc) && registered && registered != me;
}

int main(int argc, char *argv[]) {
    ymLog("===== start v%s, mount=0x%x", MODULE_VERSION, g_mountRc);
    crashLogInit(LOG_PATH, "[sys]"); // нет памяти или abort() — запишем причину перед выходом
    if (anotherInstance()) {
        ymLog("[main] another instance is running, exiting");
        return 0;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    // Порт занимаем до всего остального — он же служит замком от второго экземпляра.
    int srv = openListener(addr, 10);
    if (srv < 0) {
        ymLog("[main] bind/listen FAILED errno=%d", errno);
        probeLong();
        probeLong(); // два длинных — не открылся порт
        return 2;
    }
    ymLog("[main] listening on %d", PORT);

    curl_global_init(CURL_GLOBAL_DEFAULT);
    Core core;
    if (!core.init(TOKEN_PATH)) {
        ymLog("[main] core init FAILED");
        probeLong(); // длинный сигнал — не открылся звук/плеер
        return 1;
    }
    ymLog("[main] core ok");

    bool running = true;
    unsigned statusCount = 0, acceptErrors = 0;
    while (running) {
        // Неблокирующий accept под общим замком сокетов: пока мы его держим, curl в других
        // потоках не открывает и не закрывает соединения (иначе таблица сокетов libnx портится).
        int fd;
        {
            std::lock_guard<std::recursive_mutex> lk(socketMutex());
            fd = accept(srv, nullptr, nullptr);
        }
        if (fd < 0) {
            int err = errno;
            if (err == EAGAIN || err == EWOULDBLOCK) {
                static u64 heapAt = 0;
                u64 now = armTicksToNs(armGetSystemTick()) / 1000000ULL;
                if (now - heapAt > 300000) { // раз в 5 минут — сколько кучи занято (из 10 МБ)
                    heapAt = now;
                    unsigned used = 0, freeKb = 0;
                    heapKb(used, freeKb);
                    crashLog("куча: занято %u КБ, свободно %u КБ", used, freeKb);
                }
                svcSleepThread(20000000ULL);
                continue;
            }
            // После сна консоли система закрывает сокеты — порт «умирает». Пересоздаём его.
            if (acceptErrors++ % 50 == 0) ymLog("[main] accept errno=%d, reopening listener", err);
            {
                std::lock_guard<std::recursive_mutex> lk(socketMutex());
                close(srv);
            }
            svcSleepThread(500000000ULL);
            srv = openListener(addr, 1);
            if (srv < 0) svcSleepThread(1000000000ULL);
            continue;
        }
        acceptErrors = 0;
        // Принятое соединение наследует неблокирующий режим — возвращаем блокирующий.
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) & ~O_NONBLOCK);
        noLinger(fd);
        std::string cmd = recvLine(fd);
        if (cmd != "STATUS" || statusCount++ % 100 == 0) ymLog("[net] '%s'", cmd.c_str());
        if (cmd == "STATUS") sendLine(fd, core.statusJson());
        else if (cmd == "LIBRARY") sendLine(fd, core.libraryJson());
        else if (cmd == "PING") sendLine(fd, "PONG");
        else if (cmd == "RELEASE") sendLine(fd, core.releaseSync() ? "OK" : "EMPTY");
        else if (cmd == "EXIT") { sendLine(fd, "OK"); running = false; }
        else if (!cmd.empty()) { core.command(cmd); sendLine(fd, "OK"); }
        std::lock_guard<std::recursive_mutex> lk(socketMutex());
        close(fd);
    }
    ymLog("[main] exit");
    close(srv);
    core.shutdown();
    curl_global_cleanup();
    return 0;
}