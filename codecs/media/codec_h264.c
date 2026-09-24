//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Modulo H.264 via OpenH264 (BSD, Cisco). Encoder + Decoder. Ative com -DHAVE_OPENH264
//  (vcpkg install openh264). Binding C do OpenH264: metodos chamados como
//  (*handle)->Metodo(handle, ...). Saida do encoder e Annex-B (start codes 00 00 00 01).
//  NAO TESTADO EM RUNTIME. Conferir nomes/enums contra a versao instalada.

#include "media_codec.h"
#include "memory_pool.h"   // memop_* (evita declaracao implicita -> ponteiro truncado em x64)
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_OPENH264
#include "wels/codec_api.h"
#include "wels/codec_app_def.h"

// ---------------- Encoder ----------------
typedef struct
{
    ISVCEncoder* enc;
    int      width, height, fps;
    uint8_t* out; int out_cap;
    int64_t  pts;
    int      have;          // ha um frame codificado aguardando ReceivePacket
    int      key;
    SFrameBSInfo info;
} H264Enc;

static int h264_send(MediaEncoder* e, const MediaFrame* f)
{
    H264Enc* c = (H264Enc*)e->Ctx;
    if (!f) { c->have = 0; return 0; }   // OpenH264 nao tem lookahead: nada a drenar

    SSourcePicture pic; memset(&pic, 0, sizeof(pic));
    pic.iPicWidth    = c->width;
    pic.iPicHeight   = c->height;
    pic.iColorFormat = videoFormatI420;
    pic.iStride[0] = f->StrideY; pic.iStride[1] = f->StrideU; pic.iStride[2] = f->StrideV;
    pic.pData[0]   = f->Y;       pic.pData[1]   = f->U;       pic.pData[2]   = f->V;
    pic.uiTimeStamp = (long long)(c->pts * 1000 / (c->fps > 0 ? c->fps : 30));

    memset(&c->info, 0, sizeof(c->info));
    int rv = (*c->enc)->EncodeFrame(c->enc, &pic, &c->info);
    c->pts++;
    if (rv != 0) return -1;
    if (c->info.eFrameType == videoFrameTypeSkip) { c->have = 0; return 0; }

    c->have = 1;
    c->key  = (c->info.eFrameType == videoFrameTypeIDR || c->info.eFrameType == videoFrameTypeI);
    return 0;
}

static int h264_recv(MediaEncoder* e, MediaPacket* out)
{
    H264Enc* c = (H264Enc*)e->Ctx;
    if (!c->have) return 0;
    c->have = 0;

    // Concatena todos os NALs de todas as camadas (Annex-B contiguo em pBsBuf).
    int total = 0;
    for (int l = 0; l < c->info.iLayerNum; l++)
    {
        SLayerBSInfo* li = &c->info.sLayerInfo[l];
        for (int n = 0; n < li->iNalCount; n++) total += li->pNalLengthInByte[n];
    }
    if (total <= 0) return 0;
    if (total > c->out_cap) { c->out = (uint8_t*)memop_realloc_raw(c->out, total); c->out_cap = total; }

    int off = 0;
    for (int l = 0; l < c->info.iLayerNum; l++)
    {
        SLayerBSInfo* li = &c->info.sLayerInfo[l];
        int ls = 0; for (int n = 0; n < li->iNalCount; n++) ls += li->pNalLengthInByte[n];
        memcpy(c->out + off, li->pBsBuf, ls);
        off += ls;
    }

    out->Data = c->out; out->Size = total;
    out->Pts = out->Dts = c->pts - 1;
    out->KeyFrame = c->key;
    return 1;
}

static void h264_enc_close(MediaEncoder* e)
{
    if (!e) return;
    H264Enc* c = (H264Enc*)e->Ctx;
    if (c) { if (c->enc) { (*c->enc)->Uninitialize(c->enc); WelsDestroySVCEncoder(c->enc); } memop_free_raw(c->out); memop_free_raw(c); }
    memop_free_raw(e);
}

