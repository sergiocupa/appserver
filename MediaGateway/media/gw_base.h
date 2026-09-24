//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Base do gateway de sync: (1) adaptador de alocacao que usa SOMENTE o memory_pool
//  da xplatbase (nunca o CRT), para o monitor de memoria enxergar todo o buffer massivo;
//  (2) barramento de eventos (feedback) que a UI/handlers consomem — inclui amostra de
//  uso de memoria (alloc/memop_free_raw) para deteccao de vazamento. Ver media/SYNC_GATEWAY.md.

#pragma once
// So o memory_pool da xplatbase. Pulamos o "umbrella" (string/list/thread handlers) porque
// ele redefine simbolos que colidem com a stringlib/platformlib do appserver (string_copy,
// list_create, init_ptr...). O auto-init do CRT ja e feito pela platformlib do appserver.
#ifndef XPLATBASE_NO_AUTO_INIT
#define XPLATBASE_NO_AUTO_INIT
#endif
#define XPB_SKIP_UMBRELLA
#include "xplatbase.h"     // tipos base (uint64/boolean) + XPLATBASE_API (sem umbrella)
#include "memory_pool.h"   // memop_* (alloc/memop_free_raw/copy/zero/stats)
#include "media_time.h"

// ---- Alocacao: TODO o gateway aloca por aqui (memory_pool da xplatbase) -----
#define GW_ALLOC(n)      memop_alloc_raw((uint64)(n))
#define GW_CALLOC(c,n)   memop_calloc_raw((uint64)(c),(uint64)(n))
#define GW_REALLOC(p,n)  memop_realloc_raw((p),(uint64)(n))
#define GW_FREE(p)       memop_free_raw((p))
#define GW_COPY(d,s,n)   memop_copy_raw((d),(const void*)(s),(uint64)(n))
#define GW_ZERO(d,n)     memop_zero_raw((d),(uint64)(n))

// ---- Feedback para a UI (SSE/handlers). Puramente opcional (fb pode ser NULL) --
typedef enum
{
    GW_EV_START,        // inicio da preparacao (Total, Duration, modo)
    GW_EV_TRACK_START,  // uma rendition/pista comecou
    GW_EV_PROGRESS,     // progresso 0..1 (+ Pts corrente)
    GW_EV_TRACK_DONE,   // uma rendition/pista terminou (Playlist preenchido)
    GW_EV_DONE,         // fim (saida pronta)
    GW_EV_ERROR,        // falha (Message)
    GW_EV_CANCELLED,    // preparacao interrompida (cancelada pela UI ou abortada por falha)
    GW_EV_MEM           // amostra de uso de memoria (para o monitor de vazamento)
} GwEventKind;

typedef struct
{
    GwEventKind Kind;
    int         TrackIndex;
    const char* Name;
    int         Width, Height, Bandwidth;
    int         Total;              // nº de pistas (GW_EV_START)
    double      Progress;          // 0..1 (GW_EV_PROGRESS)
    double      Duration;          // segundos (GW_EV_START)
    mtime_us    Pts;               // tempo corrente
    const char* Playlist;          // caminho relativo do manifesto/segmento
    const char* Message;           // erro/descricao
    // GW_EV_MEM (snapshot do memory_pool)
    uint64      MemAlloc, MemFree, MemOsReserved, MemCached;
} GwEvent;

typedef void (*GwEventFn)(void* user, const GwEvent* ev);
typedef struct { GwEventFn Fn; void* User; } GwFeedback;

static inline void gw_emit(const GwFeedback* fb, const GwEvent* ev) { if (fb && fb->Fn && ev) fb->Fn(fb->User, ev); }

// Emite um GW_EV_MEM com o balanco alloc/memop_free_raw do memory_pool (delta alloc-memop_free_raw = vivo).
static inline void gw_emit_mem(const GwFeedback* fb)
{
    if (!fb || !fb->Fn) return;
    MemPoolStats s; memop_get_stats(&s);
    GwEvent e; GW_ZERO(&e, sizeof(e)); e.Kind = GW_EV_MEM;
    e.MemAlloc = s.alloc_count; e.MemFree = s.free_count;
    e.MemOsReserved = s.os_reserved_bytes; e.MemCached = s.cached_chunks;
    fb->Fn(fb->User, &e);
}

// Atalhos de emissao.
static inline void gw_error(const GwFeedback* fb, const char* msg)
{ GwEvent e; GW_ZERO(&e, sizeof(e)); e.Kind = GW_EV_ERROR; e.Message = msg; gw_emit(fb, &e); }

// Interrupcao limpa: a saida NAO foi publicada (sem manifesto/playlist). A UI deve
// tratar a sessao como cancelada, nao como pronta.
static inline void gw_cancelled(const GwFeedback* fb, const char* msg)
{ GwEvent e; GW_ZERO(&e, sizeof(e)); e.Kind = GW_EV_CANCELLED; e.Message = msg; gw_emit(fb, &e); }
