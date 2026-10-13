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

// ---- tempos por amostra (apresentacao e decodificacao) -------------------------------

static int64_t escala_us(int64_t v, uint32_t ts)
{
    return (v * 1000000 + (v >= 0 ? (int64_t)ts / 2 : -(int64_t)ts / 2)) / (int64_t)ts;
}

int mp4_video_times_us(const char* mp4_path, int64_t** out_pts, int64_t** out_dts, uint32_t* out_count)
{
    *out_pts = 0; *out_dts = 0; *out_count = 0;
    uint8_t* moov = 0; uint32_t moov_len = 0;
    if (read_moov(mp4_path, &moov, &moov_len) != 0) return -1;

    uint32_t vo, vl, movie_ts = 0;
    if (find_box(moov, moov_len, BOXT('m','v','h','d'), &vo, &vl))
        movie_ts = (moov[vo] == 1) ? rd32(moov + vo + 20) : rd32(moov + vo + 12);

    // trak de video
    const uint8_t* trak = 0; uint32_t tl = 0, p = 0;
    while (p + 8 <= moov_len && !trak)
    {
        uint32_t sz = rd32(moov + p), ty = rd32(moov + p + 4), hl = 8; uint64_t bsz = sz;
        if (sz == 1) { if (p + 16 > moov_len) break; bsz = rd64(moov + p + 8); hl = 16; }
        if (bsz == 0) bsz = moov_len - p;
        if (bsz < hl || p + bsz > moov_len) break;
        if (ty == BOXT('t','r','a','k'))
        {
            const uint8_t* t = moov + p + hl; uint32_t n = (uint32_t)(bsz - hl), mo, ml, ho, hlen;
            if (find_box(t, n, BOXT('m','d','i','a'), &mo, &ml) && find_box(t + mo, ml, BOXT('h','d','l','r'), &ho, &hlen)
                && rd32(t + mo + ho + 8) == BOXT('v','i','d','e')) { trak = t; tl = n; }
        }
        p += (uint32_t)bsz;
    }
    uint32_t mo, ml, mdo, mdl, io, il, so, sl, to, tlen;
    if (!trak || !find_box(trak, tl, BOXT('m','d','i','a'), &mo, &ml)) { memop_free_raw(moov); return -2; }
    const uint8_t* mdia = trak + mo;
    if (!find_box(mdia, ml, BOXT('m','d','h','d'), &mdo, &mdl)) { memop_free_raw(moov); return -2; }
    uint32_t ts = (mdia[mdo] == 1) ? rd32(mdia + mdo + 20) : rd32(mdia + mdo + 12);
    if (!ts || !find_box(mdia, ml, BOXT('m','i','n','f'), &io, &il) || !find_box(mdia + io, il, BOXT('s','t','b','l'), &so, &sl))
    { memop_free_raw(moov); return -2; }
    const uint8_t* stbl = mdia + io + so;
    if (!find_box(stbl, sl, BOXT('s','t','t','s'), &to, &tlen)) { memop_free_raw(moov); return -3; }
    const uint8_t* stts = stbl + to;
    uint32_t sn = rd32(stts + 4), total = 0;
    for (uint32_t e = 0; e < sn; e++) total += rd32(stts + 8 + e * 8);
    if (total == 0) { memop_free_raw(moov); return -4; }

    const uint8_t* ctts = 0; uint32_t cn = 0, cver = 0, co, cl;
    if (find_box(stbl, sl, BOXT('c','t','t','s'), &co, &cl)) { ctts = stbl + co; cver = ctts[0]; cn = rd32(ctts + 4); }

    // primeira edicao nao vazia (e as vazias antes dela)
    int64_t vazio = 0, media_time = 0; int tem = 0;
    uint32_t eo, el, lo, ll;
    if (find_box(trak, tl, BOXT('e','d','t','s'), &eo, &el) && find_box(trak + eo, el, BOXT('e','l','s','t'), &lo, &ll))
    {
        const uint8_t* e = trak + eo + lo; uint32_t ver = e[0], n = rd32(e + 4), q = 8;
        for (uint32_t i = 0; i < n && !tem; i++)
        {
            uint64_t sd; int64_t mt;
            if (ver == 1) { if (q + 20 > ll) break; sd = rd64(e + q); mt = (int64_t)rd64(e + q + 8); q += 20; }
            else          { if (q + 12 > ll) break; sd = rd32(e + q); mt = (int64_t)(int32_t)rd32(e + q + 4); q += 12; }
            if (mt < 0) vazio += (int64_t)sd; else { media_time = mt; tem = 1; }
        }
    }
    if (!tem) { vazio = 0; media_time = 0; }
    int64_t soma_us = movie_ts ? vazio * 1000000 / (int64_t)movie_ts : 0;

    int64_t* pts = (int64_t*)memop_alloc_raw(sizeof(int64_t) * total);
    int64_t* dts = (int64_t*)memop_alloc_raw(sizeof(int64_t) * total);
    if (!pts || !dts) { if (pts) memop_free_raw(pts); if (dts) memop_free_raw(dts); memop_free_raw(moov); return -5; }

    int64_t acc = 0; uint32_t si = 0, ce = 0, ck = 0;
    for (uint32_t e = 0; e < sn && si < total; e++)
    {
        uint32_t cnt = rd32(stts + 8 + e * 8), delta = rd32(stts + 12 + e * 8);
        for (uint32_t k = 0; k < cnt && si < total; k++)
        {
            int64_t off = 0;
            if (ctts && ce < cn)
            {
                uint32_t bruto = rd32(ctts + 12 + ce * 8);
                off = (cver == 1) ? (int64_t)(int32_t)bruto : (int64_t)bruto;
                if (++ck >= rd32(ctts + 8 + ce * 8)) { ce++; ck = 0; }
            }
            dts[si] = escala_us(acc - media_time, ts) + soma_us;
            pts[si] = escala_us(acc + off - media_time, ts) + soma_us;
            si++; acc += delta;
        }
    }
    memop_free_raw(moov);
    *out_pts = pts; *out_dts = dts; *out_count = total;
    return 0;
}

