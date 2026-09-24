//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  sink_live: saida AO VIVO (camera) em fMP4, entregue a um LiveStore em MEMORIA.
//  Mesmas caixas do HLS VOD (init.mp4 + segmentos moof/mdat), so que nada e' gravado:
//  cada Cut() monta o segmento com mp4_build_segment e o empurra na janela deslizante.
//
//  UMA pista de video: ao vivo a resolucao e' a que o usuario escolheu na barra do player,
//  e uma escada de renditions multiplicaria o encode em tempo real sem ninguem para
//  consumi-la. Audio ainda nao entra (a captura de camera aqui e' so video).

#include "media_sink.h"
#include "live_store.h"
#include "mux_mp4.h"
#include "h26x_util.h"
#include <stdio.h>

typedef struct { uint8_t* data; int size; int64_t ms; int key; } LSample;

typedef struct
{
    LiveStore* store;                 // NAO e' dono: quem criou a sessao ao vivo e' que libera
    H26xToLen  conv;
    int        w, h, bw, fps, init_done;
    uint32_t   seq;
    LSample*   s; int n, cap;
    int        vtrack;                // indice da pista de video (as outras sao ignoradas)
    const GwFeedback* fb;
}
SinkLive;

static void flush_seg(SinkLive* k)
{
    if (k->n <= 0) return;

    int last = k->fps > 0 ? (1000 / k->fps) : 33;
    Mp4Sample* ms = (Mp4Sample*)GW_ALLOC((size_t)k->n * sizeof(Mp4Sample));
    if (!ms) return;

    double dur_s = 0.0;
    for (int i = 0; i < k->n; i++)
    {
        int d = (i + 1 < k->n) ? (int)(k->s[i + 1].ms - k->s[i].ms) : last; if (d <= 0) d = last;
        ms[i].data = k->s[i].data; ms[i].size = k->s[i].size; ms[i].duration_ms = d; ms[i].keyframe = k->s[i].key;
        dur_s += d / 1000.0;
    }

    uint8_t* seg = 0; int seg_len = 0;
    if (mp4_build_segment(&seg, &seg_len, k->seq + 1, k->s[0].ms, ms, k->n) == 0 && seg)
    {
        live_store_push(k->store, seg, seg_len, dur_s);
        GW_FREE(seg);
        k->seq++;
    }

    for (int i = 0; i < k->n; i++) GW_FREE(k->s[i].data);
    GW_FREE(ms);
    k->n = 0;
}

static int slive_start(MediaSink* sk, const MediaTrackOut* tracks, int count)
{
    SinkLive* k = (SinkLive*)sk->Ctx;
    for (int i = 0; i < count; i++)
    {
        if (tracks[i].Type != MSTREAM_VIDEO) continue;
        k->vtrack = i;
        k->w = tracks[i].Width; k->h = tracks[i].Height;
        k->bw = tracks[i].Bandwidth; k->fps = (int)(tracks[i].Fps + 0.5);
        h26x_tl_init(&k->conv, tracks[i].Codec == MEDIA_CODEC_H265);
        return 0;
    }
    return -1;   // sem video nao ha o que transmitir
}

static int slive_write(MediaSink* sk, int track, const GwPacket* pkt)
{
    SinkLive* k = (SinkLive*)sk->Ctx;
    if (!pkt || pkt->Size <= 0 || track != k->vtrack) return 0;

    uint8_t* out; int alen = h26x_tl_feed(&k->conv, pkt->Data, pkt->Size, &out);

    // O init so pode sair depois que SPS/PPS aparecem no bitstream (o avcC vem dali).
    if (!k->init_done && h26x_tl_ready(&k->conv))
    {
        uint8_t* ini = 0; int ini_len = 0;
        if (mp4_build_init(&ini, &ini_len, k->w, k->h, k->conv.cfg, k->conv.cfg_len) == 0 && ini)
        {
            live_store_set_init(k->store, ini, ini_len, k->w, k->h, k->bw);
            GW_FREE(ini);
            k->init_done = 1;
        }
    }
    if (!k->init_done || alen <= 0) return 0;

    if (k->n == k->cap)
    {
        k->cap = k->cap ? k->cap * 2 : 64;
        k->s = (LSample*)GW_REALLOC(k->s, (size_t)k->cap * sizeof(LSample));
        if (!k->s) { k->n = 0; k->cap = 0; return -1; }
    }
    uint8_t* cp = (uint8_t*)GW_ALLOC((size_t)alen);
    if (!cp) return -1;
    GW_COPY(cp, out, alen);

    k->s[k->n].data = cp; k->s[k->n].size = alen;
    k->s[k->n].ms = mt_to_ms(pkt->Pts); k->s[k->n].key = pkt->KeyFrame;
    k->n++;
    return 0;
}

static int slive_cut(MediaSink* sk, mtime_us at)
{
    (void)at;
    flush_seg((SinkLive*)sk->Ctx);
    return 0;
}

static void slive_close(MediaSink* sk)
{
    if (!sk) return;
    SinkLive* k = (SinkLive*)sk->Ctx;
    if (k)
    {
        if (!sk->Aborted) flush_seg(k);           // ultimo segmento
        else for (int i = 0; i < k->n; i++) GW_FREE(k->s[i].data);
        live_store_finish(k->store);              // playlist ganha ENDLIST: o player para limpo
        h26x_tl_free(&k->conv);
        GW_FREE(k->s);
        GW_FREE(k);
    }
    GW_FREE(sk);
}

MediaSink* sink_live_open(const MediaProfile* profile, const char* base_dir, const GwFeedback* fb)
{
    (void)base_dir;   // ao vivo nao usa disco
    if (!profile || !profile->Live) return 0;

    SinkLive* k = (SinkLive*)GW_CALLOC(1, sizeof(SinkLive));
    if (!k) return 0;
    k->store = (LiveStore*)profile->Live;
    k->fb = fb;
    k->vtrack = -1;

    MediaSink* sk = (MediaSink*)GW_CALLOC(1, sizeof(MediaSink));
    if (!sk) { GW_FREE(k); return 0; }
    sk->Ctx = k;
    sk->Start = slive_start; sk->Write = slive_write; sk->Cut = slive_cut; sk->Close = slive_close;
    return sk;
}
