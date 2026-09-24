//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  source_webm: fonte PULL de arquivo WebM/Matroska (demux, sem ffmpeg).
//
//  Existe pelo mesmo motivo do gw_decode: os decoders de VP9 (libvpx) e AV1 (dav1d)
//  estavam prontos mas inalcancaveis -- o unico demux da casa era MP4, e VP9/AV1 vivem
//  em .webm. Com este modulo o par entrada->decode fecha para os quatro codecs.
//
//  Escopo do parser: o subconjunto do Matroska que o WebM realmente usa --
//  Info(TimestampScale/Duration), Tracks(video/audio) e Cluster(SimpleBlock/BlockGroup).
//  Le em STREAMING (FILE*), sem carregar o arquivo inteiro; so o corpo de Info e Tracks
//  (poucos KB, antes do primeiro Cluster) vai para memoria de uma vez.
//
//  NAO suportado (e por que nao atrapalha): lacing (usado so em audio de quadro curto --
//  Vorbis/MP3; Opus em WebM sai sem lacing), e cifragem/compressao de faixa.

#include "media_source.h"
#include "gw_path.h"
#include "memory_pool.h"
#include <stdio.h>
#include <string.h>

// ---- IDs do Matroska (com o bit marcador do vint, como aparecem no arquivo) ----
#define ID_EBML         0x1A45DFA3u
#define ID_SEGMENT      0x18538067u
#define ID_INFO         0x1549A966u
#define ID_TIMESCALE    0x2AD7B1u
#define ID_DURATION     0x4489u
#define ID_TRACKS       0x1654AE6Bu
#define ID_TRACKENTRY   0xAEu
#define ID_TRACKNUMBER  0xD7u
#define ID_TRACKTYPE    0x83u
#define ID_CODECID      0x86u
#define ID_CODECPRIVATE 0x63A2u
#define ID_DEFDURATION  0x23E383u
#define ID_VIDEO        0xE0u
#define ID_PIXW         0xB0u
#define ID_PIXH         0xBAu
#define ID_AUDIO        0xE1u
#define ID_SAMPFREQ     0xB5u
#define ID_CHANNELS     0x9Fu
#define ID_CLUSTER      0x1F43B675u
#define ID_TIMESTAMP    0xE7u
#define ID_SIMPLEBLOCK  0xA3u
#define ID_BLOCKGROUP   0xA0u
#define ID_BLOCK        0xA1u
#define ID_BLOCKDUR     0x9Bu

#define WBM_UNKNOWN  ((uint64_t)-1)
#define WBM_MAXTR    8

typedef struct
{
    int        number;               // TrackNumber do arquivo
    int        type;                 // 1 = video, 2 = audio
    MediaCodec codec;
    int        w, h;
    double     fps;
    int        rate, channels;
    uint8_t*   priv; int priv_len;   // CodecPrivate (avcC/hvcC, ASC do AAC, OpusHead...)

    // H264/H265 em Matroska vem LENGTH-PREFIXED (avcC/hvcC), nao Annex-B -- que e o que
    // openh264/libde265 esperam. Estes dois campos guardam o que converter: o tamanho do
    // prefixo e os parameter sets (SPS/PPS/VPS) ja em Annex-B, para injetar no keyframe.
    int        nal_len_size;         // 0 = o stream ja e Annex-B (VP9/AV1 nao usam nada disso)
    uint8_t*   ps; int ps_len;       // parameter sets em Annex-B
} WbmTrack;

typedef struct
{
    FILE*    fp;
    uint64_t seg_end;                // fim do Segment (WBM_UNKNOWN = ate o EOF)
    uint64_t cluster_end;            // fim do Cluster corrente (0 = nenhum aberto)
    int64_t  cluster_ts;             // Timestamp do Cluster, na unidade do TimestampScale
    uint64_t tscale;                 // TimestampScale em ns (padrao 1.000.000 = 1ms)
    double   duration;               // segundos

    WbmTrack tr[WBM_MAXTR]; int ntr;
    int      vt, at;                 // indices em tr[] das faixas escolhidas (-1 = nenhuma)

    uint8_t* buf; int cap, buf_len;  // corpo do bloco corrente (reaproveitado)
    uint8_t* conv; int conv_cap, conv_len;   // saida da conversao avcC/hvcC -> Annex-B
} SrcWebm;

