//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Degrau HARDWARE no Windows: encoder via Media Foundation (MFT de hardware).
//
//  Por que Media Foundation e nao os SDKs de cada fabricante: o Windows expoe o bloco de
//  encode de TODOS os fabricantes como MFT -- NVIDIA (NVENC), Intel (QSV), AMD (VCN/AMF)
//  e Qualcomm (Windows ARM64) -- atras da mesma interface, usando so o Windows SDK. Um
//  backend cobre os quatro. Os SDKs nativos continuam possiveis como degrau extra acima
//  deste, se um dia o controle fino deles fizer falta.
//
//  Os MFT de hardware sao ASSINCRONOS: nao se chama ProcessInput quando se quer, e sim
//  quando o MFT pede (evento METransformNeedInput), e a saida so existe depois de
//  METransformHaveOutput. Este modulo traduz esse protocolo para o SendFrame/ReceivePacket
//  sincrono do MediaEncoder, com uma fila interna de pacotes prontos.
//
//  Toda espera tem PRAZO. Um driver travado nao pode segurar o job para sempre -- esse e
//  justamente o tipo de falha que "quebra a proxima sessao".

#include "media_codec.h"
#include "enc_select.h"
#include "memory_pool.h"
#include <string.h>
#include <stdio.h>

#ifdef _WIN32

#define COBJMACROS
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <strmif.h>        // ICodecAPI
#include <initguid.h>      // a partir daqui DEFINE_GUID define (selectany) -- so os CODECAPI_*
#include <codecapi.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "strmiids.lib")

#define MF_TIMEOUT_MS 5000   // prazo de qualquer espera por evento do MFT

// Plataforma MF: sobe UMA vez por processo e nunca desce.
//
// Medido (100 ciclos por variante): ativar o MFT da NVIDIA e depois derrubar a plataforma
// com MFShutdown vaza de 8 a 13 handles POR SESSAO -- o driver prende recursos a ela e nao
// os solta no teardown. Com a plataforma viva pelo processo, o mesmo ciclo completo fica
// em +0,03 por ciclo. O COM por thread (CoInitialize/CoUninitialize a cada sessao) foi
// isolado e NAO vaza; so o MF precisa ser segurado.
//
// Nao ha MFShutdown no fim do processo de proposito: chama-lo durante o encerramento da CRT
// arrisca travar esperando threads de trabalho do MF ja encerradas, e o SO recupera tudo
// na saida de qualquer forma.
static LONG g_mf_platform;   // 0 = nunca tentado, 2 = subindo, 1 = pronto, -1 = falhou

int mf_platform_ready(void)   // tambem usada pelo decode por GPU (hw_dec_mf.c)
{
    LONG st = InterlockedCompareExchange(&g_mf_platform, 2, 0);
    if (st == 0)   // esta thread ganhou a vez de subir
    {
        LONG r = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE)) ? 1 : -1;
        InterlockedExchange(&g_mf_platform, r);
        return r == 1;
    }
    while (st == 2) { SwitchToThread(); st = g_mf_platform; }   // outra thread esta subindo
    return st == 1;
}

typedef struct { uint8_t* data; int size; int64_t pts; int key; } MfPkt;

typedef struct
{
    IMFActivate*            act;
    IMFTransform*           mft;
    IMFMediaEventGenerator* gen;
    ICodecAPI*              api;
    DWORD                   in_id, out_id;
    int                     provides_samples;
    DWORD                   out_cb;

    int        w, h, fps;
    MediaCodec codec;

    int        need_input;       // eventos NeedInput recebidos e ainda nao atendidos
    int        drained;
    int64_t    in_count;

    MfPkt*     q; int qn, qcap, qhead;   // pacotes prontos, em ordem
    uint8_t*   cur;                      // pacote entregue (vale ate a proxima chamada)

    uint8_t*   seqhdr; int seqhdr_len, seqhdr_tried;

    int        com_uninit;
} MfEnc;

static const GUID* mf_subtype(MediaCodec c)
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

