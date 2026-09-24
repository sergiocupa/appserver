//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Implementacao de device_enum: parte comum + backend Media Foundation (Windows) e
//  backend V4L2 (Linux). Os dois produzem a MESMA arvore device -> stream -> resolucao -> fps,
//  entao a rota HTTP e a UI nao sabem em qual plataforma estao rodando.

#include "device_enum.h"
#include "gw_base.h"
#include <string.h>
#include <stdio.h>

#define FCC(a,b,c,d) ((unsigned int)(a) | ((unsigned int)(b) << 8) | ((unsigned int)(c) << 16) | ((unsigned int)(d) << 24))

// ---- parte comum -----------------------------------------------------------

DevList* device_list_new(void)
{
    DevList* l = (DevList*)GW_CALLOC(1, sizeof(DevList));
    return l;
}

void device_list_free(DevList* l) { GW_FREE(l); }


void device_map_fourcc(unsigned int fourcc, DevStream* out)
{
    if (!out) return;

    // Rotulo: quase todo formato de camera e um FOURCC imprimivel. O que nao for
    // (RGB do MF usa GUIDs numericos) recebe rotulo hexadecimal e fica marcado como
    // nao suportado, em vez de virar um item mudo na lista.
    char* L = out->Label;
    unsigned char b[4] = { (unsigned char)(fourcc & 0xFF), (unsigned char)((fourcc >> 8) & 0xFF),
                           (unsigned char)((fourcc >> 16) & 0xFF), (unsigned char)((fourcc >> 24) & 0xFF) };
    int printable = 1;
    for (int i = 0; i < 4; i++) if (b[i] < 0x20 || b[i] > 0x7E) printable = 0;
    if (printable) { for (int i = 0; i < 4; i++) L[i] = (char)b[i]; L[4] = '\0'; }
    else snprintf(L, sizeof(out->Label), "0x%08X", fourcc);

    out->Raw = 0; out->Codec = MEDIA_CODEC_NONE; out->PixFmt = GW_PIX_NONE; out->Supported = 0;

    switch (fourcc)
    {
        // --- formatos CRUS (o gateway pula o decode) ---
        case FCC('N','V','1','2'): out->Raw = 1; out->PixFmt = GW_PIX_NV12;  out->Supported = 1; break;
        case FCC('Y','U','Y','2'):
        case FCC('Y','U','Y','V'): out->Raw = 1; out->PixFmt = GW_PIX_YUYV;  out->Supported = 1; break;
        case FCC('I','4','2','0'):
        case FCC('I','Y','U','V'):
        case FCC('Y','U','1','2'): out->Raw = 1; out->PixFmt = GW_PIX_I420;  out->Supported = 1; break;
        case FCC('R','G','B','3'):
        case FCC('B','G','R','3'): out->Raw = 1; out->PixFmt = GW_PIX_RGB24; out->Supported = 1; break;

        // --- bitstreams ---
        case FCC('H','2','6','4'):
        case FCC('h','2','6','4'):
        case FCC('A','V','C','1'):
        case FCC('a','v','c','1'): out->Codec = MEDIA_CODEC_H264; out->Supported = 1; break;
        case FCC('H','E','V','C'):
        case FCC('h','e','v','c'):
        case FCC('H','2','6','5'):
        case FCC('h','2','6','5'): out->Codec = MEDIA_CODEC_H265; out->Supported = 1; break;

        // VP9/AV1 aparecem em algumas webcams e em fontes de rede. Agora o decode existe
        // (VP9 por vpx_dx, AV1 por dav1d) e o gateway alcanca os dois via gw_decode, entao
        // a disponibilidade e perguntada ao proprio registro de codecs em vez de chumbada.
        case FCC('V','P','9','0'):
        case FCC('v','p','0','9'): out->Codec = MEDIA_CODEC_VP9; out->Supported = media_decoder_available(MEDIA_CODEC_VP9); break;
        case FCC('A','V','0','1'):
        case FCC('a','v','0','1'): out->Codec = MEDIA_CODEC_AV1; out->Supported = media_decoder_available(MEDIA_CODEC_AV1); break;

        // MJPEG e o formato mais comum de webcam em resolucoes altas, mas nao ha decoder
        // JPEG na cadeia atual. Listado (a UI mostra) e explicitamente nao suportado.
        case FCC('M','J','P','G'):
        case FCC('m','j','p','g'):
        case FCC('J','P','E','G'): out->Codec = MEDIA_CODEC_NONE; out->Supported = 0; break;

        default: break;
    }
}