// ---- lista de edicao -----------------------------------------------------------------

// Deslocamento de UMA faixa (ver mp4_timing.h), em us. 'trak' = conteudo do box trak.
// *dur_s recebe a duracao APRESENTADA (soma das edicoes), ou fica como esta se nao ha elst.
static int64_t trak_edit_offset(const uint8_t* trak, uint32_t tl, uint32_t movie_ts, double* dur_s)
{
    uint32_t mo, ml, mdo, mdl, io, il, so, sl;
    if (!find_box(trak, tl, BOXT('m','d','i','a'), &mo, &ml)) return 0;
    const uint8_t* mdia = trak + mo;
    if (!find_box(mdia, ml, BOXT('m','d','h','d'), &mdo, &mdl)) return 0;
    uint32_t ts = (mdia[mdo] == 1) ? rd32(mdia + mdo + 20) : rd32(mdia + mdo + 12);
    if (!ts || !movie_ts) return 0;

    // menor tempo de composicao (stts + ctts) no comeco da faixa: com quadros B o primeiro
    // quadro APRESENTADO nao esta no tempo de midia 0, e a edicao costuma apontar para ele
    int64_t menor = 0;
    if (find_box(mdia, ml, BOXT('m','i','n','f'), &io, &il) && find_box(mdia + io, il, BOXT('s','t','b','l'), &so, &sl))
    {
        const uint8_t* stbl = mdia + io + so;
        uint32_t co, cl, to, tlen;
        if (find_box(stbl, sl, BOXT('c','t','t','s'), &co, &cl) && find_box(stbl, sl, BOXT('s','t','t','s'), &to, &tlen))
        {
            const uint8_t *ctts = stbl + co, *stts = stbl + to;
            uint32_t cn = rd32(ctts + 4), sn = rd32(stts + 4), ver = ctts[0];
            uint32_t ce = 0, ck = 0, se = 0, sk = 0; int64_t dts = 0; int primeiro = 1;
            for (int i = 0; i < 64 && ce < cn && se < sn; i++)
            {
                uint32_t bruto = rd32(ctts + 12 + ce * 8);
                int64_t cts = dts + (ver == 1 ? (int64_t)(int32_t)bruto : (int64_t)bruto);
                if (primeiro || cts < menor) { menor = cts; primeiro = 0; }
                dts += rd32(stts + 12 + se * 8);
                if (++ck >= rd32(ctts + 8 + ce * 8)) { ce++; ck = 0; }
                if (++sk >= rd32(stts + 8 + se * 8)) { se++; sk = 0; }
            }
        }
    }

    int64_t vazio = 0, media_time = 0; uint64_t apresentado = 0; int tem = 0;
    uint32_t eo, el, lo, ll;
    if (find_box(trak, tl, BOXT('e','d','t','s'), &eo, &el) && find_box(trak + eo, el, BOXT('e','l','s','t'), &lo, &ll))
    {
        const uint8_t* e = trak + eo + lo; uint32_t ver = e[0], n = rd32(e + 4), p = 8;
        for (uint32_t i = 0; i < n; i++)
        {
            uint64_t sd; int64_t mt;
            if (ver == 1) { if (p + 20 > ll) break; sd = rd64(e + p); mt = (int64_t)rd64(e + p + 8); p += 20; }
            else          { if (p + 12 > ll) break; sd = rd32(e + p); mt = (int64_t)(int32_t)rd32(e + p + 4); p += 12; }
            apresentado += sd;
            if (tem) continue;
            if (mt < 0) vazio += (int64_t)sd;      // edicao vazia: a faixa entra atrasada
            else { media_time = mt; tem = 1; }
        }
    }
    if (!tem) media_time = menor, vazio = 0;       // sem lista: comeca no primeiro quadro apresentado
    else if (dur_s && apresentado > 0) *dur_s = (double)apresentado / movie_ts;

    return vazio * 1000000 / (int64_t)movie_ts - media_time * 1000000 / (int64_t)ts + menor * 1000000 / (int64_t)ts;
}

