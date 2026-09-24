//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Modulo Opus (audio) embutido via libopus (BSD). Ative com -DHAVE_LIBOPUS.
//  Recebe MediaFrame com PCM S16 intercalado e produz pacotes Opus de 20 ms
//  (para o mux WebM junto do VP9/AV1). NAO TESTADO EM RUNTIME.

#include "media_codec.h"
#include "memory_pool.h"   // memop_* (evita declaracao implicita -> ponteiro truncado em x64)
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_LIBOPUS
#include <opus.h>

typedef struct
{
    OpusEncoder* enc;
    int      rate, ch;
    int      frame;       // amostras/canal por pacote (20 ms)
    int16_t* acc;         // acumulador intercalado (frame*ch)
    int      fill;        // amostras/canal ja acumuladas
    uint8_t  out[4000];   // 1 pacote Opus
    int64_t  samples_ts;  // total de amostras enviadas (p/ pts em ms)
} OpusEnc;

static int opus_send(MediaEncoder* e, const MediaFrame* f)
{
    OpusEnc* c = (OpusEnc*)e->Ctx;
    if (!f) return 0;  // flush: pacotes parciais sao descartados (simplificacao)

    int idx = 0;
    while (idx < f->PcmSamples)
    {
        int take = f->PcmSamples - idx;
        if (take > c->frame - c->fill) take = c->frame - c->fill;
        memcpy(c->acc + (size_t)c->fill * c->ch,
               f->Pcm + (size_t)idx * c->ch,
               (size_t)take * c->ch * sizeof(int16_t));
        c->fill += take;
        idx     += take;
        // (a codificacao de fato ocorre em ReceivePacket, quando ha 1 frame cheio)
        if (c->fill == c->frame) break;
    }
    return 0;
}

static int opus_recv(MediaEncoder* e, MediaPacket* out)
{
    OpusEnc* c = (OpusEnc*)e->Ctx;
    if (c->fill < c->frame) return 0;   // ainda nao fechou 20 ms

    int n = opus_encode(c->enc, c->acc, c->frame, c->out, sizeof(c->out));
    c->fill = 0;
    if (n < 0) return -1;

    out->Data     = c->out;
    out->Size     = n;
    out->Pts      = out->Dts = (c->samples_ts * 1000) / c->rate; // ms
    out->KeyFrame = 1; // audio: todo pacote e "key"
    c->samples_ts += c->frame;
    return 1;
}

static void opus_close_(MediaEncoder* e)
{
    if (!e) return;
    OpusEnc* c = (OpusEnc*)e->Ctx;
    if (c) { if (c->enc) opus_encoder_destroy(c->enc); memop_free_raw(c->acc); memop_free_raw(c); }
    memop_free_raw(e);
}

MediaEncoder* opus_encoder_open(const MediaEncoderParams* p)
{
    if (!p || p->SampleRate <= 0 || p->Channels <= 0) return 0;

    OpusEnc* c = (OpusEnc*)memop_calloc_raw(1, sizeof(OpusEnc));
    if (!c) return 0;
    c->rate  = p->SampleRate;
    c->ch    = p->Channels;
    c->frame = p->SampleRate / 50;               // 20 ms
    c->acc   = (int16_t*)memop_alloc_raw((size_t)c->frame * c->ch * sizeof(int16_t));
    if (!c->acc) { memop_free_raw(c); return 0; }

    int err = 0;
    c->enc = opus_encoder_create(p->SampleRate, p->Channels, OPUS_APPLICATION_AUDIO, &err);
    if (err != OPUS_OK || !c->enc) { memop_free_raw(c->acc); memop_free_raw(c); return 0; }
    opus_encoder_ctl(c->enc, OPUS_SET_BITRATE(p->BitrateBps > 0 ? p->BitrateBps : 128000));

    MediaEncoder* e = (MediaEncoder*)memop_calloc_raw(1, sizeof(MediaEncoder));
    if (!e) { opus_encoder_destroy(c->enc); memop_free_raw(c->acc); memop_free_raw(c); return 0; }
    e->Codec = MEDIA_CODEC_OPUS; e->Ctx = c;
    e->SendFrame = opus_send; e->ReceivePacket = opus_recv; e->Close = opus_close_;
    return e;
}

#else   // libopus nao compilada

MediaEncoder* opus_encoder_open(const MediaEncoderParams* p) { (void)p; return 0; }

#endif


// ============================================================================
//  O descritor deste projeto -- o UNICO simbolo que o nucleo enxerga daqui.
//  Ver codec_plugin.h.
// ============================================================================
#include "codec_plugin.h"

#ifdef HAVE_LIBOPUS
  #define OPUS_COMPILADO 1
#else
  #define OPUS_COMPILADO 0
#endif

static MediaEncoder* opus_plugin_enc(const MediaEncoderParams* p, char* detail, int size)
{
    (void)detail; (void)size;
    return opus_encoder_open(p);
}

// Audio: nao ha degrau de hardware nem decoder nesta camada.
static const CodecPlugin OPUS[] =
{
    {
        CODEC_PLUGIN_ABI, MEDIA_CODEC_OPUS, CODEC_ROLE_ENCODE,
        "libopus", "libopus",
        CODEC_OS_ALL, CODEC_ARCH_ALL, OPUS_COMPILADO, 0,
        ISA_NONE, ISA_NONE, 0,
        opus_plugin_enc,
        0, 0, 0,
        0
    },
};

const CodecPluginSet codec_set_opus =
{ CODEC_PLUGIN_ABI, "codec_opus", (int)(sizeof(OPUS) / sizeof(OPUS[0])), OPUS };

// Compilado como plugin: o conjunto vira o export do modulo. Compilado estatico, esta
// linha some e o nucleo pega o conjunto pelo extern de sempre.
#ifdef CODEC_PLUGIN_BUILD
CODEC_PLUGIN_DECLARE(codec_set_opus)
#endif