// ---- leitura primitiva -----------------------------------------------------

static int rd(SrcWebm* s, void* d, int n)
{
    return n > 0 && fread(d, 1, (size_t)n, s->fp) == (size_t)n;
}

static uint64_t wpos(SrcWebm* s) { return (uint64_t)gw_ftell64(s->fp); }

// vint do EBML. keep_marker=1 para IDs (o marcador faz parte do ID), 0 para tamanhos.
// Devolve WBM_UNKNOWN no tamanho "todos os bits de dado em 1" (tamanho desconhecido).
static int read_vint(SrcWebm* s, uint64_t* out, int keep_marker)
{
    int c = fgetc(s->fp);
    if (c < 0) return 0;

    uint8_t b0 = (uint8_t)c;
    int len = 0;
    for (int i = 0; i < 8; i++) if (b0 & (0x80 >> i)) { len = i + 1; break; }
    if (!len) return 0;                                  // byte 0x00: fluxo corrompido

    uint64_t v = keep_marker ? b0 : (uint64_t)(b0 & (0xFF >> len));
    int all_ones = ((b0 & (0xFF >> len)) == (0xFF >> len));

    for (int i = 1; i < len; i++)
    {
        c = fgetc(s->fp);
        if (c < 0) return 0;
        v = (v << 8) | (uint8_t)c;
        if ((uint8_t)c != 0xFF) all_ones = 0;
    }
    *out = (!keep_marker && all_ones) ? WBM_UNKNOWN : v;
    return 1;
}

// Cabecalho de um elemento: ID + tamanho.
static int read_elem(SrcWebm* s, uint32_t* id, uint64_t* size)
{
    uint64_t i, z;
    if (!read_vint(s, &i, 1)) return 0;
    if (!read_vint(s, &z, 0)) return 0;
    *id = (uint32_t)i; *size = z;
    return 1;
}

static uint64_t be_uint(const uint8_t* d, int n)
{
    uint64_t v = 0;
    for (int i = 0; i < n; i++) v = (v << 8) | d[i];
    return v;
}

static double be_float(const uint8_t* d, int n)
{
    if (n == 4) { uint32_t u = (uint32_t)be_uint(d, 4); float f;  memcpy(&f, &u, 4); return (double)f; }
    if (n == 8) { uint64_t u = be_uint(d, 8);           double f; memcpy(&f, &u, 8); return f; }
    return 0.0;
}

// ---- parse de Info / Tracks (em memoria) -----------------------------------
// Mesmo vint de cima, mas sobre um buffer: Info e Tracks sao pequenos e ja estao lidos.

typedef struct { const uint8_t* d; int n, i; } Mem;

static int mem_vint(Mem* m, uint64_t* out, int keep_marker)
{
    if (m->i >= m->n) return 0;
    uint8_t b0 = m->d[m->i];
    int len = 0;
    for (int k = 0; k < 8; k++) if (b0 & (0x80 >> k)) { len = k + 1; break; }
    if (!len || m->i + len > m->n) return 0;

    uint64_t v = keep_marker ? b0 : (uint64_t)(b0 & (0xFF >> len));
    for (int k = 1; k < len; k++) v = (v << 8) | m->d[m->i + k];
    m->i += len; *out = v;
    return 1;
}

static int mem_elem(Mem* m, uint32_t* id, const uint8_t** body, int* blen)
{
    uint64_t i, z;
    if (!mem_vint(m, &i, 1)) return 0;
    if (!mem_vint(m, &z, 0)) return 0;
    if (z > (uint64_t)(m->n - m->i)) z = (uint64_t)(m->n - m->i);   // trunca em vez de ler fora
    *id = (uint32_t)i; *body = m->d + m->i; *blen = (int)z;
    m->i += (int)z;
    return 1;
}

