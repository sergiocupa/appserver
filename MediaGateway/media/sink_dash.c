//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  sink_dash: saida DASH-WebM. Uma WebM SEGMENTADA por rendition de video (VP9/AV1),
//  com o audio Opus distribuido (muxado) em cada rendition (codecs="<v>,opus"), e um
//  manifest-<codec>.mpd (SegmentTemplate). O WebM se auto-segmenta por keyframe/seg_ms, entao
//  Cut() e no-op. Alocacoes via memory_pool.
//
//  MANIFESTO: lista o inicio e a duracao REAIS de cada chunk (SegmentTimeline). Declarar
//  "chunks de seg_ms" mandava o player pedir arquivos que nao existem: o corte cai no quadro-
//  chave que o encoder escolheu (medido: ~3,2 s pedindo 2 s, e contagens diferentes por
//  resolucao -- 73/72/69/44 chunks). Resolucoes com os mesmos cortes compartilham uma lista;
//  senao, cada uma leva a sua.
//
//  AUDIO: AAC original numa faixa fMP4 separada (aud_fmp4), sem conversao. O ramo Opus
//  (muxado no WebM) so vale se um dia houver decoder de AAC para a conversao.

#include "media_sink.h"
#include "aud_fmp4.h"
#include "mpd_util.h"
#include "mux_webm.h"
#include <stdio.h>

#define DASH_MAXR 8

typedef struct
{
    char     base[1024];
    int      seg_ms, is_av1, vcount;
    WebmMux* wm[DASH_MAXR];
    int      rw[DASH_MAXR], rh[DASH_MAXR], rbw[DASH_MAXR], rfps[DASH_MAXR];
    char     rname[DASH_MAXR][32];
    int      has_audio, arate, ach, ohl; uint8_t ohead[19];
    int      track_kind[DASH_MAXR + 2], track_sub[DASH_MAXR + 2];
    int64_t  last_ms;
    AudFmp4* aud;                                      // audio AAC em faixa fMP4 propria
    MpdSeg*  tl[DASH_MAXR]; int tl_n[DASH_MAXR];       // cortes reais de cada resolucao (ms)
    const GwFeedback* fb;
}
SinkDash;

static void write_mpd(SinkDash* s)
{
    char path[1200]; snprintf(path, sizeof(path), "%s/%s", s->base, mpd_webm_manifest(s->is_av1));
    FILE* f = 0; if (fopen_s(&f, path, "wb") != 0 || !f) return;

    const char* vc = s->is_av1 ? "av01.0.08M.08" : "vp9";
    char codecs[64];
    if (s->has_audio) snprintf(codecs, sizeof(codecs), "%s,opus", vc);
    else              snprintf(codecs, sizeof(codecs), "%s", vc);

    int alinhado = 1;
    for (int v = 1; v < s->vcount; v++) if (!mpd_seg_iguais(s->tl[0], s->tl_n[0], s->tl[v], s->tl_n[v])) alinhado = 0;

    double dur = s->last_ms / 1000.0, adur = aud_fmp4_duration(s->aud);
    // a duracao declarada inclui o ultimo quadro: o fim do ultimo chunk listado
    if (s->vcount > 0 && s->tl_n[0] > 0)
    {
        double fim = (double)(s->tl[0][s->tl_n[0] - 1].t + s->tl[0][s->tl_n[0] - 1].d) / 1000.0;
        if (fim > dur) dur = fim;
    }
    if (adur > dur) dur = adur;

    static const char TPL[] = "<SegmentTemplate timescale=\"1000\" startNumber=\"1\" "
                              "initialization=\"init-$RepresentationID$.webm\" media=\"chunk-$RepresentationID$-$Number$.webm\">\n";
    fprintf(f,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<MPD xmlns=\"urn:mpeg:dash:schema:mpd:2011\" type=\"static\" "
        "mediaPresentationDuration=\"PT%.3fS\" minBufferTime=\"PT2S\" "
        "profiles=\"urn:mpeg:dash:profile:isoff-live:2011\">\n"
        "  <Period>\n"
        "    <AdaptationSet contentType=\"video\" mimeType=\"video/webm\" codecs=\"%s\" segmentAlignment=\"%s\" startWithSAP=\"1\">\n",
        dur, codecs, alinhado ? "true" : "false");

    if (alinhado)
    {
        fprintf(f, "      %s", TPL);
        mpd_timeline(f, s->tl[0], s->tl_n[0], "        ");
        fprintf(f, "      </SegmentTemplate>\n");
        for (int i = 0; i < s->vcount; i++)
            fprintf(f, "      <Representation id=\"%s\" bandwidth=\"%d\" width=\"%d\" height=\"%d\"/>\n",
                    s->rname[i], s->rbw[i], s->rw[i], s->rh[i]);
    }
    else
    {
        for (int i = 0; i < s->vcount; i++)
        {
            fprintf(f, "      <Representation id=\"%s\" bandwidth=\"%d\" width=\"%d\" height=\"%d\">\n        %s",
                    s->rname[i], s->rbw[i], s->rw[i], s->rh[i], TPL);
            mpd_timeline(f, s->tl[i], s->tl_n[i], "          ");
            fprintf(f, "        </SegmentTemplate>\n      </Representation>\n");
        }
    }
    fprintf(f, "    </AdaptationSet>\n");
    aud_fmp4_mpd(s->aud, f);
    fprintf(f, "  </Period>\n</MPD>\n");
    fclose(f);
}

