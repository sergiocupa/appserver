//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  sink_mp4: saida MP4 single-file (H.264/H.265/VP9/AV1 + AAC passthrough) via mux_mp4.
//  Ignora Cut() (arquivo unico). Alocacoes via memory_pool.
//
//  VP9 e AV1 tambem saem em MP4 quando ha audio: o WebM so aceita Opus/Vorbis, e converter o
//  AAC da fonte exigiria um decoder de AAC que o projeto nao tem (o arquivo saia MUDO). Em MP4
//  o AAC original vai junto, sem conversao. O nome leva o codec (output-vp9.mp4 /
//  output-av1.mp4) para nao sobrescrever a conversao H.264/H.265 da mesma pasta.

#include "media_sink.h"
#include "mux_mp4.h"
#include "h26x_util.h"
#include <stdio.h>
#include <string.h>

typedef struct { uint8_t* data; int size; int dur_units; } AQItem;

typedef struct
{
    char        path[1024];
    char        rel[64];
    int         is_hevc, opened, vtrack, atrack;
    int         vw, vh, have_audio, a_rate, a_ch, asc_len;
    uint8_t     asc[64];
    Mp4Mux*     mux;
    H26xToLen   v;
    AQItem*     aq; int aqn, aqcap;      // audio chegado antes do mux abrir
    int64_t     a_ini_ms; int a_ini_ok;  // apresentacao do 1o pacote de audio (pode ser negativa)
    int         kind;                    // MP4_KIND_*: H.264, HEVC, VP9 ou AV1
    int         fps;
    uint8_t     xcfg[160]; int xcfg_len; // VP9: 8 bytes do vpcC; AV1: av1C (4 bytes + cabecalho de sequencia)
    uint8_t*    tu; int tu_cap;          // AV1: a amostra sem os delimitadores de tempo
    const GwFeedback* fb;
}
SinkMp4;

// ---- AV1: OBUs ---------------------------------------------------------------------
static int leb128(const uint8_t* p, int n, uint32_t* v)
{
    uint32_t r = 0;
    for (int i = 0; i < 5 && i < n; i++) { r |= (uint32_t)(p[i] & 0x7F) << (7 * i); if (!(p[i] & 0x80)) { *v = r; return i + 1; } }
    return 0;
}

static int bits(const uint8_t* p, int n, int* pos, int qt)
{
    int v = 0;
    for (int i = 0; i < qt; i++) { int b = *pos; v <<= 1; if ((b >> 3) < n) v |= (p[b >> 3] >> (7 - (b & 7))) & 1; (*pos)++; }
    return v;
}

// av1C a partir do OBU de cabecalho de sequencia: perfil, nivel e camada lidos do proprio
// cabecalho (8 bits, 4:2:0 -- e o que o encoder do projeto produz), mais o OBU inteiro.
static void av1c_monta(SinkMp4* s, const uint8_t* obu, int obu_len, const uint8_t* pl, int pl_len)
{
    int pos = 0, perfil = bits(pl, pl_len, &pos, 3), nivel = 8, camada = 0;
    bits(pl, pl_len, &pos, 1);                              // still_picture
    if (bits(pl, pl_len, &pos, 1)) nivel = bits(pl, pl_len, &pos, 5);   // reduced_still_picture_header
    else if (!bits(pl, pl_len, &pos, 1))                    // sem timing_info: os campos seguem direto
    {
        bits(pl, pl_len, &pos, 1);                          // initial_display_delay_present_flag
        bits(pl, pl_len, &pos, 5);                          // operating_points_cnt_minus_1
        bits(pl, pl_len, &pos, 12);                         // operating_point_idc[0]
        nivel = bits(pl, pl_len, &pos, 5);
        if (nivel > 7) camada = bits(pl, pl_len, &pos, 1);
    }
    s->xcfg[0] = 0x81;                                      // marker + version 1
    s->xcfg[1] = (uint8_t)((perfil << 5) | (nivel & 31));
    s->xcfg[2] = (uint8_t)((camada << 7) | 0x0C);           // 8 bits, subamostragem 4:2:0
    s->xcfg[3] = 0;
    GW_COPY(s->xcfg + 4, obu, obu_len);
    s->xcfg_len = 4 + obu_len;
}

