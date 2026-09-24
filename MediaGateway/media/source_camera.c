//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  source_camera: MediaSource de captura ao vivo. Media Foundation (IMFSourceReader) no
//  Windows, V4L2 (mmap) no Linux.
//
//  Decisao central: a fonte NORMALIZA todo formato cru para I420 antes de entregar.
//  O gateway ja sabe consumir I420 contiguo (e o que o decode de arquivo produz); fazer a
//  camera falar a mesma lingua evita espalhar NV12/YUY2/RGB24 por decode, escala e encode.
//  A conversao usa libyuv (vetorizada), nao laco proprio.
//
//  Streams CODIFICADOS (H.264/H.265 de camera) passam direto, sem tocar nos bytes: o
//  gateway decide entre decodificar ou fazer passthrough.
//
//  Ao vivo: Read() BLOQUEIA ate chegar um frame. Quem encerra e o GwControl do gateway.

#include "media_source.h"
#include "device_enum.h"
#include "gw_base.h"
#include <string.h>
#include <stdio.h>

#ifdef HAVE_LIBYUV
#include "libyuv/convert.h"
#endif

#define CAM_FOURCC(a,b,c,d) ((unsigned int)(a) | ((unsigned int)(b) << 8) | ((unsigned int)(c) << 16) | ((unsigned int)(d) << 24))

// Estado comum aos dois backends.
typedef struct
{
    int        width, height;
    double     fps;
    int        raw;                 // 1 = entrega I420; 0 = bitstream do proprio codec
    MediaCodec codec;               // valido quando raw == 0
    unsigned int fourcc;            // formato NATIVO do device (antes da normalizacao)

    uint8_t*   i420;  int i420_cap;  // destino da conversao (Y+U+V contiguos)
    uint8_t*   enc;   int enc_cap;   // copia do bitstream quando raw == 0
    mtime_us   pts;                  // relogio da captura
    mtime_us   frame_dur;
    int        started;

    void*      backend;              // MF: CamMf* | V4L2: CamV4l2*
}
CamCtx;

// Tamanho de um I420 contiguo.
static int i420_size(int w, int h)
{
    int cw = (w + 1) / 2, ch = (h + 1) / 2;
    return w * h + 2 * (cw * ch);
}

static int cam_ensure_i420(CamCtx* c)
{
    int need = i420_size(c->width, c->height);
    if (c->i420 && c->i420_cap >= need) return 1;
    GW_FREE(c->i420);
    c->i420 = (uint8_t*)GW_ALLOC(need);
    c->i420_cap = c->i420 ? need : 0;
    return c->i420 != 0;
}

// Converte um frame nativo para I420 no buffer da fonte. 1 = ok.
// 'stride' 0 = compacto (a maioria dos drivers entrega assim quando o buffer e mmap/MF).
static int cam_to_i420(CamCtx* c, const uint8_t* src, int src_size, int stride)
{
    if (!cam_ensure_i420(c)) return 0;

    const int w = c->width, h = c->height;
    const int cw = (w + 1) / 2, ch = (h + 1) / 2;
    uint8_t* dy = c->i420;
    uint8_t* du = dy + (size_t)w * h;
    uint8_t* dv = du + (size_t)cw * ch;

#ifdef HAVE_LIBYUV
    switch (c->fourcc)
    {
        case CAM_FOURCC('N','V','1','2'):
        {
            int sy = stride > 0 ? stride : w;
            if (src_size < sy * h + sy * ch) return 0;
            return NV12ToI420(src, sy, src + (size_t)sy * h, sy,
                              dy, w, du, cw, dv, cw, w, h) == 0;
        }
        case CAM_FOURCC('Y','U','Y','2'):
        case CAM_FOURCC('Y','U','Y','V'):
        {
            int sy = stride > 0 ? stride : w * 2;
            if (src_size < sy * h) return 0;
            return YUY2ToI420(src, sy, dy, w, du, cw, dv, cw, w, h) == 0;
        }
        case CAM_FOURCC('R','G','B','3'):
        {
            int sy = stride > 0 ? stride : w * 3;
            if (src_size < sy * h) return 0;
            return RGB24ToI420(src, sy, dy, w, du, cw, dv, cw, w, h) == 0;
        }
        case CAM_FOURCC('I','4','2','0'):
        case CAM_FOURCC('I','Y','U','V'):
        case CAM_FOURCC('Y','U','1','2'):
        {
            int sy = stride > 0 ? stride : w;
            if (src_size < i420_size(w, h)) return 0;
            return I420Copy(src, sy, src + (size_t)sy * h, (sy + 1) / 2,
                            src + (size_t)sy * h + (size_t)((sy + 1) / 2) * ch, (sy + 1) / 2,
                            dy, w, du, cw, dv, cw, w, h) == 0;
        }
        default:
            return 0;   // formato nao suportado: device_map_fourcc marca Supported=0
    }
#else
    (void)src; (void)src_size; (void)stride;
    return 0;   // sem libyuv nao ha conversao
#endif
}

