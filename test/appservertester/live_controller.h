//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  live_controller: transmissao AO VIVO de camera, fragmentada em MEMORIA (nada em disco).
//
//    POST/GET  /api/live/start/<sessao>   -> abre a camera e comeca a fragmentar
//    GET       /api/live/stop/<sessao>    -> para e libera a janela de memoria
//    GET       /api/live/status/<sessao>  -> estado, resolucao, segmentos na janela
//    GET       /api/live/media/<sessao>/live.m3u8   -> playlist HLS ao vivo (janela)
//    GET       /api/live/media/<sessao>/init.mp4    -> init fMP4
//    GET       /api/live/media/<sessao>/seg-<n>.m4s -> segmento
//
//  A configuracao (camera, codec de saida, resolucao, fps, bitrate) vem do session.json,
//  gravado pela UI em /api/session/config -- as mesmas ENTRADA/SAIDA da fragmentacao de
//  arquivo, sem uma segunda via de parametros.

#pragma once
#include "appserver.h"

Element* live_route(Message* message);

// Encerra toda transmissao viva (chamado no fim do processo).
void live_shutdown_all(void);
