//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Pipeline EMBUTIDO (sem ffmpeg): MP4 (H.264/H.265) -> VP9/WebM por resolucao + MPD (DASH).
//  Liga: demux (MediaFragmenter) -> decode h26x -> scale (libyuv) -> encode (media_encoder VP9)
//        -> mux (mux_webm) -> manifest.mpd.
//
//  Primeira versao "ligar o pipeline": VIDEO-ONLY (VP9), 1 .webm por pista + MPD estatico
//  com BaseURL. Audio (Opus) e segmentacao DASH (SegmentBase/Cues) sao o passo seguinte.

#pragma once
#include "media_codec.h"   // MediaCodec (VP9 / AV1)

typedef struct
{
    int         Width;
    int         Height;
    int         BitrateBps;
    int         Fps;      // 0 = herda a taxa da entrada
    const char* Name;     // ex.: "720p" -> gera "<out_dir>/init-720p.webm" + chunks
} PipeTrack;

// video_codec: MEDIA_CODEC_VP9 ou MEDIA_CODEC_AV1 (ambos em WebM).
// Retorna 0 em sucesso; <0 em erro (codigos negativos por etapa).
// out_dir deve existir. Gera init/chunk .webm por pista + manifest.mpd (DASH).
int pipeline_mp4_to_webm_dash(const char* source_mp4, const char* out_dir,
                              MediaCodec video_codec,
                              const PipeTrack* tracks, int track_count,
                              int fps, double duration_sec);