static int sdash_start(MediaSink* k, const MediaTrackOut* tracks, int count)
{
    SinkDash* s = (SinkDash*)k->Ctx;
    for (int i = 0; i < count && i < DASH_MAXR + 2; i++)
    {
        s->track_kind[i] = -1;
        if (tracks[i].Type == MSTREAM_VIDEO && s->vcount < DASH_MAXR)
        {
            int v = s->vcount;
            s->rw[v] = tracks[i].Width; s->rh[v] = tracks[i].Height; s->rbw[v] = tracks[i].Bandwidth;
            s->rfps[v] = (int)(tracks[i].Fps + 0.5); if (s->rfps[v] <= 0) s->rfps[v] = 30;
            // nome dos ARQUIVOS e id da Representation: com o codec na frente (ver mpd_util.h)
            snprintf(s->rname[v], sizeof(s->rname[v]), "%s-%s", mpd_webm_tag(tracks[i].Codec == MEDIA_CODEC_AV1), tracks[i].Name ? tracks[i].Name : "v");
            s->is_av1 = (tracks[i].Codec == MEDIA_CODEC_AV1);
            s->track_kind[i] = 0; s->track_sub[i] = v; s->vcount++;
        }
        else if (tracks[i].Type == MSTREAM_AUDIO && !s->has_audio && tracks[i].Codec == MEDIA_CODEC_OPUS)
        {
            s->has_audio = 1; s->arate = tracks[i].SampleRate > 0 ? tracks[i].SampleRate : 48000; s->ach = tracks[i].Channels > 0 ? tracks[i].Channels : 2;
            s->ohl = webm_build_opus_head(s->ach, s->arate, s->ohead);
            s->track_kind[i] = 1;
        }
        else if (tracks[i].Type == MSTREAM_AUDIO && !s->aud && !s->has_audio && tracks[i].Codec == MEDIA_CODEC_AAC)
        {
            // prefixo proprio: "audio-N.m4s" e do HLS, que pode morar na mesma pasta
            s->aud = aud_fmp4_open(s->base, "dash-audio", tracks[i].Extra, tracks[i].ExtraLen,
                                   tracks[i].SampleRate, tracks[i].Channels, s->seg_ms);
            if (s->aud) s->track_kind[i] = 2;
        }
    }

    const char* vc = s->is_av1 ? "V_AV1" : "V_VP9";   // Matroska CodecID (nao "av01", que e o fourcc do ISO-BMFF/DASH)
    for (int v = 0; v < s->vcount; v++)
        s->wm[v] = webm_open_segmented(s->base, s->rname[v], vc, s->rw[v], s->rh[v], s->rfps[v], s->seg_ms,
                                       s->has_audio, s->arate, s->ach, s->has_audio ? s->ohead : 0, s->ohl);
    return s->vcount > 0 ? 0 : -1;
}

