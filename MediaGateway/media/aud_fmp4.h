//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Faixa de AUDIO do DASH em WebM: o AAC ORIGINAL, sem conversao, em segmentos fMP4 proprios
//  (init-<prefixo>.mp4 + <prefixo>-N.m4s) e um AdaptationSet separado no manifesto.
//
//  Por que assim: o WebM so carrega Opus/Vorbis, e converter AAC -> Opus exige um decoder de
//  AAC que o projeto nao tem (sem ele o DASH saia MUDO, calado). O DASH aceita video e audio
//  em conteineres diferentes -- e o que os grandes servicos fazem: faixas separadas, o
//  player junta pelo relogio. Sem conversao nao ha perda, nem atraso de encoder a compensar:
//  os tempos sao os da fonte.
//
//  O audio corta por conta propria, a cada seg_ms acumulados (sempre em fronteira de quadro
//  AAC); os cortes exatos vao para o manifesto (SegmentTimeline).

#pragma once
#include "media_sink.h"
#include <stdio.h>

typedef struct AudFmp4 AudFmp4;

// Abre e grava o init. NULL se nao der (sem ASC, sem disco).
AudFmp4* aud_fmp4_open(const char* base_dir, const char* prefixo, const uint8_t* asc, int asc_len,
                       int rate, int channels, int seg_ms);

// Um pacote AAC da fonte (Pts/Dur em mtime_us).
void aud_fmp4_write(AudFmp4* a, const GwPacket* pkt);

// Grava o ultimo segmento. Chamar antes de aud_fmp4_mpd.
void aud_fmp4_finish(AudFmp4* a);

// Duracao total gravada, em segundos.
double aud_fmp4_duration(const AudFmp4* a);

// Escreve o <AdaptationSet> de audio no manifesto aberto.
void aud_fmp4_mpd(const AudFmp4* a, FILE* f);

void aud_fmp4_close(AudFmp4* a);