static DevStream* stream_find_or_add(DevInfo* d, unsigned int fourcc)
{
    DevStream probe; memset(&probe, 0, sizeof(probe));
    device_map_fourcc(fourcc, &probe);

    for (int i = 0; i < d->StreamCount; i++)
        if (strcmp(d->Streams[i].Label, probe.Label) == 0) return &d->Streams[i];

    if (d->StreamCount >= DEV_MAX_STREAM) return 0;
    DevStream* s = &d->Streams[d->StreamCount++];
    *s = probe;
    s->ResCount = 0;
    return s;
}

static DevResolution* res_find_or_add(DevStream* s, int w, int h)
{
    for (int i = 0; i < s->ResCount; i++)
        if (s->Res[i].Width == w && s->Res[i].Height == h) return &s->Res[i];
    if (s->ResCount >= DEV_MAX_RES) return 0;
    DevResolution* r = &s->Res[s->ResCount++];
    memset(r, 0, sizeof(*r));
    r->Width = w; r->Height = h;
    return r;
}

static void res_add_fps(DevResolution* r, double fps)
{
    if (!r || fps <= 0.0 || fps > 1000.0) return;
    for (int i = 0; i < r->FpsCount; i++)
        if (r->Fps[i] > fps - 0.01 && r->Fps[i] < fps + 0.01) return;   // driver repete a mesma taxa
    if (r->FpsCount >= DEV_MAX_FPS) return;

    // Insercao ordenada (maior primeiro): a camera pode expor um pino de foto de 1 fps
    // junto com o de video; sem ordenar, 1 fps podia acabar sendo a primeira opcao da UI.
    int i = r->FpsCount;
    while (i > 0 && r->Fps[i - 1] < fps) { r->Fps[i] = r->Fps[i - 1]; i--; }
    r->Fps[i] = fps;
    r->FpsCount++;
}

// ============================================================================
#ifdef _WIN32
// ============================================================================

#define COBJMACROS
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

// Os GUIDs de subtipo de video do MF sao FOURCC-based:
// {AAAABBBB-0000-0010-8000-00AA00389B71}, com Data1 = FOURCC. Derivar o FOURCC de Data1
// e mais robusto do que comparar contra uma lista fixa de GUIDs conhecidos.
static int mf_subtype_fourcc(const GUID* g, unsigned int* out)
{
    // Padrao: {FOURCC-0000-0010-8000-00AA00389B71}
    static const unsigned char tail[8] = { 0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71 };
    if (g->Data2 != 0x0000 || g->Data3 != 0x0010) return 0;
    if (memcmp(g->Data4, tail, 8) != 0) return 0;
    *out = (unsigned int)g->Data1;
    return 1;
}

// MF_MT_FRAME_SIZE e MF_MT_FRAME_RATE sao UINT64 com dois UINT32 empacotados:
// (alto << 32) | baixo = (largura, altura) ou (numerador, denominador).
static int mf_get_pair(IMFMediaType* mt, const GUID* key, UINT32* hi, UINT32* lo)
{
    UINT64 v = 0;
    if (FAILED(IMFMediaType_GetUINT64(mt, key, &v))) return 0;
    *hi = (UINT32)(v >> 32);
    *lo = (UINT32)(v & 0xFFFFFFFFull);
    return 1;
}

static void wide_to_utf8(const WCHAR* src, char* dst, int dst_size)
{
    if (!src) { if (dst_size > 0) dst[0] = '\0'; return; }
    int n = WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, dst_size, 0, 0);
    if (n <= 0 && dst_size > 0) dst[0] = '\0';
}

