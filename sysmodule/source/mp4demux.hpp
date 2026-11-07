// Минимальный разбор MP4/M4A с карты: первая аудиодорожка AAC — её конфиг и таблица кадров.
// Хватает для скачанных треков Яндекса; редактирование, фрагменты (moof) и прочее не поддерживаем.
#pragma once
#include <cstdio>
#include <string>
#include <vector>
#include <switch.h>

struct Mp4Audio {
    std::vector<u8> asc;      // AudioSpecificConfig для декодера
    std::vector<u64> offsets; // где в файле каждый кадр
    std::vector<u32> sizes;   // и его размер
    u32 timescale = 0;
    struct Run { u32 count, delta; };
    std::vector<Run> stts;    // длительности кадров

    bool open(FILE *f, std::string &err);
    // Номер кадра, с которого начать, чтобы попасть в sec.
    size_t frameAt(double sec) const;
};
