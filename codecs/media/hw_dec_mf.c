//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Decode por GPU no Windows: MFT de decode da Microsoft + gerenciador D3D11 (DXVA).
//
//  Nao existe "MFT de decode de hardware" nesta pilha: medido, MFTEnumEx com
//  MFT_ENUM_FLAG_HARDWARE devolve 0 decoders para H.264/HEVC/VP9/AV1. O caminho da GPU e
//  outro: o MFT da Microsoft (sincrono, D3D11-aware) decodifica via DXVA quando recebe um
//  IMFDXGIDeviceManager por MFT_MESSAGE_SET_D3D_MANAGER.
//
//  So se aceita quando o ID3D11VideoDevice EXPOE o perfil DXVA do codec com saida NV12. Os
//  MFT de extensao da loja (VP9/AV1) tambem aceitam o gerenciador, mas numa GPU sem o perfil
//  decodificam em software -- e o seletor diria "GPU" sem ser.
//
//  Os frames chegam como textura D3D11; IMF2DBuffer::Lock2D os traz para a memoria, e aqui
//  viram I420 (NV12 -> planos separados).

#include "hw_dec.h"
#include "memory_pool.h"
#include <string.h>
#include <stdio.h>

#ifdef _WIN32

#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3d10.h>      // ID3D10Multithread
#include <dxgi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

// hw_mf.c: plataforma MF viva pelo processo. Derruba-la a cada sessao vaza handles do driver
// (medido no encoder); o decoder usa a mesma plataforma pelo mesmo motivo.
int mf_platform_ready(void);

struct HwDec
{
    MediaCodec             codec;
    ID3D11Device*          dev;
    IMFDXGIDeviceManager*  mgr;
    UINT                   token;
    IMFActivate*           act;
    IMFTransform*          mft;
    DWORD                  in_id, out_id;
    int                    provides_samples;
    int                    coded_w, coded_h;   // superficie (costuma vir alinhada a 16)
    int                    x0, y0, w, h;       // area visivel (MF_MT_MINIMUM_DISPLAY_APERTURE)
    int64_t                in_count;
    uint8_t*               buf; int cap;       // I420 do frame entregue
    int                    com_uninit;
    char                   name[192];
};

static const GUID* dec_subtype(MediaCodec c)
{
    switch (c)
    {
        case MEDIA_CODEC_H264: return &MFVideoFormat_H264;
        case MEDIA_CODEC_H265: return &MFVideoFormat_HEVC;
        case MEDIA_CODEC_VP9:  return &MFVideoFormat_VP90;
        case MEDIA_CODEC_AV1:  return &MFVideoFormat_AV1;
        default:               return 0;
    }
}

static const GUID* dxva_profile(MediaCodec c)
{
    switch (c)
    {
        case MEDIA_CODEC_H264: return &D3D11_DECODER_PROFILE_H264_VLD_NOFGT;
        case MEDIA_CODEC_H265: return &D3D11_DECODER_PROFILE_HEVC_VLD_MAIN;
        case MEDIA_CODEC_VP9:  return &D3D11_DECODER_PROFILE_VP9_VLD_PROFILE0;
        case MEDIA_CODEC_AV1:  return &D3D11_DECODER_PROFILE_AV1_VLD_PROFILE0;
        default:               return 0;
    }
}

static int com_enter(void) { return SUCCEEDED(CoInitializeEx(NULL, COINIT_MULTITHREADED)); }

static void friendly(IMFActivate* a, char* out, int size)
{
    if (!out || size <= 0) return;
    out[0] = 0;
    WCHAR* fn = 0; UINT32 len = 0;
    if (SUCCEEDED(IMFActivate_GetAllocatedString(a, &MFT_FRIENDLY_NAME_Attribute, &fn, &len)))
    {
        WideCharToMultiByte(CP_UTF8, 0, fn, -1, out, size, 0, 0);
        CoTaskMemFree(fn);
    }
}

// Dispositivo D3D11 com suporte a video, protegido para acesso de varias threads: o MF usa
// o dispositivo a partir das threads dele.
static ID3D11Device* create_device(void)
{
    ID3D11Device* dev = 0;
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                 NULL, 0, D3D11_SDK_VERSION, &dev, NULL, NULL)) || !dev)
        return 0;
    ID3D10Multithread* mt = 0;
    if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_ID3D10Multithread, (void**)&mt)) && mt)
    {
        ID3D10Multithread_SetMultithreadProtected(mt, TRUE);
        ID3D10Multithread_Release(mt);
    }
    return dev;
}

