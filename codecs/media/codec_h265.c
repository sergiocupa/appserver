//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Modulo H.265/HEVC. Encoder: x265. Decoder: libde265 (pendente).
//  ATENCAO DE LICENCA: x265 e GPL e HEVC tem PATENTES. Embutir x265 torna o binario
//  GPL (ou exige licenca comercial). Ative com -DHAVE_X265 (vcpkg install x265),
//  ciente das implicacoes. Saida do encoder e Annex-B (start codes). NAO TESTADO EM RUNTIME.

#include "media_codec.h"
#include "codec_parallel.h"
#include "memory_pool.h"   // memop_* (evita declaracao implicita -> ponteiro truncado em x64)
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_X265
#include "x265.h"

// Fila simples de pacotes ja codificados (x265 tem lookahead: N frames de atraso).
typedef struct { uint8_t* data; int size; int64_t pts; int key; } H265Pkt;

typedef struct
{
    x265_encoder* enc;
    x265_param*   param;
    const x265_api* api;
    int      fps;
    H265Pkt* q; int qn, qcap, qhead;   // [0,qhead) ja consumidos; [qhead,qn) pendentes
    uint8_t* last;                     // buffer do ultimo pacote retornado (memop_free_raw tardio)
} H265Enc;

static void h265_queue(H265Enc* c, x265_nal* nals, uint32_t nnal, int64_t pts, int key)
{
    int total = 0;
    for (uint32_t i = 0; i < nnal; i++) total += nals[i].sizeBytes;
    if (total <= 0) return;
    uint8_t* buf = (uint8_t*)memop_alloc_raw(total);
    if (!buf) return;
    int off = 0;
    for (uint32_t i = 0; i < nnal; i++) { memcpy(buf + off, nals[i].payload, nals[i].sizeBytes); off += nals[i].sizeBytes; }

    if (c->qn == c->qcap) { c->qcap = c->qcap ? c->qcap * 2 : 16; c->q = (H265Pkt*)memop_realloc_raw(c->q, c->qcap * sizeof(H265Pkt)); }
    c->q[c->qn].data = buf; c->q[c->qn].size = total; c->q[c->qn].pts = pts; c->q[c->qn].key = key; c->qn++;
}

static int h265_send(MediaEncoder* e, const MediaFrame* f)
{
    H265Enc* c = (H265Enc*)e->Ctx;
    x265_nal* nals = 0; uint32_t nnal = 0;
    x265_picture pic_out; c->api->picture_init(c->param, &pic_out);

    if (!f)   // flush: drena tudo o que restou no lookahead
    {
        int n;
        while ((n = c->api->encoder_encode(c->enc, &nals, &nnal, 0, &pic_out)) > 0)
        {
            int key = (pic_out.sliceType == X265_TYPE_IDR || pic_out.sliceType == X265_TYPE_I);
            h265_queue(c, nals, nnal, pic_out.pts, key);
        }
        return 0;
    }

    x265_picture pic_in; c->api->picture_init(c->param, &pic_in);
    pic_in.colorSpace = X265_CSP_I420;
    pic_in.planes[0] = f->Y; pic_in.stride[0] = f->StrideY;
    pic_in.planes[1] = f->U; pic_in.stride[1] = f->StrideU;
    pic_in.planes[2] = f->V; pic_in.stride[2] = f->StrideV;
    pic_in.pts = f->Pts;

    int n = c->api->encoder_encode(c->enc, &nals, &nnal, &pic_in, &pic_out);
    if (n < 0) return -1;
    if (n > 0)
    {
        int key = (pic_out.sliceType == X265_TYPE_IDR || pic_out.sliceType == X265_TYPE_I);
        h265_queue(c, nals, nnal, pic_out.pts, key);
    }
    return 0;
}

