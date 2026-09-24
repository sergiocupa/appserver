//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Mux WebM (EBML/Matroska) embutido: VP9/AV1 + Opus. Sem ffmpeg.
//  Dois modos:
//    - ARQUIVO UNICO  (webm_open): header+Segment+Info+Tracks + Clusters no mesmo arquivo.
//    - SEGMENTADO/DASH (webm_open_segmented): escreve "init-<name>.webm" (header..Tracks) e,
//      a cada limite de segmento (keyframe apos seg_ms), um "chunk-<name>-<N>.webm" = 1 Cluster.
//      init + chunks concatenados formam o stream (padrao webm-dash / MSE).
//
//  NAO TESTADO EM RUNTIME. Revisar contra um webm de referencia.

#include "mux_webm.h"
#include "memory_pool.h"   // memop_* (evita declaracao implicita -> ponteiro truncado em x64)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VIDEO_TRACK 1
#define AUDIO_TRACK 2

typedef struct { uint8_t* d; size_t len, cap; } Buf;

static void buf_need(Buf* b, size_t extra)
{
    if (b->len + extra <= b->cap) return;
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->len + extra) cap *= 2;
    b->d = (uint8_t*)memop_realloc_raw(b->d, cap);
    b->cap = cap;
}
static void buf_u8(Buf* b, uint8_t v) { buf_need(b, 1); b->d[b->len++] = v; }
static void buf_bytes(Buf* b, const void* p, size_t n) { buf_need(b, n); memcpy(b->d + b->len, p, n); b->len += n; }
static void buf_free(Buf* b) { memop_free_raw(b->d); b->d = 0; b->len = b->cap = 0; }

// ---- primitivas EBML ------------------------------------------------------
static void ebml_id(Buf* b, uint32_t id)
{
    if (id & 0xFF000000) buf_u8(b, (id >> 24) & 0xFF);
    if (id & 0xFFFF0000) buf_u8(b, (id >> 16) & 0xFF);
    if (id & 0xFFFFFF00) buf_u8(b, (id >> 8) & 0xFF);
    buf_u8(b, id & 0xFF);
}
static void ebml_size(Buf* b, uint64_t size)
{
    if      (size < 0x7FULL)       { buf_u8(b, 0x80 | (uint8_t)size); }
    else if (size < 0x3FFFULL)     { buf_u8(b, 0x40 | (uint8_t)(size >> 8)); buf_u8(b, size & 0xFF); }
    else if (size < 0x1FFFFFULL)   { buf_u8(b, 0x20 | (uint8_t)(size >> 16)); buf_u8(b, (size >> 8) & 0xFF); buf_u8(b, size & 0xFF); }
    else if (size < 0x0FFFFFFFULL) { buf_u8(b, 0x10 | (uint8_t)(size >> 24)); buf_u8(b, (size >> 16) & 0xFF); buf_u8(b, (size >> 8) & 0xFF); buf_u8(b, size & 0xFF); }
    else { buf_u8(b, 0x08 | (uint8_t)(size >> 32)); buf_u8(b, (size >> 24) & 0xFF); buf_u8(b, (size >> 16) & 0xFF); buf_u8(b, (size >> 8) & 0xFF); buf_u8(b, size & 0xFF); }
}
static void ebml_uint_el(Buf* b, uint32_t id, uint64_t val)
{
    uint8_t tmp[8]; int n = 0;
    if (val == 0) tmp[n++] = 0;
    else { uint8_t rev[8]; int c = 0; uint64_t v = val; while (v) { rev[c++] = v & 0xFF; v >>= 8; } for (int i = c - 1; i >= 0; i--) tmp[n++] = rev[i]; }
    ebml_id(b, id); ebml_size(b, n); buf_bytes(b, tmp, n);
}
static void ebml_str_el(Buf* b, uint32_t id, const char* s) { size_t n = strlen(s); ebml_id(b, id); ebml_size(b, n); buf_bytes(b, s, n); }
static void ebml_bin_el(Buf* b, uint32_t id, const void* d, size_t n) { ebml_id(b, id); ebml_size(b, n); buf_bytes(b, d, n); }
static void ebml_float_el(Buf* b, uint32_t id, double val)
{
    uint64_t bits; memcpy(&bits, &val, 8);
    uint8_t be[8]; for (int i = 0; i < 8; i++) be[i] = (bits >> (56 - i * 8)) & 0xFF;
    ebml_bin_el(b, id, be, 8);
}

