//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Muxers MP4 embutidos (sem ffmpeg) para H.264 codificado (amostras AVCC = NALs
//  prefixados por tamanho de 4 bytes), video-only:
//    1) PROGRESSIVO single-file (convert -> .mp4 tocavel no Chrome): ftyp + mdat + moov.
//    2) FRAGMENTADO/CMAF (HLS): init.mp4 (ftyp+moov+mvex) + segment_*.m4s (moof+mdat).
//  Timescale = 1000 (ms). NAO TESTADO EM RUNTIME.

#pragma once
#include <stdint.h>

// ---------- Progressivo single-file H.264 (+ AAC opcional) ----------
typedef struct Mp4Mux Mp4Mux;
Mp4Mux* mp4_open(const char* path, int width, int height, const uint8_t* avcc, int avcc_len);
// Variante com faixa de audio AAC (passthrough). Passe asc=NULL p/ video-only.
// A faixa de audio usa timescale = rate (durations em amostras).
Mp4Mux* mp4_open2(const char* path, int width, int height, const uint8_t* avcc, int avcc_len,
                  const uint8_t* asc, int asc_len, int rate, int channels);
// HEVC: sample entry hvc1 + hvcC (cfg = HEVCDecoderConfigurationRecord). asc=NULL p/ video-only.
Mp4Mux* mp4_open_hevc(const char* path, int width, int height, const uint8_t* hvcc, int hvcc_len,
                      const uint8_t* asc, int asc_len, int rate, int channels);
void    mp4_write_video(Mp4Mux* m, int64_t pts_ms, int keyframe, const uint8_t* avcc_sample, int size);
// Escreve 1 sample AAC codificado (bufferizado, gravado no fim). dur_units em unidades do timescale de audio.
void    mp4_write_audio(Mp4Mux* m, int dur_units, const uint8_t* aac_sample, int size);
void    mp4_close(Mp4Mux* m);

// ---------- Fragmentado/CMAF (video-only H.264) ----------
// init.mp4: ftyp + moov (trak + mvex/trex), sem amostras. Retorna 0 em sucesso.
int mp4_write_init(const char* path, int width, int height, const uint8_t* avcc, int avcc_len);

// Uma amostra codificada para um segmento em construcao.
// Para faixa de audio, duration_ms e base_dt sao em unidades do timescale de audio (rate).
typedef struct { const uint8_t* data; int size; int duration_ms; int keyframe; } Mp4Sample;

// segment_*.m4s: moof(mfhd+traf[tfhd+tfdt+trun]) + mdat. base_dt_ms = tempo do 1o sample (ms).
// Retorna 0 em sucesso.
int mp4_write_segment(const char* path, uint32_t seq, int64_t base_dt_ms,
                      const Mp4Sample* samples, int count);

// Mesmas caixas, montadas em MEMORIA (o ao vivo de camera serve da RAM, sem tocar o disco).
// Em sucesso devolve 0 e um buffer do memory_pool que o CHAMADOR libera (memop_free_raw).
int mp4_build_init(uint8_t** out, int* out_len, int width, int height,
                   const uint8_t* avcc, int avcc_len);
int mp4_build_segment(uint8_t** out, int* out_len, uint32_t seq, int64_t base_dt_ms,
                      const Mp4Sample* samples, int count);

// ---------- Fragmentado/CMAF: rendition de AUDIO AAC (para HLS) ----------
// init-audio.mp4 (timescale de audio = rate). Retorna 0 em sucesso.
int mp4_write_init_audio(const char* path, const uint8_t* asc, int asc_len, int rate, int channels);
// Os segmentos de audio reutilizam mp4_write_segment (samples AAC, keyframe=1, durations em 'rate' units).