static int h265_recv(MediaEncoder* e, MediaPacket* out)
{
    H265Enc* c = (H265Enc*)e->Ctx;
    if (c->last) { memop_free_raw(c->last); c->last = 0; }   // libera o pacote retornado na chamada anterior
    if (c->qhead >= c->qn) { c->qhead = c->qn = 0; return 0; }
    H265Pkt* p = &c->q[c->qhead++];
    c->last = p->data;                              // vive ate o proximo recv/close (contrato)
    out->Data = p->data; out->Size = p->size;
    out->Pts = out->Dts = p->pts; out->KeyFrame = p->key;
    return 1;
}

static void h265_enc_close(MediaEncoder* e)
{
    if (!e) return;
    H265Enc* c = (H265Enc*)e->Ctx;
    if (c)
    {
        if (c->last) memop_free_raw(c->last);
        for (int i = c->qhead; i < c->qn; i++) memop_free_raw(c->q[i].data);
        memop_free_raw(c->q);
        if (c->enc) c->api->encoder_close(c->enc);
        if (c->param) c->api->param_free(c->param);
        memop_free_raw(c);
    }
    memop_free_raw(e);
}

MediaEncoder* h265_encoder_open(const MediaEncoderParams* p)
{
    if (!p || p->Width <= 0 || p->Height <= 0) return 0;

    const x265_api* api = x265_api_get(0);
    if (!api) return 0;

    x265_param* param = api->param_alloc();
    if (!param) return 0;
    // preset: SpeedPreset alto = mais rapido. Mapeia p/ "ultrafast".."medium".
    const char* preset = (p->SpeedPreset >= 8) ? "ultrafast" : (p->SpeedPreset >= 6 ? "superfast" : (p->SpeedPreset >= 4 ? "veryfast" : "medium"));
    if (api->param_default_preset(param, preset, 0) < 0) { api->param_free(param); return 0; }

    param->sourceWidth  = p->Width;
    param->sourceHeight = p->Height;
    param->internalCsp  = X265_CSP_I420;
    param->fpsNum       = (uint32_t)(p->Fps > 0 ? p->Fps : 30);
    param->fpsDenom     = 1;
    param->bRepeatHeaders = 1;
    // SEM B-frames. O preset "ultrafast" liga bframes=3 (x265/source/common/param.cpp), e os
    // sinks assumem DTS == PTS: com reordenacao o MP4/HLS sairia com timestamps de decode
    // errados. E o mesmo contrato que o encoder de hardware segue (AVEncMPVDefaultBPictureCount=0).
    param->bframes = 0;   // VPS/SPS/PPS junto de cada IDR -> permite montar hvcC
    param->rc.rateControlMode = X265_RC_ABR;
    param->rc.bitrate   = (p->BitrateBps > 0 ? p->BitrateBps : 2000000) / 1000;   // kbps

    // Regra do H.265: 1 x nucleos fisicos. Medido -- 2.75x com 4 pools, 3.59x com 8,
    // e de 8 para 12 o ganho e de 1%.
    //
    // Geometria: WPP processa FRENTES DE ONDA em linhas de CTU (64 px), logo o eixo e a
    // altura. Frame-parallel continua desligado (frameNumThreads = 1): ele evita
    // reordenacao e latencia, e nao foi medido -- ligar sem medir seria trocar um
    // comportamento conhecido por um palpite.
    {
        int budget = codec_nucleos_fisicos();
        int faixas = p->Height / 64;
        int t      = codec_threads(p->Threads, budget, faixas);
        char pools[16]; snprintf(pools, sizeof(pools), "%d", t);
        api->param_parse(param, "pools", pools);
        param->frameNumThreads  = 1;
        param->bEnableWavefront = 1;
    }

    x265_encoder* enc = api->encoder_open(param);
    if (!enc) { api->param_free(param); return 0; }

    H265Enc* c = (H265Enc*)memop_calloc_raw(1, sizeof(H265Enc));
    if (!c) { api->encoder_close(enc); api->param_free(param); return 0; }
    c->enc = enc; c->param = param; c->api = api; c->fps = p->Fps > 0 ? p->Fps : 30;

    MediaEncoder* e = (MediaEncoder*)memop_calloc_raw(1, sizeof(MediaEncoder));
    if (!e) { api->encoder_close(enc); api->param_free(param); memop_free_raw(c); return 0; }
    e->Codec = MEDIA_CODEC_H265; e->Ctx = c;
    e->SendFrame = h265_send; e->ReceivePacket = h265_recv; e->Close = h265_enc_close;
    return e;
}