// IDs Matroska/WebM
#define ID_EBML 0x1A45DFA3
#define ID_SEGMENT 0x18538067
#define ID_INFO 0x1549A966
#define ID_TIMECODESCALE 0x2AD7B1
#define ID_MUXINGAPP 0x4D80
#define ID_WRITINGAPP 0x5741
#define ID_TRACKS 0x1654AE6B
#define ID_TRACKENTRY 0xAE
#define ID_TRACKNUMBER 0xD7
#define ID_TRACKUID 0x73C5
#define ID_TRACKTYPE 0x83
#define ID_CODECID 0x86
#define ID_CODECPRIV 0x63A2
#define ID_VIDEO 0xE0
#define ID_PIXELWIDTH 0xB0
#define ID_PIXELHEIGHT 0xBA
#define ID_AUDIO 0xE1
#define ID_SAMPFREQ 0xB5
#define ID_CHANNELS 0x9F
#define ID_CLUSTER 0x1F43B675
#define ID_TIMECODE 0xE7
#define ID_SIMPLEBLOCK 0xA3

struct WebmMux
{
    // modo arquivo unico
    FILE*   f;
    // modo segmentado (DASH)
    int     segmented;
    char    dir[512];
    char    name[128];
    int     seg_index;   // proximo numero de chunk (startNumber=1)
    int     seg_ms;

    // cluster/segmento atual (bufferizado)
    Buf     cluster;
    int64_t cluster_base;
    int     cluster_open;
    int     has_audio;
};

static void write_unknown_size(FILE* f) { uint8_t u[8] = { 0x01,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF }; fwrite(u, 1, 8, f); }

// EBML header + Segment(unknown) + Info + Tracks -> FILE* (usado no arquivo unico e no init).
// doctype: "webm" (VP9/AV1) ou "matroska" (H.264). vpriv/vpriv_len: CodecPrivate de video
// (avcC no H.264); NULL para VP9/AV1.
static void write_headers(FILE* f, const char* doctype, const char* codec_video, int width, int height,
                          const uint8_t* vpriv, int vpriv_len,
                          int has_audio, int audio_rate, int audio_channels,
                          const uint8_t* opus_head, int opus_head_len)
{
    Buf h = { 0 };
    ebml_uint_el(&h, 0x4286, 1); ebml_uint_el(&h, 0x42F7, 1);
    ebml_uint_el(&h, 0x42F2, 4); ebml_uint_el(&h, 0x42F3, 8);
    ebml_str_el (&h, 0x4282, doctype ? doctype : "webm");
    ebml_uint_el(&h, 0x4287, 2); ebml_uint_el(&h, 0x4285, 2);
    Buf hh = { 0 }; ebml_id(&hh, ID_EBML); ebml_size(&hh, h.len); buf_bytes(&hh, h.d, h.len);
    fwrite(hh.d, 1, hh.len, f); buf_free(&h); buf_free(&hh);

    Buf sid = { 0 }; ebml_id(&sid, ID_SEGMENT); fwrite(sid.d, 1, sid.len, f); buf_free(&sid);
    write_unknown_size(f);

    Buf info = { 0 };
    ebml_uint_el(&info, ID_TIMECODESCALE, 1000000);   // 1ms
    ebml_str_el (&info, ID_MUXINGAPP,  "appservertester");
    ebml_str_el (&info, ID_WRITINGAPP, "media/mux_webm");
    Buf iw = { 0 }; ebml_id(&iw, ID_INFO); ebml_size(&iw, info.len); buf_bytes(&iw, info.d, info.len);
    fwrite(iw.d, 1, iw.len, f); buf_free(&info); buf_free(&iw);

    Buf tracks = { 0 };
    { // video
        Buf t = { 0 };
        ebml_uint_el(&t, ID_TRACKNUMBER, VIDEO_TRACK); ebml_uint_el(&t, ID_TRACKUID, VIDEO_TRACK);
        ebml_uint_el(&t, ID_TRACKTYPE, 1);
        ebml_str_el (&t, ID_CODECID, codec_video ? codec_video : "V_VP9");
        if (vpriv && vpriv_len > 0) ebml_bin_el(&t, ID_CODECPRIV, vpriv, vpriv_len);   // avcC (H.264)
        Buf v = { 0 }; ebml_uint_el(&v, ID_PIXELWIDTH, width); ebml_uint_el(&v, ID_PIXELHEIGHT, height);
        ebml_id(&t, ID_VIDEO); ebml_size(&t, v.len); buf_bytes(&t, v.d, v.len); buf_free(&v);
        ebml_id(&tracks, ID_TRACKENTRY); ebml_size(&tracks, t.len); buf_bytes(&tracks, t.d, t.len); buf_free(&t);
    }
    if (has_audio && opus_head && opus_head_len > 0)
    {
        Buf t = { 0 };
        ebml_uint_el(&t, ID_TRACKNUMBER, AUDIO_TRACK); ebml_uint_el(&t, ID_TRACKUID, AUDIO_TRACK);
        ebml_uint_el(&t, ID_TRACKTYPE, 2);
        ebml_str_el (&t, ID_CODECID, "A_OPUS");
        ebml_bin_el (&t, ID_CODECPRIV, opus_head, opus_head_len);
        Buf a = { 0 }; ebml_float_el(&a, ID_SAMPFREQ, (double)audio_rate); ebml_uint_el(&a, ID_CHANNELS, audio_channels);
        ebml_id(&t, ID_AUDIO); ebml_size(&t, a.len); buf_bytes(&t, a.d, a.len); buf_free(&a);
        ebml_id(&tracks, ID_TRACKENTRY); ebml_size(&tracks, t.len); buf_bytes(&tracks, t.d, t.len); buf_free(&t);
    }
    Buf tw = { 0 }; ebml_id(&tw, ID_TRACKS); ebml_size(&tw, tracks.len); buf_bytes(&tw, tracks.d, tracks.len);
    fwrite(tw.d, 1, tw.len, f); buf_free(&tracks); buf_free(&tw);
}