static MediaCodec codec_from_id(const char* s, int n)
{
    // Comparacao EXATA, nao por prefixo: o CodecID e uma string fechada, e casar por
    // prefixo faria um "V_VP9X" futuro passar por VP9. O tamanho declarado pode vir com
    // padding nulo, entao o terminador tambem conta como fim da string.
    #define WBM_EQ(lit) (n >= (int)sizeof(lit) - 1 && memcmp(s, lit, sizeof(lit) - 1) == 0 \
                        && (n == (int)sizeof(lit) - 1 || s[sizeof(lit) - 1] == 0))
    if (WBM_EQ("V_VP9"))            return MEDIA_CODEC_VP9;
    if (WBM_EQ("V_AV1"))            return MEDIA_CODEC_AV1;
    // "V_AV01" nao existe no Matroska -- e o fourcc do ISO-BMFF/DASH, e os sinks daqui o
    // escreviam por engano. Aceito na LEITURA para nao invalidar o que ja foi gravado;
    // na ESCRITA o correto (e o que sai agora) e "V_AV1".
    if (WBM_EQ("V_AV01"))           return MEDIA_CODEC_AV1;
    if (WBM_EQ("V_MPEG4/ISO/AVC"))  return MEDIA_CODEC_H264;
    if (WBM_EQ("V_MPEGH/ISO/HEVC")) return MEDIA_CODEC_H265;
    if (WBM_EQ("A_OPUS"))           return MEDIA_CODEC_OPUS;
    if (WBM_EQ("A_AAC"))            return MEDIA_CODEC_AAC;
    #undef WBM_EQ
    return MEDIA_CODEC_NONE;        // V_VP8, A_VORBIS... sem decoder na matriz
}

// ---- avcC / hvcC -> parameter sets em Annex-B ------------------------------
// Buffer que cresce sozinho, so para montar os parameter sets uma vez na abertura.

typedef struct { uint8_t* d; int n, cap; } Grow;

static int grow_put(Grow* g, const uint8_t* d, int n)
{
    if (g->n + n > g->cap)
    {
        int nc = g->cap > 0 ? g->cap : 256;
        while (nc < g->n + n) nc *= 2;
        uint8_t* nb = (uint8_t*)GW_REALLOC(g->d, nc);
        if (!nb) return 0;
        g->d = nb; g->cap = nc;
    }
    memcpy(g->d + g->n, d, (size_t)n);
    g->n += n;
    return 1;
}

static int grow_nal(Grow* g, const uint8_t* nal, int len)
{
    static const uint8_t sc[4] = { 0, 0, 0, 1 };
    return grow_put(g, sc, 4) && grow_put(g, nal, len);
}

// avcC (ISO/IEC 14496-15): [0]=1, [4]=0xFC|lenSizeMinus1, [5]=0xE0|numSPS, entao
// numSPS x (u16 len + NAL), depois numPPS x (u16 len + NAL).
static void parse_avcc(WbmTrack* t, const uint8_t* d, int n)
{
    if (n < 7 || d[0] != 1) return;
    t->nal_len_size = (d[4] & 0x03) + 1;

    Grow g; GW_ZERO(&g, sizeof(g));
    int i = 5;
    for (int pass = 0; pass < 2; pass++)
    {
        if (i >= n) break;
        int count = pass == 0 ? (d[i] & 0x1F) : d[i];
        i++;
        for (int k = 0; k < count && i + 2 <= n; k++)
        {
            int len = (d[i] << 8) | d[i + 1]; i += 2;
            if (len < 0 || i + len > n) { i = n; break; }
            if (!grow_nal(&g, d + i, len)) { GW_FREE(g.d); return; }
            i += len;
        }
    }
    t->ps = g.d; t->ps_len = g.n;
}

// hvcC: [0]=version, [21]=0xFC|lenSizeMinus1, [22]=numOfArrays, entao por array
// u8(completeness|tipo) + u16 numNalus + numNalus x (u16 len + NAL).
static void parse_hvcc(WbmTrack* t, const uint8_t* d, int n)
{
    if (n < 23) return;
    t->nal_len_size = (d[21] & 0x03) + 1;

    Grow g; GW_ZERO(&g, sizeof(g));
    int arrays = d[22], i = 23;
    for (int a = 0; a < arrays && i + 3 <= n; a++)
    {
        int cnt = (d[i + 1] << 8) | d[i + 2]; i += 3;
        for (int k = 0; k < cnt && i + 2 <= n; k++)
        {
            int len = (d[i] << 8) | d[i + 1]; i += 2;
            if (len < 0 || i + len > n) { i = n; break; }
            if (!grow_nal(&g, d + i, len)) { GW_FREE(g.d); return; }
            i += len;
        }
    }
    t->ps = g.d; t->ps_len = g.n;
}

