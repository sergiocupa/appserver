//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Demux da track AAC do MP4 (stbl: stsd/esds->ASC, stsz/stco/stsc/stts) + decode via fdk-aac.
//  Ative com -DHAVE_FDK_AAC. Sem a lib, aac_open falha (retorna != 0) e o pipeline segue video-only.
//
//  NAO TESTADO EM RUNTIME. Limitacoes conhecidas: assume 1 sample entry de audio;
//  co64 tratado; large-size (box 64-bit) parcial. Revisar contra MP4s reais.

#include "audio_aac.h"
#include "memory_pool.h"   // memop_* (evita declaracao implicita -> ponteiro truncado em x64)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint64_t rd64(const uint8_t* p) { return ((uint64_t)rd32(p) << 32) | rd32(p + 4); }
#define BOXT(a,b,c,d) (((uint32_t)(a)<<24)|((uint32_t)(b)<<16)|((uint32_t)(c)<<8)|(d))

// Acha o primeiro filho direto 'type' dentro de [buf,len). Retorna 1 e preenche off/size (payload).
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

typedef struct
{
    FILE*     fp;
    uint32_t  timescale;
    // tabela plana de samples
    uint64_t* off; uint32_t* sz; uint64_t* dur; uint32_t count;
    uint32_t  index;
    uint64_t  ts_acc;      // em unidades de timescale
    // ASC (AudioSpecificConfig)
    uint8_t   asc[64]; uint32_t asc_len;
    int       rate, channels;
    // buffers
    uint8_t*  sbuf; uint32_t sbuf_cap;
    int16_t*  pcm;  int pcm_cap;
    void*     dec;         // HANDLE_AACDECODER (fdk-aac)
} Aac;

// Extrai a ASC do esds (dentro do mp4a em stsd). Retorna 1 em sucesso.
static int parse_esds_asc(const uint8_t* esds, uint32_t len, uint8_t* asc, uint32_t* asc_len)
{
    // esds: version/flags(4) + tags. ES_Descr(0x03) > DecoderConfigDescr(0x04) > DecSpecificInfo(0x05)=ASC.
    uint32_t p = 4;
    // varre buscando a tag 0x05 (simplificado: pula tamanhos com bytes de continuacao 0x80).
    while (p < len)
    {
        uint8_t tag = esds[p++];
        uint32_t sz = 0; int b, cnt = 0;
        do { b = esds[p++]; sz = (sz << 7) | (b & 0x7F); } while ((b & 0x80) && ++cnt < 4 && p < len);
        if (tag == 0x05) { if (sz > 64) sz = 64; memcpy(asc, esds + p, sz); *asc_len = sz; return 1; }
        if (tag == 0x03) { p += 3; }            // ES_ID(2)+flags(1)
        else if (tag == 0x04) { p += 13; }      // objType(1)+stream(4)+bufSize(3)+maxBr(4)+avgBr(4)... aprox
        else p += sz;                            // desce/pula
    }
    return 0;
}