// 1 se a GPU expoe o perfil DXVA do codec com NV12. 'gpu' recebe o nome do adaptador.
static int gpu_has_profile(ID3D11Device* dev, MediaCodec codec, char* gpu, int size)
{
    if (gpu && size > 0) gpu[0] = 0;
    const GUID* prof = dxva_profile(codec);
    if (!prof) return 0;

    IDXGIDevice* dx = 0;
    if (gpu && SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void**)&dx)) && dx)
    {
        IDXGIAdapter* ad = 0;
        if (SUCCEEDED(IDXGIDevice_GetAdapter(dx, &ad)) && ad)
        {
            DXGI_ADAPTER_DESC desc;
            if (SUCCEEDED(IDXGIAdapter_GetDesc(ad, &desc)))
                WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, gpu, size, 0, 0);
            IDXGIAdapter_Release(ad);
        }
        IDXGIDevice_Release(dx);
    }

    ID3D11VideoDevice* vd = 0;
    if (FAILED(ID3D11Device_QueryInterface(dev, &IID_ID3D11VideoDevice, (void**)&vd)) || !vd) return 0;
    int ok = 0;
    UINT n = ID3D11VideoDevice_GetVideoDecoderProfileCount(vd);
    for (UINT i = 0; i < n && !ok; i++)
    {
        GUID g;
        if (SUCCEEDED(ID3D11VideoDevice_GetVideoDecoderProfile(vd, i, &g)) && IsEqualGUID(&g, prof))
        {
            BOOL nv12 = FALSE;
            ID3D11VideoDevice_CheckVideoDecoderFormat(vd, prof, DXGI_FORMAT_NV12, &nv12);
            ok = nv12 ? 1 : 0;
        }
    }
    ID3D11VideoDevice_Release(vd);
    return ok;
}

static int set_output_nv12(HwDec* d)
{
    for (DWORD i = 0; ; i++)
    {
        IMFMediaType* t = 0;
        if (FAILED(IMFTransform_GetOutputAvailableType(d->mft, d->out_id, i, &t)) || !t) return -1;

        GUID sub;
        int nv12 = SUCCEEDED(IMFMediaType_GetGUID(t, &MF_MT_SUBTYPE, &sub)) && IsEqualGUID(&sub, &MFVideoFormat_NV12);
        if (nv12 && SUCCEEDED(IMFTransform_SetOutputType(d->mft, d->out_id, t, 0)))
        {
            UINT64 fs = 0;
            if (SUCCEEDED(IMFMediaType_GetUINT64(t, &MF_MT_FRAME_SIZE, &fs)))
            { d->coded_w = (int)(fs >> 32); d->coded_h = (int)(fs & 0xFFFFFFFF); }
            d->x0 = d->y0 = 0; d->w = d->coded_w; d->h = d->coded_h;

            // A superficie costuma ser alinhada (1080 -> 1088); a area visivel vem a parte.
            MFVideoArea area; UINT32 len = 0;
            if (SUCCEEDED(IMFMediaType_GetBlob(t, &MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8*)&area, sizeof(area), &len))
                && len == sizeof(area) && area.Area.cx > 0 && area.Area.cy > 0)
            {
                d->x0 = area.OffsetX.value; d->y0 = area.OffsetY.value;
                d->w = area.Area.cx;        d->h = area.Area.cy;
            }
            IMFMediaType_Release(t);

            MFT_OUTPUT_STREAM_INFO si; memset(&si, 0, sizeof(si));
            if (SUCCEEDED(IMFTransform_GetOutputStreamInfo(d->mft, d->out_id, &si)))
                d->provides_samples = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
            return 0;
        }
        IMFMediaType_Release(t);
    }
}

static void release_mft(HwDec* d)
{
    if (d->mft)
    {
        IMFTransform_ProcessMessage(d->mft, MFT_MESSAGE_SET_D3D_MANAGER, 0);   // solta o gerenciador
        IMFTransform_Release(d->mft);
        d->mft = 0;
    }
    if (d->act) { IMFActivate_ShutdownObject(d->act); IMFActivate_Release(d->act); d->act = 0; }
}