static void parse_track_entry(SrcWebm* s, const uint8_t* d, int n)
{
    if (s->ntr >= WBM_MAXTR) return;

    WbmTrack t; GW_ZERO(&t, sizeof(t));
    Mem m; m.d = d; m.n = n; m.i = 0;
    uint32_t id; const uint8_t* b; int bl;
    uint64_t defdur_ns = 0;

    while (mem_elem(&m, &id, &b, &bl))
    {
        switch (id)
        {
        case ID_TRACKNUMBER:  t.number   = (int)be_uint(b, bl); break;
        case ID_TRACKTYPE:    t.type     = (int)be_uint(b, bl); break;
        case ID_CODECID:      t.codec    = codec_from_id((const char*)b, bl); break;
        case ID_DEFDURATION:  defdur_ns  = be_uint(b, bl); break;
        case ID_CODECPRIVATE:
            if (bl > 0)
            {
                t.priv = (uint8_t*)GW_ALLOC(bl);
                if (t.priv) { memcpy(t.priv, b, (size_t)bl); t.priv_len = bl; }
            }
            break;
        case ID_VIDEO:
        {
            Mem v; v.d = b; v.n = bl; v.i = 0;
            uint32_t vi; const uint8_t* vb; int vl;
            while (mem_elem(&v, &vi, &vb, &vl))
            {
                if      (vi == ID_PIXW) t.w = (int)be_uint(vb, vl);
                else if (vi == ID_PIXH) t.h = (int)be_uint(vb, vl);
            }
            break;
        }
        case ID_AUDIO:
        {
            Mem a; a.d = b; a.n = bl; a.i = 0;
            uint32_t ai; const uint8_t* ab; int al;
            while (mem_elem(&a, &ai, &ab, &al))
            {
                if      (ai == ID_SAMPFREQ) t.rate     = (int)(be_float(ab, al) + 0.5);
                else if (ai == ID_CHANNELS) t.channels = (int)be_uint(ab, al);
            }
            break;
        }
        default: break;
        }
    }

    // DefaultDuration e o unico lugar do WebM que declara a taxa; sem ele so restaria
    // medir os PTS, e o gateway precisa do fps ANTES do primeiro frame (para dimensionar
    // segmento e o gate de reamostragem).
    if (defdur_ns > 0) t.fps = 1e9 / (double)defdur_ns;

    // H26x em Matroska nunca e Annex-B: o CodecPrivate diz o tamanho do prefixo e traz os
    // parameter sets. Sem isso o decoder receberia bytes de comprimento no lugar do start
    // code e nao decodificaria nada.
    if (t.priv && t.priv_len > 0)
    {
        if      (t.codec == MEDIA_CODEC_H264) parse_avcc(&t, t.priv, t.priv_len);
        else if (t.codec == MEDIA_CODEC_H265) parse_hvcc(&t, t.priv, t.priv_len);
    }

    s->tr[s->ntr++] = t;
}

static void parse_tracks(SrcWebm* s, const uint8_t* d, int n)
{
    Mem m; m.d = d; m.n = n; m.i = 0;
    uint32_t id; const uint8_t* b; int bl;
    while (mem_elem(&m, &id, &b, &bl)) if (id == ID_TRACKENTRY) parse_track_entry(s, b, bl);
}

static void parse_info(SrcWebm* s, const uint8_t* d, int n)
{
    Mem m; m.d = d; m.n = n; m.i = 0;
    uint32_t id; const uint8_t* b; int bl;
    double dur_scaled = 0;

    while (mem_elem(&m, &id, &b, &bl))
    {
        if      (id == ID_TIMESCALE) s->tscale = be_uint(b, bl);
        else if (id == ID_DURATION)  dur_scaled = be_float(b, bl);
    }
    if (s->tscale == 0) s->tscale = 1000000;
    if (dur_scaled > 0) s->duration = dur_scaled * (double)s->tscale / 1e9;
}

// ---- interface MediaSource -------------------------------------------------

