//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Utilitario H.264/H.265 compartilhado pelos sinks MP4/fMP4: itera NALs Annex-B,
//  extrai SPS/PPS/VPS, monta avcC/hvcC e converte 1 frame para amostra length-prefixed
//  (prefixo de 4 bytes). Alocacoes via memory_pool (GW_*). NAO TESTADO EM RUNTIME.

#pragma once
#include "gw_base.h"

typedef struct
{
    int      is_hevc;
    uint8_t  sps[512], pps[512], vps[512]; int sl, pl, vl;
    uint8_t  cfg[2048]; int cfg_len;       // avcC (H.264) / hvcC (HEVC) quando pronto
    uint8_t* frame; int frame_cap;         // buffer length-prefixed reutilizado
} H26xToLen;

// Inicializa (is_hevc = 1 p/ HEVC). Zera o estado.
void h26x_tl_init(H26xToLen* c, int is_hevc);

// Consome 1 pacote Annex-B: captura os param sets (uma vez), monta c->cfg quando prontos,
// e devolve o frame length-prefixed dos NALs VCL. Retorna o tamanho VCL (0 se so param sets).
// *out aponta para c->frame (valido ate a proxima chamada).
int  h26x_tl_feed(H26xToLen* c, const uint8_t* annexb, int size, uint8_t** out);

// 1 quando c->cfg/cfg_len ja estao prontos (param sets necessarios vistos).
int  h26x_tl_ready(const H26xToLen* c);

void h26x_tl_free(H26xToLen* c);
