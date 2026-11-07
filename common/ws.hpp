// Минимальный WebSocket-клиент (wss://) поверх TLS-соединения curl: в curl из devkitPro своего WebSocket нет.
// Текстовые сообщения, ping/pong, без расширений. Не потокобезопасен — один поток на соединение.
#pragma once
#include <string>
#include <vector>

class WebSocket {
public:
    ~WebSocket() { close(); }
    // url: wss://host/path. headers — дополнительные строки заголовков («Name: value»).
    bool connect(const std::string &url, const std::vector<std::string> &headers, std::string &err);
    bool sendText(const std::string &text);
    bool ping();
    // 1 — пришло сообщение, 0 — за timeoutMs ничего, -1 — соединение закрыто.
    int recv(std::string &msg, int timeoutMs);
    void close();
    bool isOpen() const { return curl_ != nullptr; }
    int httpStatus() const { return status_; }
    // Почему сервер закрыл соединение: код и причина из кадра close.
    const std::string &closeReason() const { return closeReason_; }

private:
    bool sendFrame(int opcode, const std::string &payload);
    bool sendAll(const char *data, size_t len);
    int fill(int timeoutMs); // дочитать байты в in_: 1 — есть новые, 0 — таймаут, -1 — ошибка

    void *curl_ = nullptr;
    int fd_ = -1;
    int status_ = 0;
    std::string in_;      // принятые, ещё не разобранные байты
    std::string partial_; // фрагменты сообщения до FIN
    std::string closeReason_;
};