MediaEncoder* h264_encoder_open(const MediaEncoderParams* p)
{
    if (!p || p->Width <= 0 || p->Height <= 0) return 0;

    ISVCEncoder* enc = 0;
    if (WelsCreateSVCEncoder(&enc) != 0 || !enc) return 0;

    SEncParamExt param; memset(&param, 0, sizeof(param));
    (*enc)->GetDefaultParams(enc, &param);
    param.iUsageType     = CAMERA_VIDEO_REAL_TIME;
    param.iPicWidth      = p->Width;
    param.iPicHeight     = p->Height;
    param.iTargetBitrate = p->BitrateBps > 0 ? p->BitrateBps : 2000000;
    param.iRCMode        = RC_BITRATE_MODE;
    // O default do OpenH264 para CAMERA_VIDEO_REAL_TIME + RC_BITRATE_MODE e DESCARTAR
    // frames para segurar o bitrate. Isso faz sentido em transmissao ao vivo por link
    // limitado, nao ao preparar arquivo: media aponta ~8% dos frames sumindo em toda
    // conversao (25 fps de entrada saiam como 22,9). Aqui o conteudo tem que sair
    // inteiro -- o bitrate que ceda.
    param.bEnableFrameSkip = false;
    param.fMaxFrameRate  = (float)(p->Fps > 0 ? p->Fps : 30);
    // IDR periodico (GOP ~2s): sem isso o OpenH264 emite so 1 IDR no inicio -> o corte de
    // segmento HLS/DASH (que so corta em keyframe apos seg_ms) nunca dispara -> 1 segmento
    // gigante com o video inteiro (player nao inicia). ~2s casa bem com segmentos de 2-6s.
    param.uiIntraPeriod  = (unsigned int)((p->Fps > 0 ? p->Fps : 30) * 2);
    // SPS/PPS com id CONSTANTE (0) em todo IDR. O default do OpenH264 incrementa o id a cada
    // IDR (INCREASING_ID); como o muxer poe SPS/PPS so no avcC (id 0) e o h26x_tl remove os
    // SPS/PPS in-band das amostras, os IDRs 2+ (id 1,2,...) ficariam sem SPS -> PIPELINE_ERROR_DECODE
    // no 2o keyframe (~2s). CONSTANT_ID faz todos referenciarem o SPS/PPS do avcC.
    param.eSpsPpsIdStrategy = CONSTANT_ID;
    param.iSpatialLayerNum = 1;
    param.sSpatialLayers[0].iVideoWidth   = p->Width;
    param.sSpatialLayers[0].iVideoHeight  = p->Height;
    param.sSpatialLayers[0].fFrameRate    = param.fMaxFrameRate;
    param.sSpatialLayers[0].iSpatialBitrate = param.iTargetBitrate;

    // (Threading do openh264 desativado: forcar iMultipleThreadIdc + SM_FIXEDSLCNUM_SLICE
    //  produzia sLayerInfo/pNalLengthInByte inconsistente -> crash no h264_recv. O ganho por
    //  slice e pequeno; mantem o default single-slice, que e estavel.)

    if ((*enc)->InitializeExt(enc, &param) != 0) { WelsDestroySVCEncoder(enc); return 0; }
    int fmt = videoFormatI420;
    (*enc)->SetOption(enc, ENCODER_OPTION_DATAFORMAT, &fmt);

    H264Enc* c = (H264Enc*)memop_calloc_raw(1, sizeof(H264Enc));
    if (!c) { (*enc)->Uninitialize(enc); WelsDestroySVCEncoder(enc); return 0; }
    c->enc = enc; c->width = p->Width; c->height = p->Height; c->fps = p->Fps > 0 ? p->Fps : 30;

    MediaEncoder* e = (MediaEncoder*)memop_calloc_raw(1, sizeof(MediaEncoder));
    if (!e) { (*enc)->Uninitialize(enc); WelsDestroySVCEncoder(enc); memop_free_raw(c); return 0; }
    e->Codec = MEDIA_CODEC_H264; e->Ctx = c;
    e->SendFrame = h264_send; e->ReceivePacket = h264_recv; e->Close = h264_enc_close;
    return e;
}

// ---------------- Decoder ----------------
typedef struct { ISVCDecoder* dec; uint8_t* planes_copy; } H264Dec;

