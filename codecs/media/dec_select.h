//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Registro dos backends de decode H26x -- o espelho do enc_select.h, que ja fazia isso
//  para o encode.
//
//  Antes, o codec_video.c decidia com um `switch (codec->Type)` espalhado por quatro
//  funcoes, e o openh264 e o de265 estavam no MESMO arquivo. Com a tabela, o nucleo nao
//  conhece nenhum dos dois: ele percorre a lista e usa quem atende o codec pedido. E o que
//  permite cada codec virar projeto (e depois plugin) sem que ninguem acima perceba.
//
//  Este header e INTERNO da camada de codecs. Quem esta de fora continua usando o
//  codec_video.h, que nao mudou.

#ifndef DEC_SELECT_H
#define DEC_SELECT_H

#include "codec_video.h"

#ifdef __cplusplus
extern "C" {
#endif

// A interface que CADA backend de decode implementa. Trabalha em lote (um buffer entra,
// N imagens saem), que e o formato que o pipeline ja usa.
typedef struct
{
    int         Codec;    // 264 ou 265, como o h26x_decoder_create recebe
    const char* Name;     // "openh264", "libde265" -- para log e diagnostico

    // Cria o decoder do backend. NULL em falha.
    void* (*Create)(void);

    // Entrega um buffer (Annex-B) e acrescenta em 'images' o que sair.
    // Retorna >= 0 em sucesso, < 0 em erro.
    int   (*Decode)(void* inst, uint_fast8_t* data, int size, ImagePlaneList* images);

    // Drena o que o decoder segurou para reordenar. Retorna quantos frames sairam.
    // Depois do flush o decoder nao aceita mais dados: so se chama no fim do fluxo.
    int   (*Flush)(void* inst, ImagePlaneList* images);

    void  (*Release)(void* inst);
}
H26xDecBackend;

// Quem atende esse codec (264/265), ou NULL se nao houver backend compilado.
const H26xDecBackend* h26x_backend_for(int codec);

// Nome do backend que esta decodificando essa instancia ("openh264", ...), ou "" se nao
// houver. Serve para o log dizer QUEM decodificou, como o encode ja diz.
const char* h26x_decoder_backend(const DecoderInstance* decode);

// Copia de planos para memoria propria -- usada pelos backends, definida no image_plane.c.
ImagePlane* imagep_copy_new(uint8_t* const src[3], const int stride[3], int w, int h);

#ifdef __cplusplus
}
#endif
#endif // DEC_SELECT_H