// 1 = esta thread precisa de um CoUninitialize no final. Uma thread ja em STA devolve
// RPC_E_CHANGED_MODE: o MF funciona assim mesmo, so nao e nossa a inicializacao.
static int com_enter(void) { return SUCCEEDED(CoInitializeEx(NULL, COINIT_MULTITHREADED)); }

static int mf_enum(MediaCodec codec, IMFActivate*** acts, UINT32* n)
{
    const GUID* st = mf_subtype(codec);
    *acts = 0; *n = 0;
    if (!st) return 0;

    MFT_REGISTER_TYPE_INFO out;
    out.guidMajorType = MFMediaType_Video;
    out.guidSubtype   = *st;

    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                           MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                           NULL, &out, acts, n);
    return SUCCEEDED(hr) ? (int)*n : 0;
}

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

int mf_hw_probe(MediaCodec codec, char* detail, int size)
{
    if (detail && size > 0) detail[0] = 0;
    if (!mf_subtype(codec)) return 0;

    int un = com_enter();
    if (!mf_platform_ready()) { if (un) CoUninitialize(); return 0; }

    IMFActivate** acts = 0; UINT32 n = 0;
    int cnt = mf_enum(codec, &acts, &n);
    if (cnt > 0) friendly(acts[0], detail, size);
    for (UINT32 i = 0; i < n; i++) IMFActivate_Release(acts[i]);
    if (acts) CoTaskMemFree(acts);

    if (un) CoUninitialize();
    return cnt;
}

// ---- ICodecAPI -------------------------------------------------------------
// Falha individual e ignorada de proposito: nem todo fabricante implementa toda
// propriedade. O que IMPORTA (GOP periodico, sem B-frames) e conferido nos testes.

static void api_u4(ICodecAPI* api, const GUID* key, ULONG v)
{
    VARIANT var; memset(&var, 0, sizeof(var));
    var.vt = VT_UI4; var.ulVal = v;
    ICodecAPI_SetValue(api, key, &var);
}

static void api_bool(ICodecAPI* api, const GUID* key, int v)
{
    VARIANT var; memset(&var, 0, sizeof(var));
    var.vt = VT_BOOL; var.boolVal = v ? VARIANT_TRUE : VARIANT_FALSE;
    ICodecAPI_SetValue(api, key, &var);
}

static void set_video_geometry(IMFMediaType* t, int w, int h, int fps)
{
    IMFMediaType_SetUINT64(t, &MF_MT_FRAME_SIZE,         ((UINT64)w << 32) | (UINT32)h);
    IMFMediaType_SetUINT64(t, &MF_MT_FRAME_RATE,         ((UINT64)fps << 32) | 1);
    IMFMediaType_SetUINT64(t, &MF_MT_PIXEL_ASPECT_RATIO, ((UINT64)1 << 32) | 1);
    IMFMediaType_SetUINT32(t, &MF_MT_INTERLACE_MODE,     MFVideoInterlace_Progressive);
}

static void mf_release(MfEnc* c)
{
    if (c->api) { ICodecAPI_Release(c->api); c->api = 0; }
    if (c->gen) { IMFMediaEventGenerator_Release(c->gen); c->gen = 0; }
    if (c->mft)
    {
        // Todo MFT ASSINCRONO implementa IMFShutdown, e o contrato manda o cliente chamar
        // Shutdown(): e isso que encerra a fila de eventos dele. So soltar a referencia (e o
        // ShutdownObject no activate, abaixo) nao basta.
        IMFShutdown* sd = 0;
        if (SUCCEEDED(IMFTransform_QueryInterface(c->mft, &IID_IMFShutdown, (void**)&sd)) && sd)
        {
            IMFShutdown_Shutdown(sd);
            IMFShutdown_Release(sd);
        }
        IMFTransform_Release(c->mft);
        c->mft = 0;
    }
    // O contrato do IMFActivate manda chamar ShutdownObject ao terminar de usar o objeto
    // ativado (libera o que o activate criou para ele).
    if (c->act) { IMFActivate_ShutdownObject(c->act); IMFActivate_Release(c->act); c->act = 0; }
}