// Constroi a tabela plana de samples a partir de stsc/stsz/stco(co64)/stts do stbl de audio.
static int build_samples(Aac* a, const uint8_t* stbl, uint32_t stbl_len)
{
    uint32_t off, len;

    // stsz
    if (!find_box(stbl, stbl_len, BOXT('s','t','s','z'), &off, &len)) return 0;
    const uint8_t* stsz = stbl + off;
    uint32_t sample_size = rd32(stsz + 4);
    uint32_t sample_count = rd32(stsz + 8);
    if (sample_count == 0) return 0;

    // stco / co64
    const uint8_t* stco = 0; int co64 = 0; uint32_t chunk_count = 0;
    if (find_box(stbl, stbl_len, BOXT('s','t','c','o'), &off, &len)) { stco = stbl + off; chunk_count = rd32(stco + 4); }
    else if (find_box(stbl, stbl_len, BOXT('c','o','6','4'), &off, &len)) { stco = stbl + off; chunk_count = rd32(stco + 4); co64 = 1; }
    else return 0;

    // stsc
    if (!find_box(stbl, stbl_len, BOXT('s','t','s','c'), &off, &len)) return 0;
    const uint8_t* stsc = stbl + off;
    uint32_t stsc_n = rd32(stsc + 4);

    // stts (duracoes)
    const uint8_t* stts = 0; uint32_t stts_n = 0;
    if (find_box(stbl, stbl_len, BOXT('s','t','t','s'), &off, &len)) { stts = stbl + off; stts_n = rd32(stts + 4); }

    a->count = sample_count;
    a->off = (uint64_t*)memop_alloc_raw(sizeof(uint64_t) * sample_count);
    a->sz  = (uint32_t*)memop_alloc_raw(sizeof(uint32_t) * sample_count);
    a->dur = (uint64_t*)memop_alloc_raw(sizeof(uint64_t) * sample_count);
    if (!a->off || !a->sz || !a->dur) return 0;

    // tamanhos
    for (uint32_t i = 0; i < sample_count; i++)
        a->sz[i] = sample_size ? sample_size : rd32(stsz + 12 + i * 4);

    // duracoes (expande stts)
    { uint32_t si = 0; for (uint32_t e = 0; e < stts_n && si < sample_count; e++) {
        uint32_t cnt = rd32(stts + 8 + e * 8), delta = rd32(stts + 12 + e * 8);
        for (uint32_t k = 0; k < cnt && si < sample_count; k++) a->dur[si++] = delta;
      } while (si < sample_count) a->dur[si++] = a->dur[si ? si - 1 : 0]; }

    // offsets via stsc + stco: para cada chunk, samples_per_chunk consecutivos.
    uint32_t si = 0;
    for (uint32_t c = 0; c < chunk_count && si < sample_count; c++)
    {
        // samples_per_chunk deste chunk (procura a ultima entrada stsc com first_chunk <= c+1)
        uint32_t spc = 0;
        for (uint32_t e = 0; e < stsc_n; e++) {
            uint32_t first = rd32(stsc + 8 + e * 12);
            if (first <= c + 1) spc = rd32(stsc + 8 + e * 12 + 4); else break;
        }
        uint64_t base = co64 ? rd64(stco + 8 + c * 8) : rd32(stco + 8 + c * 4);
        uint64_t acc = base;
        for (uint32_t k = 0; k < spc && si < sample_count; k++) { a->off[si] = acc; acc += a->sz[si]; si++; }
    }
    return si == sample_count;
}

#ifdef HAVE_FDK_AAC
#include "aacdecoder_lib.h"
#endif

