//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Extrai o PTS por frame de video (ms, ordem de decodificacao) do MP4 via stts+mdhd.
//  Usado para afinacao exata dos timestamps (suporta fps fracionario/VFR).
//  Obs.: usa duracoes de decodificacao (stts); com B-frames o PTS exato exigiria ctts.

#pragma once
#include <stdint.h>

// Retorna 0 em sucesso; *out_pts (memop_alloc_raw, ms) e *out_count preenchidos. Caller libera *out_pts.
int mp4_video_pts_ms(const char* mp4_path, int64_t** out_pts, uint32_t* out_count);

// Metadados de video (sem ffprobe): largura/altura, fps, codec ("h264"/"hevc") e duracao (s).
int mp4_video_info(const char* mp4_path, int* w, int* h, double* fps, char codec[8], double* duration);

// Metadados de audio (sem ffprobe): codec ("aac"), sample rate e canais. <0 se nao houver audio.
int mp4_audio_info(const char* mp4_path, char codec[8], int* rate, int* channels);