// ============================================================================
#ifdef _WIN32
// ============================================================================

#define COBJMACROS
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

typedef struct
{
    IMFSourceReader* reader;
    IMFMediaBuffer*  locked;     // buffer da amostra corrente (destravado no proximo Read)
    IMFSample*       sample;
    int              mf_started;
    int              co_started;
}
CamMf;

static int mf_subtype_to_fourcc(const GUID* g, unsigned int* out)
{
    static const unsigned char tail[8] = { 0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71 };
    if (g->Data2 != 0x0000 || g->Data3 != 0x0010) return 0;
    if (memcmp(g->Data4, tail, 8) != 0) return 0;
    *out = (unsigned int)g->Data1;
    return 1;
}

static int mf_get_pair(IMFMediaType* mt, const GUID* key, UINT32* hi, UINT32* lo)
{
    UINT64 v = 0;
    if (FAILED(IMFMediaType_GetUINT64(mt, key, &v))) return 0;
    *hi = (UINT32)(v >> 32); *lo = (UINT32)(v & 0xFFFFFFFFull);
    return 1;
}

// Solta a amostra anterior. O IMFMediaBuffer fica travado enquanto o gateway usa os bytes,
// entao o destravamento acontece no inicio da proxima leitura -- nao no fim da anterior.
static void mf_release_current(CamMf* m)
{
    if (m->locked) { IMFMediaBuffer_Unlock(m->locked); IMFMediaBuffer_Release(m->locked); m->locked = 0; }
    if (m->sample) { IMFSample_Release(m->sample); m->sample = 0; }
}

// Escolhe, entre os tipos nativos do device, o que casa com o pedido (formato/resolucao).
// Retorna o IMFMediaType escolhido (o chamador libera) ou NULL.
static IMFMediaType* mf_pick_type(IMFSourceReader* rd, const CameraParams* p, unsigned int* out_fourcc)
{
    IMFMediaType* best = 0;
    unsigned int  best_fcc = 0;
    int           best_score = -1;

    for (DWORD i = 0; ; i++)
    {
        IMFMediaType* mt = 0;
        if (FAILED(IMFSourceReader_GetNativeMediaType(rd, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &mt)) || !mt)
            break;

        GUID sub; unsigned int fcc = 0; UINT32 w = 0, h = 0, num = 0, den = 0;
        if (SUCCEEDED(IMFMediaType_GetGUID(mt, &MF_MT_SUBTYPE, &sub)) &&
            mf_subtype_to_fourcc(&sub, &fcc) &&
            mf_get_pair(mt, &MF_MT_FRAME_SIZE, &w, &h) && w > 0 && h > 0)
        {
            DevStream probe; memset(&probe, 0, sizeof(probe));
            device_map_fourcc(fcc, &probe);

            if (probe.Supported)
            {
                // Pontuacao: casar resolucao vale mais que casar formato; entre iguais,
                // prefere o fps mais alto. Sem pedido, fica o de maior area.
                int score = 0;
                if (p->Width > 0 && p->Height > 0)
                    score += (w == (UINT32)p->Width && h == (UINT32)p->Height) ? 1000 : 0;
                else
                    score += (int)((w * h) / 10000);

                if (p->PixFmt != GW_PIX_NONE && probe.Raw && probe.PixFmt == p->PixFmt) score += 100;
                if (p->Codec  != MEDIA_CODEC_NONE && !probe.Raw && probe.Codec == p->Codec) score += 100;
                if (p->Mode == SRC_RAW     && probe.Raw)  score += 50;
                if (p->Mode == SRC_ENCODED && !probe.Raw) score += 50;

                if (mf_get_pair(mt, &MF_MT_FRAME_RATE, &num, &den) && den)
                {
                    double f = (double)num / (double)den;
                    if (p->Fps > 0.0) score += (f > p->Fps - 0.5 && f < p->Fps + 0.5) ? 30 : 0;
                    else              score += (int)(f / 10.0);
                }

                if (score > best_score)
                {
                    if (best) IMFMediaType_Release(best);
                    best = mt; best_fcc = fcc; best_score = score;
                    IMFMediaType_AddRef(best);
                }
            }
        }
        IMFMediaType_Release(mt);
    }

    if (best && out_fourcc) *out_fourcc = best_fcc;
    return best;
}