static int edit_offsets(const uint8_t* moov, uint32_t moov_len, int64_t* video_us, int64_t* audio_us, double* video_dur_s)
{
    uint32_t vo, vl, movie_ts = 0;
    if (find_box(moov, moov_len, BOXT('m','v','h','d'), &vo, &vl))
        movie_ts = (moov[vo] == 1) ? rd32(moov + vo + 20) : rd32(moov + vo + 12);
    int v = 0, a = 0; uint32_t p = 0;
    while (p + 8 <= moov_len)
    {
        uint32_t sz = rd32(moov + p), ty = rd32(moov + p + 4), hl = 8; uint64_t bsz = sz;
        if (sz == 1) { if (p + 16 > moov_len) break; bsz = rd64(moov + p + 8); hl = 16; }
        if (bsz == 0) bsz = moov_len - p;
        if (bsz < hl || p + bsz > moov_len) break;
        if (ty == BOXT('t','r','a','k'))
        {
            const uint8_t* trak = moov + p + hl; uint32_t tl = (uint32_t)(bsz - hl), mo, ml, ho, hlen;
            if (find_box(trak, tl, BOXT('m','d','i','a'), &mo, &ml) && find_box(trak + mo, ml, BOXT('h','d','l','r'), &ho, &hlen))
            {
                uint32_t handler = rd32(trak + mo + ho + 8);
                if (handler == BOXT('v','i','d','e') && !v) { int64_t o = trak_edit_offset(trak, tl, movie_ts, video_dur_s); if (video_us) *video_us = o; v = 1; }
                if (handler == BOXT('s','o','u','n') && !a) { int64_t o = trak_edit_offset(trak, tl, movie_ts, 0); if (audio_us) *audio_us = o; a = 1; }
            }
        }
        p += (uint32_t)bsz;
    }
    return 1;
}

