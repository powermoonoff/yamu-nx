// Звуковой «журнал» для отладки модуля, когда в файл писать не получается:
// probeBeep(n) — n коротких сигналов, probeLong() — один длинный низкий.
#pragma once
#include <cmath>
#include <cstring>
#include <malloc.h>
#include <switch.h>

namespace probe_detail {
inline void play(int count, int toneMs, int gapMs, float freq) {
    const int rate = 48000;
    size_t frames = (size_t)rate * (toneMs + gapMs) / 1000;
    size_t bytes = (frames * 4 + 0xFFF) & ~(size_t)0xFFF;
    s16 *buf = (s16 *)memalign(0x1000, bytes);
    if (!buf) return;
    memset(buf, 0, bytes);
    size_t toneFrames = (size_t)rate * toneMs / 1000;
    for (size_t i = 0; i < toneFrames; i++) {
        s16 v = (s16)(9000 * sinf(2 * 3.14159265f * freq * i / rate));
        buf[i * 2] = buf[i * 2 + 1] = v;
    }
    bool started = R_SUCCEEDED(audoutInitialize()) && R_SUCCEEDED(audoutStartAudioOut());
    if (started) {
        for (int k = 0; k < count; k++) {
            AudioOutBuffer src{}, *rel = nullptr;
            src.buffer = buf;
            src.buffer_size = bytes;
            src.data_size = frames * 4;
            audoutPlayBuffer(&src, &rel);
        }
        audoutStopAudioOut();
    }
    audoutExit();
    free(buf);
}
}

inline void probeBeep(int n) { probe_detail::play(n, 150, 200, 1200.0f); }
inline void probeLong() { probe_detail::play(1, 1200, 300, 300.0f); }