static int src_info(MediaSource* src, MediaStreamInfo* out, int max, int* count)
{
    SrcWebm* s = (SrcWebm*)src->Ctx;
    int n = 0;

    if (s->vt >= 0 && max > n)
    {
        WbmTrack* t = &s->tr[s->vt];
        MediaStreamInfo* v = &out[n++]; GW_ZERO(v, sizeof(*v));
        v->Type = MSTREAM_VIDEO; v->Codec = t->codec;
        v->Width = t->w; v->Height = t->h;
        v->Fps = t->fps > 0 ? t->fps : 30.0;
        v->Extra = t->priv; v->ExtraLen = t->priv_len;
    }
    if (s->at >= 0 && max > n)
    {
        WbmTrack* t = &s->tr[s->at];
        MediaStreamInfo* a = &out[n++]; GW_ZERO(a, sizeof(*a));
        a->Type = MSTREAM_AUDIO; a->Codec = t->codec;
        a->SampleRate = t->rate; a->Channels = t->channels;
        a->Extra = t->priv; a->ExtraLen = t->priv_len;
    }
    if (count) *count = n;
    return n;
}

static int ensure_cap(SrcWebm* s, int need)
{
    if (s->cap >= need) return 1;
    int nc = s->cap > 0 ? s->cap : 65536;
    while (nc < need) nc *= 2;
    uint8_t* nb = (uint8_t*)GW_REALLOC(s->buf, nc);
    if (!nb) return 0;
    s->buf = nb; s->cap = nc;
    return 1;
}

// Converte o bloco de s->buf (NALs prefixados por tamanho) para Annex-B em s->conv.
// with_ps=1 injeta os parameter sets antes -- usado nos keyframes.
static int annexb_convert(SrcWebm* s, const WbmTrack* t, int with_ps)
{
    const int ls = t->nal_len_size;
    const uint8_t* in = s->buf;
    const int n = s->buf_len;

    // Pior caso: cada NAL troca 'ls' bytes de tamanho por 4 de start code.
    int need = n + (n / (ls + 1) + 1) * 4 + (with_ps ? t->ps_len : 0) + 16;
    if (s->conv_cap < need)
    {
        uint8_t* nb = (uint8_t*)GW_REALLOC(s->conv, need);
        if (!nb) return 0;
        s->conv = nb; s->conv_cap = need;
    }

    int o = 0;
    if (with_ps && t->ps_len > 0) { memcpy(s->conv, t->ps, (size_t)t->ps_len); o = t->ps_len; }

    int i = 0;
    while (i + ls <= n)
    {
        int len = 0;
        for (int k = 0; k < ls; k++) len = (len << 8) | in[i + k];
        i += ls;
        if (len <= 0 || i + len > n) break;          // tamanho incoerente: para aqui

        s->conv[o++] = 0; s->conv[o++] = 0; s->conv[o++] = 0; s->conv[o++] = 1;
        memcpy(s->conv + o, in + i, (size_t)len);
        o += len; i += len;
    }
    s->conv_len = o;
    return 1;
}

// Corpo de SimpleBlock/Block: vint da faixa + int16 relativo + flags + dados.
// 1 = preencheu pkt; 0 = bloco de faixa que nao usamos (pulado); <0 = erro.
static int take_block(SrcWebm* s, uint64_t body_size, int is_simple, mtime_us dur, GwPacket* pkt)
{
    uint64_t start = wpos(s);
    uint64_t track;
    if (!read_vint(s, &track, 0)) return -1;

    uint8_t hdr[3];
    if (!rd(s, hdr, 3)) return -1;
    int16_t rel   = (int16_t)((hdr[0] << 8) | hdr[1]);
    int     flags = hdr[2];

    uint64_t consumed = wpos(s) - start;
    int64_t  data_len = (int64_t)body_size - (int64_t)consumed;
    if (data_len < 0) return -1;

    int idx = -1;
    for (int i = 0; i < s->ntr; i++) if (s->tr[i].number == (int)track) { idx = i; break; }

    int stream = (idx >= 0 && idx == s->vt) ? 0 : ((idx >= 0 && idx == s->at) ? 1 : -1);
    if (stream < 0)                                    // faixa que nao usamos: pula o corpo
    { gw_fseek64(s->fp, data_len, SEEK_CUR); return 0; }

    if (!ensure_cap(s, (int)data_len + 1)) return -1;
    if (!rd(s, s->buf, (int)data_len)) return -1;
    s->buf_len = (int)data_len;

    // Timestamp: (cluster + relativo) na unidade do TimestampScale (ns) -> us.
    int64_t  ticks = s->cluster_ts + rel;
    mtime_us pts   = (mtime_us)((double)ticks * (double)s->tscale / 1000.0);

    uint8_t* out_data = s->buf;
    int      out_size = (int)data_len;

    // avcC/hvcC -> Annex-B, com os parameter sets na frente de cada keyframe (o decoder
    // pode ser aberto no meio do arquivo, e repetir SPS/PPS e barato).
    WbmTrack* trk = &s->tr[idx];
    if (trk->nal_len_size > 0)
    {
        int is_key = is_simple ? ((flags & 0x80) != 0) : 1;
        if (!annexb_convert(s, trk, is_key)) return -1;
        out_data = s->conv; out_size = s->conv_len;
    }

    GW_ZERO(pkt, sizeof(*pkt));
    pkt->Stream = stream;
    pkt->Data = out_data; pkt->Size = out_size;
    pkt->Pts = pkt->Dts = pts;
    pkt->Dur = dur;
    // SimpleBlock traz o bit de keyframe em 0x80. O Block dentro de um BlockGroup nao tem
    // esse bit -- quem marca e a AUSENCIA de ReferenceBlock; assumir key e o lado seguro
    // (no pior caso o sink corta um segmento a mais, nunca corta no lugar errado).
    pkt->KeyFrame = is_simple ? ((flags & 0x80) != 0) : 1;
    return 1;
}