// Demux puro (SEM fdk-aac): abre o MP4, monta a tabela de samples AAC + ASC + rate/ch.
// Retorna Aac* (sem decoder) ou NULL. Sempre compilado.
static Aac* aac_demux_open(const char* mp4_path)
{
    FILE* fp = 0;
    if (fopen_s(&fp, mp4_path, "rb") != 0 || !fp) return 0;

    // 1) achar 'moov' e ler para memoria.
    fseek(fp, 0, SEEK_SET);
    uint8_t hdr[16]; long moov_off = -1; uint32_t moov_len = 0;
    for (;;) {
        long pos = ftell(fp);
        if (fread(hdr, 1, 8, fp) != 8) break;
        uint32_t sz = rd32(hdr), ty = rd32(hdr + 4), hl = 8; uint64_t bsz = sz;
        if (sz == 1) { if (fread(hdr + 8, 1, 8, fp) != 8) break; bsz = rd64(hdr + 8); hl = 16; }
        if (ty == BOXT('m','o','o','v')) { moov_off = pos + hl; moov_len = (uint32_t)(bsz - hl); break; }
        if (bsz < (uint64_t)hl) break;
        fseek(fp, pos + (long)bsz, SEEK_SET);
    }
    if (moov_off < 0) { fclose(fp); return 0; }

    uint8_t* moov = (uint8_t*)memop_alloc_raw(moov_len);
    if (!moov) { fclose(fp); return 0; }
    fseek(fp, moov_off, SEEK_SET);
    if (fread(moov, 1, moov_len, fp) != moov_len) { memop_free_raw(moov); fclose(fp); return 0; }

    // 2) achar a trak de audio ('soun'): itera 'trak' -> mdia -> hdlr.
    const uint8_t* stbl = 0; uint32_t stbl_len = 0, timescale = 0;
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
                if (find_box(mdia, ml, BOXT('h','d','l','r'), &ho, &hlen) &&
                    rd32(mdia + ho + 8) == BOXT('s','o','u','n'))
                {
                    uint32_t mdo, mdl;
                    if (find_box(mdia, ml, BOXT('m','d','h','d'), &mdo, &mdl)) timescale = rd32(mdia + mdo + 12);
                    uint32_t io, il;
                    if (find_box(mdia, ml, BOXT('m','i','n','f'), &io, &il))
                    {
                        uint32_t so, sl;
                        if (find_box(mdia + io, il, BOXT('s','t','b','l'), &so, &sl)) { stbl = mdia + io + so; stbl_len = sl; }
                    }
                    break;
                }
            }
        }
        p += (uint32_t)bsz;
    }
    if (!stbl) { memop_free_raw(moov); fclose(fp); return 0; }

    Aac* a = (Aac*)memop_calloc_raw(1, sizeof(Aac));
    if (!a) { memop_free_raw(moov); fclose(fp); return 0; }
    a->fp = fp; a->timescale = timescale ? timescale : 48000;

    // 3) ASC via stsd->mp4a->esds; rate/channels do mp4a.
    uint32_t sdo, sdl;
    if (find_box(stbl, stbl_len, BOXT('s','t','s','d'), &sdo, &sdl))
    {
        const uint8_t* stsd = stbl + sdo;               // version/flags(4)+entrycount(4)+entry...
        const uint8_t* entry = stsd + 8;                // primeira sample entry
        // AudioSampleEntry: 6 reserved + 2 dataref + 8 reserved + 2 channelcount + 2 samplesize + 4 + 4 samplerate(16.16)
        a->channels = (entry[16 + 8] << 8) | entry[16 + 9];
        a->rate     = (int)(rd32(entry + 16 + 16) >> 16);
        // Os filhos de uma AudioSampleEntry comecam em +36, nao em +28. O cabecalho e:
        //   8 box(size+type) + 6 reservado + 2 data_ref + 8 reservado
        //   + 2 channelcount + 2 samplesize + 2 pre_defined + 2 reservado + 4 samplerate
        // = 36. Os proprios campos lidos acima confirmam: channelcount em +24 e
        // samplerate em +32..36.
        //
        // Com +28 o find_box lia como tamanho/tipo de box os ultimos 8 bytes do
        // cabecalho (no arquivo de teste: 00 00 ac 44, que e 44100 << 16), nao achava
        // o esds e a ASC saia vazia. Como aac_demux_open recusa asc_len == 0, TODO
        // arquivo perdia o audio: a fragmentacao HLS saia so com video, sem grupo
        // EXT-X-MEDIA, e o player nao tinha o que tocar.
        #define AAC_SAMPLE_ENTRY_HDR 36
        uint32_t eo, el;
        uint32_t entry_sz = rd32(entry);
        if (entry_sz > AAC_SAMPLE_ENTRY_HDR &&
            find_box(entry + AAC_SAMPLE_ENTRY_HDR, entry_sz - AAC_SAMPLE_ENTRY_HDR,
                     BOXT('e','s','d','s'), &eo, &el))
            parse_esds_asc(entry + AAC_SAMPLE_ENTRY_HDR + eo, el, a->asc, &a->asc_len);
    }
    if (a->channels <= 0) a->channels = 2;
    if (a->rate <= 0) a->rate = (int)a->timescale;

    int oks = build_samples(a, stbl, stbl_len);
    memop_free_raw(moov);
    if (!oks || a->asc_len == 0) { aac_close(a); return 0; }
    return a;
}