static int mf_setup(MfEnc* c, IMFActivate* act, const MediaEncoderParams* p)
{
    HRESULT hr = IMFActivate_ActivateObject(act, &IID_IMFTransform, (void**)&c->mft);
    if (FAILED(hr) || !c->mft) return 0;
    c->act = act; IMFActivate_AddRef(act);

    // So o protocolo ASSINCRONO e implementado -- e o de todo MFT de hardware. Um MFT
    // sincrono aqui seria inesperado; recusar faz o seletor cair para o proximo degrau.
    IMFAttributes* at = 0;
    if (FAILED(IMFTransform_GetAttributes(c->mft, &at)) || !at) return 0;
    UINT32 async = 0;
    IMFAttributes_GetUINT32(at, &MF_TRANSFORM_ASYNC, &async);
    if (async) IMFAttributes_SetUINT32(at, &MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
    IMFAttributes_Release(at);
    if (!async) return 0;

    if (FAILED(IMFTransform_QueryInterface(c->mft, &IID_IMFMediaEventGenerator, (void**)&c->gen))) return 0;

    DWORD iid = 0, oid = 0;
    if (IMFTransform_GetStreamIDs(c->mft, 1, &iid, 1, &oid) == S_OK) { c->in_id = iid; c->out_id = oid; }
    else { c->in_id = 0; c->out_id = 0; }   // E_NOTIMPL = ids sequenciais a partir de 0

    const UINT32 br = (UINT32)(p->BitrateBps > 0 ? p->BitrateBps : 2000000);

    // ICodecAPI ANTES dos tipos: alguns encoders so aceitam o modo de rate control antes
    // de o tipo de saida fixar a configuracao.
    if (SUCCEEDED(IMFTransform_QueryInterface(c->mft, &IID_ICodecAPI, (void**)&c->api)) && c->api)
    {
        api_u4(c->api, &CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
        api_u4(c->api, &CODECAPI_AVEncCommonMeanBitRate, br);
        // GOP de ~2s, igual ao openh264: os sinks so cortam segmento em keyframe, e sem
        // IDR periodico sairia um segmento unico com o video inteiro.
        api_u4(c->api, &CODECAPI_AVEncMPVGOPSize, (ULONG)(c->fps * 2));
        // Sem B-frames: os sinks assumem DTS == PTS (sem reordenacao).
        api_u4(c->api, &CODECAPI_AVEncMPVDefaultBPictureCount, 0);
        api_bool(c->api, &CODECAPI_AVLowLatencyMode, 1);
    }

    IMFMediaType* ot = 0;
    if (FAILED(MFCreateMediaType(&ot))) return 0;
    IMFMediaType_SetGUID(ot, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(ot, &MF_MT_SUBTYPE, mf_subtype(p->Codec));
    IMFMediaType_SetUINT32(ot, &MF_MT_AVG_BITRATE, br);
    set_video_geometry(ot, c->w, c->h, c->fps);
    if      (p->Codec == MEDIA_CODEC_H264) IMFMediaType_SetUINT32(ot, &MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
    else if (p->Codec == MEDIA_CODEC_H265) IMFMediaType_SetUINT32(ot, &MF_MT_MPEG2_PROFILE, eAVEncH265VProfile_Main_420_8);
    hr = IMFTransform_SetOutputType(c->mft, c->out_id, ot, 0);
    IMFMediaType_Release(ot);
    if (FAILED(hr)) return 0;

    // Entrada: NV12, que e o formato nativo dos blocos de hardware. Procura entre os
    // tipos oferecidos em vez de montar do zero -- o MFT sabe o que aceita.
    int ok = 0;
    for (DWORD i = 0; !ok; i++)
    {
        IMFMediaType* it = 0;
        hr = IMFTransform_GetInputAvailableType(c->mft, c->in_id, i, &it);
        if (FAILED(hr) || !it) break;
        GUID sub;
        if (SUCCEEDED(IMFMediaType_GetGUID(it, &MF_MT_SUBTYPE, &sub)) && IsEqualGUID(&sub, &MFVideoFormat_NV12))
        {
            set_video_geometry(it, c->w, c->h, c->fps);
            ok = SUCCEEDED(IMFTransform_SetInputType(c->mft, c->in_id, it, 0));
        }
        IMFMediaType_Release(it);
    }
    if (!ok) return 0;

    MFT_OUTPUT_STREAM_INFO si; memset(&si, 0, sizeof(si));
    if (SUCCEEDED(IMFTransform_GetOutputStreamInfo(c->mft, c->out_id, &si)))
    {
        c->provides_samples = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        c->out_cb = si.cbSize;
    }

    IMFTransform_ProcessMessage(c->mft, MFT_MESSAGE_COMMAND_FLUSH, 0);
    if (FAILED(IMFTransform_ProcessMessage(c->mft, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0))) return 0;
    if (FAILED(IMFTransform_ProcessMessage(c->mft, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0))) return 0;
    return 1;
}

// ---- bitstream -------------------------------------------------------------

static void nal_scan(const uint8_t* d, int n, MediaCodec codec, int* has_ps, int* has_idr)
{
    *has_ps = *has_idr = 0;
    for (int i = 0; i + 3 < n; i++)
    {
        if (d[i] != 0 || d[i + 1] != 0 || d[i + 2] != 1) continue;
        int b = d[i + 3];
        if (codec == MEDIA_CODEC_H265)
        {
            int t = (b >> 1) & 0x3F;
            if (t == 32 || t == 33 || t == 34) *has_ps = 1;
            if (t >= 16 && t <= 21)            *has_idr = 1;
        }
        else
        {
            int t = b & 0x1F;
            if (t == 7 || t == 8) *has_ps = 1;
            if (t == 5)           *has_idr = 1;
        }
        i += 2;
    }
}

// Alguns MFT so poem SPS/PPS no tipo de saida (MF_MT_MPEG_SEQUENCE_HEADER) e nao no
// fluxo. Os sinks extraem os parameter sets do proprio fluxo, entao sem eles o avcC/hvcC
// nunca ficaria pronto. Busca uma vez e injeta nos keyframes que vierem sem.
static void fetch_seqhdr(MfEnc* c)
{
    if (c->seqhdr_tried) return;
    c->seqhdr_tried = 1;

    IMFMediaType* t = 0;
    if (FAILED(IMFTransform_GetOutputCurrentType(c->mft, c->out_id, &t)) || !t) return;
    UINT32 sz = 0;
    if (SUCCEEDED(IMFMediaType_GetBlobSize(t, &MF_MT_MPEG_SEQUENCE_HEADER, &sz)) && sz > 0)
    {
        c->seqhdr = (uint8_t*)memop_alloc_raw(sz);
        if (c->seqhdr && SUCCEEDED(IMFMediaType_GetBlob(t, &MF_MT_MPEG_SEQUENCE_HEADER, c->seqhdr, sz, &sz)))
            c->seqhdr_len = (int)sz;
    }
    IMFMediaType_Release(t);
}

static int q_push(MfEnc* c, uint8_t* data, int size, int64_t pts, int key)
{
    if (c->qhead > 0 && c->qhead == c->qn) c->qhead = c->qn = 0;
    if (c->qn == c->qcap)
    {
        if (c->qhead > 0)   // compacta antes de crescer
        {
            memmove(c->q, c->q + c->qhead, (size_t)(c->qn - c->qhead) * sizeof(MfPkt));
            c->qn -= c->qhead; c->qhead = 0;
        }
        if (c->qn == c->qcap)
        {
            int nc = c->qcap ? c->qcap * 2 : 8;
            MfPkt* nq = (MfPkt*)memop_realloc_raw(c->q, (uint64)nc * sizeof(MfPkt));
            if (!nq) return 0;
            c->q = nq; c->qcap = nc;
        }
    }
    MfPkt* k = &c->q[c->qn++];
    k->data = data; k->size = size; k->pts = pts; k->key = key;
    return 1;
}

static int queue_sample(MfEnc* c, IMFSample* s)
{
    IMFMediaBuffer* b = 0;
    if (FAILED(IMFSample_ConvertToContiguousBuffer(s, &b)) || !b) return -1;

    BYTE* p = 0; DWORD len = 0;
    if (FAILED(IMFMediaBuffer_Lock(b, &p, 0, &len))) { IMFMediaBuffer_Release(b); return -1; }

    UINT32 clean = 0; IMFSample_GetUINT32(s, &MFSampleExtension_CleanPoint, &clean);
    LONGLONG t = 0;   IMFSample_GetSampleTime(s, &t);

    int has_ps = 0, has_idr = 0;
    int h26x = (c->codec == MEDIA_CODEC_H264 || c->codec == MEDIA_CODEC_H265);
    if (h26x) nal_scan(p, (int)len, c->codec, &has_ps, &has_idr);
    int key = clean || has_idr;

    int extra = 0;
    if (h26x && key && !has_ps) { fetch_seqhdr(c); extra = c->seqhdr_len; }

    uint8_t* d = (uint8_t*)memop_alloc_raw((uint64)len + (uint64)extra);
    int ok = d != 0;
    if (ok)
    {
        if (extra) memcpy(d, c->seqhdr, (size_t)extra);
        memcpy(d + extra, p, len);
        // tempo em 100ns -> indice de frame (a mesma unidade dos encoders de software)
        int64_t pts = (int64_t)((t * c->fps + 5000000) / 10000000);
        ok = q_push(c, d, (int)len + extra, pts, key);
        if (!ok) memop_free_raw(d);
    }

    IMFMediaBuffer_Unlock(b);
    IMFMediaBuffer_Release(b);
    return ok ? 1 : -1;
}

static int pull_output(MfEnc* c)
{
    for (int attempt = 0; attempt < 3; attempt++)
    {
        MFT_OUTPUT_DATA_BUFFER ob; memset(&ob, 0, sizeof(ob));
        ob.dwStreamID = c->out_id;

        IMFSample* own = 0;
        if (!c->provides_samples)
        {
            IMFMediaBuffer* mb = 0;
            DWORD cb = c->out_cb ? c->out_cb : (DWORD)(c->w * c->h * 2);
            if (FAILED(MFCreateSample(&own)) || FAILED(MFCreateMemoryBuffer(cb, &mb)))
            { if (own) IMFSample_Release(own); if (mb) IMFMediaBuffer_Release(mb); return -1; }
            IMFSample_AddBuffer(own, mb);
            IMFMediaBuffer_Release(mb);
            ob.pSample = own;
        }

        DWORD status = 0;
        HRESULT hr = IMFTransform_ProcessOutput(c->mft, 0, 1, &ob, &status);
        if (ob.pEvents) IMFCollection_Release(ob.pEvents);
        IMFSample* got = ob.pSample;

        if (hr == MF_E_TRANSFORM_STREAM_CHANGE)
        {
            // O encoder renegociou o tipo de saida (comum no primeiro frame). Aceita o
            // que ele oferece e tenta de novo -- o frame pendente sai na proxima volta.
            if (got && got != own) IMFSample_Release(got);
            if (own) IMFSample_Release(own);
            IMFMediaType* t = 0;
            if (FAILED(IMFTransform_GetOutputAvailableType(c->mft, c->out_id, 0, &t)) || !t) return -1;
            hr = IMFTransform_SetOutputType(c->mft, c->out_id, t, 0);
            IMFMediaType_Release(t);
            if (FAILED(hr)) return -1;
            c->seqhdr_tried = 0;   // o cabecalho de sequencia pode ter mudado junto
            continue;
        }

        int r;
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) r = 0;
        else if (FAILED(hr))                      r = -1;
        else                                      r = got ? queue_sample(c, got) : 0;

        if (got && got != own) IMFSample_Release(got);
        if (own) IMFSample_Release(own);
        return r;
    }
    return -1;
}

// Processa UM evento, sem bloquear. 1 = processou, 0 = nenhum pendente, <0 = erro.
static int pump_one(MfEnc* c)
{
    IMFMediaEvent* ev = 0;
    HRESULT hr = IMFMediaEventGenerator_GetEvent(c->gen, MF_EVENT_FLAG_NO_WAIT, &ev);
    if (hr == MF_E_NO_EVENTS_AVAILABLE) return 0;
    if (FAILED(hr) || !ev) return -1;

    MediaEventType t = MEUnknown; IMFMediaEvent_GetType(ev, &t);
    HRESULT st = S_OK;            IMFMediaEvent_GetStatus(ev, &st);
    IMFMediaEvent_Release(ev);
    if (FAILED(st)) return -1;

    switch (t)
    {
        case METransformNeedInput:     c->need_input++; break;
        case METransformHaveOutput:    if (pull_output(c) < 0) return -1; break;
        case METransformDrainComplete: c->drained = 1; break;
        default: break;
    }
    return 1;
}

static int cond_need(MfEnc* c)    { return c->need_input > 0; }
static int cond_drained(MfEnc* c) { return c->drained; }

// Bombeia eventos ate a condicao valer, com prazo. Nos primeiros ~2ms so cede a vez
// (SwitchToThread): o proximo evento costuma chegar em microssegundos, e um Sleep(1)
// custaria ate 15ms pela granularidade do timer do Windows -- teto de ~60 fps. Depois
// disso dorme, para nao queimar um nucleo enquanto o hardware trabalha.
static int pump_until(MfEnc* c, int (*cond)(MfEnc*), int timeout_ms)
{
    ULONGLONG t0 = GetTickCount64();
    while (!cond(c))
    {
        int r = pump_one(c);
        if (r < 0) return -1;
        if (r > 0) continue;

        ULONGLONG el = GetTickCount64() - t0;
        if (el > (ULONGLONG)timeout_ms) return -1;
        if (el < 2) SwitchToThread(); else Sleep(1);
    }
    return 0;
}

// ---- MediaEncoder ----------------------------------------------------------

static int mf_send(MediaEncoder* e, const MediaFrame* f)
{
    MfEnc* c = (MfEnc*)e->Ctx;

    if (!f)   // drain: o encoder entrega o que segura e sinaliza DrainComplete
    {
        if (c->drained) return 0;
        IMFTransform_ProcessMessage(c->mft, MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        if (FAILED(IMFTransform_ProcessMessage(c->mft, MFT_MESSAGE_COMMAND_DRAIN, 0))) return -1;
        return pump_until(c, cond_drained, MF_TIMEOUT_MS) < 0 ? -1 : 0;
    }

    if (pump_until(c, cond_need, MF_TIMEOUT_MS) < 0) return -1;

    // I420 (o formato da cadeia) -> NV12 (o do hardware): Y igual, U/V intercalados.
    const int w = c->w, h = c->h, cw = (w + 1) / 2, ch = (h + 1) / 2;
    const DWORD size = (DWORD)(w * h + 2 * cw * ch);

    IMFMediaBuffer* mb = 0;
    if (FAILED(MFCreateMemoryBuffer(size, &mb))) return -1;

    BYTE* dst = 0;
    if (FAILED(IMFMediaBuffer_Lock(mb, &dst, 0, 0))) { IMFMediaBuffer_Release(mb); return -1; }
    for (int y = 0; y < h; y++)
        memcpy(dst + (size_t)y * w, f->Y + (size_t)y * f->StrideY, (size_t)w);
    BYTE* uv = dst + (size_t)w * h;
    for (int y = 0; y < ch; y++)
    {
        const uint8_t* su = f->U + (size_t)y * f->StrideU;
        const uint8_t* sv = f->V + (size_t)y * f->StrideV;
        BYTE* row = uv + (size_t)y * 2 * cw;
        for (int x = 0; x < cw; x++) { row[2 * x] = su[x]; row[2 * x + 1] = sv[x]; }
    }
    IMFMediaBuffer_Unlock(mb);
    IMFMediaBuffer_SetCurrentLength(mb, size);

    IMFSample* s = 0;
    if (FAILED(MFCreateSample(&s))) { IMFMediaBuffer_Release(mb); return -1; }
    IMFSample_AddBuffer(s, mb);
    IMFMediaBuffer_Release(mb);

    const LONGLONG dur = 10000000LL / c->fps;
    IMFSample_SetSampleTime(s, c->in_count * dur);
    IMFSample_SetSampleDuration(s, dur);

    HRESULT hr = IMFTransform_ProcessInput(c->mft, c->in_id, s, 0);
    IMFSample_Release(s);
    if (FAILED(hr)) return -1;

    c->need_input--;
    c->in_count++;
    return 0;
}

static int mf_recv(MediaEncoder* e, MediaPacket* out)
{
    MfEnc* c = (MfEnc*)e->Ctx;

    // Fila vazia: consome eventos ja pendentes (sem esperar). Um NeedInput visto aqui
    // fica contado para o proximo SendFrame -- nao pode ser perdido.
    while (c->qhead >= c->qn)
    {
        int r = pump_one(c);
        if (r < 0)  return -1;
        if (r == 0) { c->qhead = c->qn = 0; return 0; }
    }

    MfPkt* k = &c->q[c->qhead++];
    if (c->cur) memop_free_raw(c->cur);
    c->cur = k->data;

    out->Data = k->data; out->Size = k->size;
    out->Pts = out->Dts = k->pts;
    out->KeyFrame = k->key;
    return 1;
}

static void mf_free_ctx(MfEnc* c)
{
    if (!c) return;
    if (c->mft) IMFTransform_ProcessMessage(c->mft, MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    mf_release(c);
    for (int i = c->qhead; i < c->qn; i++) memop_free_raw(c->q[i].data);
    memop_free_raw(c->q);
    memop_free_raw(c->cur);
    memop_free_raw(c->seqhdr);
    if (c->com_uninit) CoUninitialize();
    memop_free_raw(c);
}

static void mf_close(MediaEncoder* e)
{
    if (!e) return;
    mf_free_ctx((MfEnc*)e->Ctx);
    memop_free_raw(e);
}

MediaEncoder* mf_hw_encoder_open(const MediaEncoderParams* p, char* detail, int size)
{
    if (detail && size > 0) detail[0] = 0;
    if (!p || !mf_subtype(p->Codec) || p->Width <= 0 || p->Height <= 0) return 0;

    MfEnc* c = (MfEnc*)memop_calloc_raw(1, sizeof(MfEnc));
    if (!c) return 0;
    c->w = p->Width; c->h = p->Height; c->fps = p->Fps > 0 ? p->Fps : 30; c->codec = p->Codec;
    c->com_uninit = com_enter();
    if (!mf_platform_ready()) { mf_free_ctx(c); return 0; }

    // Mais de um adaptador (notebook com iGPU + dGPU): tenta cada um na ordem que o
    // proprio MF recomenda (SORTANDFILTER), ate um aceitar a configuracao.
    IMFActivate** acts = 0; UINT32 n = 0;
    mf_enum(p->Codec, &acts, &n);
    int ok = 0;
    for (UINT32 i = 0; i < n && !ok; i++)
    {
        if (mf_setup(c, acts[i], p)) { ok = 1; friendly(acts[i], detail, size); }
        else
        {
            mf_release(c);
            c->need_input = 0; c->drained = 0;
        }
    }
    for (UINT32 i = 0; i < n; i++) IMFActivate_Release(acts[i]);
    if (acts) CoTaskMemFree(acts);

    if (!ok) { mf_free_ctx(c); return 0; }

    MediaEncoder* e = (MediaEncoder*)memop_calloc_raw(1, sizeof(MediaEncoder));
    if (!e) { mf_free_ctx(c); return 0; }
    e->Codec = p->Codec; e->Ctx = c;
    e->SendFrame = mf_send; e->ReceivePacket = mf_recv; e->Close = mf_close;
    return e;
}

#else   // ---- fora do Windows: nao ha Media Foundation (Linux usa VAAPI/V4L2, Fase 7) ----

int mf_hw_probe(MediaCodec codec, char* detail, int size)
{ (void)codec; if (detail && size > 0) detail[0] = 0; return 0; }

MediaEncoder* mf_hw_encoder_open(const MediaEncoderParams* p, char* detail, int size)
{ (void)p; if (detail && size > 0) detail[0] = 0; return 0; }

#endif


// ============================================================================
//  O descritor deste projeto -- o UNICO simbolo que o nucleo enxerga daqui.
//  Ver codec_plugin.h.
// ============================================================================
#include "codec_plugin.h"

int hw_dec_probe(MediaCodec codec, char* detail, int size);   // hw_dec_mf.c

#ifdef _WIN32
  #define MF_COMPILADO   1
  #define DXVA_COMPILADO 1
#else
  #define MF_COMPILADO   0
  #define DXVA_COMPILADO 0
#endif
#ifdef HAVE_VAAPI
  #define VAAPI_COMPILADO 1
#else
  #define VAAPI_COMPILADO 0
#endif

#define MF_NOTA "Media Foundation (bloco de encode da GPU)"

// NVENC / QSV / AMF / Qualcomm entram todos por aqui. Nao ha degrau de "GPU por shader":
// nenhuma lib da pilha codifica por compute, e o NVENC e bloco fixo dentro da GPU, entao
// conta como HARDWARE.
#define ENC_HW(codec) \
    { CODEC_PLUGIN_ABI, (codec), CODEC_ROLE_ENCODE, "mf-hw", MF_NOTA, \
      CODEC_OS_WINDOWS, CODEC_ARCH_ALL, MF_COMPILADO, 1, \
      ISA_NONE, ISA_NONE, 0, \
      mf_hw_encoder_open, 0, 0, 0, mf_hw_probe }

// Decode por GPU: Windows = MFT da Microsoft + D3D11 (DXVA); Linux = VAAPI. Estas entradas
// nao abrem nada -- quem tenta a GPU e o gw_decode, pela hw_dec.h. Elas existem para a
// lista de diagnostico dizer o que ha nesta maquina.
#define DEC_HW(nome, codec, os, compilado) \
    { CODEC_PLUGIN_ABI, (codec), CODEC_ROLE_DECODE, (nome), (nome), \
      (os), CODEC_ARCH_ALL, (compilado), 1, \
      ISA_NONE, ISA_NONE, 0, \
      0, 0, 0, 0, hw_dec_probe }

static const CodecPlugin HW[] =
{
    ENC_HW(MEDIA_CODEC_H264),
    ENC_HW(MEDIA_CODEC_H265),
    ENC_HW(MEDIA_CODEC_VP9),
    ENC_HW(MEDIA_CODEC_AV1),

    DEC_HW("dxva",  MEDIA_CODEC_H264, CODEC_OS_WINDOWS, DXVA_COMPILADO),
    DEC_HW("dxva",  MEDIA_CODEC_H265, CODEC_OS_WINDOWS, DXVA_COMPILADO),
    DEC_HW("dxva",  MEDIA_CODEC_VP9,  CODEC_OS_WINDOWS, DXVA_COMPILADO),
    DEC_HW("dxva",  MEDIA_CODEC_AV1,  CODEC_OS_WINDOWS, DXVA_COMPILADO),
    DEC_HW("vaapi", MEDIA_CODEC_H264, CODEC_OS_LINUX,   VAAPI_COMPILADO),
    DEC_HW("vaapi", MEDIA_CODEC_H265, CODEC_OS_LINUX,   VAAPI_COMPILADO),
    DEC_HW("vaapi", MEDIA_CODEC_VP9,  CODEC_OS_LINUX,   VAAPI_COMPILADO),
    DEC_HW("vaapi", MEDIA_CODEC_AV1,  CODEC_OS_LINUX,   VAAPI_COMPILADO),
};

const CodecPluginSet codec_set_hw =
{ CODEC_PLUGIN_ABI, "codec_hw", (int)(sizeof(HW) / sizeof(HW[0])), HW };