WebmMux* webm_open(const char* path, const char* codec_video, int width, int height, int fps,
                   int has_audio, int audio_rate, int audio_channels,
                   const uint8_t* opus_head, int opus_head_len)
{
    (void)fps;
    WebmMux* m = (WebmMux*)memop_calloc_raw(1, sizeof(WebmMux));
    if (!m) return 0;
    if (fopen_s(&m->f, path, "wb") != 0 || !m->f) { memop_free_raw(m); return 0; }
    m->has_audio = has_audio;
    write_headers(m->f, "webm", codec_video, width, height, 0, 0, has_audio, audio_rate, audio_channels, opus_head, opus_head_len);
    return m;
}

// MKV single-file com H.264 (CodecID V_MPEG4/ISO/AVC + avcC). Frames em AVCC (length-prefixed).
WebmMux* webm_open_h264(const char* path, const uint8_t* avcc, int avcc_len,
                        int width, int height, int fps,
                        int has_audio, int audio_rate, int audio_channels,
                        const uint8_t* opus_head, int opus_head_len)
{
    (void)fps;
    WebmMux* m = (WebmMux*)memop_calloc_raw(1, sizeof(WebmMux));
    if (!m) return 0;
    if (fopen_s(&m->f, path, "wb") != 0 || !m->f) { memop_free_raw(m); return 0; }
    m->has_audio = has_audio;
    write_headers(m->f, "matroska", "V_MPEG4/ISO/AVC", width, height, avcc, avcc_len,
                  has_audio, audio_rate, audio_channels, opus_head, opus_head_len);
    return m;
}

WebmMux* webm_open_segmented(const char* dir, const char* name, const char* codec_video,
                             int width, int height, int fps, int seg_ms,
                             int has_audio, int audio_rate, int audio_channels,
                             const uint8_t* opus_head, int opus_head_len)
{
    (void)fps;
    WebmMux* m = (WebmMux*)memop_calloc_raw(1, sizeof(WebmMux));
    if (!m) return 0;
    m->segmented = 1;
    m->seg_index = 1;                       // startNumber=1
    m->seg_ms = seg_ms > 0 ? seg_ms : 2000;
    m->has_audio = has_audio;
    snprintf(m->dir, sizeof(m->dir), "%s", dir);
    snprintf(m->name, sizeof(m->name), "%s", name);

    // init-<name>.webm = header + Segment(unknown) + Info + Tracks (sem clusters).
    char path[1024];
    snprintf(path, sizeof(path), "%s/init-%s.webm", dir, name);
    FILE* initf = 0;
    if (fopen_s(&initf, path, "wb") != 0 || !initf) { memop_free_raw(m); return 0; }
    write_headers(initf, "webm", codec_video, width, height, 0, 0, has_audio, audio_rate, audio_channels, opus_head, opus_head_len);
    fclose(initf);
    return m;
}

// Abre um muxer para gravar UM UNICO chunk (1 cluster) de um segmento ja conhecido.
// NAO escreve init (o init-<name>.webm e gerado uma vez, por webm_open_segmented).
// Uso: webm_open_chunk -> webm_write_video (N frames, o 1o deve ser keyframe) -> webm_close,
// que grava "<dir>/chunk-<name>-<seg_number>.webm". seg_ms alto = nao corta no meio.
// Feito para o segment-parallel: cada task grava seu chunk isolado, sem contencao.
WebmMux* webm_open_chunk(const char* dir, const char* name, int seg_number)
{
    WebmMux* m = (WebmMux*)memop_calloc_raw(1, sizeof(WebmMux));
    if (!m) return 0;
    m->segmented = 1;
    m->seg_index = seg_number;        // grava chunk-<name>-<seg_number>.webm
    m->seg_ms    = 0x7fffffff;        // sem auto-cut interno: 1 cluster p/ o segmento inteiro
    snprintf(m->dir,  sizeof(m->dir),  "%s", dir);
    snprintf(m->name, sizeof(m->name), "%s", name);
    return m;                          // init NAO escrito aqui
}

