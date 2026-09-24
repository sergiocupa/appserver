//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Decode de video na GPU: interface unica, uma implementacao por plataforma.
//    - Windows: hw_dec_mf.c    (MFT de decode da Microsoft + gerenciador D3D11 = DXVA)
//    - Linux:   hw_dec_vaapi.c (VAAPI)
//
//  So se considera "GPU" quando a GPU expoe o perfil de decode do codec. Um decoder que
//  aceitasse a GPU mas decodificasse em software (MFT de extensao numa GPU sem o perfil)
//  faria o seletor mentir.
//
//  Os frames saem copiados para memoria propria em I420 -- o formato da cadeia -- e valem
//  ate a proxima chamada. A leitura de volta da GPU tem custo; o ganho inteiro do decode por
//  GPU so aparece quando o frame fica na GPU ate um encoder na mesma GPU.

#pragma once
#include "media_codec.h"

typedef struct HwDec HwDec;

// >0 se ha decode por GPU para o codec nesta maquina. 'detail' recebe o que foi achado
// (decoder + GPU) ou por que nao ha.
int         hw_dec_probe(MediaCodec codec, char* detail, int size);

// NULL se nao houver GPU/perfil/decoder para o codec.
HwDec*      hw_dec_open(MediaCodec codec);

// Entrega 1 pacote (Annex-B para H.264/H.265). data == NULL faz o drain (fim do fluxo).
int         hw_dec_send(HwDec* d, const uint8_t* data, int size);

// 1 = um frame I420 em planes/strides/w/h; 0 = precisa de mais entrada (ou drain concluido);
// <0 = erro. Chame em laco ate 0 depois de cada send.
int         hw_dec_next(HwDec* d, uint8_t** planes, int* strides, int* w, int* h);

void        hw_dec_close(HwDec** d);

// Nome do que esta decodificando (ex.: "Microsoft H264 Video Decoder MFT via DXVA (NVIDIA ...)").
const char* hw_dec_name(const HwDec* d);