// Le todas as combinacoes de um IMFMediaSource ja ativado.
static void mf_read_types(IMFMediaSource* src, DevInfo* dev)
{
    IMFPresentationDescriptor* pd = 0;
    if (FAILED(IMFMediaSource_CreatePresentationDescriptor(src, &pd)) || !pd) return;

    DWORD sd_count = 0;
    IMFPresentationDescriptor_GetStreamDescriptorCount(pd, &sd_count);

    for (DWORD i = 0; i < sd_count; i++)
    {
        BOOL selected = FALSE;
        IMFStreamDescriptor* sd = 0;
        if (FAILED(IMFPresentationDescriptor_GetStreamDescriptorByIndex(pd, i, &selected, &sd)) || !sd) continue;

        IMFMediaTypeHandler* h = 0;
        if (SUCCEEDED(IMFStreamDescriptor_GetMediaTypeHandler(sd, &h)) && h)
        {
            DWORD tcount = 0;
            IMFMediaTypeHandler_GetMediaTypeCount(h, &tcount);
            for (DWORD t = 0; t < tcount; t++)
            {
                IMFMediaType* mt = 0;
                if (FAILED(IMFMediaTypeHandler_GetMediaTypeByIndex(h, t, &mt)) || !mt) continue;

                GUID major, sub;
                unsigned int fcc = 0;
                UINT32 w = 0, hgt = 0, num = 0, den = 0;

                if (SUCCEEDED(IMFMediaType_GetGUID(mt, &MF_MT_MAJOR_TYPE, &major)) &&
                    IsEqualGUID(&major, &MFMediaType_Video) &&
                    SUCCEEDED(IMFMediaType_GetGUID(mt, &MF_MT_SUBTYPE, &sub)) &&
                    mf_subtype_fourcc(&sub, &fcc) &&
                    mf_get_pair(mt, &MF_MT_FRAME_SIZE, &w, &hgt) && w > 0 && hgt > 0)
                {
                    DevStream* st = stream_find_or_add(dev, fcc);
                    DevResolution* r = st ? res_find_or_add(st, (int)w, (int)hgt) : 0;
                    if (r)
                    {
                        // Taxa nominal do tipo + os extremos da faixa, quando o driver a expoe
                        // (webcams costumam declarar uma faixa em vez de valores discretos).
                        if (mf_get_pair(mt, &MF_MT_FRAME_RATE, &num, &den) && den)
                            res_add_fps(r, (double)num / (double)den);
                        if (mf_get_pair(mt, &MF_MT_FRAME_RATE_RANGE_MAX, &num, &den) && den)
                            res_add_fps(r, (double)num / (double)den);
                        if (mf_get_pair(mt, &MF_MT_FRAME_RATE_RANGE_MIN, &num, &den) && den)
                            res_add_fps(r, (double)num / (double)den);
                    }
                }
                IMFMediaType_Release(mt);
            }
            IMFMediaTypeHandler_Release(h);
        }
        IMFStreamDescriptor_Release(sd);
    }
    IMFPresentationDescriptor_Release(pd);
}

int device_enum_video(DevList* out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));

    // MFSTARTUP_LITE: so a plataforma, sem o pipeline completo -- basta para enumerar.
    // O CoInitialize e por thread; o servidor atende cada requisicao numa thread propria.
    HRESULT co = CoInitializeEx(0, COINIT_MULTITHREADED);
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE)))
    { if (SUCCEEDED(co)) CoUninitialize(); return -1; }

    IMFAttributes* attr = 0;
    IMFActivate**  devices = 0;
    UINT32 count = 0;
    int    enumerated = 0;

    if (SUCCEEDED(MFCreateAttributes(&attr, 1)) &&
        SUCCEEDED(IMFAttributes_SetGUID(attr, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                                        &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID)) &&
        SUCCEEDED(MFEnumDeviceSources(attr, &devices, &count)))
    {
        enumerated = 1;
        for (UINT32 i = 0; i < count && out->Count < DEV_MAX_DEVICE; i++)
        {
            IMFActivate* act = devices[i];
            if (!act) continue;

            DevInfo* dev = &out->Items[out->Count];
            memset(dev, 0, sizeof(*dev));
            snprintf(dev->Kind, sizeof(dev->Kind), "camera");

            WCHAR* wname = 0; UINT32 wlen = 0;
            if (SUCCEEDED(IMFActivate_GetAllocatedString(act, &MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &wname, &wlen)))
            { wide_to_utf8(wname, dev->Name, (int)sizeof(dev->Name)); CoTaskMemFree(wname); }

            WCHAR* wlink = 0; UINT32 llen = 0;
            if (SUCCEEDED(IMFActivate_GetAllocatedString(act, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &wlink, &llen)))
            { wide_to_utf8(wlink, dev->Id, (int)sizeof(dev->Id)); CoTaskMemFree(wlink); }

            if (dev->Id[0] == '\0') snprintf(dev->Id, sizeof(dev->Id), "mf-%u", i);
            if (dev->Name[0] == '\0') snprintf(dev->Name, sizeof(dev->Name), "Camera %u", i + 1);

            // Ativar a fonte e o unico jeito de ler os tipos de midia. E' rapido e nao inicia
            // captura; uma camera ja em uso por outro app pode falhar aqui -- nesse caso o
            // dispositivo entra na lista sem formatos, em vez de sumir sem explicacao.
            IMFMediaSource* src = 0;
            if (SUCCEEDED(IMFActivate_ActivateObject(act, &IID_IMFMediaSource, (void**)&src)) && src)
            {
                mf_read_types(src, dev);
                IMFMediaSource_Shutdown(src);
                IMFMediaSource_Release(src);
            }
            IMFActivate_ShutdownObject(act);

            out->Count++;
        }

        for (UINT32 i = 0; i < count; i++) if (devices[i]) IMFActivate_Release(devices[i]);
        if (devices) CoTaskMemFree(devices);
    }

    if (attr) IMFAttributes_Release(attr);
    MFShutdown();
    if (SUCCEEDED(co)) CoUninitialize();
    return enumerated ? out->Count : -1;
}