static void flush_cluster(WebmMux* m)
{
    if (!m->cluster_open || m->cluster.len == 0) { m->cluster_open = 0; m->cluster.len = 0; return; }

    Buf head = { 0 };
    ebml_uint_el(&head, ID_TIMECODE, (uint64_t)m->cluster_base);
    Buf full = { 0 };
    ebml_id(&full, ID_CLUSTER);
    ebml_size(&full, head.len + m->cluster.len);
    buf_bytes(&full, head.d, head.len);
    buf_bytes(&full, m->cluster.d, m->cluster.len);

    if (m->segmented)
    {
        char path[1024];
        snprintf(path, sizeof(path), "%s/chunk-%s-%d.webm", m->dir, m->name, m->seg_index);
        FILE* seg = 0;
        if (fopen_s(&seg, path, "wb") == 0 && seg) { fwrite(full.d, 1, full.len, seg); fclose(seg); m->seg_index++; }
    }
    else
    {
        fwrite(full.d, 1, full.len, m->f);
    }

    buf_free(&head); buf_free(&full);
    m->cluster.len = 0;
    m->cluster_open = 0;
}

static void write_block(WebmMux* m, int track, int64_t ts_ms, int keyframe, const uint8_t* data, int size)
{
    // Limite de cluster/segmento: em keyframe de video. No modo segmentado, so quando ja
    // passou seg_ms desde o inicio do segmento atual (alinha os segmentos DASH).
    int boundary = 0;
    if (!m->cluster_open) boundary = 1;
    else if (track == VIDEO_TRACK && keyframe)
        boundary = m->segmented ? ((ts_ms - m->cluster_base) >= m->seg_ms) : 1;

    if (boundary) { flush_cluster(m); m->cluster_base = ts_ms; m->cluster_open = 1; }

    int64_t rel = ts_ms - m->cluster_base;
    if (rel < -32768) rel = -32768; if (rel > 32767) rel = 32767;

    Buf blk = { 0 };
    buf_u8(&blk, 0x80 | (uint8_t)track);
    buf_u8(&blk, (uint8_t)((rel >> 8) & 0xFF));
    buf_u8(&blk, (uint8_t)(rel & 0xFF));
    buf_u8(&blk, keyframe ? 0x80 : 0x00);
    buf_bytes(&blk, data, size);
    ebml_id(&m->cluster, ID_SIMPLEBLOCK);
    ebml_size(&m->cluster, blk.len);
    buf_bytes(&m->cluster, blk.d, blk.len);
    buf_free(&blk);
}

int webm_write_video(WebmMux* m, int64_t ts_ms, int keyframe, const uint8_t* data, int size)
{
    if (!m || !data || size <= 0) return -1;
    write_block(m, VIDEO_TRACK, ts_ms, keyframe, data, size);
    return 0;
}
int webm_write_audio(WebmMux* m, int64_t ts_ms, const uint8_t* data, int size)
{
    if (!m || !m->has_audio || !data || size <= 0) return -1;
    write_block(m, AUDIO_TRACK, ts_ms, 0, data, size);
    return 0;
}

int webm_build_opus_head(int channels, int rate, uint8_t out[19])
{
    memcpy(out, "OpusHead", 8);
    out[8]  = 1;                                  // version
    out[9]  = (uint8_t)channels;                  // channel count
    uint16_t preskip = 3840;                      // 80ms @48k (padrao libopus)
    out[10] = preskip & 0xFF; out[11] = (preskip >> 8) & 0xFF;                 // LE
    out[12] = rate & 0xFF; out[13] = (rate >> 8) & 0xFF;                        // input samplerate LE
    out[14] = (rate >> 16) & 0xFF; out[15] = (rate >> 24) & 0xFF;
    out[16] = 0; out[17] = 0;                     // output gain
    out[18] = 0;                                  // channel mapping family (mono/stereo)
    return 19;
}

void webm_close(WebmMux* m)
{
    if (!m) return;
    flush_cluster(m);
    if (m->f) fclose(m->f);
    buf_free(&m->cluster);
    memop_free_raw(m);
}
