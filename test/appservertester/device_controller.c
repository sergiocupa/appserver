//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Rota de descoberta das fontes de entrada. Serializa a arvore de device_enum com yason.

#include "device_controller.h"
#include "enc_select.h"   // seletor de backend (hardware -> GPU -> SIMD -> threads)
#include "device_enum.h"
#include "frag_session.h"
#include "yason_build.h"
#include "gw_path.h"
#include "mp4_timing.h"
#include <string.h>
#include <stdio.h>

static void route_at(Message* message, int idx, char* out, size_t out_size)
{
    if (out_size > 0) out[0] = '\0';
    if (idx < 0 || idx >= message->Route.Count) return;
    StringX* s = (StringX*)message->Route.Items[idx];
    int n = s->Length < (int)out_size - 1 ? s->Length : (int)out_size - 1;
    memcpy(out, s->Content, n);
    out[n] = '\0';
}

static const char* codec_slug(MediaCodec c)
{
    switch (c)
    {
        case MEDIA_CODEC_H264: return "h264";
        case MEDIA_CODEC_H265: return "h265";
        case MEDIA_CODEC_VP9:  return "vp9";
        case MEDIA_CODEC_AV1:  return "av1";
        case MEDIA_CODEC_OPUS: return "opus";
        case MEDIA_CODEC_AAC:  return "aac";
        default:               return "";
    }
}

// Uma entrada por codec de video, com os backends na ORDEM em que o seletor os tenta.
// "selected" e o primeiro disponivel -- o que de fato vai atender. Os pulados vem com o
// motivo: e isso que deixa entender por que uma maquina nao usou a GPU.
static void emit_backends(Element* root, const char* key, int (*list)(MediaCodec, EncBackendInfo*, int))
{
    static const MediaCodec vc[] = { MEDIA_CODEC_H264, MEDIA_CODEC_H265, MEDIA_CODEC_VP9, MEDIA_CODEC_AV1 };
    Element* arr = yb_array(root, key);
    for (int ci = 0; ci < (int)(sizeof(vc) / sizeof(vc[0])); ci++)
    {
        EncBackendInfo bi[16];
        int nb = list(vc[ci], bi, 16);
        if (nb > 16) nb = 16;

        Element* eo = yb_array_object(arr);
        yb_str(eo, "codec", codec_slug(vc[ci]));
        const char* selected = "";
        Element* ba = yb_array(eo, "backends");
        for (int k = 0; k < nb; k++)
        {
            Element* b = yb_array_object(ba);
            yb_str (b, "name",      bi[k].Name);
            yb_str (b, "detail",    bi[k].Detail);
            yb_str (b, "tier",      enc_tier_name(bi[k].Tier));
            yb_bool(b, "available", bi[k].Available);
            yb_str (b, "why",       bi[k].Why ? bi[k].Why : "");
            Element* os = yb_array(b, "os");
            if (bi[k].Os & ENC_OS_WINDOWS)   yb_array_str(os, "windows");
            if (bi[k].Os & ENC_OS_LINUX)     yb_array_str(os, "linux");
            Element* ar = yb_array(b, "arch");
            if (bi[k].Arch & ENC_ARCH_X64)   yb_array_str(ar, "x64");
            if (bi[k].Arch & ENC_ARCH_ARM64) yb_array_str(ar, "arm64");
            if (!*selected && bi[k].Available) selected = bi[k].Name;
        }
        yb_str(eo, "selected", selected);
    }
}

// Plataforma deste servidor: a mesma rota que lista os dispositivos diz tambem em que
// SO/arquitetura roda e que SIMD a CPU tem -- e o que decide os degraus acima.
static void emit_platform(Element* root)
{
    Element* pf = yb_object(root, "platform");
    yb_str(pf, "os",   enc_current_os()   == ENC_OS_WINDOWS ? "windows" : "linux");
    yb_str(pf, "arch", enc_current_arch() == ENC_ARCH_ARM64 ? "arm64"   : "x64");
    const CpuFeatures* f = cpu_features();
    Element* simd = yb_array(pf, "simd");
    if (f->sse2)   yb_array_str(simd, "sse2");
    if (f->ssse3)  yb_array_str(simd, "ssse3");
    if (f->sse41)  yb_array_str(simd, "sse4.1");
    if (f->avx)    yb_array_str(simd, "avx");
    if (f->avx2)   yb_array_str(simd, "avx2");
    if (f->avx512) yb_array_str(simd, "avx512");
    if (f->neon)   yb_array_str(simd, "neon");
}

static const char* pixfmt_slug(GwPixFmt p)
{
    switch (p)
    {
        case GW_PIX_I420:  return "i420";
        case GW_PIX_NV12:  return "nv12";
        case GW_PIX_YUYV:  return "yuyv";
        case GW_PIX_RGB24: return "rgb24";
        default:           return "";
    }
}

