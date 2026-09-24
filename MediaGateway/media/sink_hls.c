//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  sink_hls: saida HLS multi-resolucao (fMP4). Cada pista de video vira uma rendition
//  (init-<name>.mp4 + <name>-N.m4s + <name>.m3u8); o audio AAC e' UMA rendition
//  compartilhada (grupo AUDIO). Cut() fecha o segmento corrente em todas as pistas.
//  Video H.264 (avc1). Alocacoes via memory_pool. NAO TESTADO EM RUNTIME.

#include "media_sink.h"
#include "mux_mp4.h"
#include "h26x_util.h"
#include <stdio.h>

#define HLS_MAX_V 8

typedef struct { uint8_t* data; int size; int64_t ms; int key; } VSample;
typedef struct { uint8_t* data; int size; int dur_units; int64_t dts_units; } ASample;

typedef struct
{
    char name[32]; int w, h, bw, fps;
    H26xToLen conv; int init_written, seg_index;
    VSample* seg; int seg_n, seg_cap; int64_t seg_start_ms;
    FILE* pl;
}
VTrack;

typedef struct
{
    int have, rate, ch, asc_len; uint8_t asc[64];
    int seg_index;
    ASample* seg; int seg_n, seg_cap; int64_t seg_base_units, seg_dur_units;
    FILE* pl;
}
ATrack;

typedef struct
{
    char  base[1024];
    int   frag_ms;
    VTrack v[HLS_MAX_V]; int vcount;
    ATrack a;
    int   track_kind[HLS_MAX_V + 2];   // 0=video,1=audio,-1
    int   track_sub[HLS_MAX_V + 2];    // indice em v[] (video)
    const GwFeedback* fb;
}
SinkHls;

static void flush_v(SinkHls* s, VTrack* t)
{
    if (t->seg_n <= 0) return;
    int n = t->seg_n, last = t->fps > 0 ? (1000 / t->fps) : 33;
    Mp4Sample* ms = (Mp4Sample*)GW_ALLOC((size_t)n * sizeof(Mp4Sample));
    double dur_s = 0;
    for (int i = 0; i < n; i++)
    {
        int d = (i + 1 < n) ? (int)(t->seg[i+1].ms - t->seg[i].ms) : last; if (d <= 0) d = last;
        ms[i].data = t->seg[i].data; ms[i].size = t->seg[i].size; ms[i].duration_ms = d; ms[i].keyframe = t->seg[i].key;
        dur_s += d / 1000.0;
    }
    char p[1200]; snprintf(p, sizeof(p), "%s/%s-%d.m4s", s->base, t->name, t->seg_index);
    mp4_write_segment(p, (uint32_t)(t->seg_index + 1), t->seg[0].ms, ms, n);
    if (t->pl) fprintf(t->pl, "#EXTINF:%.3f,\n%s-%d.m4s\n", dur_s, t->name, t->seg_index);
    for (int i = 0; i < n; i++) GW_FREE(t->seg[i].data);
    GW_FREE(ms);
    t->seg_n = 0; t->seg_index++;
}

static void flush_a(SinkHls* s, ATrack* a)
{
    if (a->seg_n <= 0) return;
    int n = a->seg_n;
    Mp4Sample* ms = (Mp4Sample*)GW_ALLOC((size_t)n * sizeof(Mp4Sample));
    for (int i = 0; i < n; i++)
    { ms[i].data = a->seg[i].data; ms[i].size = a->seg[i].size; ms[i].duration_ms = a->seg[i].dur_units; ms[i].keyframe = 1; }
    char p[1200]; snprintf(p, sizeof(p), "%s/audio-%d.m4s", s->base, a->seg_index);
    mp4_write_segment(p, (uint32_t)(a->seg_index + 1), a->seg_base_units, ms, n);
    if (a->pl) fprintf(a->pl, "#EXTINF:%.3f,\naudio-%d.m4s\n", (double)a->seg_dur_units / (a->rate > 0 ? a->rate : 48000), a->seg_index);
    for (int i = 0; i < n; i++) GW_FREE(a->seg[i].data);
    GW_FREE(ms);
    a->seg_n = 0; a->seg_dur_units = 0; a->seg_index++;
}

