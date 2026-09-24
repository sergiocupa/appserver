//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  A escolha do backend de decode e o ciclo de vida do DecoderInstance. Ver dec_select.h.
//
//  Quem diz o que existe neste build sao os DESCRITORES que cada projeto de codec exporta
//  (codec_plugin.h). Nenhuma funcao daqui sabe o nome de openh264 ou de265.

#include "dec_select.h"
#include "codec_plugin.h"
#include "memory_pool.h"

// Nao ha mais lista aqui. Os backends vem dos descritores que cada projeto de codec
// exporta (ver codec_plugin.h): o nucleo procura quem atende 264/265 no papel de decode e
// traz um backend em lote. Antes esta funcao carregava `extern const H26xDecBackend
// h264_dec_backend` e `h265_dec_backend` -- ou seja, o nome de duas funcoes de codec.

// Acha o backend em lote E de qual conjunto ele veio. O conjunto e o que segura o modulo
// enquanto o DecoderInstance viver: aqui o ciclo de vida e na mao (create/release), nao ha
// vtable com Close para desviar.
static const H26xDecBackend* procura(int codec, const CodecPluginSet** dono)
{
    MediaCodec mc = codec == 264 ? MEDIA_CODEC_H264 :
                    codec == 265 ? MEDIA_CODEC_H265 : MEDIA_CODEC_NONE;
    if (dono) *dono = 0;
    if (mc == MEDIA_CODEC_NONE) return 0;

    codec_plugin_ensure(mc);

    for (int i = 0, n = codec_plugin_count(); i < n; i++)
    {
        const CodecPlugin* p = codec_plugin_at(i);
        // O primeiro que atende o codec pode ser o de hardware, que nao traz decode em
        // lote: interessa quem tem H26x.
        if (p->Codec == mc && (p->Role & CODEC_ROLE_DECODE) && p->Compiled && p->H26x)
        {
            if (dono) *dono = codec_plugin_set_of(p);
            return p->H26x;
        }
    }
    return 0;
}

const H26xDecBackend* h26x_backend_for(int codec)
{
    return procura(codec, 0);
}


const char* h26x_decoder_backend(const DecoderInstance* decode)
{
    const H26xDecBackend* b = decode ? h26x_backend_for(decode->Type) : 0;
    return b ? b->Name : "";
}

// ---- ciclo de vida ---------------------------------------------------------

DecoderInstance* h26x_decoder_create(int codec)
{
    const CodecPluginSet* dono = 0;
    const H26xDecBackend* b = procura(codec, &dono);
    if (!b) return 0;

    void* inst = b->Create();
    // Antes, uma falha aqui devolvia um DecoderInstance com Instance == NULL, e o decode
    // seguinte fazia deref nele. Agora a falha e falha.
    if (!inst) return 0;

    DecoderInstance* result = (DecoderInstance*)memop_alloc_raw(sizeof(DecoderInstance));
    if (!result) { b->Release(inst); return 0; }

    result->ID          = 0;
    result->Initialized = 1;
    result->Media       = MEDIA_TYPE_VIDEO;
    result->Type        = codec;
    result->Instance    = inst;
    codec_use_begin(dono);     // o modulo fica preso ate o release
    return result;
}

int h26x_decode_frames(DecoderInstance* codec, MediaBuffer* input, ImagePlaneList* images)
{
    if (!codec || !input || !images) return -1;

    const H26xDecBackend* b = h26x_backend_for(codec->Type);
    if (!b) return -2;

    b->Decode(codec->Instance, input->Data, input->Size, images);
    return 0;
}

// Drena o que o decoder segurou para reordenar. Sem isso, H.264/H.265 com B-frames (video
// de celular/camera) ou que nao declaram num_reorder_frames=0 (o fluxo do NVENC) perdiam
// os ultimos frames em TODA conversao -- medido: 149/150 no H.264 do NVENC, 26/30 no HEVC.
// Depois do flush o decoder nao aceita mais dados: so se chama no fim do fluxo.
int h26x_decoder_flush(DecoderInstance* codec, ImagePlaneList* images)
{
    if (!codec || !images) return -1;

    const H26xDecBackend* b = h26x_backend_for(codec->Type);
    if (!b) return -2;

    return b->Flush(codec->Instance, images);
}

int h26x_decoder_release(DecoderInstance** decode)
{
    const CodecPluginSet* dono = 0;
    const H26xDecBackend* b;

    if (!decode || !*decode) return -1;

    b = procura((*decode)->Type, &dono);
    if (!b) return -1;

    b->Release((*decode)->Instance);
    codec_use_end(dono);
    memop_free_raw(*decode);
    *decode = 0;
    return 0;
}