int aac_open(const char* mp4_path, int* rate, int* channels, void** ctx)
{
    *ctx = 0;
#ifndef HAVE_FDK_AAC
    (void)mp4_path; (void)rate; (void)channels;
    return -100;   // decoder AAC nao compilado -> sem audio
#else
    Aac* a = aac_demux_open(mp4_path);
    if (!a) return -1;

    // abrir o decoder fdk-aac com a ASC (raw).
    HANDLE_AACDECODER dec = aacDecoder_Open(TT_MP4_RAW, 1);
    if (!dec) { aac_close(a); return -8; }
    UCHAR* asc = a->asc; UINT asc_len = a->asc_len;
    if (aacDecoder_ConfigRaw(dec, &asc, &asc_len) != AAC_DEC_OK) { aacDecoder_Close(dec); aac_close(a); return -9; }
    a->dec = dec;

    a->pcm_cap = 8 * 2048;
    a->pcm = (int16_t*)memop_alloc_raw(sizeof(int16_t) * a->pcm_cap);

    *rate = a->rate; *channels = a->channels; *ctx = a;
    return 0;
#endif
}

// ---- Passthrough (AAC codificado) -----------------------------------------
int aac_open_raw(const char* mp4_path, AacRawInfo* info, void** ctx)
{
    if (ctx) *ctx = 0;
    Aac* a = aac_demux_open(mp4_path);
    if (!a) return -1;
    if (info)
    {
        memcpy(info->asc, a->asc, a->asc_len);
        info->asc_len = (int)a->asc_len;
        info->rate = a->rate; info->channels = a->channels; info->timescale = a->timescale;
    }
    if (ctx) *ctx = a;
    return 0;
}

int aac_read_raw(void* ctx, const uint8_t** data, int* size, int64_t* dts_units, int* dur_units)
{
    Aac* a = (Aac*)ctx;
    if (!a || a->index >= a->count) return 0;
    uint32_t idx = a->index++;
    uint32_t need = a->sz[idx];
    if (need > a->sbuf_cap) { a->sbuf = (uint8_t*)memop_realloc_raw(a->sbuf, need); a->sbuf_cap = need; }
    _fseeki64(a->fp, (long long)a->off[idx], SEEK_SET);
    if (fread(a->sbuf, 1, need, a->fp) != need) return -1;
    if (data) *data = a->sbuf;
    if (size) *size = (int)need;
    if (dts_units) *dts_units = (int64_t)a->ts_acc;
    if (dur_units) *dur_units = (int)a->dur[idx];
    a->ts_acc += a->dur[idx];
    return 1;
}

int aac_read(void* ctx, int16_t** pcm, int* samples, int64_t* ts_ms)
{
#ifndef HAVE_FDK_AAC
    (void)ctx; (void)pcm; (void)samples; (void)ts_ms; return 0;
#else
    Aac* a = (Aac*)ctx;
    if (!a || a->index >= a->count) return 0;

    uint32_t idx = a->index++;
    uint32_t need = a->sz[idx];
    if (need > a->sbuf_cap) { a->sbuf = (uint8_t*)memop_realloc_raw(a->sbuf, need); a->sbuf_cap = need; }
    _fseeki64(a->fp, (long long)a->off[idx], SEEK_SET);
    if (fread(a->sbuf, 1, need, a->fp) != need) return 0;

    UCHAR* in = a->sbuf; UINT insz = need, valid = need;
    if (aacDecoder_Fill((HANDLE_AACDECODER)a->dec, &in, &insz, &valid) != AAC_DEC_OK) return -1;

    AAC_DECODER_ERROR e = aacDecoder_DecodeFrame((HANDLE_AACDECODER)a->dec, a->pcm, a->pcm_cap, 0);
    if (e != AAC_DEC_OK) return aac_read(ctx, pcm, samples, ts_ms); // pula frame incompleto

    CStreamInfo* si = aacDecoder_GetStreamInfo((HANDLE_AACDECODER)a->dec);
    *pcm = a->pcm;
    *samples = si ? si->frameSize : 1024;      // amostras por canal
    *ts_ms = (int64_t)((a->ts_acc * 1000) / a->timescale);
    a->ts_acc += a->dur[idx];
    return 1;
#endif
}