static int try_setup(HwDec* d, IMFActivate* act, const GUID* sub, char* fname, int fsize)
{
    if (FAILED(IMFActivate_ActivateObject(act, &IID_IMFTransform, (void**)&d->mft)) || !d->mft) return 0;
    d->act = act; IMFActivate_AddRef(act);
    friendly(act, fname, fsize);

    // So o protocolo SINCRONO e implementado aqui (e o do MFT da Microsoft), e so serve um
    // MFT que saiba usar o dispositivo D3D11.
    IMFAttributes* at = 0; UINT32 aware = 0, async = 0;
    if (SUCCEEDED(IMFTransform_GetAttributes(d->mft, &at)) && at)
    {
        IMFAttributes_GetUINT32(at, &MF_SA_D3D11_AWARE, &aware);
        IMFAttributes_GetUINT32(at, &MF_TRANSFORM_ASYNC, &async);
        IMFAttributes_Release(at);
    }
    if (!aware || async) return 0;
    if (FAILED(IMFTransform_ProcessMessage(d->mft, MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)d->mgr))) return 0;

    DWORD iid = 0, oid = 0;
    if (IMFTransform_GetStreamIDs(d->mft, 1, &iid, 1, &oid) == S_OK) { d->in_id = iid; d->out_id = oid; }
    else { d->in_id = 0; d->out_id = 0; }

    IMFMediaType* it = 0;
    if (FAILED(MFCreateMediaType(&it))) return 0;
    IMFMediaType_SetGUID(it, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(it, &MF_MT_SUBTYPE, sub);
    HRESULT hr = IMFTransform_SetInputType(d->mft, d->in_id, it, 0);
    IMFMediaType_Release(it);
    if (FAILED(hr)) return 0;

    set_output_nv12(d);   // pode so ficar disponivel depois do 1o MF_E_TRANSFORM_STREAM_CHANGE
    IMFTransform_ProcessMessage(d->mft, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    IMFTransform_ProcessMessage(d->mft, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return 1;
}

int hw_dec_probe(MediaCodec codec, char* detail, int size)
{
    if (detail && size > 0) detail[0] = 0;
    const GUID* sub = dec_subtype(codec);
    if (!sub) return 0;

    int un = com_enter(), ok = 0;
    if (mf_platform_ready())
    {
        ID3D11Device* dev = create_device();
        char gpu[96] = { 0 };
        if (!dev)
        {
            if (detail) snprintf(detail, size, "sem dispositivo D3D11 com suporte a video");
        }
        else if (!gpu_has_profile(dev, codec, gpu, sizeof(gpu)))
        {
            if (detail) snprintf(detail, size, "GPU sem perfil DXVA para este codec (%s)", gpu);
        }
        else
        {
            UINT token = 0; IMFDXGIDeviceManager* mgr = 0;
            if (SUCCEEDED(MFCreateDXGIDeviceManager(&token, &mgr)) && SUCCEEDED(IMFDXGIDeviceManager_ResetDevice(mgr, (IUnknown*)dev, token)))
            {
                MFT_REGISTER_TYPE_INFO in = { MFMediaType_Video, *sub };
                IMFActivate** acts = 0; UINT32 n = 0;
                MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER, &in, NULL, &acts, &n);
                for (UINT32 i = 0; i < n && !ok; i++)
                {
                    HwDec tmp; memset(&tmp, 0, sizeof(tmp));
                    tmp.mgr = mgr;
                    char f[96] = { 0 };
                    if (try_setup(&tmp, acts[i], sub, f, sizeof(f)))
                    {
                        ok = 1;
                        if (detail) snprintf(detail, size, "%s via DXVA (%s)", f, gpu);
                    }
                    release_mft(&tmp);
                }
                for (UINT32 i = 0; i < n; i++) IMFActivate_Release(acts[i]);
                if (acts) CoTaskMemFree(acts);
                if (!ok && detail) snprintf(detail, size, "nenhum decoder do Media Foundation usa a GPU para este codec (%s)", gpu);
            }
            if (mgr) IMFDXGIDeviceManager_Release(mgr);
        }
        if (dev) ID3D11Device_Release(dev);
    }
    if (un) CoUninitialize();
    return ok;
}

HwDec* hw_dec_open(MediaCodec codec)
{
    const GUID* sub = dec_subtype(codec);
    if (!sub) return 0;

    HwDec* d = (HwDec*)memop_calloc_raw(1, sizeof(HwDec));
    if (!d) return 0;
    d->codec = codec;
    d->com_uninit = com_enter();

    char gpu[96] = { 0 };
    if (!mf_platform_ready()) goto fail;
    if (!(d->dev = create_device())) goto fail;
    if (!gpu_has_profile(d->dev, codec, gpu, sizeof(gpu))) goto fail;
    if (FAILED(MFCreateDXGIDeviceManager(&d->token, &d->mgr))) goto fail;
    if (FAILED(IMFDXGIDeviceManager_ResetDevice(d->mgr, (IUnknown*)d->dev, d->token))) goto fail;

    {
        MFT_REGISTER_TYPE_INFO in = { MFMediaType_Video, *sub };
        IMFActivate** acts = 0; UINT32 n = 0;
        MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER, &in, NULL, &acts, &n);
        int ok = 0;
        char f[96] = { 0 };
        for (UINT32 i = 0; i < n && !ok; i++)
        {
            if (try_setup(d, acts[i], sub, f, sizeof(f))) ok = 1;
            else release_mft(d);
        }
        for (UINT32 i = 0; i < n; i++) IMFActivate_Release(acts[i]);
        if (acts) CoTaskMemFree(acts);
        if (!ok) goto fail;
        snprintf(d->name, sizeof(d->name), "%s via DXVA (%s)", f, gpu);
    }
    return d;

fail:
    hw_dec_close(&d);
    return 0;
}