static void cam_close_win(CamCtx* c)
{
    CamMf* m = (CamMf*)c->backend;
    if (!m) return;
    mf_release_current(m);
    if (m->reader) IMFSourceReader_Release(m->reader);
    if (m->mf_started) MFShutdown();
    if (m->co_started) CoUninitialize();
    GW_FREE(m);
    c->backend = 0;
}

static int cam_open_win(CamCtx* c, const CameraParams* p)
{
    CamMf* m = (CamMf*)GW_CALLOC(1, sizeof(CamMf));
    if (!m) return 0;
    c->backend = m;

    m->co_started = SUCCEEDED(CoInitializeEx(0, COINIT_MULTITHREADED));
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return 0;
    m->mf_started = 1;

    // Abre pelo caminho simbolico que device_enum devolveu (id do dispositivo).
    IMFAttributes* attr = 0;
    if (FAILED(MFCreateAttributes(&attr, 2))) return 0;

    WCHAR wlink[512];
    MultiByteToWideChar(CP_UTF8, 0, p->Device ? p->Device : "", -1, wlink, 512);

    int ok = SUCCEEDED(IMFAttributes_SetGUID(attr, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                                             &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID)) &&
             SUCCEEDED(IMFAttributes_SetString(attr, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, wlink));

    IMFMediaSource* src = 0;
    if (ok) ok = SUCCEEDED(MFCreateDeviceSource(attr, &src)) && src != 0;
    IMFAttributes_Release(attr);
    if (!ok) return 0;

    ok = SUCCEEDED(MFCreateSourceReaderFromMediaSource(src, 0, &m->reader)) && m->reader != 0;
    IMFMediaSource_Release(src);
    if (!ok) return 0;

    unsigned int fcc = 0;
    IMFMediaType* chosen = mf_pick_type(m->reader, p, &fcc);
    if (!chosen) return 0;

    UINT32 w = 0, h = 0, num = 0, den = 0;
    mf_get_pair(chosen, &MF_MT_FRAME_SIZE, &w, &h);
    ok = SUCCEEDED(IMFSourceReader_SetCurrentMediaType(m->reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, chosen));
    if (ok && mf_get_pair(chosen, &MF_MT_FRAME_RATE, &num, &den) && den) c->fps = (double)num / (double)den;
    IMFMediaType_Release(chosen);
    if (!ok) return 0;

    c->width = (int)w; c->height = (int)h; c->fourcc = fcc;

    DevStream probe; memset(&probe, 0, sizeof(probe));
    device_map_fourcc(fcc, &probe);
    c->raw = probe.Raw; c->codec = probe.Codec;

    if (c->fps <= 0.0) c->fps = 30.0;
    c->frame_dur = (mtime_us)(1000000.0 / c->fps + 0.5);
    return 1;
}

