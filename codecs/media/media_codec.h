//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Camada de codecs EMBUTIDA (sem ffmpeg). Uma lib por codec, um wrapper central
//  e um enum de selecao. Esta e a camada de ENCODE/DECODE (frame <-> pacote).
//  O pipeline completo "sem ffmpeg" tambem precisa de: demux (MP4), e mux/segmentacao
//  (WebM/DASH/HLS) - modulos separados (ver media/README.md).
//
//  Topologia:
//     MP4 --demux--> pacote --[decoder]--> MediaFrame (YUV) --scale-->
//         MediaFrame --[encoder]--> MediaPacket (bitstream) --mux--> WebM/MP4/DASH

#pragma once

#include <stdint.h>
#include <stddef.h>

// Selecao de codec (o "enum de selecao" pedido).
typedef enum
{
    MEDIA_CODEC_NONE = 0,
    MEDIA_CODEC_H264,   // AVC   (video)
    MEDIA_CODEC_H265,   // HEVC  (video)  -- ver ressalva de licenca (GPL/patente)
    MEDIA_CODEC_VP9,    //       (video)
    MEDIA_CODEC_AV1,    //       (video)
    MEDIA_CODEC_OPUS,   //       (audio)
    MEDIA_CODEC_AAC     //       (audio)  -- normalmente so decode na entrada
} MediaCodec;

typedef enum { MEDIA_KIND_VIDEO = 0, MEDIA_KIND_AUDIO = 1 } MediaKind;

// Formato cru: video planar I420 (YUV 4:2:0) ou audio PCM S16 intercalado.
typedef struct
{
    MediaKind Kind;

    // --- video (I420) ---
    int      Width, Height;
    uint8_t* Y; int StrideY;
    uint8_t* U; int StrideU;
    uint8_t* V; int StrideV;

    // --- audio (PCM S16) ---
    int      SampleRate, Channels;
    int16_t* Pcm; int PcmSamples;   // amostras por canal

    // --- comum ---
    int64_t  Pts;        // na base de tempo do encoder (1/Fps por padrao)
    int      KeyFrame;   // hint: forcar keyframe
} MediaFrame;

// Pacote comprimido (bitstream do codec) produzido pelo encoder.
typedef struct
{
    uint8_t* Data; int Size;   // Data valido ate a proxima chamada de SendFrame (ou Copie).
    int64_t  Pts, Dts;
    int      KeyFrame;
} MediaPacket;

// Aceleracao pedida pelo chamador. O padrao (0) deixa o seletor decidir.
typedef enum
{
    MEDIA_ACCEL_AUTO     = 0,   // hardware -> GPU -> SIMD -> threads, o que existir
    MEDIA_ACCEL_SOFTWARE = 1,   // pula os degraus de hardware (ex.: saida que precisa de
                                //   um mesmo encoder em muitas tasks paralelas)
    MEDIA_ACCEL_HARDWARE = 2    // so hardware: falha se nao houver (diagnostico/teste)
} MediaAccel;

// Parametros de abertura.
typedef struct
{
    MediaCodec Codec;

    // video
    int Width, Height;
    int BitrateBps;
    int Fps;
    int SpeedPreset;     // 0..N (rapido..lento) - cada codec interpreta a seu modo
    int Threads;         // 0 = default do codec; >0 = fixa g_threads (pool paraleliza entre renditions)
    int Accel;           // MediaAccel; 0 = AUTO (ver enc_select.h)

    // audio
    int SampleRate, Channels;
} MediaEncoderParams;

// ---- Encoder: interface (vtable) que CADA codec implementa -----------------
typedef struct MediaEncoder MediaEncoder;
struct MediaEncoder
{
    MediaCodec Codec;
    void*      Ctx;   // estado interno do modulo do codec

    // Preenchidos pelo seletor (media_encoder_open): QUEM atendeu. Servem para a UI e
    // o log mostrarem se a GPU foi usada -- e, se nao, que degrau entrou no lugar.
    const char* Backend;             // "mf-hw", "openh264", "x265", ...
    int         Tier;                // EncTier (enc_select.h)
    char        BackendDetail[96];   // ex.: "NVIDIA H.264 Encoder MFT", "AVX2"

    // Envia 1 frame para o encoder. frame == NULL -> flush/drain (fim do stream).
    // Retorna 0 em sucesso, <0 em erro.
    int  (*SendFrame)(MediaEncoder* e, const MediaFrame* frame);

    // Puxa 1 pacote de saida. Retorna 1 = preencheu out; 0 = precisa de mais input;
    // <0 = erro. Chame em laco ate retornar 0 apos cada SendFrame.
    int  (*ReceivePacket)(MediaEncoder* e, MediaPacket* out);

    void (*Close)(MediaEncoder* e);
};

// ---- Decoder: interface (vtable) - pacote comprimido -> MediaFrame ----------
typedef struct MediaDecoder MediaDecoder;
struct MediaDecoder
{
    MediaCodec Codec;
    void*      Ctx;

    int  (*SendPacket)(MediaDecoder* d, const MediaPacket* pkt); // pkt NULL = flush
    int  (*ReceiveFrame)(MediaDecoder* d, MediaFrame* out);
    void (*Close)(MediaDecoder* d);
};

// ---- Wrapper central (dispatch por enum) -----------------------------------
// Abre o encoder/decoder do codec escolhido. O ENCODER passa pela cascata de
// enc_select.c (hardware -> GPU -> SIMD -> threads). Retorna NULL se o codec nao estiver
// compilado (a lib correspondente nao foi linkada) ou os parametros forem invalidos.
MediaEncoder* media_encoder_open(const MediaEncoderParams* params);
MediaDecoder* media_decoder_open(MediaCodec codec);

// Utilitarios.
const char* media_codec_name(MediaCodec codec);
int         media_codec_available(MediaCodec codec);   // 1 se o ENCODER do codec foi compilado
int         media_decoder_available(MediaCodec codec); // 1 se o DECODER do codec foi compilado
