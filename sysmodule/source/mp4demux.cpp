#include "mp4demux.hpp"
#include <cstring>

namespace {

u32 be32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
u64 be64(const u8 *p) { return ((u64)be32(p) << 32) | be32(p + 4); }

// Обход дочерних боксов в памяти: [p, end).
struct Box { const u8 *data; size_t size; char type[5]; };

bool nextBox(const u8 *&p, const u8 *end, Box &b) {
    if (end - p < 8) return false;
    u64 size = be32(p);
    memcpy(b.type, p + 4, 4);
    b.type[4] = 0;
    size_t hdr = 8;
    if (size == 1) {
        if (end - p < 16) return false;
        size = be64(p + 8);
        hdr = 16;
    } else if (size == 0) {
        size = end - p;
    }
    if (size < hdr || size > (u64)(end - p)) return false;
    b.data = p + hdr;
    b.size = size - hdr;
    p += size;
    return true;
}

bool findChild(const u8 *p, size_t n, const char *type, Box &out) {
    const u8 *end = p + n;
    Box b;
    while (nextBox(p, end, b))
        if (!memcmp(b.type, type, 4)) { out = b; return true; }
    return false;
}

// Длина дескриптора MPEG-4: до 4 байт по 7 бит.
size_t descLen(const u8 *&p, const u8 *end) {
    size_t len = 0;
    for (int i = 0; i < 4 && p < end; i++) {
        u8 c = *p++;
        len = (len << 7) | (c & 0x7F);
        if (!(c & 0x80)) break;
    }
    return len;
}

// esds → DecoderSpecificInfo (тег 5) — это и есть AudioSpecificConfig.
bool parseEsds(const u8 *p, size_t n, std::vector<u8> &asc) {
    const u8 *end = p + n;
    p += 4; // версия и флаги
    while (p + 2 <= end) {
        u8 tag = *p++;
        size_t len = descLen(p, end);
        if (tag == 0x03) { // ES_Descriptor: заходим внутрь
            if (p + 3 > end) return false;
            u8 flags = p[2];
            p += 3;
            if (flags & 0x80) p += 2;
            if (flags & 0x40) { if (p >= end) return false; p += 1 + *p; }
            if (flags & 0x20) p += 2;
            continue;
        }
        if (tag == 0x04) { p += 13; continue; } // DecoderConfigDescriptor: пропускаем шапку
        if (tag == 0x05) {
            if (p + len > end) return false;
            asc.assign(p, p + len);
            return true;
        }
        p += len;
    }
    return false;
}

} // namespace