// Copia a unidade temporal SEM os OBUs delimitadores de tempo (no MP4 eles nao vao) e, na
// primeira que trouxer o cabecalho de sequencia, monta o av1C. Devolve o tamanho da amostra.
static int av1_amostra(SinkMp4* s, const uint8_t* d, int n, const uint8_t** out)
{
    if (n > s->tu_cap) { s->tu_cap = n + 4096; s->tu = (uint8_t*)GW_REALLOC(s->tu, (size_t)s->tu_cap); }
    *out = s->tu;
    if (!s->tu) return 0;
    int o = 0, p = 0;
    while (p < n)
    {
        int h = d[p], tipo = (h >> 3) & 0x0F, ext = (h >> 2) & 1, tem = (h >> 1) & 1;
        int cab = 1 + ext, lb = 0; uint32_t tam = 0;
        if (p + cab > n) break;
        if (tem) { lb = leb128(d + p + cab, n - p - cab, &tam); if (!lb) break; }
        else tam = (uint32_t)(n - p - cab);                 // sem campo de tamanho: vai ate o fim
        int total = cab + lb + (int)tam;
        if (total <= 0 || p + total > n) break;
        if (tipo == 1 && !s->xcfg_len && tem && total <= (int)sizeof(s->xcfg) - 4)
            av1c_monta(s, d + p, total, d + p + cab + lb, (int)tam);
        if (tipo != 2) { GW_COPY(s->tu + o, d + p, total); o += total; }   // 2 = delimitador de tempo
        p += total;
    }
    return o;
}

// vpcC do VP9 (perfil 0, 8 bits, 4:2:0). O nivel sai do tamanho do quadro e da taxa.
static void vpcc_monta(SinkMp4* s)
{
    long px = (long)s->vw * s->vh; int rapido = s->fps > 30;
    int nivel = px <= 256L * 144 ? 10 : px <= 640L * 360 ? 21 : px <= 854L * 480 ? 30 : px <= 1280L * 720 ? 31
              : px <= 1920L * 1080 ? (rapido ? 41 : 40) : (rapido ? 51 : 50);
    s->xcfg[0] = 0; s->xcfg[1] = (uint8_t)nivel; s->xcfg[2] = 0x82;   // 8 bits | 4:2:0 | faixa limitada
    s->xcfg[3] = 1; s->xcfg[4] = 1; s->xcfg[5] = 1;                   // BT.709
    s->xcfg[6] = 0; s->xcfg[7] = 0;                                   // sem dados de inicializacao
    s->xcfg_len = 8;
}

static void open_mux(SinkMp4* s)
{
    if (s->opened) return;
    if (s->kind == MP4_KIND_VP9 || s->kind == MP4_KIND_AV1)
    {
        if (s->kind == MP4_KIND_VP9 && !s->xcfg_len) vpcc_monta(s);
        if (!s->xcfg_len) return;                           // AV1: espera o cabecalho de sequencia
        s->mux = mp4_open_kind(s->path, s->kind, s->vw, s->vh, s->xcfg, s->xcfg_len,
                               s->have_audio ? s->asc : 0, s->asc_len, s->a_rate, s->a_ch);
    }
    else
    {
        if (!h26x_tl_ready(&s->v)) return;
        s->mux = s->is_hevc
            ? mp4_open_hevc(s->path, s->vw, s->vh, s->v.cfg, s->v.cfg_len,
                            s->have_audio ? s->asc : 0, s->asc_len, s->a_rate, s->a_ch)
            : mp4_open2(s->path, s->vw, s->vh, s->v.cfg, s->v.cfg_len,
                        s->have_audio ? s->asc : 0, s->asc_len, s->a_rate, s->a_ch);
    }
    s->opened = 1;
    if (s->mux && s->a_ini_ok) mp4_set_audio_start(s->mux, s->a_ini_ms);
    for (int i = 0; s->mux && i < s->aqn; i++)   // drena o audio pendente
    {
        mp4_write_audio(s->mux, s->aq[i].dur_units, s->aq[i].data, s->aq[i].size);
        GW_FREE(s->aq[i].data);
    }
    s->aqn = 0;
}

static int smp4_start(MediaSink* k, const MediaTrackOut* tracks, int count)
{
    SinkMp4* s = (SinkMp4*)k->Ctx;
    s->vtrack = s->atrack = -1;
    for (int i = 0; i < count; i++)
    {
        if (tracks[i].Type == MSTREAM_VIDEO && s->vtrack < 0)
        {
            s->vtrack = i; s->is_hevc = (tracks[i].Codec == MEDIA_CODEC_H265);
            s->kind = tracks[i].Codec == MEDIA_CODEC_AV1 ? MP4_KIND_AV1 : tracks[i].Codec == MEDIA_CODEC_VP9 ? MP4_KIND_VP9
                    : s->is_hevc ? MP4_KIND_HEVC : MP4_KIND_H264;
            s->vw = tracks[i].Width; s->vh = tracks[i].Height; s->fps = (int)(tracks[i].Fps + 0.5);
            h26x_tl_init(&s->v, s->is_hevc);
            if (s->kind == MP4_KIND_VP9 || s->kind == MP4_KIND_AV1)
            {
                const char* c = s->kind == MP4_KIND_AV1 ? "av1" : "vp9";
                size_t L = strlen(s->path);
                if (L > 10) snprintf(s->path + L - 10, sizeof(s->path) - (L - 10), "output-%s.mp4", c);   // troca "output.mp4"
                snprintf(s->rel, sizeof(s->rel), "output-%s.mp4", c);
            }
        }
        else if (tracks[i].Type == MSTREAM_AUDIO && s->atrack < 0 && tracks[i].Codec == MEDIA_CODEC_AAC)
        {
            s->atrack = i; s->have_audio = 1; s->a_rate = tracks[i].SampleRate; s->a_ch = tracks[i].Channels;
            s->asc_len = tracks[i].ExtraLen < 64 ? tracks[i].ExtraLen : 64;
            if (tracks[i].Extra && s->asc_len > 0) GW_COPY(s->asc, tracks[i].Extra, s->asc_len);
        }
    }
    return s->vtrack >= 0 ? 0 : -1;
}