static int sdash_write(MediaSink* k, int track, const GwPacket* pkt)
{
    SinkDash* s = (SinkDash*)k->Ctx;
    if (!pkt || pkt->Size <= 0 || track < 0 || track >= DASH_MAXR + 2) return 0;
    int64_t ms = mt_to_ms(pkt->Pts);
    if (ms > s->last_ms) s->last_ms = ms;

    if (s->track_kind[track] == 0)
    {
        int v = s->track_sub[track];
        if (s->wm[v]) webm_write_video(s->wm[v], ms, pkt->KeyFrame, pkt->Data, pkt->Size);
    }
    else if (s->track_kind[track] == 1 && s->has_audio)
    {
        for (int v = 0; v < s->vcount; v++) if (s->wm[v]) webm_write_audio(s->wm[v], ms, pkt->Data, pkt->Size);
    }
    else if (s->track_kind[track] == 2) aud_fmp4_write(s->aud, pkt);
    return 0;
}

static int sdash_cut(MediaSink* k, mtime_us at) { (void)k; (void)at; return 0; }   // WebM auto-segmenta

static void sdash_close(MediaSink* k)
{
    if (!k) return;
    SinkDash* s = (SinkDash*)k->Ctx;
    if (s)
    {
        // Tempos reais dos chunks ANTES de fechar os muxers (o ponteiro vale ate o webm_close).
        for (int v = 0; v < s->vcount; v++)
        {
            const int64_t* t = 0; int n = s->wm[v] ? webm_chunk_times(s->wm[v], &t) : 0, cap = 0;
            int ultimo = s->rfps[v] > 0 ? 1000 / s->rfps[v] : 33;
            for (int i = 0; i < n; i++)
                mpd_seg_add(&s->tl[v], &s->tl_n[v], &cap, t[i], (i + 1 < n) ? t[i + 1] - t[i] : (s->last_ms + ultimo - t[i]));
            if (s->wm[v]) webm_close(s->wm[v]);
        }
        aud_fmp4_finish(s->aud);
        if (!k->Aborted)
        {
            write_mpd(s);
            GwEvent e; GW_ZERO(&e, sizeof(e)); e.Kind = GW_EV_DONE; e.Playlist = mpd_webm_manifest(s->is_av1); gw_emit(s->fb, &e);
            gw_emit_mem(s->fb);
        }
        aud_fmp4_close(s->aud);
        for (int v = 0; v < s->vcount; v++) GW_FREE(s->tl[v]);
        GW_FREE(s);
    }
    GW_FREE(k);
}

MediaSink* sink_dash_open(const MediaProfile* profile, const char* base_dir, const GwFeedback* fb)
{
    SinkDash* s = (SinkDash*)GW_CALLOC(1, sizeof(SinkDash));
    if (!s) return 0;
    s->fb = fb; s->seg_ms = (profile && profile->SegmentMs > 0) ? profile->SegmentMs : 2000;
    snprintf(s->base, sizeof(s->base), "%s", base_dir ? base_dir : ".");

    MediaSink* k = (MediaSink*)GW_CALLOC(1, sizeof(MediaSink));
    if (!k) { GW_FREE(s); return 0; }
    k->Ctx = s; k->Start = sdash_start; k->Write = sdash_write; k->Cut = sdash_cut; k->Close = sdash_close;
    return k;
}
