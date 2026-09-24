//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Fabrica do coordenador de saida: despacha pelo Container do perfil.

#include "media_sink.h"

MediaSink* media_sink_open(const MediaProfile* profile, const char* base_dir, const GwFeedback* fb)
{
    if (!profile) return 0;
    switch (profile->Container)
    {
        case CONT_MP4_FILE:  return sink_mp4_open(profile, base_dir, fb);
        case CONT_HLS_FMP4:  return sink_hls_open(profile, base_dir, fb);
        case CONT_DASH_WEBM:
        case CONT_DASH_MP4:  return sink_dash_open(profile, base_dir, fb);
        case CONT_LIVE_MEM:  return sink_live_open(profile, base_dir, fb);
        default:             return 0;
    }
}
