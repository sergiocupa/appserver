//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Wrapper central dos codecs: seleciona o modulo por enum (MediaCodec).
//  Cada modulo (codec_vp9.c, codec_av1.c, ...) expoe *_encoder_open / *_decoder_open.
//  Se a lib do codec nao estiver compilada (HAVE_* nao definido), o modulo retorna NULL.

#include "media_codec.h"
#include "codec_plugin.h"

// media_encoder_open vive em enc_select.c: la ele deixa de ser um switch por codec e vira
// a cascata hardware -> GPU -> SIMD -> threads.
//
// Aqui tambem nao ha mais switch: antes este arquivo declarava h264_decoder_open,
// h265_decoder_open, vp9_decoder_open e av1_decoder_open -- quatro nomes de funcao de
// codec dentro do nucleo. Agora ele pergunta ao registro.

MediaDecoder* media_decoder_open(MediaCodec codec)
{
    codec_plugin_ensure(codec);
    for (int i = 0, n = codec_plugin_count(); i < n; i++)
    {
        const CodecPlugin* p = codec_plugin_at(i);
        // O primeiro que atende pode ser o de hardware, que nao abre por esta via.
        if (p->Codec == codec && (p->Role & CODEC_ROLE_DECODE) && p->Compiled && p->DecoderOpen)
        {
            MediaDecoder* d = p->DecoderOpen();
            if (d) codec_use_track_decoder(codec_plugin_set_of(p), d);
            return d;
        }
    }
    return 0;
}

const char* media_codec_name(MediaCodec codec)
{
    switch (codec)
    {
        case MEDIA_CODEC_H264: return "H.264";
        case MEDIA_CODEC_H265: return "H.265";
        case MEDIA_CODEC_VP9:  return "VP9";
        case MEDIA_CODEC_AV1:  return "AV1";
        case MEDIA_CODEC_OPUS: return "Opus";
        case MEDIA_CODEC_AAC:  return "AAC";
        default:               return "none";
    }
}

// Disponibilidade de ENCODE: existe backend compilado para esse codec?
// Os #ifdef HAVE_* que ficavam aqui agora sao o campo Compiled de cada descritor, no
// projeto do proprio codec -- que e quem sabe se a lib dele entrou no build.
int media_codec_available(MediaCodec codec)
{
    codec_plugin_ensure(codec);
    for (int i = 0, n = codec_plugin_count(); i < n; i++)
    {
        const CodecPlugin* p = codec_plugin_at(i);
        if (p->Codec == codec && (p->Role & CODEC_ROLE_ENCODE) && p->Compiled && p->EncoderOpen)
            return 1;
    }
    return 0;
}

// Disponibilidade de DECODE, pela via da vtable (media_decoder_open). Precisa ser separada
// do encode porque as duas metades vem de libs DIFERENTES em dois codecs: AV1 codifica com
// SVT-AV1 e decodifica com dav1d (a Intel removeu o decoder do SVT), e o H.265 codifica com
// x265 mas ainda nao tem decoder nesta via -- o HEVC de entrada passa pelo caminho h26x
// (ver dec_select.c), que e outra coisa.
int media_decoder_available(MediaCodec codec)
{
    codec_plugin_ensure(codec);
    for (int i = 0, n = codec_plugin_count(); i < n; i++)
    {
        const CodecPlugin* p = codec_plugin_at(i);
        if (p->Codec == codec && (p->Role & CODEC_ROLE_DECODE) && p->Compiled && p->DecoderOpen)
            return 1;
    }
    return 0;
}
