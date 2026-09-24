//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Operacoes embutidas (sem ffmpeg) que faltavam fora do DASH:
//   - HLS: passthrough H.264/H.265 -> fMP4 (init.mp4 + segment_*.m4s) + playlist.m3u8
//          via MediaFragmenter (mp4builder). Resolucao da fonte (sem reencode/scale).
//   - Converter: reencode para ARQUIVO unico WebM (VP9/AV1) + Opus, resolucao da fonte.

#pragma once
#include "media_codec.h"
#include "pipeline.h"   // PipeTrack

// HLS embutido (1 rendition = fonte). Gera <base>/init.mp4, <base>/segment_*.m4s e
// <base>/playlist.m3u8. Retorna 0 em sucesso.
int embed_build_hls(const char* source_mp4, const char* base, int frag_seconds,
                    int* out_width, int* out_height, double* out_duration, int* out_segments);

// Converte o MP4 inteiro para um WebM unico no codec dado (MEDIA_CODEC_VP9/AV1) + Opus.
// Resolucao da fonte (sem scale). Retorna 0 em sucesso.
int embed_transcode_webm(const char* source_mp4, const char* out_path, MediaCodec vcodec);

// Reencoda para H.264 (OpenH264) num MP4 single-file (avcC + AVCC, + AAC passthrough) via mux_mp4.
// Retorna 0 em sucesso.
int embed_transcode_h264(const char* source_mp4, const char* out_mp4_path);

// Reencoda para H.265/HEVC (x265) num MP4 single-file (hvcC + HVCC, + AAC passthrough).
// ATENCAO: x265 e GPL / HEVC tem patentes. Retorna 0 em sucesso.
int embed_transcode_h265(const char* source_mp4, const char* out_mp4_path);

// HLS MULTI-RESOLUCAO em H.264 (sem ffmpeg): por pista faz decode->scale(libyuv)->OpenH264->
// fMP4 (init-<name>.mp4 + <name>-N.m4s) + <name>.m3u8; escreve master.m3u8. Retorna 0 em sucesso.
int embed_build_hls_h264(const char* source_mp4, const char* base,
                         const PipeTrack* tracks, int track_count, int frag_seconds);