static int smp4_write(MediaSink* k, int track, const GwPacket* pkt)
{
    SinkMp4* s = (SinkMp4*)k->Ctx;
    if (!pkt || pkt->Size <= 0) return 0;

    if (track == s->vtrack && (s->kind == MP4_KIND_VP9 || s->kind == MP4_KIND_AV1))
    {
        const uint8_t* d = pkt->Data; int n = pkt->Size;
        if (s->kind == MP4_KIND_AV1) n = av1_amostra(s, pkt->Data, pkt->Size, &d);
        open_mux(s);
        if (s->opened && s->mux && n > 0) mp4_write_video2(s->mux, mt_to_ms(pkt->Pts), mt_to_ms(pkt->Dts), pkt->KeyFrame, d, n);
        return 0;
    }
    if (track == s->vtrack)
    {
        uint8_t* out; int alen = h26x_tl_feed(&s->v, pkt->Data, pkt->Size, &out);
        open_mux(s);
        // os dois tempos: copia sem recodificar de fonte com quadros B tem pts != dts
        if (s->opened && s->mux && alen > 0) mp4_write_video2(s->mux, mt_to_ms(pkt->Pts), mt_to_ms(pkt->Dts), pkt->KeyFrame, out, alen);
        return 0;
    }
    if (track == s->atrack && s->have_audio)
    {
        int dur = pkt->Dur > 0 ? (int)mt_to_scale(pkt->Dur, s->a_rate) : 1024;
        if (!s->a_ini_ok) { s->a_ini_ok = 1; s->a_ini_ms = mt_to_ms(pkt->Pts); if (s->mux) mp4_set_audio_start(s->mux, s->a_ini_ms); }
        if (s->opened && s->mux) { mp4_write_audio(s->mux, dur, pkt->Data, pkt->Size); return 0; }
        // ainda sem mux: enfileira (copia)
        if (s->aqn == s->aqcap) { s->aqcap = s->aqcap ? s->aqcap * 2 : 64; s->aq = (AQItem*)GW_REALLOC(s->aq, s->aqcap * sizeof(AQItem)); }
        uint8_t* cp = (uint8_t*)GW_ALLOC(pkt->Size); GW_COPY(cp, pkt->Data, pkt->Size);
        s->aq[s->aqn].data = cp; s->aq[s->aqn].size = pkt->Size; s->aq[s->aqn].dur_units = dur; s->aqn++;
    }
    return 0;
}

static int smp4_cut(MediaSink* k, mtime_us at) { (void)k; (void)at; return 0; }   // arquivo unico

static void smp4_close(MediaSink* k)
{
    if (!k) return;
    SinkMp4* s = (SinkMp4*)k->Ctx;
    if (s)
    {
        if (!k->Aborted)
        {
            open_mux(s);
            if (s->mux) mp4_close(s->mux);
            if (s->mux)
            {
                GwEvent e; GW_ZERO(&e, sizeof(e)); e.Kind = GW_EV_DONE; e.Playlist = s->rel; gw_emit(s->fb, &e);
                gw_emit_mem(s->fb);
            }
            else gw_error(s->fb, "sink_mp4: nenhum video codificado (avcC/hvcC ausente).");
        }
        for (int i = 0; i < s->aqn; i++) GW_FREE(s->aq[i].data);
        GW_FREE(s->aq);
        GW_FREE(s->tu);
        h26x_tl_free(&s->v);
        GW_FREE(s);
    }
    GW_FREE(k);
}

MediaSink* sink_mp4_open(const MediaProfile* profile, const char* base_dir, const GwFeedback* fb)
{
    (void)profile;
    SinkMp4* s = (SinkMp4*)GW_CALLOC(1, sizeof(SinkMp4));
    if (!s) return 0;
    s->fb = fb;
    snprintf(s->rel, sizeof(s->rel), "output.mp4");
    snprintf(s->path, sizeof(s->path), "%s/output.mp4", base_dir ? base_dir : ".");

    MediaSink* k = (MediaSink*)GW_CALLOC(1, sizeof(MediaSink));
    if (!k) { GW_FREE(s); return 0; }
    k->Ctx = s; k->Start = smp4_start; k->Write = smp4_write; k->Cut = smp4_cut; k->Close = smp4_close;
    return k;
}