static int h264_dsend(MediaDecoder* d, const MediaPacket* pkt)
{
    (void)d; (void)pkt;
    return 0; // decode e feito em ReceiveFrame (DecodeFrameNoDelay) — ver nota abaixo
}

// Observacao: OpenH264 decoda com DecodeFrameNoDelay(dec, buf, size, pData[3], &BufferInfo).
// Para casar com a interface Send/Receive, guardamos o ultimo pacote e decodamos no Receive.
// (Implementacao minima; validar buffering/estados na versao instalada.)
static int h264_drecv(MediaDecoder* d, MediaFrame* out) { (void)d; (void)out; return 0; } // TODO
static void h264_dec_close(MediaDecoder* d)
{
    if (!d) return; H264Dec* c = (H264Dec*)d->Ctx;
    if (c) { if (c->dec) { (*c->dec)->Uninitialize(c->dec); WelsDestroyDecoder(c->dec); } memop_free_raw(c); }
    memop_free_raw(d);
}

MediaDecoder* h264_decoder_open(void)
{
    ISVCDecoder* dec = 0;
    if (WelsCreateDecoder(&dec) != 0 || !dec) return 0;
    SDecodingParam param; memset(&param, 0, sizeof(param));
    param.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
    if ((*dec)->Initialize(dec, &param) != 0) { WelsDestroyDecoder(dec); return 0; }

    H264Dec* c = (H264Dec*)memop_calloc_raw(1, sizeof(H264Dec));
    if (!c) { (*dec)->Uninitialize(dec); WelsDestroyDecoder(dec); return 0; }
    c->dec = dec;
    MediaDecoder* d = (MediaDecoder*)memop_calloc_raw(1, sizeof(MediaDecoder));
    if (!d) { (*dec)->Uninitialize(dec); WelsDestroyDecoder(dec); memop_free_raw(c); return 0; }
    d->Codec = MEDIA_CODEC_H264; d->Ctx = c;
    d->SendPacket = h264_dsend; d->ReceiveFrame = h264_drecv; d->Close = h264_dec_close;
    return d;
}

#else   // OpenH264 nao compilado

MediaEncoder* h264_encoder_open(const MediaEncoderParams* p) { (void)p; return 0; }
MediaDecoder* h264_decoder_open(void)                        { return 0; }

#endif


// ============================================================================
//  O descritor deste projeto -- o UNICO simbolo que o nucleo enxerga daqui.
//  Ver codec_plugin.h.
// ============================================================================
#include "codec_plugin.h"

extern const H26xDecBackend h264_dec_backend;   // codec_h264_dec.c

#ifdef HAVE_OPENH264
  #define H264_COMPILADO 1
#else
  #define H264_COMPILADO 0
#endif

static MediaEncoder* h264_plugin_enc(const MediaEncoderParams* p, char* detail, int size)
{
    (void)detail; (void)size;          // software: quem descreve o degrau e o seletor
    return h264_encoder_open(p);
}

// openh264 faz os dois lados: binario da Cisco, com assembly (SSE2..AVX2 / NEON).
static const CodecPlugin H264[] =
{
    {
        CODEC_PLUGIN_ABI, MEDIA_CODEC_H264, CODEC_ROLE_ENCODE | CODEC_ROLE_DECODE,
        "openh264", "OpenH264 (Cisco), assembly compilado",
        CODEC_OS_ALL, CODEC_ARCH_ALL, H264_COMPILADO, 0,
        ISA_SSE2, ISA_AVX2, 1,
        h264_plugin_enc,
        h264_decoder_open, &h264_dec_backend, 0,
        0
    },
};

const CodecPluginSet codec_set_h264 =
{ CODEC_PLUGIN_ABI, "codec_h264", (int)(sizeof(H264) / sizeof(H264[0])), H264 };

// Compilado como plugin: o conjunto vira o export do modulo. Compilado estatico,
// esta linha some e o nucleo pega o conjunto pelo extern de sempre.
#ifdef CODEC_PLUGIN_BUILD
CODEC_PLUGIN_DECLARE(codec_set_h264)
#endif