// 1 = frame entregue, 0 = fim, <0 = erro.
static int cam_read_win(CamCtx* c, GwPacket* pkt)
{
    CamMf* m = (CamMf*)c->backend;
    if (!m || !m->reader) return -1;

    mf_release_current(m);

    for (;;)
    {
        DWORD stream = 0, flags = 0;
        LONGLONG ts = 0;
        IMFSample* sample = 0;

        HRESULT hr = IMFSourceReader_ReadSample(m->reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                                0, &stream, &flags, &ts, &sample);
        if (FAILED(hr)) return -1;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { if (sample) IMFSample_Release(sample); return 0; }
        if (!sample) continue;   // gap/format change: o reader devolve amostra nula

        IMFMediaBuffer* buf = 0;
        if (FAILED(IMFSample_ConvertToContiguousBuffer(sample, &buf)) || !buf)
        { IMFSample_Release(sample); continue; }

        BYTE* data = 0; DWORD cur = 0;
        if (FAILED(IMFMediaBuffer_Lock(buf, &data, 0, &cur)))
        { IMFMediaBuffer_Release(buf); IMFSample_Release(sample); continue; }

        // MF conta em unidades de 100ns.
        mtime_us pts = (mtime_us)(ts / 10);
        if (pts <= 0) pts = c->pts;

        GW_ZERO(pkt, sizeof(*pkt));
        pkt->Stream = 0;
        pkt->Pts = pkt->Dts = pts;
        pkt->Dur = c->frame_dur;

        if (c->raw)
        {
            int ok = cam_to_i420(c, data, (int)cur, 0);
            IMFMediaBuffer_Unlock(buf);
            IMFMediaBuffer_Release(buf);
            IMFSample_Release(sample);
            if (!ok) return -1;

            pkt->Data = c->i420;
            pkt->Size = i420_size(c->width, c->height);
            pkt->KeyFrame = 1;
        }
        else
        {
            // Bitstream: o buffer fica TRAVADO ate a proxima leitura, que e exatamente o
            // contrato do MediaSource ("valido ate a proxima chamada do produtor").
            m->locked = buf; m->sample = sample;
            pkt->Data = (uint8_t*)data;
            pkt->Size = (int)cur;
            pkt->KeyFrame = 0;   // o gateway/parser identifica IDR no bitstream
        }

        c->pts = pts + c->frame_dur;
        return 1;
    }
}

// ============================================================================
#else   // ------------------------------- Linux (V4L2) -----------------------
// ============================================================================

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <linux/videodev2.h>

#define CAM_V4L2_BUFFERS 4

typedef struct { void* start; size_t length; } V4l2Buf;

typedef struct
{
    int      fd;
    V4l2Buf  bufs[CAM_V4L2_BUFFERS];
    int      nbufs;
    int      streaming;
    int      queued;          // indice devolvido ao driver na proxima leitura
    int      has_queued;
}
CamV4l2;

static void cam_close_linux(CamCtx* c)
{
    CamV4l2* v = (CamV4l2*)c->backend;
    if (!v) return;

    if (v->streaming)
    {
        enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(v->fd, VIDIOC_STREAMOFF, &t);
    }
    for (int i = 0; i < v->nbufs; i++)
        if (v->bufs[i].start && v->bufs[i].start != MAP_FAILED) munmap(v->bufs[i].start, v->bufs[i].length);
    if (v->fd >= 0) close(v->fd);
    GW_FREE(v);
    c->backend = 0;
}

