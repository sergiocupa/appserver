//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  MediaSink: interface PUSH de saida (coordenador). Recebe pacotes ja codificados e
//  sincronizados, acumula o fragmento corrente e o fecha em Cut() (fronteira de segmento
//  = keyframe de video). A fabrica escolhe a implementacao pelo Container do perfil.
//  Ver media/SYNC_GATEWAY.md.

#pragma once
#include "gw_types.h"

typedef struct MediaSink MediaSink;
struct MediaSink
{
    void* Ctx;
    // 1 = a preparacao foi interrompida (cancelada/erro). O Close() ainda libera tudo,
    // mas NAO publica manifesto/playlist nem emite GW_EV_DONE: uma saida meio-escrita
    // que se apresenta como pronta e o que quebra a proxima abertura da sessao.
    int   Aborted;
    int  (*Start)(MediaSink* k, const MediaTrackOut* tracks, int count);
    int  (*Write)(MediaSink* k, int track, const GwPacket* pkt);
    int  (*Cut)(MediaSink* k, mtime_us at);   // fecha o fragmento corrente em todas as pistas
    void (*Close)(MediaSink* k);
};

// Fabrica: cria o sink do Container do perfil. 'base_dir' = pasta de saida (arquivos e
// manifestos sao gravados ali; nomes relativos). 'fb' opcional (feedback p/ UI).
// Retorna NULL se o Container nao tiver sink disponivel.
MediaSink* media_sink_open(const MediaProfile* profile, const char* base_dir, const GwFeedback* fb);

// Implementacoes concretas (usadas pela fabrica).
MediaSink* sink_mp4_open (const MediaProfile* profile, const char* base_dir, const GwFeedback* fb); // CONT_MP4_FILE
MediaSink* sink_hls_open (const MediaProfile* profile, const char* base_dir, const GwFeedback* fb); // CONT_HLS_FMP4
MediaSink* sink_dash_open(const MediaProfile* profile, const char* base_dir, const GwFeedback* fb); // CONT_DASH_* (proximo passo)
MediaSink* sink_live_open(const MediaProfile* profile, const char* base_dir, const GwFeedback* fb); // CONT_LIVE_MEM (camera ao vivo)