// Percorre os filhos de um BlockGroup ate achar o Block.
static int take_block_group(SrcWebm* s, uint64_t size, GwPacket* pkt)
{
    uint64_t end = wpos(s) + size;
    mtime_us dur = 0;
    int got = 0;

    while (wpos(s) < end)
    {
        uint32_t id; uint64_t sz;
        if (!read_elem(s, &id, &sz)) return -1;
        if (sz == WBM_UNKNOWN || wpos(s) + sz > end) return -1;

        if (id == ID_BLOCK && !got)
        {
            int r = take_block(s, sz, 0, dur, pkt);
            if (r < 0) return -1;
            got = r;
        }
        else if (id == ID_BLOCKDUR)
        {
            uint8_t tmp[8]; int n = sz > 8 ? 8 : (int)sz;
            if (!rd(s, tmp, n)) return -1;
            dur = (mtime_us)((double)be_uint(tmp, n) * (double)s->tscale / 1000.0);
            if (got) pkt->Dur = dur;                   // BlockDuration pode vir depois do Block
            if (sz > (uint64_t)n) gw_fseek64(s->fp, (int64_t)(sz - n), SEEK_CUR);
        }
        else gw_fseek64(s->fp, (int64_t)sz, SEEK_CUR);
    }
    return got;
}

static int src_read(MediaSource* src, GwPacket* pkt)
{
    SrcWebm* s = (SrcWebm*)src->Ctx;

    for (;;)
    {
        // Dentro de um Cluster: consome os blocos ate o fim dele.
        if (s->cluster_end && wpos(s) < s->cluster_end)
        {
            uint32_t id; uint64_t sz;
            if (!read_elem(s, &id, &sz)) { s->cluster_end = 0; continue; }
            if (sz == WBM_UNKNOWN)       { s->cluster_end = 0; continue; }

            if (id == ID_TIMESTAMP)
            {
                uint8_t tmp[8]; int n = sz > 8 ? 8 : (int)sz;
                if (!rd(s, tmp, n)) return -1;
                s->cluster_ts = (int64_t)be_uint(tmp, n);
                if (sz > (uint64_t)n) gw_fseek64(s->fp, (int64_t)(sz - n), SEEK_CUR);
                continue;
            }
            if (id == ID_SIMPLEBLOCK)
            {
                int r = take_block(s, sz, 1, 0, pkt);
                if (r < 0) return -1;
                if (r == 1) return 1;
                continue;
            }
            if (id == ID_BLOCKGROUP)
            {
                int r = take_block_group(s, sz, pkt);
                if (r < 0) return -1;
                if (r == 1) return 1;
                continue;
            }
            gw_fseek64(s->fp, (int64_t)sz, SEEK_CUR);
            continue;
        }

        s->cluster_end = 0;

        // Proximo elemento de topo do Segment: daqui em diante so Cluster interessa.
        if (s->seg_end != WBM_UNKNOWN && wpos(s) >= s->seg_end) return 0;   // EOF

        uint32_t id; uint64_t sz;
        if (!read_elem(s, &id, &sz)) return 0;                              // EOF limpo

        if (id == ID_CLUSTER)
        {
            s->cluster_end = (sz == WBM_UNKNOWN) ? WBM_UNKNOWN : wpos(s) + sz;
            s->cluster_ts  = 0;
            continue;
        }
        if (sz == WBM_UNKNOWN) return 0;
        gw_fseek64(s->fp, (int64_t)sz, SEEK_CUR);
    }
}

