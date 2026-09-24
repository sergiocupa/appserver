//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Parse embutido de MP4 (sem ffprobe): timing por frame (stts) + metadados de video/audio
//  (stsd/mdhd). Usado para eliminar o ffprobe do prepare/DASH.
//  NAO TESTADO EM RUNTIME. Sem ctts (B-frames) o PTS e aproximado (ordem de decode).

#include "mp4_timing.h"
#include "memory_pool.h"   // memop_* (evita declaracao implicita -> ponteiro truncado em x64)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint64_t rd64(const uint8_t* p) { return ((uint64_t)rd32(p) << 32) | rd32(p + 4); }
static uint32_t rd16(const uint8_t* p) { return ((uint32_t)p[0] << 8) | p[1]; }
#define BOXT(a,b,c,d) (((uint32_t)(a)<<24)|((uint32_t)(b)<<16)|((uint32_t)(c)<<8)|(d))

static int find_box(const uint8_t* buf, uint32_t len, uint32_t type, uint32_t* off, uint32_t* size)
{
    uint32_t p = 0;
    while (p + 8 <= len)
    {
        uint32_t sz = rd32(buf + p), ty = rd32(buf + p + 4), hdr = 8;
        uint64_t bsz = sz;
        if (sz == 1) { if (p + 16 > len) break; bsz = rd64(buf + p + 8); hdr = 16; }
        if (bsz == 0) bsz = len - p;
        if (bsz < hdr || p + bsz > len) break;
        if (ty == type) { *off = p + hdr; *size = (uint32_t)(bsz - hdr); return 1; }
        p += (uint32_t)bsz;
    }
    return 0;
}

// Le o box 'moov' inteiro para memoria. Caller libera *out.
static int read_moov(const char* path, uint8_t** out, uint32_t* out_len)
{
    *out = 0; *out_len = 0;
    FILE* fp = 0;
    if (fopen_s(&fp, path, "rb") != 0 || !fp) return -1;

    uint8_t hdr[16]; long moov_off = -1; uint32_t moov_len = 0;
    for (;;)
    {
        long pos = ftell(fp);
        if (fread(hdr, 1, 8, fp) != 8) break;
        uint32_t sz = rd32(hdr), ty = rd32(hdr + 4), hl = 8; uint64_t bsz = sz;
        if (sz == 1) { if (fread(hdr + 8, 1, 8, fp) != 8) break; bsz = rd64(hdr + 8); hl = 16; }
        if (ty == BOXT('m','o','o','v')) { moov_off = pos + hl; moov_len = (uint32_t)(bsz - hl); break; }
        if (bsz < (uint64_t)hl) break;
        fseek(fp, pos + (long)bsz, SEEK_SET);
    }
    if (moov_off < 0) { fclose(fp); return -2; }

    uint8_t* moov = (uint8_t*)memop_alloc_raw(moov_len);
    if (!moov) { fclose(fp); return -3; }
    fseek(fp, moov_off, SEEK_SET);
    size_t rd = fread(moov, 1, moov_len, fp);
    fclose(fp);
    if (rd != moov_len) { memop_free_raw(moov); return -4; }
    *out = moov; *out_len = moov_len;
    return 0;
}

// Acha a trak (handler 'vide' ou 'soun') e devolve ponteiros para stbl (+ mdia) e timescale.
static int find_track(const uint8_t* moov, uint32_t moov_len, uint32_t handler,
                      const uint8_t** stbl, uint32_t* stbl_len,
                      const uint8_t** mdia_out, uint32_t* mdia_len, uint32_t* timescale)
{
    uint32_t p = 0;
    while (p + 8 <= moov_len)
    {
        uint32_t sz = rd32(moov + p), ty = rd32(moov + p + 4), hl = 8; uint64_t bsz = sz;
        if (sz == 1) { bsz = rd64(moov + p + 8); hl = 16; }
        if (bsz == 0) bsz = moov_len - p;
        if (ty == BOXT('t','r','a','k'))
        {
            const uint8_t* trak = moov + p + hl; uint32_t tl = (uint32_t)(bsz - hl);
            uint32_t mo, ml;
            if (find_box(trak, tl, BOXT('m','d','i','a'), &mo, &ml))
            {
                const uint8_t* mdia = trak + mo;
                uint32_t ho, hlen;
                if (find_box(mdia, ml, BOXT('h','d','l','r'), &ho, &hlen) && rd32(mdia + ho + 8) == handler)
                {
                    uint32_t mdo, mdl;
                    if (find_box(mdia, ml, BOXT('m','d','h','d'), &mdo, &mdl)) *timescale = rd32(mdia + mdo + 12);
                    uint32_t io, il;
                    if (find_box(mdia, ml, BOXT('m','i','n','f'), &io, &il))
                    {
                        uint32_t so, sl;
                        if (find_box(mdia + io, il, BOXT('s','t','b','l'), &so, &sl))
                        {
                            *stbl = mdia + io + so; *stbl_len = sl;
                            if (mdia_out) { *mdia_out = mdia; *mdia_len = ml; }
                            return 1;
                        }
                    }
                }
            }
        }
        p += (uint32_t)bsz;
    }
    return 0;
}

