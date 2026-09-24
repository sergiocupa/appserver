//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  device_enum: descoberta das CAPACIDADES das fontes de entrada, Windows + Linux.
//
//  A UI precisa, para cada dispositivo, do conjunto real de combinacoes que ele aceita:
//  formato (codec ou pixel format) x resolucao x fps. Nao sao tres listas independentes --
//  uma camera pode dar 1920x1080 a 30fps em MJPG mas so 5fps em YUY2 --, entao a estrutura
//  e uma ARVORE (device -> stream -> resolucao -> fps) e uma unica consulta traz tudo.
//
//  Backends: Media Foundation (MF_DEVSOURCE + IMFMediaTypeHandler) no Windows;
//  V4L2 (ENUM_FMT / ENUM_FRAMESIZES / ENUM_FRAMEINTERVALS) no Linux.

#pragma once

#include "gw_types.h"   // MediaCodec, GwPixFmt

#define DEV_MAX_FPS      24
#define DEV_MAX_RES      64
#define DEV_MAX_STREAM   16
#define DEV_MAX_DEVICE   16

typedef struct
{
    int    Width, Height;
    double Fps[DEV_MAX_FPS];
    int    FpsCount;            // 0 = o driver nao informou taxas para esta resolucao
}
DevResolution;

typedef struct
{
    char       Label[16];       // rotulo do formato como o driver o expoe: "NV12", "MJPG", "H264"
    int        Raw;             // 1 = frames crus (PixFmt valido); 0 = bitstream (Codec valido)
    MediaCodec Codec;           // MEDIA_CODEC_NONE quando Raw, ou quando o codec nao e suportado
    GwPixFmt   PixFmt;          // GW_PIX_NONE quando nao e cru (ou formato cru desconhecido)
    int        Supported;       // 1 = o gateway consegue consumir este stream hoje
    DevResolution Res[DEV_MAX_RES];
    int        ResCount;
}
DevStream;

typedef struct
{
    char Id[320];               // caminho simbolico (Windows) ou /dev/videoN (Linux)
    char Name[160];             // nome amigavel para a UI
    char Kind[16];              // "camera" | "file"
    DevStream Streams[DEV_MAX_STREAM];
    int  StreamCount;
}
DevInfo;

// ATENCAO: ~3 MB. NAO declarar em variavel local -- estoura a pilha de 1 MB da thread
// (o servidor atende cada requisicao numa thread propria). Use device_list_new/free.
typedef struct
{
    DevInfo Items[DEV_MAX_DEVICE];
    int     Count;
}
DevList;

// Aloca/libera um DevList no memory_pool da xplatbase.
DevList* device_list_new(void);
void     device_list_free(DevList* l);

// Enumera as cameras de captura de video da maquina. Retorna a quantidade encontrada
// (0 e um resultado valido: maquina sem camera), ou <0 se o subsistema nao inicializou.
int device_enum_video(DevList* out);

// Mapeia um FOURCC de driver (ex.: 'NV12', 'MJPG', 'H264') para o modelo do gateway.
// Preenche raw/codec/pixfmt/supported. Exposto para o backend de captura reaproveitar.
void device_map_fourcc(unsigned int fourcc, DevStream* out);