#else
MediaEncoder* h265_encoder_open(const MediaEncoderParams* p) { (void)p; return 0; }
#endif

MediaDecoder* h265_decoder_open(void) { return 0; } // TODO: decode HEVC via libde265 (HAVE_LIBDE265)


// ============================================================================
//  O descritor deste projeto -- o UNICO simbolo que o nucleo enxerga daqui.
//  Ver codec_plugin.h.
// ============================================================================
#include "codec_plugin.h"

extern const H26xDecBackend h265_dec_backend;   // codec_h265_dec.c

#ifdef HAVE_X265
  #define X265_COMPILADO 1
#else
  #define X265_COMPILADO 0
#endif

// O assembly do x265 depende de QUAL build entrou no binario.
//  - Windows (x265/_vsbuild): ENABLE_ASSEMBLY=ON com NASM 2.16.03.
//  - Linux (codecs/CMakeLists.txt): ainda FORCA ENABLE_ASSEMBLY=OFF; ligar exige nasm (x64)
//    ou gas (arm64) no host. Ate la o x265 no Linux e degrau THREADS.
#ifdef _WIN32
  #define X265_ISA_MIN ISA_SSE2
  #define X265_ISA_MAX ISA_AVX2
  #define X265_NOTE    "x265, assembly compilado (SSE2..AVX2)"
#else
  #define X265_ISA_MIN ISA_NONE
  #define X265_ISA_MAX ISA_NONE
  #define X265_NOTE    "x265 SEM assembly no build Linux (ENABLE_ASSEMBLY=OFF)"
#endif

static MediaEncoder* h265_plugin_enc(const MediaEncoderParams* p, char* detail, int size)
{
    (void)detail; (void)size;
    return h265_encoder_open(p);
}

// Os dois lados do H.265 vem de libs DIFERENTES: x265 codifica, libde265 decodifica.
// Por isso sao duas entradas, com nomes proprios -- e o que a lista de diagnostico mostra.
static const CodecPlugin H265[] =
{
    {
        CODEC_PLUGIN_ABI, MEDIA_CODEC_H265, CODEC_ROLE_ENCODE,
        "x265", X265_NOTE,
        CODEC_OS_ALL, CODEC_ARCH_ALL, X265_COMPILADO, 0,
        X265_ISA_MIN, X265_ISA_MAX, 0,
        h265_plugin_enc,
        0, 0, 0,
        0
    },
    {
        // libde265: no Windows entra como binario pre-compilado (codecs/libde265) e no Linux
        // como pacote da distribuicao; nos dois casos o SIMD dele nao e verificavel daqui.
        // h265_decoder_open ainda e um stub: o HEVC de entrada passa pela via h26x.
        CODEC_PLUGIN_ABI, MEDIA_CODEC_H265, CODEC_ROLE_DECODE,
        "libde265", "libde265",
        CODEC_OS_ALL, CODEC_ARCH_ALL, 1, 0,
        ISA_NONE, ISA_NONE, 0,
        0,
        // Sem DecoderOpen de proposito: h265_decoder_open ainda e um stub que devolve 0,
        // e anunciar a via da vtable faria media_decoder_available mentir. O HEVC de
        // entrada decodifica pela via h26x, abaixo.
        0, &h265_dec_backend, "binario pre-compilado: SIMD nao verificavel",
        0
    },
};

const CodecPluginSet codec_set_h265 =
{ CODEC_PLUGIN_ABI, "codec_h265", (int)(sizeof(H265) / sizeof(H265[0])), H265 };

// Compilado como plugin: o conjunto vira o export do modulo. Compilado estatico,
// esta linha some e o nucleo pega o conjunto pelo extern de sempre.
#ifdef CODEC_PLUGIN_BUILD
CODEC_PLUGIN_DECLARE(codec_set_h265)
#endif