static int shls_start(MediaSink* k, const MediaTrackOut* tracks, int count)
{
    SinkHls* s = (SinkHls*)k->Ctx;
    int td = s->frag_ms / 1000 + 1;
    for (int i = 0; i < count && i < HLS_MAX_V + 2; i++)
    {
        s->track_kind[i] = -1;
        if (tracks[i].Type == MSTREAM_VIDEO && s->vcount < HLS_MAX_V)
        {
            VTrack* t = &s->v[s->vcount];
            snprintf(t->name, sizeof(t->name), "%s", tracks[i].Name ? tracks[i].Name : "v");
            t->w = tracks[i].Width; t->h = tracks[i].Height; t->bw = tracks[i].Bandwidth; t->fps = (int)(tracks[i].Fps + 0.5);
            h26x_tl_init(&t->conv, tracks[i].Codec == MEDIA_CODEC_H265);   // (HLS assume avc1; ver nota)
            char pp[1200]; snprintf(pp, sizeof(pp), "%s/%s.m3u8", s->base, t->name); fopen_s(&t->pl, pp, "wb");
            if (t->pl) fprintf(t->pl, "#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-PLAYLIST-TYPE:VOD\n"
                                      "#EXT-X-TARGETDURATION:%d\n#EXT-X-MEDIA-SEQUENCE:0\n#EXT-X-MAP:URI=\"init-%s.mp4\"\n", td, t->name);
            s->track_kind[i] = 0; s->track_sub[i] = s->vcount; s->vcount++;
        }
        else if (tracks[i].Type == MSTREAM_AUDIO && !s->a.have && tracks[i].Codec == MEDIA_CODEC_AAC)
        {
            s->a.have = 1; s->a.rate = tracks[i].SampleRate; s->a.ch = tracks[i].Channels;
            s->a.asc_len = tracks[i].ExtraLen < 64 ? tracks[i].ExtraLen : 64;
            if (tracks[i].Extra && s->a.asc_len > 0) GW_COPY(s->a.asc, tracks[i].Extra, s->a.asc_len);
            char ip[1200]; snprintf(ip, sizeof(ip), "%s/init-audio.mp4", s->base);
            mp4_write_init_audio(ip, s->a.asc, s->a.asc_len, s->a.rate, s->a.ch);
            char pp[1200]; snprintf(pp, sizeof(pp), "%s/audio.m3u8", s->base); fopen_s(&s->a.pl, pp, "wb");
            if (s->a.pl) fprintf(s->a.pl, "#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-PLAYLIST-TYPE:VOD\n"
                                          "#EXT-X-TARGETDURATION:%d\n#EXT-X-MEDIA-SEQUENCE:0\n#EXT-X-MAP:URI=\"init-audio.mp4\"\n", td);
            s->track_kind[i] = 1;
        }
    }
    return s->vcount > 0 ? 0 : -1;
}

static int shls_write(MediaSink* k, int track, const GwPacket* pkt)
{
    SinkHls* s = (SinkHls*)k->Ctx;
    if (!pkt || pkt->Size <= 0 || track < 0 || track >= HLS_MAX_V + 2) return 0;

    if (s->track_kind[track] == 0)
    {
        VTrack* t = &s->v[s->track_sub[track]];
        uint8_t* out; int alen = h26x_tl_feed(&t->conv, pkt->Data, pkt->Size, &out);
        if (!t->init_written && h26x_tl_ready(&t->conv))
        { char ip[1200]; snprintf(ip, sizeof(ip), "%s/init-%s.mp4", s->base, t->name); mp4_write_init(ip, t->w, t->h, t->conv.cfg, t->conv.cfg_len); t->init_written = 1; }
        if (!t->init_written || alen <= 0) return 0;
        int64_t ms = mt_to_ms(pkt->Pts);
        if (t->seg_n == 0) t->seg_start_ms = ms;
        if (t->seg_n == t->seg_cap) { t->seg_cap = t->seg_cap ? t->seg_cap * 2 : 64; t->seg = (VSample*)GW_REALLOC(t->seg, (size_t)t->seg_cap * sizeof(VSample)); }
        uint8_t* cp = (uint8_t*)GW_ALLOC(alen); GW_COPY(cp, out, alen);
        t->seg[t->seg_n].data = cp; t->seg[t->seg_n].size = alen; t->seg[t->seg_n].ms = ms; t->seg[t->seg_n].key = pkt->KeyFrame; t->seg_n++;
    }
    else if (s->track_kind[track] == 1 && s->a.have)
    {
        ATrack* a = &s->a;
        int dur = pkt->Dur > 0 ? (int)mt_to_scale(pkt->Dur, a->rate) : 1024;
        int64_t dts = mt_to_scale(pkt->Pts, a->rate);
        if (a->seg_n == 0) a->seg_base_units = dts;
        if (a->seg_n == a->seg_cap) { a->seg_cap = a->seg_cap ? a->seg_cap * 2 : 128; a->seg = (ASample*)GW_REALLOC(a->seg, (size_t)a->seg_cap * sizeof(ASample)); }
        uint8_t* cp = (uint8_t*)GW_ALLOC(pkt->Size); GW_COPY(cp, pkt->Data, pkt->Size);
        a->seg[a->seg_n].data = cp; a->seg[a->seg_n].size = pkt->Size; a->seg[a->seg_n].dur_units = dur; a->seg[a->seg_n].dts_units = dts;
        a->seg_n++; a->seg_dur_units += dur;
    }
    return 0;
}

