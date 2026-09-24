//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Decode de VIDEO unificado do gateway: um so ponto que atende H264/H265/VP9/AV1.
//
//  Antes o sync_gateway chamava h26x_decode_frames() direto, o que amarrava o pipeline a
//  openh264/de265: os decoders de VP9 (libvpx) e AV1 (dav1d) existiam e passavam nos testes
//  de round-trip, mas NAO tinham como receber um arquivo -- nenhum caminho do gateway
//  chegava ate eles. Este modulo e esse caminho.
//
//  Duas implementacoes atras da mesma interface:
//    - H264/H265 -> h26x_decoder_create/h26x_decode_frames (openh264 / libde265)
//    - VP9/AV1   -> media_decoder_open (vtable SendPacket/ReceiveFrame de media_codec.h)
//
//  A interface e de ITERADOR (send + next), nao de lista, de proposito: o decoder de VP9
//  reaproveita UM unico buffer de saida entre frames, entao uma lista com dois frames do
//  mesmo pacote teria os dois apontando para o mesmo pixel. Iterando, cada frame e
//  consumido antes do proximo existir.
//
//  Os planos pertencem ao DECODER e valem ate a proxima chamada -- copie (e o que
//  sp_seg_append e encode_frame ja fazem); nunca os guarde entre pacotes.

#pragma once
#include "gw_base.h"
#include "media_codec.h"

typedef struct                 // 1 frame decodificado, I420 planar
{
    uint8_t* Planes[3];
    int      Strides[3];
    int      Width, Height;
} GwImage;

typedef struct GwVideoDec GwVideoDec;

// Abre o decoder do codec de ENTRADA. NULL se o codec nao for suportado/compilado.
GwVideoDec* gw_vdec_open(MediaCodec codec);

// Igual, escolhendo a aceleracao (MediaAccel): AUTO tenta a GPU e cai para o software;
// SOFTWARE pula a GPU; HARDWARE so GPU (NULL se nao houver, e sem queda para software).
GwVideoDec* gw_vdec_open_accel(MediaCodec codec, int accel);

// Quem esta decodificando: o decoder de software ou o caminho de GPU (para log e UI).
const char* gw_vdec_backend(const GwVideoDec* d);

// Entrega 1 pacote codificado ao decoder. data==NULL faz o flush (drena o que ficou
// pendente por reordenacao). Retorna 0 em sucesso, <0 em erro.
int gw_vdec_send(GwVideoDec* d, const uint8_t* data, int size);

// Puxa o proximo frame do pacote enviado. 1 = preencheu *out; 0 = acabou (mande outro
// pacote); <0 = erro. Um pacote pode render 0 frames (so param sets) ou mais de um.
int gw_vdec_next(GwVideoDec* d, GwImage* out);

void gw_vdec_close(GwVideoDec** d);

// 1 se existe decoder compilado para esse codec (sem abrir nada).
int gw_vdec_available(MediaCodec codec);