static int cam_open_linux(CamCtx* c, const CameraParams* p)
{
    CamV4l2* v = (CamV4l2*)GW_CALLOC(1, sizeof(CamV4l2));
    if (!v) return 0;
    v->fd = -1;
    c->backend = v;

    v->fd = open(p->Device ? p->Device : "/dev/video0", O_RDWR);
    if (v->fd < 0) return 0;

    // Formato: usa o pedido quando ha um; senao o primeiro que o gateway sabe consumir.
    unsigned int want = 0;
    if (p->PixFmt == GW_PIX_NV12)  want = CAM_FOURCC('N','V','1','2');
    if (p->PixFmt == GW_PIX_YUYV)  want = CAM_FOURCC('Y','U','Y','V');
    if (p->PixFmt == GW_PIX_I420)  want = CAM_FOURCC('Y','U','1','2');
    if (p->PixFmt == GW_PIX_RGB24) want = CAM_FOURCC('R','G','B','3');
    if (p->Codec  == MEDIA_CODEC_H264) want = CAM_FOURCC('H','2','6','4');

    if (!want)
    {
        struct v4l2_fmtdesc fmt;
        for (unsigned int i = 0; ; i++)
        {
            memset(&fmt, 0, sizeof(fmt));
            fmt.index = i; fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            if (ioctl(v->fd, VIDIOC_ENUM_FMT, &fmt) != 0) break;
            DevStream probe; memset(&probe, 0, sizeof(probe));
            device_map_fourcc(fmt.pixelformat, &probe);
            if (probe.Supported) { want = fmt.pixelformat; break; }
        }
    }
    if (!want) return 0;

    struct v4l2_format f;
    memset(&f, 0, sizeof(f));
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    f.fmt.pix.width  = p->Width  > 0 ? (unsigned int)p->Width  : 1280;
    f.fmt.pix.height = p->Height > 0 ? (unsigned int)p->Height : 720;
    f.fmt.pix.pixelformat = want;
    f.fmt.pix.field = V4L2_FIELD_NONE;
    if (ioctl(v->fd, VIDIOC_S_FMT, &f) != 0) return 0;

    // O driver pode ter ajustado o pedido; o que vale e o que ele devolveu.
    c->width  = (int)f.fmt.pix.width;
    c->height = (int)f.fmt.pix.height;
    c->fourcc = f.fmt.pix.pixelformat;

    DevStream probe; memset(&probe, 0, sizeof(probe));
    device_map_fourcc(c->fourcc, &probe);
    c->raw = probe.Raw; c->codec = probe.Codec;

    if (p->Fps > 0.0)
    {
        struct v4l2_streamparm sp;
        memset(&sp, 0, sizeof(sp));
        sp.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        sp.parm.capture.timeperframe.numerator   = 1000;
        sp.parm.capture.timeperframe.denominator = (unsigned int)(p->Fps * 1000.0 + 0.5);
        ioctl(v->fd, VIDIOC_S_PARM, &sp);
        if (sp.parm.capture.timeperframe.numerator)
            c->fps = (double)sp.parm.capture.timeperframe.denominator / (double)sp.parm.capture.timeperframe.numerator;
    }
    if (c->fps <= 0.0) c->fps = 30.0;
    c->frame_dur = (mtime_us)(1000000.0 / c->fps + 0.5);

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = CAM_V4L2_BUFFERS;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(v->fd, VIDIOC_REQBUFS, &req) != 0 || req.count < 2) return 0;

    for (unsigned int i = 0; i < req.count && i < CAM_V4L2_BUFFERS; i++)
    {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; b.memory = V4L2_MEMORY_MMAP; b.index = i;
        if (ioctl(v->fd, VIDIOC_QUERYBUF, &b) != 0) return 0;

        v->bufs[i].length = b.length;
        v->bufs[i].start  = mmap(0, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, v->fd, b.m.offset);
        if (v->bufs[i].start == MAP_FAILED) return 0;
        v->nbufs++;

        if (ioctl(v->fd, VIDIOC_QBUF, &b) != 0) return 0;
    }

    enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(v->fd, VIDIOC_STREAMON, &t) != 0) return 0;
    v->streaming = 1;
    return 1;
}