static int shls_cut(MediaSink* k, mtime_us at)
{
    (void)at; SinkHls* s = (SinkHls*)k->Ctx;
    for (int i = 0; i < s->vcount; i++) flush_v(s, &s->v[i]);
    if (s->a.have) flush_a(s, &s->a);
    return 0;
}

static void shls_close(MediaSink* k)
{
    if (!k) return;
    SinkHls* s = (SinkHls*)k->Ctx;
    if (s)
    {
        if (!k->Aborted) shls_cut(k, 0);   // ultimo segmento

        for (int i = 0; i < s->vcount; i++) if (s->v[i].pl) { if (!k->Aborted) fprintf(s->v[i].pl, "#EXT-X-ENDLIST\n"); fclose(s->v[i].pl); }
        if (s->a.pl) { if (!k->Aborted) fprintf(s->a.pl, "#EXT-X-ENDLIST\n"); fclose(s->a.pl); }

        char mp[1200]; snprintf(mp, sizeof(mp), "%s/master.m3u8", s->base);
        FILE* m = 0; if (!k->Aborted) fopen_s(&m, mp, "wb");
        if (m)
        {
            fprintf(m, "#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-INDEPENDENT-SEGMENTS\n");
            if (s->a.have) fprintf(m, "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",NAME=\"audio\",DEFAULT=YES,AUTOSELECT=YES,URI=\"audio.m3u8\"\n");
            const char* codecs = s->a.have ? "avc1.640028,mp4a.40.2" : "avc1.640028";
            for (int i = 0; i < s->vcount; i++)
                fprintf(m, "#EXT-X-STREAM-INF:BANDWIDTH=%d,RESOLUTION=%dx%d,CODECS=\"%s\"%s\n%s.m3u8\n",
                        s->v[i].bw > 0 ? s->v[i].bw : 2000000, s->v[i].w, s->v[i].h, codecs,
                        s->a.have ? ",AUDIO=\"aud\"" : "", s->v[i].name);
            fclose(m);
            GwEvent e; GW_ZERO(&e, sizeof(e)); e.Kind = GW_EV_DONE; e.Playlist = "master.m3u8"; gw_emit(s->fb, &e);
            gw_emit_mem(s->fb);
        }

        for (int i = 0; i < s->vcount; i++) { for (int j = 0; j < s->v[i].seg_n; j++) GW_FREE(s->v[i].seg[j].data); GW_FREE(s->v[i].seg); h26x_tl_free(&s->v[i].conv); }
        for (int j = 0; j < s->a.seg_n; j++) GW_FREE(s->a.seg[j].data); GW_FREE(s->a.seg);
        GW_FREE(s);
    }
    GW_FREE(k);
}

MediaSink* sink_hls_open(const MediaProfile* profile, const char* base_dir, const GwFeedback* fb)
{
    SinkHls* s = (SinkHls*)GW_CALLOC(1, sizeof(SinkHls));
    if (!s) return 0;
    s->fb = fb; s->frag_ms = (profile && profile->SegmentMs > 0) ? profile->SegmentMs : 2000;
    snprintf(s->base, sizeof(s->base), "%s", base_dir ? base_dir : ".");

    MediaSink* k = (MediaSink*)GW_CALLOC(1, sizeof(MediaSink));
    if (!k) { GW_FREE(s); return 0; }
    k->Ctx = s; k->Start = shls_start; k->Write = shls_write; k->Cut = shls_cut; k->Close = shls_close;
    return k;
}
