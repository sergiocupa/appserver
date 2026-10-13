//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Pecas comuns aos manifestos DASH (.mpd): a lista de segmentos com o tempo e a duracao
//  EXATOS de cada um (SegmentTimeline). Um manifesto que so declara "segmentos de N s" manda
//  o player pedir arquivos que nao existem quando o corte real cai em outro ponto -- e o
//  corte real depende de onde o encoder poe quadro-chave. Com a lista, o manifesto diz o
//  que foi de fato gravado.

#pragma once
#include "gw_base.h"
#include <stdio.h>
#include <stdint.h>

// DASH em WebM: o codec vai no NOME dos arquivos -- manifest-vp9.mpd, init-vp9-1080p.webm,
// chunk-vp9-1080p-N.webm (e o mesmo com "av1"). Antes VP9 e AV1 gravavam os mesmos nomes na
// mesma pasta e preparar um apagava o outro. A Representation leva o mesmo id ("vp9-1080p"),
// entao o SegmentTemplate continua sendo init-$RepresentationID$ / chunk-$RepresentationID$.
static inline const char* mpd_webm_tag(int av1)      { return av1 ? "av1" : "vp9"; }
static inline const char* mpd_webm_manifest(int av1) { return av1 ? "manifest-av1.mpd" : "manifest-vp9.mpd"; }

typedef struct { int64_t t, d; } MpdSeg;   // inicio e duracao de um segmento, na escala da faixa

static inline void mpd_seg_add(MpdSeg** v, int* n, int* cap, int64_t t, int64_t d)
{
    if (*n == *cap) { *cap = *cap ? *cap * 2 : 128; *v = (MpdSeg*)GW_REALLOC(*v, (size_t)*cap * sizeof(MpdSeg)); }
    (*v)[*n].t = t; (*v)[*n].d = d; (*n)++;
}

// A duracao de cada segmento, menos a do ultimo, e a distancia ate o inicio do seguinte:
// assim o manifesto bate com o tempo gravado dentro de cada arquivo.
static inline int64_t mpd_seg_dur(const MpdSeg* v, int n, int i) { return (i + 1 < n) ? v[i + 1].t - v[i].t : v[i].d; }

// <SegmentTimeline> com as corridas de segmentos iguais agrupadas (r = repeticoes).
static inline void mpd_timeline(FILE* f, const MpdSeg* v, int n, const char* recuo)
{
    fprintf(f, "%s<SegmentTimeline>\n", recuo);
    for (int i = 0; i < n; )
    {
        int64_t d = mpd_seg_dur(v, n, i);
        int r = 0;
        while (i + r + 1 < n && mpd_seg_dur(v, n, i + r + 1) == d) r++;
        if (r > 0) fprintf(f, "%s  <S t=\"%lld\" d=\"%lld\" r=\"%d\"/>\n", recuo, (long long)v[i].t, (long long)d, r);
        else       fprintf(f, "%s  <S t=\"%lld\" d=\"%lld\"/>\n", recuo, (long long)v[i].t, (long long)d);
        i += r + 1;
    }
    fprintf(f, "%s</SegmentTimeline>\n", recuo);
}

// 1 se as duas listas descrevem os mesmos cortes (resolucoes alinhadas).
static inline int mpd_seg_iguais(const MpdSeg* a, int na, const MpdSeg* b, int nb)
{
    if (na != nb) return 0;
    for (int i = 0; i < na; i++) if (a[i].t != b[i].t) return 0;
    return 1;
}