int hw_dec_send(HwDec* d, const uint8_t* data, int size)
{
    if (!d || !d->mft) return -1;

    if (!data || size <= 0)   // drain: o que o decoder segura sai pelo hw_dec_next
    {
        IMFTransform_ProcessMessage(d->mft, MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        return SUCCEEDED(IMFTransform_ProcessMessage(d->mft, MFT_MESSAGE_COMMAND_DRAIN, 0)) ? 0 : -1;
    }

    IMFMediaBuffer* mb = 0;
    if (FAILED(MFCreateMemoryBuffer((DWORD)size, &mb))) return -1;
    BYTE* p = 0;
    if (FAILED(IMFMediaBuffer_Lock(mb, &p, 0, 0))) { IMFMediaBuffer_Release(mb); return -1; }
    memcpy(p, data, (size_t)size);
    IMFMediaBuffer_Unlock(mb);
    IMFMediaBuffer_SetCurrentLength(mb, (DWORD)size);

    IMFSample* s = 0;
    if (FAILED(MFCreateSample(&s))) { IMFMediaBuffer_Release(mb); return -1; }
    IMFSample_AddBuffer(s, mb);
    IMFMediaBuffer_Release(mb);
    IMFSample_SetSampleTime(s, d->in_count * 333333);
    d->in_count++;

    // O chamador sempre esvazia a saida (hw_dec_next ate 0) antes de mandar outro pacote,
    // entao MF_E_NOTACCEPTING aqui seria quebra do protocolo, nao contrapressao normal.
    HRESULT hr = IMFTransform_ProcessInput(d->mft, d->in_id, s, 0);
    IMFSample_Release(s);
    return SUCCEEDED(hr) ? 0 : -1;
}

static int ensure_buf(HwDec* d, int need)
{
    if (d->cap >= need) return 1;
    uint8_t* nb = (uint8_t*)memop_realloc_raw(d->buf, (uint64)need);
    if (!nb) return 0;
    d->buf = nb; d->cap = need;
    return 1;
}

// NV12 (textura lida de volta) -> I420 em d->buf, recortando a area visivel.
static int copy_frame(HwDec* d, IMFSample* s)
{
    if (d->w <= 0 || d->h <= 0 || d->coded_h <= 0) return -1;

    IMFMediaBuffer* mb = 0;
    if (FAILED(IMFSample_GetBufferByIndex(s, 0, &mb)) || !mb) return -1;

    BYTE* base = 0; LONG pitch = 0; DWORD len = 0;
    IMF2DBuffer* b2 = 0;
    int locked2d = 0;
    if (SUCCEEDED(IMFMediaBuffer_QueryInterface(mb, &IID_IMF2DBuffer, (void**)&b2)) && b2
        && SUCCEEDED(IMF2DBuffer_Lock2D(b2, &base, &pitch)))
        locked2d = 1;
    else
    {
        if (b2) { IMF2DBuffer_Release(b2); b2 = 0; }
        if (FAILED(IMFMediaBuffer_Lock(mb, &base, 0, &len))) { IMFMediaBuffer_Release(mb); return -1; }
        pitch = d->coded_w;
    }

    int r = -1;
    if (base && pitch > 0)   // NV12 e sempre de cima para baixo
    {
        const int w = d->w, h = d->h, cw = (w + 1) / 2, ch = (h + 1) / 2;
        if (ensure_buf(d, w * h + 2 * cw * ch))
        {
            uint8_t* Y = d->buf, *U = Y + (size_t)w * h, *V = U + (size_t)cw * ch;
            // O plano UV comeca depois da ALTURA CODIFICADA da superficie, nao da visivel.
            const BYTE* uv = base + (size_t)pitch * (size_t)d->coded_h;
            for (int y = 0; y < h; y++)
                memcpy(Y + (size_t)y * w, base + (size_t)(y + d->y0) * pitch + d->x0, (size_t)w);
            for (int y = 0; y < ch; y++)
            {
                const BYTE* row = uv + (size_t)(y + d->y0 / 2) * pitch + (size_t)(d->x0 & ~1);
                for (int x = 0; x < cw; x++) { U[(size_t)y * cw + x] = row[2 * x]; V[(size_t)y * cw + x] = row[2 * x + 1]; }
            }
            r = 1;
        }
    }

    if (locked2d) { IMF2DBuffer_Unlock2D(b2); IMF2DBuffer_Release(b2); }
    else IMFMediaBuffer_Unlock(mb);
    IMFMediaBuffer_Release(mb);
    return r;
}

int hw_dec_next(HwDec* d, uint8_t** planes, int* strides, int* w, int* h)
{
    if (!d || !d->mft) return -1;

    for (int attempt = 0; attempt < 4; attempt++)
    {
        MFT_OUTPUT_DATA_BUFFER ob; memset(&ob, 0, sizeof(ob));
        ob.dwStreamID = d->out_id;

        IMFSample* own = 0;
        if (!d->provides_samples)
        {
            IMFMediaBuffer* mb = 0;
            const int cw = d->coded_w > 0 ? d->coded_w : 1920, chh = d->coded_h > 0 ? d->coded_h : 1088;
            if (FAILED(MFCreateSample(&own)) || FAILED(MFCreateMemoryBuffer((DWORD)(cw * chh * 3 / 2), &mb)))
            { if (own) IMFSample_Release(own); if (mb) IMFMediaBuffer_Release(mb); return -1; }
            IMFSample_AddBuffer(own, mb);
            IMFMediaBuffer_Release(mb);
            ob.pSample = own;
        }

        DWORD status = 0;
        HRESULT hr = IMFTransform_ProcessOutput(d->mft, 0, 1, &ob, &status);
        if (ob.pEvents) IMFCollection_Release(ob.pEvents);
        IMFSample* got = ob.pSample;

        if (hr == MF_E_TRANSFORM_STREAM_CHANGE)   // o decoder descobriu o tamanho: renegocia
        {
            if (got && got != own) IMFSample_Release(got);
            if (own) IMFSample_Release(own);
            if (set_output_nv12(d) < 0) return -1;
            continue;
        }

        int r;
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) r = 0;
        else if (FAILED(hr) || !got)              r = -1;
        else                                      r = copy_frame(d, got);

        if (got && got != own) IMFSample_Release(got);
        if (own) IMFSample_Release(own);

        if (r == 1)
        {
            const int cw = (d->w + 1) / 2, ch = (d->h + 1) / 2;
            planes[0] = d->buf;
            planes[1] = d->buf + (size_t)d->w * d->h;
            planes[2] = planes[1] + (size_t)cw * ch;
            strides[0] = d->w; strides[1] = cw; strides[2] = cw;
            *w = d->w; *h = d->h;
        }
        return r;
    }
    return -1;
}

