//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  live_store: janela deslizante de segmentos fMP4 em MEMORIA, para a saida AO VIVO de
//  camera. Nada vai para o disco: o gateway empurra segmentos por uma ponta, os mais
//  antigos caem pela outra, e as requisicoes HTTP leem de qualquer thread.
//
//  Por que janela e nao arquivo: ao vivo nao tem fim. Gravar geraria uma pasta que cresce
//  para sempre e que ninguem apaga; a janela limita o uso de RAM em bytes previsiveis
//  (window x tamanho do segmento) e e' exatamente o que um playlist HLS ao vivo publica.
//
//  Concorrencia: um PRODUTOR (a thread do gateway) e N LEITORES (as threads que atendem
//  requisicoes). Tudo sob um mutex curto; as leituras devolvem COPIA para o socket poder
//  ser escrito sem segurar o lock -- um send() lento nao pode travar a captura.

#pragma once
#include "gw_types.h"

typedef struct LiveStore LiveStore;

// window = quantos segmentos ficam vivos (o playlist publica esses). Minimo 2.
// target_seconds = TARGETDURATION anunciado ate existir medida real de segmento.
LiveStore* live_store_create(int window, int target_seconds);
void       live_store_free(LiveStore** store);

// ---- produtor (thread do gateway) ----------------------------------------
// O init (ftyp+moov) e' unico e so muda se a pista for reaberta.
void live_store_set_init(LiveStore* store, const uint8_t* data, int size,
                         int width, int height, int bitrate);
void live_store_push(LiveStore* store, const uint8_t* data, int size, double duration_s);
void live_store_finish(LiveStore* store);   // fim da transmissao: playlist ganha ENDLIST

// ---- leitores (threads das requisicoes) ----------------------------------
// Playlist HLS ao vivo da janela atual. Retorna o tamanho escrito (0 = ainda sem segmento).
int live_store_playlist(LiveStore* store, char* out, int out_size);
// Copias (memory_pool) que o CHAMADOR libera com memop_free_raw. 0 = nao ha/expirou.
int live_store_init_copy(LiveStore* store, uint8_t** out, int* size);
int live_store_segment_copy(LiveStore* store, long long seq, uint8_t** out, int* size);
// Estado para a UI/diagnostico. Qualquer ponteiro pode ser NULL.
void live_store_info(LiveStore* store, int* width, int* height, int* bitrate,
                     long long* last_seq, int* segments, int* finished);
