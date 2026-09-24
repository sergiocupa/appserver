//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  sync_gateway: orquestrador. Puxa do MediaSource, decodifica (ou bypass p/ frame cru,
//  ou passthrough p/ in==out), escala (libyuv) por rendition, codifica no codec de saida,
//  corta o fragmento no keyframe e escreve no MediaSink. Ver media/SYNC_GATEWAY.md.

#pragma once
#include "gw_types.h"
#include "media_source.h"
#include "media_sink.h"

typedef struct { volatile int Stop; } GwControl;   // parada cooperativa (live/fase 2)

// Executa a preparacao ate o EOF do source (VOD). 'fb' opcional (feedback p/ UI).
// Retorna 0 em sucesso. Cria o sink pela fabrica a partir de 'profile'.
int gateway_run(MediaSource* src, const MediaProfile* profile, const char* base_dir,
                const GwFeedback* fb, GwControl* ctl);
