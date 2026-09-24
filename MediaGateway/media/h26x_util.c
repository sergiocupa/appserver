//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Implementacao do utilitario H.264/H.265 (ver h26x_util.h). Alocacoes via memory_pool.

#include "h26x_util.h"

// Itera NALs Annex-B (start code 00 00 01 ou 00 00 00 01). Retorna o ponteiro do NAL
// (apos o start code) e seu tamanho; 0 no fim.
static const uint8_t* nal_next(const uint8_t* b, int len, int* pos, int* nlen)
{
    int i = *pos;
    while (i + 3 <= len && !(b[i] == 0 && b[i+1] == 0 && (b[i+2] == 1 || (i+3 < len && b[i+2] == 0 && b[i+3] == 1)))) i++;
    if (i + 3 > len) { *pos = len; return 0; }
    int sc = (b[i+2] == 1) ? 3 : 4;
    int start = i + sc;
    int j = start;
    while (j + 3 <= len && !(b[j] == 0 && b[j+1] == 0 && (b[j+2] == 1 || (j+3 < len && b[j+2] == 0 && b[j+3] == 1)))) j++;
    int end = (j + 3 > len) ? len : j;
    *pos = end; *nlen = end - start;
    return b + start;
}

static void put_be32(uint8_t* p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (uint8_t)v; }

static int build_avcc(const uint8_t* sps, int sl, const uint8_t* pps, int pl, uint8_t* o)
{
    int n = 0;
    o[n++] = 1; o[n++] = sps[1]; o[n++] = sps[2]; o[n++] = sps[3];
    o[n++] = 0xFF; o[n++] = 0xE1;
    o[n++] = (sl >> 8) & 0xFF; o[n++] = sl & 0xFF; GW_COPY(o + n, sps, sl); n += sl;
    o[n++] = 1; o[n++] = (pl >> 8) & 0xFF; o[n++] = pl & 0xFF; GW_COPY(o + n, pps, pl); n += pl;
    return n;
}

static int build_hvcc(const uint8_t* vps, int vl, const uint8_t* sps, int sl, const uint8_t* pps, int pl, uint8_t* o)
{
    int n = 0;
    o[n++] = 1;
    if (sl >= 15) GW_COPY(o + n, sps + 3, 12); else GW_ZERO(o + n, 12);
    n += 12;
    o[n++] = 0xF0; o[n++] = 0x00; o[n++] = 0xFC; o[n++] = 0xFD; o[n++] = 0xF8; o[n++] = 0xF8;
    o[n++] = 0x00; o[n++] = 0x00; o[n++] = 0x0B; o[n++] = 3;
    o[n++] = 32; o[n++] = 0; o[n++] = 1; o[n++] = (vl >> 8) & 0xFF; o[n++] = vl & 0xFF; GW_COPY(o + n, vps, vl); n += vl;
    o[n++] = 33; o[n++] = 0; o[n++] = 1; o[n++] = (sl >> 8) & 0xFF; o[n++] = sl & 0xFF; GW_COPY(o + n, sps, sl); n += sl;
    o[n++] = 34; o[n++] = 0; o[n++] = 1; o[n++] = (pl >> 8) & 0xFF; o[n++] = pl & 0xFF; GW_COPY(o + n, pps, pl); n += pl;
    return n;
}

void h26x_tl_init(H26xToLen* c, int is_hevc) { GW_ZERO(c, sizeof(*c)); c->is_hevc = is_hevc; }

int h26x_tl_ready(const H26xToLen* c) { return c->cfg_len > 0; }

int h26x_tl_feed(H26xToLen* c, const uint8_t* d, int size, uint8_t** out)
{
    int pos = 0, nlen, alen = 0; const uint8_t* nal;
    while ((nal = nal_next(d, size, &pos, &nlen)))
    {
        int type = c->is_hevc ? ((nal[0] >> 1) & 0x3F) : (nal[0] & 0x1F);
        int is_ps, is_vcl;
        if (c->is_hevc) { is_ps = (type == 32 || type == 33 || type == 34); is_vcl = (type <= 31); }
        else            { is_ps = (type == 7 || type == 8);                is_vcl = (type == 1 || type == 5); }

        if (is_ps)
        {
            if (c->is_hevc)
            {
                if (type == 32 && c->vl == 0 && nlen <= (int)sizeof(c->vps)) { GW_COPY(c->vps, nal, nlen); c->vl = nlen; }
                if (type == 33 && c->sl == 0 && nlen <= (int)sizeof(c->sps)) { GW_COPY(c->sps, nal, nlen); c->sl = nlen; }
                if (type == 34 && c->pl == 0 && nlen <= (int)sizeof(c->pps)) { GW_COPY(c->pps, nal, nlen); c->pl = nlen; }
            }
            else
            {
                if (type == 7 && c->sl == 0 && nlen <= (int)sizeof(c->sps)) { GW_COPY(c->sps, nal, nlen); c->sl = nlen; }
                if (type == 8 && c->pl == 0 && nlen <= (int)sizeof(c->pps)) { GW_COPY(c->pps, nal, nlen); c->pl = nlen; }
            }
        }
        else if (is_vcl)
        {
            int need = alen + 4 + nlen;
            if (need > c->frame_cap) { c->frame = (uint8_t*)GW_REALLOC(c->frame, need); c->frame_cap = need; }
            put_be32(c->frame + alen, (uint32_t)nlen);
            GW_COPY(c->frame + alen + 4, nal, nlen);
            alen += 4 + nlen;
        }
    }

    if (c->cfg_len == 0)
    {
        if (c->is_hevc && c->vl && c->sl && c->pl) c->cfg_len = build_hvcc(c->vps, c->vl, c->sps, c->sl, c->pps, c->pl, c->cfg);
        else if (!c->is_hevc && c->sl && c->pl)    c->cfg_len = build_avcc(c->sps, c->sl, c->pps, c->pl, c->cfg);
    }

    if (out) *out = c->frame;
    return alen;
}

void h26x_tl_free(H26xToLen* c) { if (c && c->frame) { GW_FREE(c->frame); c->frame = 0; c->frame_cap = 0; } }
