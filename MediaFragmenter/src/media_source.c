//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  source_mp4: fonte PULL de arquivo. Demux de video (H.264/H.265 Annex-B) via
//  MediaFragmenter + audio AAC (passthrough) via audio_aac, intercalados por pts (us).
//  Alocacoes proprias via memory_pool (GW_*). NAO TESTADO EM RUNTIME.

#include "media_source.h"
#include "memory_pool.h"   // memop_* (evita declaracao implicita -> ponteiro truncado em x64)
#include "MediaFragmenter.h"
#include "audio_aac.h"
#include "mp4_timing.h"
#include <stdio.h>
#include <stdlib.h>   // memop_free_raw() dos buffers de libs legadas (MediaFragmenter/mp4_timing)

typedef struct
{
    // video
    FILE*           fp;
    FrameIndexList* fl;
    VideoMetadata*  meta;
    int             vcodec;          // MEDIA_CODEC_H264/H265
    int64_t*        vpts; uint32_t vpts_n;   // pts por frame, em ms
    uint64_t        vi;              // proximo indice de video
    MediaBuffer     vbuf;            // annexb do frame corrente (buffer da MediaFragmenter)
    int             v_pending, v_key; mtime_us v_pts, v_dur;

    // audio (AAC passthrough)
    void*      actx; AacRawInfo ainfo; int has_audio;
    uint8_t*   abuf; int abuf_cap, a_size, a_pending; mtime_us a_pts, a_dur;

    int    vw, vh; double fps, duration;
    int    stream_count;
}
SrcMp4;

// Detecta keyframe (IDR/IRAP) varrendo os start codes do Annex-B.
static int annexb_is_key(const uint8_t* d, int n, int codec)
{
    int i = 0;
    while (i + 4 <= n)
    {
        if (d[i] == 0 && d[i+1] == 0)
        {
            int sc = (d[i+2] == 1) ? 3 : ((i+3 < n && d[i+2] == 0 && d[i+3] == 1) ? 4 : 0);
            if (sc)
            {
                int h = i + sc;
                if (h < n)
                {
                    int b0 = d[h];
                    if (codec == 265) { int t = (b0 >> 1) & 0x3F; if (t >= 16 && t <= 21) return 1; }
                    else              { int t = b0 & 0x1F;       if (t == 5) return 1; }
                }
                i = h; continue;
            }
        }
        i++;
    }
    return 0;
}

// Garante 1 pacote de video pendente (lookahead de profundidade 1).
static void ensure_video(SrcMp4* s)
{
    if (s->v_pending || !s->fp || s->vi >= s->fl->Count) return;
    s->vbuf.Size = 0;
    h26x_put_single_frame(s->fp, s->fl->Frames[s->vi], s->meta, &s->vbuf);
    double fps = s->fps > 0 ? s->fps : 30;
    int64_t ms   = (s->vpts && s->vi < s->vpts_n)     ? s->vpts[s->vi]     : (int64_t)(s->vi * 1000.0 / fps + 0.5);
    int64_t next = (s->vpts && s->vi + 1 < s->vpts_n) ? s->vpts[s->vi + 1] : ms + (int64_t)(1000.0 / fps + 0.5);
    s->v_pts = mt_from_ms(ms);
    s->v_dur = mt_from_ms(next - ms > 0 ? next - ms : (int64_t)(1000.0 / fps + 0.5));
    s->v_key = annexb_is_key(s->vbuf.Data, s->vbuf.Size, s->vcodec);
    s->v_pending = 1; s->vi++;
}

// Garante 1 pacote de audio pendente (copia para buffer proprio: aac_read_raw reusa o seu).
static void ensure_audio(SrcMp4* s)
{
    if (s->a_pending || !s->has_audio) return;
    const uint8_t* d; int sz, dur; int64_t dts;
    int r = aac_read_raw(s->actx, &d, &sz, &dts, &dur);
    if (r != 1) { s->has_audio = 0; return; }   // fim do audio -> segue so video
    if (sz > s->abuf_cap) { s->abuf = (uint8_t*)GW_REALLOC(s->abuf, sz); s->abuf_cap = sz; }
    GW_COPY(s->abuf, d, sz);
    s->a_size = sz;
    s->a_pts  = mt_from_scale(dts, (int)s->ainfo.timescale);
    s->a_dur  = mt_from_scale(dur, (int)s->ainfo.timescale);
    s->a_pending = 1;
}

static int src_info(MediaSource* src, MediaStreamInfo* out, int max, int* count)
{
    SrcMp4* s = (SrcMp4*)src->Ctx;
    int n = 0;
    if (max > n)
    {
        MediaStreamInfo* v = &out[n++]; GW_ZERO(v, sizeof(*v));
        v->Type = MSTREAM_VIDEO; v->Codec = s->vcodec == 265 ? MEDIA_CODEC_H265 : MEDIA_CODEC_H264;
        v->Width = s->vw; v->Height = s->vh; v->Fps = s->fps;
    }
    if (s->has_audio && max > n)
    {
        MediaStreamInfo* a = &out[n++]; GW_ZERO(a, sizeof(*a));
        a->Type = MSTREAM_AUDIO; a->Codec = MEDIA_CODEC_AAC;
        a->SampleRate = s->ainfo.rate; a->Channels = s->ainfo.channels;
        a->Extra = s->ainfo.asc; a->ExtraLen = s->ainfo.asc_len;
    }
    if (count) *count = n;
    return n;
}