// ---- Decoder AAC packet-fed (transcode) ------------------------------------
typedef struct { void* dec; int16_t* pcm; int pcm_cap; } AacDec;

void* aac_dec_open(const uint8_t* asc, int asc_len)
{
#ifndef HAVE_FDK_AAC
    (void)asc; (void)asc_len; return 0;
#else
    if (!asc || asc_len <= 0) return 0;
    HANDLE_AACDECODER d = aacDecoder_Open(TT_MP4_RAW, 1);
    if (!d) return 0;
    UCHAR* a = (UCHAR*)asc; UINT al = (UINT)asc_len;
    if (aacDecoder_ConfigRaw(d, &a, &al) != AAC_DEC_OK) { aacDecoder_Close(d); return 0; }
    AacDec* c = (AacDec*)memop_calloc_raw(1, sizeof(AacDec));
    if (!c) { aacDecoder_Close(d); return 0; }
    c->dec = d; c->pcm_cap = 8 * 2048; c->pcm = (int16_t*)memop_alloc_raw(sizeof(int16_t) * c->pcm_cap);
    return c;
#endif
}

int aac_dec_decode(void* ctx, const uint8_t* data, int size, int16_t** pcm, int* samples)
{
#ifndef HAVE_FDK_AAC
    (void)ctx; (void)data; (void)size; (void)pcm; (void)samples; return -1;
#else
    AacDec* c = (AacDec*)ctx;
    if (!c || !data || size <= 0) return -1;
    UCHAR* in = (UCHAR*)data; UINT insz = (UINT)size, valid = (UINT)size;
    if (aacDecoder_Fill((HANDLE_AACDECODER)c->dec, &in, &insz, &valid) != AAC_DEC_OK) return -1;
    AAC_DECODER_ERROR e = aacDecoder_DecodeFrame((HANDLE_AACDECODER)c->dec, c->pcm, c->pcm_cap, 0);
    if (e == AAC_DEC_NOT_ENOUGH_BITS) return 0;
    if (e != AAC_DEC_OK) return -1;
    CStreamInfo* si = aacDecoder_GetStreamInfo((HANDLE_AACDECODER)c->dec);
    if (pcm) *pcm = c->pcm;
    if (samples) *samples = si ? si->frameSize : 1024;
    return 1;
#endif
}

void aac_dec_close(void* ctx)
{
    AacDec* c = (AacDec*)ctx;
    if (!c) return;
#ifdef HAVE_FDK_AAC
    if (c->dec) aacDecoder_Close((HANDLE_AACDECODER)c->dec);
#endif
    memop_free_raw(c->pcm); memop_free_raw(c);
}

void aac_close(void* ctx)
{
    Aac* a = (Aac*)ctx;
    if (!a) return;
#ifdef HAVE_FDK_AAC
    if (a->dec) aacDecoder_Close((HANDLE_AACDECODER)a->dec);
#endif
    if (a->fp) fclose(a->fp);
    memop_free_raw(a->off); memop_free_raw(a->sz); memop_free_raw(a->dur); memop_free_raw(a->sbuf); memop_free_raw(a->pcm);
    memop_free_raw(a);
}
