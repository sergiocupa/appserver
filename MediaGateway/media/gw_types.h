//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Tipos do gateway: pacote, info de stream, perfil de saida, pista de saida.
//  Ver media/SYNC_GATEWAY.md.

#pragma once
#include "gw_base.h"
#include "media_codec.h"   // MediaCodec
#include "pipeline.h"      // PipeTrack (renditions)

typedef enum { MSTREAM_VIDEO = 0, MSTREAM_AUDIO = 1 } MediaStreamType;

// Formato de pixel dos frames CRUS (bypass de decode). Empacotamento contiguo em GwPacket.Data.
typedef enum { GW_PIX_NONE = 0, GW_PIX_I420, GW_PIX_NV12, GW_PIX_YUYV, GW_PIX_RGB24 } GwPixFmt;

typedef struct
{
    MediaStreamType Type;
    // Raw=1: frames CRUS (sem codec) -> o gateway PULA o decode (bypass). Codec fica NONE.
    // Raw=0: bitstream codificado -> Codec identifica (ou foi auto-detectado).
    int             Raw;
    MediaCodec      Codec;                 // codec de ENTRADA (NONE se Raw)
    GwPixFmt        PixFmt;                 // formato do frame cru (video, quando Raw)
    int    Width, Height; double Fps;      // video
    int    SampleRate, Channels;           // audio (PCM S16 quando Raw)
    const uint8_t* Extra; int ExtraLen;    // SPS/PPS/VPS/ASC (opcional; so Raw=0)
} MediaStreamInfo;

typedef struct                              // pacote codificado (entrada ou saida)
{
    int       Stream;                       // indice do stream de origem
    uint8_t*  Data; int Size;               // valido ate a proxima chamada do produtor
    mtime_us  Pts, Dts, Dur;                // Dur = duracao do sample (us); 0 se desconhecida
    int       KeyFrame;
} GwPacket;

typedef enum { CONT_DASH_WEBM, CONT_DASH_MP4, CONT_HLS_FMP4, CONT_MP4_FILE,
               CONT_LIVE_MEM   // ao vivo: fMP4 numa janela em memoria (live_store), sem disco
             } Container;
typedef enum { GW_VOD = 0, GW_LIVE = 1 } GwMode;

typedef struct
{
    Container   Container;
    MediaCodec  VideoCodec;                 // saida (H264/H265/VP9/AV1); NONE = herda a entrada
    MediaCodec  AudioCodec;                 // saida (OPUS p/ webm; AAC passthrough p/ mp4/hls)
    int         SegmentMs;                  // duracao alvo do segmento
    GwMode      Mode;

    // ---- parametros de saida ----------------------------------------------
    // Convencao: 0 = "nao escolhido, herda da entrada". Quando TODOS sao 0/NONE e nao ha
    // renditions, o gateway trabalha em BYPASS (remux puro, sem decode/encode) -- e o que
    // torna "nada selecionado" barato de verdade em vez de um reencode disfarcado.
    int         Width, Height;              // 0 = resolucao da entrada
    int         Fps;                        // 0 = taxa da entrada; senao reamostra por PTS
    int         BitrateBps;                 // 0 = estimado pela resolucao

    const PipeTrack* Renditions; int RenditionCount;   // multi-resolucao (0 = usa os campos acima)

    // CONT_LIVE_MEM: o LiveStore que recebe os segmentos. Fica aqui porque quem abre o sink
    // e' o gateway (media_sink_open), e o destino ao vivo nao e' um caminho em disco.
    void* Live;
} MediaProfile;

typedef struct                              // uma pista de saida ja configurada (para o sink)
{
    MediaStreamType Type; MediaCodec Codec;
    int Width, Height; double Fps;          // video
    int Bandwidth;                          // bps (video: p/ BANDWIDTH do manifesto)
    int SampleRate, Channels;               // audio
    const uint8_t* Extra; int ExtraLen;     // avcC/hvcC/OpusHead/ASC gerado no encode
    const char* Name;                       // rotulo da rendition (ex.: "720p")
} MediaTrackOut;