static int src_islive(MediaSource* src) { (void)src; return 0; }

static void src_close_inner(SrcWebm* s)
{
    if (!s) return;
    for (int i = 0; i < s->ntr; i++) { GW_FREE(s->tr[i].priv); GW_FREE(s->tr[i].ps); }
    if (s->fp) fclose(s->fp);
    GW_FREE(s->buf);
    GW_FREE(s->conv);
    GW_FREE(s);
}

static void src_close(MediaSource* src)
{
    if (!src) return;
    src_close_inner((SrcWebm*)src->Ctx);
    GW_FREE(src);
}

MediaSource* source_webm_open(const char* path)
{
    if (!path) return 0;

    SrcWebm* s = (SrcWebm*)GW_CALLOC(1, sizeof(SrcWebm));
    if (!s) return 0;
    s->tscale = 1000000; s->vt = -1; s->at = -1; s->seg_end = WBM_UNKNOWN;

    s->fp = gw_fopen_rb(path);
    if (!s->fp) { GW_FREE(s); return 0; }

    // 1) cabecalho EBML (so valida que e Matroska/WebM) e entrada no Segment.
    uint32_t id; uint64_t sz;
    if (!read_elem(s, &id, &sz) || id != ID_EBML) { src_close_inner(s); return 0; }
    gw_fseek64(s->fp, (int64_t)sz, SEEK_CUR);

    if (!read_elem(s, &id, &sz) || id != ID_SEGMENT) { src_close_inner(s); return 0; }
    s->seg_end = (sz == WBM_UNKNOWN) ? WBM_UNKNOWN : wpos(s) + sz;

    // 2) filhos do Segment ate o PRIMEIRO Cluster. Info e Tracks vem antes dele em
    //    qualquer arquivo valido; ao topar no Cluster paramos, deixando o ponteiro no
    //    corpo dele -- que e exatamente onde o src_read comeca a trabalhar.
    for (;;)
    {
        if (s->seg_end != WBM_UNKNOWN && wpos(s) >= s->seg_end) break;
        if (!read_elem(s, &id, &sz)) break;

        if (id == ID_CLUSTER)
        {
            s->cluster_end = (sz == WBM_UNKNOWN) ? WBM_UNKNOWN : wpos(s) + sz;
            s->cluster_ts  = 0;
            break;
        }
        if ((id == ID_INFO || id == ID_TRACKS) && sz != WBM_UNKNOWN && sz < (16u << 20))
        {
            uint8_t* body = (uint8_t*)GW_ALLOC((int)sz);
            if (!body) { src_close_inner(s); return 0; }
            if (!rd(s, body, (int)sz)) { GW_FREE(body); src_close_inner(s); return 0; }
            if (id == ID_INFO) parse_info(s, body, (int)sz); else parse_tracks(s, body, (int)sz);
            GW_FREE(body);
            continue;
        }
        if (sz == WBM_UNKNOWN) break;
        gw_fseek64(s->fp, (int64_t)sz, SEEK_CUR);
    }

    // 3) primeira faixa de video e primeira de audio que sabemos decodificar.
    for (int i = 0; i < s->ntr; i++)
    {
        if (s->tr[i].codec == MEDIA_CODEC_NONE) continue;
        if      (s->tr[i].type == 1 && s->vt < 0) s->vt = i;
        else if (s->tr[i].type == 2 && s->at < 0) s->at = i;
    }
    if (s->vt < 0) { src_close_inner(s); return 0; }   // sem video o gateway nao tem o que fazer

    MediaSource* src = (MediaSource*)GW_CALLOC(1, sizeof(MediaSource));
    if (!src) { src_close_inner(s); return 0; }
    src->Ctx = s;
    src->Info = src_info; src->Read = src_read; src->IsLive = src_islive; src->Close = src_close;
    return src;
}