bool Mp4Audio::open(FILE *f, std::string &err) {
    // Ищем moov среди боксов верхнего уровня и читаем его целиком.
    fseek(f, 0, SEEK_END);
    u64 fileSize = (u64)ftell(f);
    u64 pos = 0;
    std::vector<u8> moov;
    while (pos + 8 <= fileSize) {
        u8 h[16];
        fseek(f, (long)pos, SEEK_SET);
        if (fread(h, 1, 16, f) < 8) break;
        u64 size = be32(h);
        size_t hdr = 8;
        if (size == 1) { size = be64(h + 8); hdr = 16; }
        else if (size == 0) size = fileSize - pos;
        if (size < hdr) break;
        if (!memcmp(h + 4, "moov", 4)) {
            if (size > (8u << 20)) { err = "moov слишком большой"; return false; }
            moov.resize(size - hdr);
            fseek(f, (long)(pos + hdr), SEEK_SET);
            if (fread(moov.data(), 1, moov.size(), f) != moov.size()) { err = "moov не читается"; return false; }
            break;
        }
        pos += size;
    }
    if (moov.empty()) { err = "нет moov"; return false; }

    const u8 *p = moov.data(), *end = p + moov.size();
    Box trak;
    while (nextBox(p, end, trak)) {
        if (memcmp(trak.type, "trak", 4)) continue;
        Box mdia, hdlr, mdhd, minf, stbl, stsd, sttsB, stsc, stsz, stco;
        if (!findChild(trak.data, trak.size, "mdia", mdia)) continue;
        if (!findChild(mdia.data, mdia.size, "hdlr", hdlr) || hdlr.size < 12 || memcmp(hdlr.data + 8, "soun", 4)) continue;
        if (!findChild(mdia.data, mdia.size, "mdhd", mdhd) || mdhd.size < 24) continue;
        timescale = mdhd.data[0] == 1 ? be32(mdhd.data + 20) : be32(mdhd.data + 12);
        if (!findChild(mdia.data, mdia.size, "minf", minf) || !findChild(minf.data, minf.size, "stbl", stbl)) continue;
        if (!findChild(stbl.data, stbl.size, "stsd", stsd) || !findChild(stbl.data, stbl.size, "stts", sttsB) ||
            !findChild(stbl.data, stbl.size, "stsc", stsc) || !findChild(stbl.data, stbl.size, "stsz", stsz))
            continue;
        bool co64 = false;
        if (!findChild(stbl.data, stbl.size, "stco", stco)) {
            if (!findChild(stbl.data, stbl.size, "co64", stco)) continue;
            co64 = true;
        }

        // stsd → mp4a → esds
        if (stsd.size < 8) continue;
        const u8 *ep = stsd.data + 8, *eend = stsd.data + stsd.size;
        Box entry;
        if (!nextBox(ep, eend, entry) || memcmp(entry.type, "mp4a", 4)) { err = "не AAC"; return false; }
        if (entry.size < 28) continue;
        u16 ver = (entry.data[8] << 8) | entry.data[9]; // версия звукового описания QuickTime
        size_t skip = 28 + (ver == 1 ? 16 : ver == 2 ? 36 : 0);
        if (entry.size < skip) continue;
        Box esds;
        if (!findChild(entry.data + skip, entry.size - skip, "esds", esds) || !parseEsds(esds.data, esds.size, asc)) {
            err = "нет конфига AAC";
            return false;
        }

        // Длительности кадров
        if (sttsB.size < 8) continue;
        u32 n = be32(sttsB.data + 4);
        for (u32 i = 0; i < n && 8 + i * 8 + 8 <= sttsB.size; i++)
            stts.push_back({be32(sttsB.data + 8 + i * 8), be32(sttsB.data + 12 + i * 8)});

        // Размеры кадров
        if (stsz.size < 12) continue;
        u32 fixed = be32(stsz.data + 4), count = be32(stsz.data + 8);
        sizes.resize(count);
        for (u32 i = 0; i < count; i++)
            sizes[i] = fixed ? fixed : (12 + i * 4 + 4 <= stsz.size ? be32(stsz.data + 12 + i * 4) : 0);

        // Чанки и сколько кадров в каждом → смещение каждого кадра
        u32 chunks = stco.size >= 8 ? be32(stco.data + 4) : 0;
        u32 runs = stsc.size >= 8 ? be32(stsc.data + 4) : 0;
        offsets.reserve(count);
        u32 sample = 0;
        for (u32 c = 0; c < chunks && sample < count; c++) {
            u64 off = co64 ? (8 + c * 8 + 8 <= stco.size ? be64(stco.data + 8 + c * 8) : 0)
                           : (8 + c * 4 + 4 <= stco.size ? be32(stco.data + 8 + c * 4) : 0);
            u32 per = 0; // кадров в чанке c+1: последний run, чей first_chunk <= c+1
            for (u32 r = 0; r < runs && 8 + r * 12 + 12 <= stsc.size; r++) {
                if (be32(stsc.data + 8 + r * 12) > c + 1) break;
                per = be32(stsc.data + 12 + r * 12);
            }
            for (u32 s = 0; s < per && sample < count; s++, sample++) {
                offsets.push_back(off);
                off += sizes[sample];
            }
        }
        sizes.resize(offsets.size());
        if (offsets.empty()) { err = "пустая таблица кадров"; return false; }
        return true;
    }
    if (err.empty()) err = "нет аудиодорожки";
    return false;
}

size_t Mp4Audio::frameAt(double sec) const {
    if (sec <= 0 || !timescale) return 0;
    u64 target = (u64)(sec * timescale), t = 0;
    size_t idx = 0;
    for (const Run &r : stts) {
        if (r.delta && t + (u64)r.count * r.delta > target) return idx + (target - t) / r.delta;
        t += (u64)r.count * r.delta;
        idx += r.count;
    }
    return idx;
}