// ============================================================================
#else   // ---------------------------- Linux (V4L2) ---------------------------
// ============================================================================

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include <errno.h>

static void v4l2_read_intervals(int fd, unsigned int pixfmt, DevResolution* r)
{
    struct v4l2_frmivalenum fi;
    for (unsigned int i = 0; ; i++)
    {
        memset(&fi, 0, sizeof(fi));
        fi.index = i; fi.pixel_format = pixfmt;
        fi.width = (unsigned int)r->Width; fi.height = (unsigned int)r->Height;
        if (ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &fi) != 0) break;

        if (fi.type == V4L2_FRMIVAL_TYPE_DISCRETE)
        {
            if (fi.discrete.numerator) res_add_fps(r, (double)fi.discrete.denominator / (double)fi.discrete.numerator);
        }
        else
        {
            // Faixa continua: registra os extremos (min/max de intervalo = max/min de fps).
            if (fi.stepwise.min.numerator) res_add_fps(r, (double)fi.stepwise.min.denominator / (double)fi.stepwise.min.numerator);
            if (fi.stepwise.max.numerator) res_add_fps(r, (double)fi.stepwise.max.denominator / (double)fi.stepwise.max.numerator);
            break;
        }
    }
}

static void v4l2_read_sizes(int fd, DevStream* st, unsigned int pixfmt)
{
    struct v4l2_frmsizeenum fs;
    for (unsigned int i = 0; ; i++)
    {
        memset(&fs, 0, sizeof(fs));
        fs.index = i; fs.pixel_format = pixfmt;
        if (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fs) != 0) break;

        if (fs.type == V4L2_FRMSIZE_TYPE_DISCRETE)
        {
            DevResolution* r = res_find_or_add(st, (int)fs.discrete.width, (int)fs.discrete.height);
            if (r) v4l2_read_intervals(fd, pixfmt, r);
        }
        else
        {
            // Stepwise/continuous: enumerar tudo daria centenas de itens inuteis na UI.
            // Registra os extremos, que e o que o usuario efetivamente escolhe.
            DevResolution* a = res_find_or_add(st, (int)fs.stepwise.max_width, (int)fs.stepwise.max_height);
            if (a) v4l2_read_intervals(fd, pixfmt, a);
            DevResolution* b = res_find_or_add(st, (int)fs.stepwise.min_width, (int)fs.stepwise.min_height);
            if (b) v4l2_read_intervals(fd, pixfmt, b);
            break;
        }
    }
}

int device_enum_video(DevList* out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));

    for (int n = 0; n < 64 && out->Count < DEV_MAX_DEVICE; n++)
    {
        char path[32];
        snprintf(path, sizeof(path), "/dev/video%d", n);

        int fd = open(path, O_RDWR | O_NONBLOCK);
        if (fd < 0) continue;

        struct v4l2_capability cap;
        memset(&cap, 0, sizeof(cap));
        if (ioctl(fd, VIDIOC_QUERYCAP, &cap) != 0) { close(fd); continue; }

        // Um mesmo hardware costuma expor varios /dev/videoN; so o no de CAPTURA interessa.
        unsigned int caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
        if (!(caps & V4L2_CAP_VIDEO_CAPTURE)) { close(fd); continue; }

        DevInfo* dev = &out->Items[out->Count];
        memset(dev, 0, sizeof(*dev));
        snprintf(dev->Kind, sizeof(dev->Kind), "camera");
        snprintf(dev->Id,   sizeof(dev->Id),   "%s", path);
        snprintf(dev->Name, sizeof(dev->Name), "%s", (const char*)cap.card);

        struct v4l2_fmtdesc fmt;
        for (unsigned int i = 0; ; i++)
        {
            memset(&fmt, 0, sizeof(fmt));
            fmt.index = i; fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            if (ioctl(fd, VIDIOC_ENUM_FMT, &fmt) != 0) break;

            DevStream* st = stream_find_or_add(dev, fmt.pixelformat);
            if (st) v4l2_read_sizes(fd, st, fmt.pixelformat);
        }

        close(fd);
        out->Count++;
    }
    return out->Count;
}

#endif