int mp4_edit_offsets(const char* mp4_path, int64_t* video_us, int64_t* audio_us)
{
    if (video_us) *video_us = 0;
    if (audio_us) *audio_us = 0;
    uint8_t* moov = 0; uint32_t moov_len = 0;
    if (read_moov(mp4_path, &moov, &moov_len) != 0) return 0;
    edit_offsets(moov, moov_len, video_us, audio_us, 0);
    memop_free_raw(moov);
    return 1;
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
    // A duracao que vale e a APRESENTADA: num arquivo cortado a midia tem o trecho anterior ao
    // corte, que a lista de edicao esconde (e que a fonte agora tambem descarta).
    if (duration) edit_offsets(moov, moov_len, 0, 0, duration);
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

// ---- metadados descritivos (titulo, autor, ...) ------------------------------

// Copia texto UTF-8 de 'n' bytes, sem caracteres de controle e sem espacos nas pontas.
static void meta_texto(char* out, size_t cap, const uint8_t* p, uint32_t n)
{
    size_t k = 0;
    for (uint32_t i = 0; i < n && k + 1 < cap; i++)
    {
        uint8_t c = p[i];
        if (c == 0) break;
        if (c < 0x20) c = ' ';
        if (k == 0 && c == ' ') continue;
        out[k++] = (char)c;
    }
    while (k > 0 && out[k - 1] == ' ') k--;
    out[k] = 0;
}

static char* meta_campo(Mp4Meta* m, uint32_t tipo, size_t* cap)
{
    switch (tipo)
    {
        case BOXT(0xA9,'n','a','m'):                              *cap = sizeof(m->Titulo);    return m->Titulo;
        case BOXT(0xA9,'A','R','T'): case BOXT('a','A','R','T'): *cap = sizeof(m->Autor);     return m->Autor;
        case BOXT(0xA9,'a','l','b'):                              *cap = sizeof(m->Album);     return m->Album;
        case BOXT(0xA9,'d','a','y'):                              *cap = sizeof(m->Data);      return m->Data;
        case BOXT('d','e','s','c'): case BOXT('l','d','e','s'):
        case BOXT(0xA9,'c','m','t'): case BOXT(0xA9,'d','e','s'): *cap = sizeof(m->Descricao); return m->Descricao;
        case BOXT(0xA9,'g','e','n'):                              *cap = sizeof(m->Genero);    return m->Genero;
        default: return 0;
    }
}

// ilst: cada item e um box com o tipo do campo e, dentro, um 'data' [tipo 4][locale 4][valor].
// So texto UTF-8 (tipo 1); o primeiro valor encontrado de cada campo vale.
static void meta_ilst(Mp4Meta* m, const uint8_t* b, uint32_t len)
{
    uint32_t p = 0;
    while (p + 8 <= len)
    {
        uint32_t sz = rd32(b + p), ty = rd32(b + p + 4);
        if (sz < 8 || p + sz > len) break;
        size_t cap; char* dst = meta_campo(m, ty, &cap);
        uint32_t d_o, d_l;
        if (dst && !dst[0] && find_box(b + p + 8, sz - 8, BOXT('d','a','t','a'), &d_o, &d_l) && d_l >= 8 && rd32(b + p + 8 + d_o) == 1)
            meta_texto(dst, cap, b + p + 8 + d_o + 8, d_l - 8);
        p += sz;
    }
}

// meta e um "full box" (4 bytes de versao/flags antes dos filhos); o ilst esta dentro dele.
static void meta_box(Mp4Meta* m, const uint8_t* b, uint32_t len)
{
    if (len < 4) return;
    uint32_t io, il;
    if (find_box(b + 4, len - 4, BOXT('i','l','s','t'), &io, &il)) meta_ilst(m, b + 4 + io, il);
}

// udta: o meta/ilst (iTunes) ou, no QuickTime antigo, os campos direto ([tam 2][idioma 2][texto]).
static void meta_udta(Mp4Meta* m, const uint8_t* b, uint32_t len)
{
    uint32_t p = 0;
    while (p + 8 <= len)
    {
        uint32_t sz = rd32(b + p), ty = rd32(b + p + 4);
        if (sz < 8 || p + sz > len) break;
        if (ty == BOXT('m','e','t','a')) meta_box(m, b + p + 8, sz - 8);
        else
        {
            size_t cap; char* dst = meta_campo(m, ty, &cap);
            if (dst && !dst[0] && sz >= 12)
            {
                uint32_t n = rd16(b + p + 8);
                if (n <= sz - 12) meta_texto(dst, cap, b + p + 12, n);
            }
        }
        p += sz;
    }
}

int mp4_metadata(const char* mp4_path, Mp4Meta* m)
{
    memset(m, 0, sizeof(*m));
    uint8_t* moov = 0; uint32_t moov_len = 0;
    if (read_moov(mp4_path, &moov, &moov_len) != 0) return 0;
    uint32_t o, l;
    if (find_box(moov, moov_len, BOXT('u','d','t','a'), &o, &l)) meta_udta(m, moov + o, l);
    if (find_box(moov, moov_len, BOXT('m','e','t','a'), &o, &l)) meta_box(m, moov + o, l);   // meta direto no moov
    memop_free_raw(moov);
    return 1;
}

int mp4_metadata_descritiva(const Mp4Meta* m)
{
    return m && (m->Titulo[0] || m->Autor[0] || m->Album[0] || m->Descricao[0]);
}