static int cam_read_linux(CamCtx* c, GwPacket* pkt)
{
    CamV4l2* v = (CamV4l2*)c->backend;
    if (!v || v->fd < 0) return -1;

    // Devolve ao driver o buffer entregue na leitura anterior.
    if (v->has_queued)
    {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; b.memory = V4L2_MEMORY_MMAP; b.index = (unsigned int)v->queued;
        ioctl(v->fd, VIDIOC_QBUF, &b);
        v->has_queued = 0;
    }

    for (;;)
    {
        fd_set fds; FD_ZERO(&fds); FD_SET(v->fd, &fds);
        struct timeval tv; tv.tv_sec = 2; tv.tv_usec = 0;

        int r = select(v->fd + 1, &fds, 0, 0, &tv);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) continue;   // timeout: camera muda so quando ha luz/movimento; segue esperando

        struct v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; b.memory = V4L2_MEMORY_MMAP;
        if (ioctl(v->fd, VIDIOC_DQBUF, &b) != 0)
        {
            if (errno == EAGAIN || errno == EINTR) continue;
            return -1;
        }

        mtime_us pts = (mtime_us)b.timestamp.tv_sec * 1000000 + b.timestamp.tv_usec;
        if (pts <= 0) pts = c->pts;

        GW_ZERO(pkt, sizeof(*pkt));
        pkt->Stream = 0;
        pkt->Pts = pkt->Dts = pts;
        pkt->Dur = c->frame_dur;

        if (c->raw)
        {
            int ok = cam_to_i420(c, (const uint8_t*)v->bufs[b.index].start, (int)b.bytesused, 0);
            // Buffer ja pode voltar ao driver: os bytes foram convertidos para o nosso.
            ioctl(v->fd, VIDIOC_QBUF, &b);
            if (!ok) return -1;

            pkt->Data = c->i420;
            pkt->Size = i420_size(c->width, c->height);
            pkt->KeyFrame = 1;
        }
        else
        {
            // Bitstream: entrega o mmap direto e so devolve na proxima leitura.
            v->queued = (int)b.index; v->has_queued = 1;
            pkt->Data = (uint8_t*)v->bufs[b.index].start;
            pkt->Size = (int)b.bytesused;
            pkt->KeyFrame = 0;
        }

        c->pts = pts + c->frame_dur;
        return 1;
    }
}

#endif

// ============================================================================
//  MediaSource
// ============================================================================

static int cam_info(MediaSource* s, MediaStreamInfo* out, int max, int* count)
{
    CamCtx* c = (CamCtx*)s->Ctx;
    int n = 0;
    if (max > 0)
    {
        MediaStreamInfo* v = &out[n++];
        GW_ZERO(v, sizeof(*v));
        v->Type = MSTREAM_VIDEO;
        v->Raw = c->raw;
        // Normalizado na fonte: para o gateway, camera crua e sempre I420.
        v->PixFmt = c->raw ? GW_PIX_I420 : GW_PIX_NONE;
        v->Codec  = c->raw ? MEDIA_CODEC_NONE : c->codec;
        v->Width = c->width; v->Height = c->height; v->Fps = c->fps;
    }
    if (count) *count = n;
    return n;
}

static int cam_read(MediaSource* s, GwPacket* pkt)
{
#ifdef _WIN32
    return cam_read_win((CamCtx*)s->Ctx, pkt);
#else
    return cam_read_linux((CamCtx*)s->Ctx, pkt);
#endif
}

static int cam_islive(MediaSource* s) { (void)s; return 1; }

static void cam_close(MediaSource* s)
{
    if (!s) return;
    CamCtx* c = (CamCtx*)s->Ctx;
    if (c)
    {
#ifdef _WIN32
        cam_close_win(c);
#else
        cam_close_linux(c);
#endif
        GW_FREE(c->i420);
        GW_FREE(c->enc);
        GW_FREE(c);
    }
    GW_FREE(s);
}

MediaSource* source_camera_open(const CameraParams* p)
{
    if (!p) return 0;

    CamCtx* c = (CamCtx*)GW_CALLOC(1, sizeof(CamCtx));
    if (!c) return 0;
    c->fps = p->Fps;

#ifdef _WIN32
    int ok = cam_open_win(c, p);
    if (!ok) cam_close_win(c);
#else
    int ok = cam_open_linux(c, p);
    if (!ok) cam_close_linux(c);
#endif
    if (!ok) { GW_FREE(c->i420); GW_FREE(c->enc); GW_FREE(c); return 0; }

    MediaSource* s = (MediaSource*)GW_CALLOC(1, sizeof(MediaSource));
    if (!s)
    {
#ifdef _WIN32
        cam_close_win(c);
#else
        cam_close_linux(c);
#endif
        GW_FREE(c->i420); GW_FREE(c->enc); GW_FREE(c);
        return 0;
    }
    s->Ctx = c;
    s->Info = cam_info; s->Read = cam_read; s->IsLive = cam_islive; s->Close = cam_close;
    return s;
}
