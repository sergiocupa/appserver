//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  MediaSource: interface PULL de entrada (arquivo agora; camera na fase 2).
//  Entrega GwPacket ja intercalado por tempo (pts em us). Ver media/SYNC_GATEWAY.md.

#pragma once
#include "gw_types.h"

typedef struct MediaSource MediaSource;
struct MediaSource
{
    void* Ctx;
    int  (*Info)(MediaSource* s, MediaStreamInfo* out, int max, int* count);
    int  (*Read)(MediaSource* s, GwPacket* pkt);   // 1=ok, 0=EOF, <0=erro; live: bloqueia
    int  (*IsLive)(MediaSource* s);
    void (*Close)(MediaSource* s);
};

// Fonte de ARQUIVO MP4 (fase 1): demux H.264/H.265 + AAC via MediaFragmenter/audio_aac,
// intercalando video/audio por pts. Retorna NULL em falha.
MediaSource* source_mp4_open(const char* path);

// Fonte de ARQUIVO WebM/Matroska: demux VP9/AV1/H26x + Opus/AAC. Retorna NULL em falha.
MediaSource* source_webm_open(const char* path);

// Abre o arquivo escolhendo o demux pelo CONTEUDO (assinatura), com a extensao apenas
// como desempate. E o que os controllers devem chamar: sem isso cada chamador teria de
// adivinhar o container, e foi assim que .webm nunca chegou aos decoders de VP9/AV1.
MediaSource* source_file_open(const char* path);

// Modo de interpretacao da entrada (camera pode ser cru OU codificado).
typedef enum
{
    SRC_AUTO = 0,   // detecta sozinho se e' cru ou codificado, e qual codec (recomendado)
    SRC_RAW,        // forca frames CRUS (bypass de decode; sem codec)
    SRC_ENCODED     // forca bitstream codificado (Codec dado ou auto-detectado)
}
SourceMode;

typedef struct CameraParams
{
    const char* Device;      // identificador do dispositivo/URL da camera
    SourceMode  Mode;        // AUTO / RAW / ENCODED
    MediaCodec  Codec;       // dica de codec (NONE = auto-detectar quando ENCODED/AUTO)
    GwPixFmt    PixFmt;      // formato do frame cru (quando RAW/AUTO detectar cru)
    int Width, Height; double Fps;
    int SampleRate, Channels;
}
CameraParams;

// Fonte de CAMERA / stream continuo (fase 2). A API ja congela AUTO/RAW/ENCODED.
MediaSource* source_camera_open(const CameraParams* p);

// Detector (sniffer) de 1 buffer de entrada: decide cru vs codificado e o codec.
// Preenche *is_raw (1=cru) e retorna o MediaCodec (NONE se cru ou desconhecido).
// H.264/H.265 por tipo de NAL (Annex-B); MJPEG por SOI (FF D8); caso contrario -> cru.
MediaCodec gw_sniff_stream(const uint8_t* data, int size, int* is_raw);