int mp4_video_pts_ms(const char* mp4_path, int64_t** out_pts, uint32_t* out_count)
{
    *out_pts = 0; *out_count = 0;
    uint8_t* moov = 0; uint32_t moov_len = 0;
    if (read_moov(mp4_path, &moov, &moov_len) != 0) return -1;

    const uint8_t* stbl = 0; uint32_t stbl_len = 0, timescale = 0;
    if (!find_track(moov, moov_len, BOXT('v','i','d','e'), &stbl, &stbl_len, 0, 0, &timescale) || !timescale)
    { memop_free_raw(moov); return -2; }

    uint32_t to, tlen;
    if (!find_box(stbl, stbl_len, BOXT('s','t','t','s'), &to, &tlen)) { memop_free_raw(moov); return -3; }
    const uint8_t* stts = stbl + to;

    uint32_t n = rd32(stts + 4), total = 0;
    for (uint32_t e = 0; e < n; e++) total += rd32(stts + 8 + e * 8);
    if (total == 0) { memop_free_raw(moov); return -4; }

    int64_t* pts = (int64_t*)memop_alloc_raw(sizeof(int64_t) * total);
    if (!pts) { memop_free_raw(moov); return -5; }

    uint64_t acc = 0; uint32_t si = 0;
    for (uint32_t e = 0; e < n && si < total; e++)
    {
        uint32_t cnt = rd32(stts + 8 + e * 8), delta = rd32(stts + 12 + e * 8);
        for (uint32_t k = 0; k < cnt && si < total; k++) { pts[si++] = (int64_t)((acc * 1000) / timescale); acc += delta; }
    }
    memop_free_raw(moov);
    *out_pts = pts; *out_count = total;
    return 0;
}

int mp4_video_info(const char* mp4_path, int* w, int* h, double* fps, char codec[8], double* duration)
{
    if (w) *w = 0; if (h) *h = 0; if (fps) *fps = 0; if (duration) *duration = 0; if (codec) codec[0] = 0;
    uint8_t* moov = 0; uint32_t moov_len = 0;
    if (read_moov(mp4_path, &moov, &moov_len) != 0) return -1;

    const uint8_t* stbl = 0; uint32_t stbl_len = 0, timescale = 0;
    if (!find_track(moov, moov_len, BOXT('v','i','d','e'), &stbl, &stbl_len, 0, 0, &timescale)) { memop_free_raw(moov); return -2; }

    // stsd -> VisualSampleEntry: width@entry+32, height@entry+34; type = codec.
    uint32_t so, sl;
    if (find_box(stbl, stbl_len, BOXT('s','t','s','d'), &so, &sl))
    {
        const uint8_t* entry = stbl + so + 8;   // apos version/flags(4)+entrycount(4)
        uint32_t ty = rd32(entry + 4);
        if (w) *w = (int)rd16(entry + 32);
        if (h) *h = (int)rd16(entry + 34);
        if (codec)
        {
            if (ty == BOXT('a','v','c','1') || ty == BOXT('a','v','c','3')) strcpy_s(codec, 8, "h264");
            else if (ty == BOXT('h','v','c','1') || ty == BOXT('h','e','v','1')) strcpy_s(codec, 8, "hevc");
            else strcpy_s(codec, 8, "h264");
        }
    }

    // fps e duracao a partir do stts.
    uint32_t to, tlen;
    if (find_box(stbl, stbl_len, BOXT('s','t','t','s'), &to, &tlen) && timescale)
    {
        const uint8_t* stts = stbl + to;
        uint32_t n = rd32(stts + 4); uint64_t total_dur = 0, total_cnt = 0;
        for (uint32_t e = 0; e < n; e++) { uint32_t c = rd32(stts + 8 + e * 8), d = rd32(stts + 12 + e * 8); total_cnt += c; total_dur += (uint64_t)c * d; }
        if (duration) *duration = total_dur / (double)timescale;
        if (fps && total_dur > 0) *fps = total_cnt * (double)timescale / (double)total_dur;
    }
    memop_free_raw(moov);
    return 0;
}

int mp4_audio_info(const char* mp4_path, char codec[8], int* rate, int* channels)
{
    if (codec) codec[0] = 0; if (rate) *rate = 0; if (channels) *channels = 0;
    uint8_t* moov = 0; uint32_t moov_len = 0;
    if (read_moov(mp4_path, &moov, &moov_len) != 0) return -1;

    const uint8_t* stbl = 0; uint32_t stbl_len = 0, timescale = 0;
    if (!find_track(moov, moov_len, BOXT('s','o','u','n'), &stbl, &stbl_len, 0, 0, &timescale)) { memop_free_raw(moov); return -2; }

    uint32_t so, sl;
    if (find_box(stbl, stbl_len, BOXT('s','t','s','d'), &so, &sl))
    {
        const uint8_t* entry = stbl + so + 8;   // AudioSampleEntry
        uint32_t ty = rd32(entry + 4);
        if (channels) *channels = (int)rd16(entry + 24);
        if (rate)     *rate     = (int)(rd32(entry + 32) >> 16);
        if (codec)    strcpy_s(codec, 8, (ty == BOXT('m','p','4','a')) ? "aac" : "aac");
    }
    memop_free_raw(moov);
    return 0;
}