void hw_dec_close(HwDec** pd)
{
    if (!pd || !*pd) return;
    HwDec* d = *pd;
    if (d->mft) IMFTransform_ProcessMessage(d->mft, MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    release_mft(d);
    if (d->mgr) IMFDXGIDeviceManager_Release(d->mgr);
    if (d->dev) ID3D11Device_Release(d->dev);
    memop_free_raw(d->buf);
    if (d->com_uninit) CoUninitialize();
    memop_free_raw(d);
    *pd = 0;
}

const char* hw_dec_name(const HwDec* d) { return d ? d->name : ""; }

#elif !defined(HAVE_VAAPI)   // ---- sem implementacao nesta plataforma ----

int         hw_dec_probe(MediaCodec c, char* detail, int size) { (void)c; if (detail && size > 0) detail[0] = 0; return 0; }
HwDec*      hw_dec_open(MediaCodec c)                          { (void)c; return 0; }
int         hw_dec_send(HwDec* d, const uint8_t* data, int n)  { (void)d; (void)data; (void)n; return -1; }
int         hw_dec_next(HwDec* d, uint8_t** p, int* s, int* w, int* h) { (void)d; (void)p; (void)s; (void)w; (void)h; return -1; }
void        hw_dec_close(HwDec** d)                            { if (d) *d = 0; }
const char* hw_dec_name(const HwDec* d)                        { (void)d; return ""; }

#endif
