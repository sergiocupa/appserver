//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Mux WebM (EBML/Matroska) EMBUTIDO - para VP9/AV1 + Opus, sem ffmpeg.
//  Escreve um .webm streamable (Segment de tamanho desconhecido; um Cluster por
//  keyframe de video). Base para o DASH-WebM (segmentar clusters + gerar .mpd).
//
//  Uso:
//     WebmMux* m = webm_open("output.webm", w, h, fps, has_audio, 48000, 2, opus_head, len);
//     webm_write_video(m, ts_ms, keyframe, pkt.Data, pkt.Size);
//     webm_write_audio(m, ts_ms, pkt.Data, pkt.Size);
//     webm_close(m);

#pragma once
#include <stdint.h>

typedef struct WebmMux WebmMux;

// codec_video: CodecID do Matroska -- "V_VP9" ou "V_AV1" (NAO "V_AV01": esse e o fourcc
// do ISO-BMFF/DASH, outro namespace, e nenhum player reconhece dentro de WebM).
// opus_head = bytes do OpusHead (CodecPrivate),
// obrigatorio se has_audio. Retorna NULL em erro.
WebmMux* webm_open(const char* path,
                   const char* codec_video, int width, int height, int fps,
                   int has_audio, int audio_rate, int audio_channels,
                   const uint8_t* opus_head, int opus_head_len);

// Modo SEGMENTADO (DASH): escreve "<dir>/init-<name>.webm" e, a cada seg_ms (em keyframe),
// um "<dir>/chunk-<name>-<N>.webm" (N a partir de 1). Use com SegmentTemplate no .mpd.
WebmMux* webm_open_segmented(const char* dir, const char* name, const char* codec_video,
                             int width, int height, int fps, int seg_ms,
                             int has_audio, int audio_rate, int audio_channels,
                             const uint8_t* opus_head, int opus_head_len);

// Segment-parallel: grava UM chunk isolado (chunk-<name>-<seg_number>.webm). NAO escreve init.
// webm_open_chunk -> webm_write_video (N frames, o 1o = keyframe) -> webm_close.
WebmMux* webm_open_chunk(const char* dir, const char* name, int seg_number);

// ts_ms = timestamp absoluto em milissegundos (TimecodeScale = 1ms).
int  webm_write_video(WebmMux* m, int64_t ts_ms, int keyframe, const uint8_t* data, int size);
int  webm_write_audio(WebmMux* m, int64_t ts_ms, const uint8_t* data, int size);

// MKV single-file com H.264 (V_MPEG4/ISO/AVC + avcC). Escreva os frames com webm_write_video
// em AVCC (NALs prefixados por tamanho de 4 bytes). Use webm_close ao final.
WebmMux* webm_open_h264(const char* path, const uint8_t* avcc, int avcc_len,
                        int width, int height, int fps,
                        int has_audio, int audio_rate, int audio_channels,
                        const uint8_t* opus_head, int opus_head_len);

// Monta o CodecPrivate "OpusHead" (19 bytes) exigido pela track Opus no WebM.
// Retorna o tamanho (19). preskip usa 3840 (padrao do libopus a 48k).
int webm_build_opus_head(int channels, int rate, uint8_t out[19]);

void webm_close(WebmMux* m);
