//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Entrada de audio: demux da track AAC do MP4 + decode -> PCM S16 (via fdk-aac).
//  Ative com -DHAVE_FDK_AAC (vcpkg install fdk-aac). Sem isso, aac_open retorna != 0
//  (sem audio) e o pipeline segue video-only.
//
//  API compativel com o uso em pipeline.c:
//     void* ctx; int rate, ch;
//     if (aac_open(mp4, &rate, &ch, &ctx) == 0) {
//         int16_t* pcm; int n; int64_t ts;
//         while (aac_read(ctx, &pcm, &n, &ts) == 1) { ...encode Opus... }
//         aac_close(ctx);
//     }

#pragma once
#include <stdint.h>

// Retorna 0 em sucesso (abriu track AAC + decoder). rate/channels preenchidos.
int  aac_open(const char* mp4_path, int* rate, int* channels, void** ctx);

// 1 = preencheu pcm (S16 intercalado, 'samples' por canal, ts em ms); 0 = fim; <0 = erro.
// O buffer 'pcm' pertence ao ctx (valido ate a proxima chamada).
int  aac_read(void* ctx, int16_t** pcm, int* samples, int64_t* ts_ms);

void aac_close(void* ctx);

// ---- Passthrough (AAC codificado, SEM decodificar) ------------------------
// NAO exige HAVE_FDK_AAC: apenas demux do MP4. Serve p/ remuxar AAC em MP4/fMP4
// (nativo em Chrome/Safari), evitando re-encode.
typedef struct
{
    uint8_t  asc[64]; int asc_len;   // AudioSpecificConfig (p/ esds)
    int      rate, channels;
    uint32_t timescale;              // timescale da media de audio (normalmente == rate)
} AacRawInfo;

// Abre a track AAC e preenche info. Retorna 0 em sucesso. 'ctx' deve ser liberado com aac_close.
int  aac_open_raw(const char* mp4_path, AacRawInfo* info, void** ctx);

// 1 = preencheu (data pertence ao ctx ate a proxima chamada); 0 = fim; <0 = erro.
// dts_units/dur_units em unidades do timescale de audio (AacRawInfo.timescale).
int  aac_read_raw(void* ctx, const uint8_t** data, int* size, int64_t* dts_units, int* dur_units);

// ---- Decoder AAC packet-fed (para transcode AAC->Opus no gateway) ----------
// Exige HAVE_FDK_AAC. 'asc' = AudioSpecificConfig (de AacRawInfo/MediaStreamInfo.Extra).
// Retorna NULL se fdk-aac nao compilado ou falha.
void* aac_dec_open(const uint8_t* asc, int asc_len);
// Decodifica 1 pacote AAC -> PCM S16 intercalado. 'pcm' pertence ao decoder (ate a proxima
// chamada). 1 = ok, 0 = precisa de mais dados, <0 = erro. 'samples' = amostras por canal.
int   aac_dec_decode(void* dec, const uint8_t* data, int size, int16_t** pcm, int* samples);
void  aac_dec_close(void* dec);