static int src_read(MediaSource* src, GwPacket* pkt)
{
    SrcMp4* s = (SrcMp4*)src->Ctx;
    ensure_video(s); ensure_audio(s);

    int take_audio;
    if (s->v_pending && s->a_pending) take_audio = (s->a_pts <= s->v_pts);
    else if (s->a_pending)            take_audio = 1;
    else if (s->v_pending)            take_audio = 0;
    else                              return 0;   // EOF

    GW_ZERO(pkt, sizeof(*pkt));
    if (take_audio)
    {
        pkt->Stream = 1; pkt->Data = s->abuf; pkt->Size = s->a_size;
        pkt->Pts = pkt->Dts = s->a_pts; pkt->Dur = s->a_dur; pkt->KeyFrame = 1;   // AAC: todo frame e sync
        s->a_pending = 0;
    }
    else
    {
        pkt->Stream = 0; pkt->Data = s->vbuf.Data; pkt->Size = s->vbuf.Size;
        pkt->Pts = pkt->Dts = s->v_pts; pkt->Dur = s->v_dur; pkt->KeyFrame = s->v_key;   // sem B-frames na fase 1: dts=pts
        s->v_pending = 0;
    }
    return 1;
}

static int src_islive(MediaSource* src) { (void)src; return 0; }

// Libera o contexto (usado tambem no caminho de erro do open).
static void src_close_inner(SrcMp4* s)
{
    if (!s) return;
    if (s->actx) aac_close(s->actx);
    if (s->vbuf.Data) GW_FREE(s->vbuf.Data);    // mbuffer da MediaFragmenter agora aloca via memop (nao memop_free_raw() do CRT)
    if (s->fl) mframe_list_release(&s->fl);
    if (s->fp) fclose(s->fp);
    if (s->vpts) memop_free_raw(s->vpts);                  // buffer do mp4_timing (lib legada)
    GW_FREE(s->abuf);
    GW_FREE(s);
}

static void src_close(MediaSource* src)
{
    if (!src) return;
    src_close_inner((SrcMp4*)src->Ctx);
    GW_FREE(src);
}

MediaSource* source_mp4_open(const char* path)
{
    FrameIndexList* fl = mp4builder_get_frames(path);
    if (!fl || fl->Count == 0) { if (fl) mframe_list_release(&fl); return 0; }

    SrcMp4* s = (SrcMp4*)GW_CALLOC(1, sizeof(SrcMp4));
    if (!s) { mframe_list_release(&fl); return 0; }
    s->fl = fl; s->meta = &fl->Metadata; s->vcodec = fl->Metadata.Codec;
    s->vw = fl->Metadata.Width; s->vh = fl->Metadata.Height;
    s->fps = fl->Metadata.Fps > 0.0 ? fl->Metadata.Fps : 30.0;

    mp4_video_pts_ms(path, &s->vpts, &s->vpts_n);
    mp4_video_info(path, &s->vw, &s->vh, 0, 0, &s->duration);
    mbuffer_init(&s->vbuf);
    if (fopen_s(&s->fp, path, "rb") != 0 || !s->fp)
    {
        if (s->vpts) memop_free_raw(s->vpts);
        mframe_list_release(&fl); GW_FREE(s); return 0;
    }

    // audio (opcional): sem AAC na fonte -> segue video-only
    if (aac_open_raw(path, &s->ainfo, &s->actx) == 0 && s->actx) s->has_audio = 1;

    MediaSource* src = (MediaSource*)GW_CALLOC(1, sizeof(MediaSource));
    if (!src) { src_close_inner(s); return 0; }
    src->Ctx = s;
    src->Info = src_info; src->Read = src_read; src->IsLive = src_islive; src->Close = src_close;
    return src;
}

// Sniffer: cru vs codificado + codec, por 1 buffer de entrada.
MediaCodec gw_sniff_stream(const uint8_t* d, int n, int* is_raw)
{
    if (is_raw) *is_raw = 0;
    if (!d || n < 5) { if (is_raw) *is_raw = 1; return MEDIA_CODEC_NONE; }

    int found = -1;                                   // procura start code Annex-B nos 1os bytes
    for (int i = 0; i + 3 <= n && i < 64; i++)
        if (d[i] == 0 && d[i+1] == 0 && (d[i+2] == 1 || (i+3 < n && d[i+2] == 0 && d[i+3] == 1)))
        { found = i + (d[i+2] == 1 ? 3 : 4); break; }

    if (found >= 0 && found < n)
    {
        int b0 = d[found];
        int t265 = (b0 >> 1) & 0x3F;
        if (t265 == 32 || t265 == 33 || t265 == 34 || (t265 >= 16 && t265 <= 21)) return MEDIA_CODEC_H265;
        int t264 = b0 & 0x1F;
        if (t264 == 7 || t264 == 8 || t264 == 5 || t264 == 1) return MEDIA_CODEC_H264;
        return MEDIA_CODEC_H264;                       // start code presente, tipo ambiguo -> assume H.264
    }
    if (n >= 2 && d[0] == 0xFF && d[1] == 0xD8) return MEDIA_CODEC_NONE;   // MJPEG (sem decoder na matriz)
    if (is_raw) *is_raw = 1;
    return MEDIA_CODEC_NONE;                            // frames crus
}

// source_camera_open vive em MediaGateway/media/source_camera.c (MSMF + V4L2). Nao
// depende do demux MP4, entao nao ha motivo para estar nesta camada.