Element* device_route(Message* message)
{
    if (!message) return 0;

    // Rota: .../devices[/<sessao>]
    int base = -1;
    for (int i = 0; i < message->Route.Count; i++)
    {
        StringX* s = (StringX*)message->Route.Items[i];
        if (s->Length == 7 && memcmp(s->Content, "devices", 7) == 0) { base = i; break; }
    }
    char session[80]; session[0] = '\0';
    if (base >= 0) route_at(message, base + 1, session, sizeof(session));

    Element* root = yb_root_object();
    Element* arr  = yb_array(root, "devices");

    // ---- arquivo da sessao (quando houver): mesmo formato das cameras -------
    // Para arquivo nao ha escolha -- resolucao, fps e codec vem do proprio video --,
    // entao ele aparece como um device com UM stream e UMA combinacao, e a UI so
    // desabilita os campos.
    if (session[0] && frag_session_exists(session))
    {
        char dir[768], src[900];
        frag_session_dir(session, dir, sizeof(dir));
        gw_path_join(src, sizeof(src), dir, "source.mp4");

        if (gw_file_exists(src))
        {
            int    w = 0, h = 0;
            double fps = 0.0, dur = 0.0;
            char   vcodec[16] = { 0 };
            mp4_video_info(src, &w, &h, &fps, vcodec, &dur);

            if (w > 0 && h > 0)
            {
                Element* d = yb_array_object(arr);
                yb_str(d, "id",   "file:source.mp4");
                yb_str(d, "name", "Arquivo da sessao (source.mp4)");
                yb_str(d, "kind", "file");
                yb_num(d, "duration", dur, 3);

                Element* streams = yb_array(d, "streams");
                Element* s = yb_array_object(streams);
                yb_str (s, "label",     vcodec[0] ? vcodec : "video");
                yb_bool(s, "raw",       0);
                yb_str (s, "codec",     vcodec);
                yb_str (s, "pixfmt",    "");
                yb_bool(s, "supported", 1);

                Element* res = yb_array(s, "resolutions");
                Element* ro  = yb_array_object(res);
                yb_int(ro, "width",  w);
                yb_int(ro, "height", h);
                Element* fl = yb_array(ro, "fps");
                yb_array_int(fl, (long long)(fps + 0.5));
            }
        }
    }

    // ---- cameras -----------------------------------------------------------
    // No heap: DevList tem ~3 MB e a thread que atende a requisicao tem pilha de 1 MB.
    DevList* list = device_list_new();
    int n = list ? device_enum_video(list) : -1;
    for (int i = 0; list && i < n && i < list->Count; i++)
    {
        const DevInfo* dev = &list->Items[i];
        Element* d = yb_array_object(arr);
        yb_str(d, "id",   dev->Id);
        yb_str(d, "name", dev->Name);
        yb_str(d, "kind", dev->Kind);

        Element* streams = yb_array(d, "streams");
        for (int k = 0; k < dev->StreamCount; k++)
        {
            const DevStream* st = &dev->Streams[k];
            Element* s = yb_array_object(streams);
            yb_str (s, "label",     st->Label);
            yb_bool(s, "raw",       st->Raw);
            yb_str (s, "codec",     codec_slug(st->Codec));
            yb_str (s, "pixfmt",    pixfmt_slug(st->PixFmt));
            yb_bool(s, "supported", st->Supported);

            Element* res = yb_array(s, "resolutions");
            for (int ri = 0; ri < st->ResCount; ri++)
            {
                const DevResolution* r = &st->Res[ri];
                Element* ro = yb_array_object(res);
                yb_int(ro, "width",  r->Width);
                yb_int(ro, "height", r->Height);
                Element* fl = yb_array(ro, "fps");
                for (int f = 0; f < r->FpsCount; f++) yb_array_num(fl, r->Fps[f], 3);
            }
        }
    }

    device_list_free(list);

    // n < 0 = o subsistema de captura nao subiu (MF/V4L2). A lista pode legitimamente
    // vir vazia numa maquina sem camera, entao os dois casos precisam ser distinguiveis.
    emit_platform(root);
    emit_backends(root, "encoders", media_encoder_list);
    emit_backends(root, "decoders", media_decoder_list);
    yb_bool(root, "captureAvailable", n >= 0);

    StringX* out = yb_render(root);
    if (out)
    {
        message->Response = message_response_create_content(HTTP_STATUS_OK, APPLICATION_JSON, out->Content, out->Length);
        yb_free_render(out);
    }
    else message->Response = message_response_create_text(HTTP_STATUS_INTERNAL_ERROR, "render falhou");
    yb_free(root);
    return 0;
}
