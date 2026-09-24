//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  sink_mp4: saida MP4 single-file (H.264/H.265 + AAC passthrough) via mux_mp4.
//  Ignora Cut() (arquivo unico). Alocacoes via memory_pool. NAO TESTADO EM RUNTIME.

#include "media_sink.h"
#include "mux_mp4.h"
#include "h26x_util.h"
#include <stdio.h>

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
    const GwFeedback* fb;
}
SinkMp4;

static void open_mux(SinkMp4* s)
{
    if (s->opened || !h26x_tl_ready(&s->v)) return;
    s->mux = s->is_hevc
        ? mp4_open_hevc(s->path, s->vw, s->vh, s->v.cfg, s->v.cfg_len,
                        s->have_audio ? s->asc : 0, s->asc_len, s->a_rate, s->a_ch)
        : mp4_open2(s->path, s->vw, s->vh, s->v.cfg, s->v.cfg_len,
                    s->have_audio ? s->asc : 0, s->asc_len, s->a_rate, s->a_ch);
    s->opened = 1;
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
            s->vw = tracks[i].Width; s->vh = tracks[i].Height;
            h26x_tl_init(&s->v, s->is_hevc);
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

    if (track == s->vtrack)
    {
        uint8_t* out; int alen = h26x_tl_feed(&s->v, pkt->Data, pkt->Size, &out);
        open_mux(s);
        if (s->opened && s->mux && alen > 0) mp4_write_video(s->mux, mt_to_ms(pkt->Pts), pkt->KeyFrame, out, alen);
        return 0;
    }
    if (track == s->atrack && s->have_audio)
    {
        int dur = pkt->Dur > 0 ? (int)mt_to_scale(pkt->Dur, s->a_rate) : 1024;
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
