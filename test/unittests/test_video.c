//  Testes do caminho de VIDEO: tempos do MP4 (lista de edicao, quadros B), copia sem
//  recodificar, manifestos e sessao.
//
//  O que eles guardam e o que ja quebrou e so apareceu olhando a saida com ferramenta de fora:
//  trecho escondido de arquivo cortado aparecendo no HLS, video comecando um quadro depois do
//  zero, audio atrasado que fazia o player pular o comeco, texto de codec fixo no manifesto,
//  init de H.265 gravado como H.264, manifesto que nao batia com os segmentos a 29,97 fps.
//
//  Todos sao AUTOCONTIDOS: a entrada e gerada aqui, com o encoder e o muxer do projeto. O
//  conteudo dos quadros nao importa para quem copia sem recodificar -- importam os TEMPOS --,
//  entao um arquivo "com quadros B" e um arquivo comum gravado com os tempos de apresentacao
//  e de decodificacao que um arquivo com quadros B teria. A saida e conferida lendo as caixas
//  dos arquivos gerados (tfdt, trun, avcC, hvcC), nao por ida e volta no proprio leitor.

#include "testes.h"
#include "media_codec.h"
#include "memory_pool.h"
#include "media_source.h"
#include "sync_gateway.h"
#include "gw_decode.h"       // gw_vdec_available: H.264/H.265 nao passam pela tabela de decoders de media_codec
#include "gw_path.h"
#include "mux_mp4.h"
#include "mp4_timing.h"
#include "h26x_util.h"
#include "aac_silence.h"
#include "frag_session.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define LARG 320
#define ALT  240
#define PASTA "unittests_tmp/video"

#define MAXQ  256     // quadros de uma entrada
#define MAXA  1024    // amostras de um segmento
#define MAXS  16      // segmentos de uma faixa

#define AUD_TAXA   44100
#define AUD_BYTES  20     // tamanho do quadro "de verdade" (diferente do de silencio, que tem 9)
static const unsigned char ASC_LC_ESTEREO[2] = { 0x12, 0x10 };   // AAC-LC, 44,1 kHz, 2 canais

// ---- leitura de arquivos e caixas --------------------------------------------------------

static unsigned char* le_arquivo(const char* path, long* n)
{
    FILE* f = fopen(path, "rb");
    unsigned char* d;
    long t;
    *n = 0;
    if (!f) return 0;
    fseek(f, 0, SEEK_END); t = ftell(f); fseek(f, 0, SEEK_SET);
    d = (unsigned char*)malloc((size_t)t + 1);
    if (d && fread(d, 1, (size_t)t, f) != (size_t)t) { free(d); d = 0; }
    fclose(f);
    if (d) { d[t] = 0; *n = t; }
    return d;
}

// Posicao do tipo de caixa (4 letras) a partir de 'de'; -1 se nao ha.
static long acha4(const unsigned char* d, long n, const char* t, long de)
{
    long i;
    for (i = de < 0 ? 0 : de; i + 4 <= n; i++) if (memcmp(d + i, t, 4) == 0) return i;
    return -1;
}

static int tem_caixa(const char* path, const char* tipo)
{
    long n; unsigned char* d = le_arquivo(path, &n);
    int ok = d && acha4(d, n, tipo, 0) >= 0;
    free(d);
    return ok;
}

static unsigned long be32(const unsigned char* p)
{ return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) | ((unsigned long)p[2] << 8) | p[3]; }

static long long be64(const unsigned char* p) { return ((long long)be32(p) << 32) | (long long)be32(p + 4); }

typedef struct { long long dts, pts; int dur, tam, chave; } Amostra;
typedef struct { long long tfdt; int trun_ver, n; long long menor_pts; Amostra a[MAXA]; } Segmento;

// Um segmento fMP4 (moof + mdat): tempo-base (tfdt) e as amostras do trun. 0 em sucesso.
static int le_segmento(const char* path, Segmento* s)
{
    long n, p, q; unsigned char* d = le_arquivo(path, &n);
    unsigned long fl, cnt, i;
    long long dts;
    long moof;

    memset(s, 0, sizeof(*s));
    if (!d) return -1;
    if (n < 16 || memcmp(d + 4, "moof", 4) != 0) { free(d); return -2; }
    moof = (long)be32(d);
    if (moof > n) moof = n;

    p = acha4(d, moof, "tfdt", 0);
    if (p < 0) { free(d); return -3; }
    s->tfdt = (d[p + 4] == 1) ? be64(d + p + 8) : (long long)be32(d + p + 8);

    p = acha4(d, moof, "trun", 0);
    if (p < 0) { free(d); return -4; }
    s->trun_ver = d[p + 4];
    fl  = be32(d + p + 4) & 0xFFFFFF;
    cnt = be32(d + p + 8);
    q = p + 12;
    if (fl & 0x001) q += 4;
    if (fl & 0x004) q += 4;
    dts = s->tfdt;
    for (i = 0; i < cnt && s->n < MAXA && q + 16 <= n; i++)
    {
        Amostra* a = &s->a[s->n];
        long cts = 0; unsigned long flg = 0;
        if (fl & 0x100) { a->dur = (int)be32(d + q); q += 4; }
        if (fl & 0x200) { a->tam = (int)be32(d + q); q += 4; }
        if (fl & 0x400) { flg = be32(d + q); q += 4; }
        if (fl & 0x800) { unsigned long b = be32(d + q); cts = (s->trun_ver == 1) ? (long)(int)b : (long)b; q += 4; }
        a->dts = dts; a->pts = dts + cts; a->chave = (flg & 0x10000) ? 0 : 1;
        if (s->n == 0 || a->pts < s->menor_pts) s->menor_pts = a->pts;
        dts += a->dur; s->n++;
    }
    free(d);
    return (unsigned long)s->n == cnt ? 0 : -5;
}

static Segmento g_seg[MAXS];    // os testes rodam um de cada vez
static Segmento g_aud[MAXS];

// Todos os segmentos <pasta>/<nome>-N.m4s, em ordem. Devolve quantos leu.
static int le_faixa(const char* pasta, const char* nome, Segmento* segs)
{
    int k;
    for (k = 0; k < MAXS; k++)
    {
        char arq[600];
        snprintf(arq, sizeof(arq), "%s/%s-%d.m4s", pasta, nome, k);
        if (!gw_file_exists(arq)) break;
        if (le_segmento(arq, &segs[k]) != 0) return -1;
    }
    return k;
}

static int total_amostras(const Segmento* s, int n) { int t = 0, i; for (i = 0; i < n; i++) t += s[i].n; return t; }

// Inicio de cada segmento da 'qual'-esima <SegmentTimeline> do manifesto (r expandido).
static int le_timeline(const char* txt, int qual, long long* t, int max)
{
    const char* p = txt; const char* fim;
    int i, n = 0;
    for (i = 0; i <= qual; i++) { p = strstr(p, "<SegmentTimeline>"); if (!p) return -1; p += 17; }
    fim = strstr(p, "</SegmentTimeline>");
    if (!fim) return -1;
    while ((p = strstr(p, "<S ")) != 0 && p < fim)
    {
        const char* ft = strstr(p, "t=\""); const char* fd = strstr(p, "d=\""); const char* fr = strstr(p, "r=\"");
        const char* fecha = strchr(p, '>');
        long long tt, dd; int rr = 0, k;
        if (!ft || !fd || !fecha || ft > fecha || fd > fecha) return -1;
        tt = atoll(ft + 3); dd = atoll(fd + 3);
        if (fr && fr < fecha) rr = atoi(fr + 3);
        for (k = 0; k <= rr && n < max; k++) t[n++] = tt + dd * k;
        p = fecha;
    }
    return n;
}

static int cmp_ll(const void* a, const void* b)
{ long long x = *(const long long*)a, y = *(const long long*)b; return x < y ? -1 : x > y ? 1 : 0; }

// ---- entrada: MP4 gravado com os tempos que se quer testar -------------------------------

typedef struct
{
    MediaCodec codec;
    int quadros;
    int num, den;        // taxa de quadros = num/den
    int quadros_b;       // 1: tempos de arquivo com quadros B (I P B B P B B ..., por grupo); 2: so nos grupos depois de zero
    int escondidos;      // quadros apresentados antes de zero (arquivo cortado sem recodificar)
    int audio;           // 1 = faixa AAC
    int audio_ini_ms;    // onde o audio entra: > 0 atrasado (edicao vazia), < 0 comeco escondido
    int chave_cada;      // pede quadro-chave a cada N (o encoder pode ter a propria cadencia)
    int termina_em_chave;   // 1: descarta o que vem depois do ultimo quadro-chave
}
Receita;

typedef struct { int n, n_audio; long long pts[MAXQ], dts[MAXQ]; int chave[MAXQ], tam[MAXQ]; } Tempos;   // ms; tam em bytes

static long long t_ms(const Receita* r, long long quadro) { return quadro * 1000 * r->den / r->num; }

// Ordem de APRESENTACAO do quadro que e o i-esimo na ordem de decodificacao. Com quadros B o
// grupo e I P B B P B B ...: o P e decodificado antes dos dois B que aparecem antes dele. Os
// grupos comecam onde o ENCODER pos quadro-chave (tp->chave), nao onde foi pedido: um
// quadro-chave nunca e apresentado antes de ser decodificado, e um tempo inventado que
// violasse isso nao existiria num arquivo de verdade.
static long long apresentacao(const Receita* r, const Tempos* tp, int i)
{
    int g0 = i, g1 = i + 1, l, tam, trio;
    if (!r->quadros_b) return i;
    while (g0 > 0 && !tp->chave[g0]) g0--;
    while (g1 < tp->n && !tp->chave[g1]) g1++;
    l = i - g0; tam = g1 - g0;
    if (l == 0) return g0;
    // 2: o grupo que tem o trecho escondido fica em ordem de decodificacao. Esse grupo PASSA
    // PELO DECODER, e ali um tempo de quadro B inventado sobre um fluxo que nao reordena de
    // verdade nao bate com a ordem em que o decoder devolve os quadros.
    if (r->quadros_b == 2 && g0 <= r->escondidos) return i;
    trio = l - ((l - 1) % 3);                 // primeiro do trio P B B
    if (trio + 2 >= tam) return i;            // trio incompleto no fim do grupo: ordem de decodificacao
    return g0 + (((l - 1) % 3 == 0) ? l + 2 : l - 1);
}

static void preenche(unsigned char* y, unsigned char* u, unsigned char* v, int quadro)
{
    int i, j;
    for (j = 0; j < ALT; j++)
        for (i = 0; i < LARG; i++)
            y[j * LARG + i] = (unsigned char)((i + j + quadro * 3) & 0xFF);
    memset(u, 128, (size_t)(LARG / 2) * (ALT / 2));
    memset(v, 128, (size_t)(LARG / 2) * (ALT / 2));
}

typedef struct { H26xToLen conv; Tempos* tp; unsigned char* dado[MAXQ]; int tam[MAXQ]; } Coletor;

// Guarda o pacote (ja com prefixo de tamanho): os tempos so sao atribuidos depois do encode,
// quando se sabe onde cairam os quadros-chave.
static void guarda_pacote(Coletor* g, const MediaPacket* pk)
{
    uint8_t* out = 0;
    int alen = h26x_tl_feed(&g->conv, pk->Data, pk->Size, &out);
    int i = g->tp->n;
    if (alen <= 0 || i >= MAXQ) return;
    g->dado[i] = (unsigned char*)malloc((size_t)alen);
    memcpy(g->dado[i], out, (size_t)alen);
    g->tam[i] = alen;
    g->tp->chave[i] = pk->KeyFrame ? 1 : 0;
    g->tp->n++;
}

static Coletor g_coletor;   // grande demais para a pilha; os testes rodam um de cada vez

// Gera o MP4 da receita. 0 em sucesso; 'tp' recebe os tempos gravados (ms).
static int gera_mp4(const char* path, const Receita* rc, Tempos* tp)
{
    MediaEncoderParams p;
    MediaEncoder* enc;
    MediaPacket pk;
    Coletor* g = &g_coletor;
    Mp4Mux* mux = 0;
    unsigned char *y, *u, *v;
    int f, i, rc_envio = 0, codificados = 0;

    memset(tp, 0, sizeof(*tp));
    memset(&p, 0, sizeof(p));
    p.Codec = rc->codec; p.Width = LARG; p.Height = ALT;
    p.Fps = (rc->num + rc->den / 2) / rc->den; p.BitrateBps = 400000; p.Threads = 2;

    enc = media_encoder_open(&p);
    if (!enc) return -1;

    memset(g, 0, sizeof(*g));
    g->tp = tp;
    h26x_tl_init(&g->conv, rc->codec == MEDIA_CODEC_H265);

    y = (unsigned char*)malloc(LARG * ALT);
    u = (unsigned char*)malloc(LARG * ALT / 4);
    v = (unsigned char*)malloc(LARG * ALT / 4);

    for (f = 0; f < rc->quadros; f++)
    {
        MediaFrame fr;
        preenche(y, u, v, f);
        memset(&fr, 0, sizeof(fr));
        fr.Kind = MEDIA_KIND_VIDEO; fr.Width = LARG; fr.Height = ALT;
        fr.Y = y; fr.StrideY = LARG; fr.U = u; fr.StrideU = LARG / 2; fr.V = v; fr.StrideV = LARG / 2;
        fr.Pts = f;
        fr.KeyFrame = (rc->chave_cada > 0 && f % rc->chave_cada == 0);
        if (enc->SendFrame(enc, &fr) < 0) { rc_envio = -3; break; }
        while (enc->ReceivePacket(enc, &pk) == 1) guarda_pacote(g, &pk);
    }
    if (rc_envio == 0)
    {
        enc->SendFrame(enc, 0);
        while (enc->ReceivePacket(enc, &pk) == 1) guarda_pacote(g, &pk);
    }

    if (rc_envio == 0 && tp->n > 0 && h26x_tl_ready(&g->conv))
    {
        const uint8_t* asc = rc->audio ? ASC_LC_ESTEREO : 0;
        int asc_len = rc->audio ? (int)sizeof(ASC_LC_ESTEREO) : 0;
        if (rc->codec == MEDIA_CODEC_H265)
            mux = mp4_open_hevc(path, LARG, ALT, g->conv.cfg, g->conv.cfg_len, asc, asc_len, AUD_TAXA, 2);
        else
            mux = mp4_open2(path, LARG, ALT, g->conv.cfg, g->conv.cfg_len, asc, asc_len, AUD_TAXA, 2);
    }

    codificados = tp->n;
    if (rc->termina_em_chave)
    {
        int ult = tp->n - 1;
        while (ult > 0 && !tp->chave[ult]) ult--;
        if (ult > 0) tp->n = ult + 1;
    }

    if (mux)
    {
        long long fim_ms;
        tp->chave[0] = 1;   // o primeiro quadro abre o primeiro grupo
        for (i = 0; i < tp->n; i++)
        {
            tp->tam[i] = g->tam[i];
            tp->pts[i] = t_ms(rc, apresentacao(rc, tp, i) - rc->escondidos);
            tp->dts[i] = t_ms(rc, (long long)i - (rc->quadros_b ? 1 : 0) - rc->escondidos);
            mp4_write_video2(mux, tp->pts[i], tp->dts[i], tp->chave[i], g->dado[i], g->tam[i]);
        }
        if (rc->audio)
        {
            unsigned char quadro[AUD_BYTES];
            long long amostras;
            fim_ms = t_ms(rc, tp->n);
            amostras = fim_ms * AUD_TAXA / 1000;
            int na = (int)((amostras + 1023) / 1024), k;
            memset(quadro, 0x5A, sizeof(quadro));
            mp4_set_audio_start(mux, rc->audio_ini_ms);
            for (k = 0; k < na; k++) mp4_write_audio(mux, 1024, quadro, (int)sizeof(quadro));
            tp->n_audio = na;
        }
        mp4_close(mux);
    }

    for (i = 0; i < codificados; i++) free(g->dado[i]);
    h26x_tl_free(&g->conv);
    enc->Close(enc);
    free(y); free(u); free(v);

    if (rc_envio != 0) return rc_envio;
    if (!mux) return -2;
    return 0;
}

static void caminho(char* out, int tam, const char* nome)
{
    gw_mkdir_p(PASTA);
    snprintf(out, (size_t)tam, "%s/%s", PASTA, nome);
}

// ---- gateway --------------------------------------------------------------------------------

typedef struct { int done, erro; char pista[400]; } Ev;

static void ao_evento(void* user, const GwEvent* e)
{
    Ev* g = (Ev*)user;
    switch (e->Kind)
    {
        case GW_EV_DONE:  g->done = 1; break;
        case GW_EV_ERROR: g->erro = 1; break;
        case GW_EV_TRACK_START:
            if (!g->pista[0] && e->Message) snprintf(g->pista, sizeof(g->pista), "%s", e->Message);
            break;
        default: break;
    }
}

static int roda_gw(const char* entrada, const MediaProfile* p, const char* pasta, Ev* ev)
{
    MediaSource* src;
    GwFeedback fb;
    int rc;

    gw_rmtree(pasta);
    gw_mkdir_p(pasta);
    src = source_file_open(entrada);
    if (!src) return -100;

    memset(ev, 0, sizeof(*ev));
    memset(&fb, 0, sizeof(fb));
    fb.Fn = ao_evento; fb.User = ev;
    rc = gateway_run(src, p, pasta, &fb, 0);
    src->Close(src);
    return rc;
}

// Perfil que herda tudo da entrada: mesma resolucao, mesmo codec, uma pista -- e o pedido de
// copia sem recodificar.
static void perfil_copia(MediaProfile* p, Container c)
{
    memset(p, 0, sizeof(*p));
    p->Container = c; p->AudioCodec = MEDIA_CODEC_AAC; p->SegmentMs = 1000; p->Mode = GW_VOD;
}

#define PRECISA_ENCODER(r, c) \
    do { if (!media_codec_available(c)) T_SKIP((r), "encoder de %s nao disponivel neste build", media_codec_name(c)); } while (0)
#define PRECISA_DECODER(r, c) \
    do { if (!gw_vdec_available(c)) T_SKIP((r), "decoder de %s nao disponivel neste build", media_codec_name(c)); } while (0)

// As tres receitas que se repetem.
static const Receita ARQ_COMUM    = { MEDIA_CODEC_H264, 93, 25, 1, 1, 0,  1, 0,    31 };   // com quadros B
static const Receita ARQ_CORTADO  = { MEDIA_CODEC_H264, 60, 25, 1, 0, 10, 1, -200, 30 };   // 10 quadros escondidos
static const Receita ARQ_ATRASADO = { MEDIA_CODEC_H264, 61, 25, 1, 0, 0,  1, 500,  30 };   // audio entra em 0,5 s

// ---- tempos lidos do MP4 --------------------------------------------------------------------

// Confere a tabela de tempos de 'arq' contra os tempos gravados. "" se bate; senao, o motivo.
static const char* confere_tempos(const char* arq, const Tempos* tp, char* motivo, int tam)
{
    int64_t *pts = 0, *dts = 0; uint32_t n = 0, i;
    int rc = mp4_video_times_us(arq, &pts, &dts, &n);
    motivo[0] = 0;
    if (rc != 0) snprintf(motivo, (size_t)tam, "mp4_video_times_us devolveu %d", rc);
    else if ((int)n != tp->n) snprintf(motivo, (size_t)tam, "esperava %d quadros na tabela, vieram %u", tp->n, n);
    else
        for (i = 0; i < n; i++)
            if (pts[i] != tp->pts[i] * 1000 || dts[i] != tp->dts[i] * 1000)
            {
                snprintf(motivo, (size_t)tam, "quadro %u: esperava pts=%lld dts=%lld us, veio pts=%lld dts=%lld",
                         i, tp->pts[i] * 1000, tp->dts[i] * 1000, (long long)pts[i], (long long)dts[i]);
                break;
            }
    if (pts) memop_free_raw(pts);
    if (dts) memop_free_raw(dts);
    return motivo;
}

typedef struct { int nst, v_neg, n_video, n_audio; long long preroll, v0_pts, v0_dts, a0_pts; } Leitura;

// Le a fonte inteira pelo caminho que o gateway usa.
static int le_fonte(const char* arq, Leitura* l)
{
    MediaSource* src = source_file_open(arq);
    MediaStreamInfo st[4];
    GwPacket pkt;
    int vidx = -1, aidx = -1, i;

    memset(l, 0, sizeof(*l));
    if (!src) return -1;
    memset(st, 0, sizeof(st));
    src->Info(src, st, 4, &l->nst);
    for (i = 0; i < l->nst; i++) { if (st[i].Type == MSTREAM_VIDEO && vidx < 0) vidx = i; else if (st[i].Type == MSTREAM_AUDIO && aidx < 0) aidx = i; }
    if (vidx >= 0) l->preroll = (long long)st[vidx].PrerollUs;
    while (src->Read(src, &pkt) == 1)
    {
        if (pkt.Stream == vidx)
        {
            if (l->n_video == 0) { l->v0_pts = (long long)pkt.Pts; l->v0_dts = (long long)pkt.Dts; }
            if (pkt.Pts < 0) l->v_neg++;
            l->n_video++;
        }
        else if (pkt.Stream == aidx)
        {
            if (l->n_audio == 0) l->a0_pts = (long long)pkt.Pts;
            l->n_audio++;
        }
    }
    src->Close(src);
    return 0;
}

static long long modulo(long long v) { return v < 0 ? -v : v; }

void teste_video_tempos_arquivo_comum(TestResult* r)
{
    char arq[512], motivo[256];
    Tempos tp; Leitura l;
    int64_t vo = 1, ao = 1;
    int rc;

    t_start(r);
    PRECISA_ENCODER(r, MEDIA_CODEC_H264);
    caminho(arq, sizeof(arq), "comum.mp4");

    rc = gera_mp4(arq, &ARQ_COMUM, &tp);
    T_ASSERT(r, rc == 0, "nao foi possivel gerar a entrada (codigo %d)", rc);
    T_ASSERT(r, tp.n == ARQ_COMUM.quadros, "o encoder devolveu %d pacotes para %d quadros", tp.n, ARQ_COMUM.quadros);

    T_ASSERT(r, confere_tempos(arq, &tp, motivo, sizeof(motivo))[0] == 0, "tabela de tempos: %s", motivo);

    // arquivo comum: a lista de edicao so compensa o atraso dos quadros B -- nada escondido
    T_ASSERT(r, mp4_edit_offsets(arq, &vo, &ao) == 1, "mp4_edit_offsets nao leu o moov");
    T_ASSERT(r, vo == 0 && ao == 0, "esperava deslocamento 0/0, veio video=%lld audio=%lld us", (long long)vo, (long long)ao);

    T_ASSERT(r, le_fonte(arq, &l) == 0, "a fonte nao abriu o arquivo");
    T_ASSERT(r, l.nst == 2, "esperava video + audio, vieram %d faixas", l.nst);
    T_ASSERT(r, l.preroll == 0, "arquivo comum nao tem pre-rolo, a fonte informou %lld us", l.preroll);
    T_ASSERT(r, l.v0_pts == 0 && l.v0_dts == -40000,
             "primeiro quadro: esperava pts=0 dts=-40000 us (um quadro antes de zero), veio pts=%lld dts=%lld", l.v0_pts, l.v0_dts);
    T_ASSERT(r, l.n_video == tp.n && l.v_neg == 0, "esperava %d quadros e nenhum antes de zero, vieram %d e %d", tp.n, l.n_video, l.v_neg);
    T_ASSERT(r, l.a0_pts == 0, "o audio devia comecar em zero, comecou em %lld us", l.a0_pts);
}

void teste_video_tempos_arquivo_cortado(TestResult* r)
{
    char arq[512], motivo[256];
    Tempos tp; Leitura l;
    int64_t vo = 0, ao = 0;
    int rc;

    t_start(r);
    PRECISA_ENCODER(r, MEDIA_CODEC_H264);
    caminho(arq, sizeof(arq), "cortado.mp4");

    rc = gera_mp4(arq, &ARQ_CORTADO, &tp);
    T_ASSERT(r, rc == 0, "nao foi possivel gerar a entrada (codigo %d)", rc);
    T_ASSERT(r, tp.n == ARQ_CORTADO.quadros, "o encoder devolveu %d pacotes para %d quadros", tp.n, ARQ_CORTADO.quadros);

    T_ASSERT(r, confere_tempos(arq, &tp, motivo, sizeof(motivo))[0] == 0, "tabela de tempos: %s", motivo);

    // 10 quadros de 40 ms escondidos no video; 200 ms escondidos no audio
    T_ASSERT(r, mp4_edit_offsets(arq, &vo, &ao) == 1, "mp4_edit_offsets nao leu o moov");
    T_ASSERT(r, vo == -400000, "deslocamento do video: esperava -400000 us, veio %lld", (long long)vo);
    T_ASSERT(r, ao == -200000, "deslocamento do audio: esperava -200000 us, veio %lld", (long long)ao);

    T_ASSERT(r, le_fonte(arq, &l) == 0, "a fonte nao abriu o arquivo");
    T_ASSERT(r, l.preroll == 400000, "pre-rolo: esperava 400000 us, a fonte informou %lld", l.preroll);
    T_ASSERT(r, l.v_neg == 10, "esperava 10 quadros antes de zero, vieram %d", l.v_neg);
    T_ASSERT(r, l.n_video == tp.n, "esperava %d quadros, vieram %d", tp.n, l.n_video);
    T_ASSERT(r, modulo(l.a0_pts + 200000) <= 100, "o audio do pre-rolo devia sair a partir de -200000 us, saiu de %lld", l.a0_pts);
}

void teste_video_tempos_audio_atrasado(TestResult* r)
{
    char arq[512];
    Tempos tp; Leitura l;
    int64_t vo = 1, ao = 0;
    int rc;

    t_start(r);
    PRECISA_ENCODER(r, MEDIA_CODEC_H264);
    caminho(arq, sizeof(arq), "atrasado.mp4");

    rc = gera_mp4(arq, &ARQ_ATRASADO, &tp);
    T_ASSERT(r, rc == 0, "nao foi possivel gerar a entrada (codigo %d)", rc);

    T_ASSERT(r, mp4_edit_offsets(arq, &vo, &ao) == 1, "mp4_edit_offsets nao leu o moov");
    T_ASSERT(r, vo == 0, "deslocamento do video: esperava 0, veio %lld us", (long long)vo);
    T_ASSERT(r, ao == 500000, "edicao vazia do audio: esperava 500000 us, veio %lld", (long long)ao);

    T_ASSERT(r, le_fonte(arq, &l) == 0, "a fonte nao abriu o arquivo");
    T_ASSERT(r, l.preroll == 0, "audio atrasado nao e pre-rolo de video, a fonte informou %lld us", l.preroll);
    T_ASSERT(r, modulo(l.a0_pts - 500000) <= 100, "o primeiro pacote de audio devia sair em 500000 us, saiu em %lld", l.a0_pts);
    T_ASSERT(r, l.n_audio == tp.n_audio, "esperava %d pacotes de audio, vieram %d", tp.n_audio, l.n_audio);
}

// ---- copia sem recodificar ------------------------------------------------------------------

void teste_video_copia_mp4_com_quadros_b(TestResult* r)
{
    char arq[512], pasta[512], saida[600], motivo[256];
    Tempos tp; Ev ev; MediaProfile p;
    int rc, rate = 0, ch = 0; char acodec[8] = { 0 };

    t_start(r);
    PRECISA_ENCODER(r, MEDIA_CODEC_H264);
    caminho(arq, sizeof(arq), "copia_b.mp4");
    caminho(pasta, sizeof(pasta), "copia_b_out");

    rc = gera_mp4(arq, &ARQ_COMUM, &tp);
    T_ASSERT(r, rc == 0 && tp.n == ARQ_COMUM.quadros, "nao foi possivel gerar a entrada (codigo %d, %d quadros)", rc, tp.n);

    perfil_copia(&p, CONT_MP4_FILE);
    rc = roda_gw(arq, &p, pasta, &ev);
    T_ASSERT(r, rc == 0 && ev.done && !ev.erro, "gateway_run devolveu %d (done=%d erro=%d)", rc, ev.done, ev.erro);
    T_ASSERT(r, strstr(ev.pista, "bypass") != 0, "esperava copia sem recodificar, a pista informou: %s", ev.pista);

    gw_path_join(saida, sizeof(saida), pasta, "output.mp4");
    T_ASSERT(r, gw_file_exists(saida), "output.mp4 nao foi gerado");
    // os dois tempos de cada quadro chegam inteiros ao arquivo de saida
    T_ASSERT(r, confere_tempos(saida, &tp, motivo, sizeof(motivo))[0] == 0, "tempos da copia: %s", motivo);
    T_ASSERT(r, tem_caixa(saida, "ctts"), "a copia de fonte com quadros B precisa da tabela de composicao (ctts)");
    T_ASSERT(r, tem_caixa(saida, "elst"), "a copia de fonte com quadros B precisa da lista de edicao (elst)");
    T_ASSERT(r, mp4_audio_info(saida, acodec, &rate, &ch) >= 0 && rate == AUD_TAXA, "o audio nao foi junto (taxa lida: %d)", rate);
}

void teste_video_copia_hls_comeca_em_zero(TestResult* r)
{
    char arq[512], pasta[512], nome[600], esperado[32];
    Tempos tp; Ev ev; MediaProfile p;
    long long entrada[MAXQ], saida[MAXQ];
    unsigned char* d; char* txt; long n, pos;
    int rc, nseg, naud, i, k, tot = 0, negativos = 0;

    t_start(r);
    esperado[0] = 0;
    PRECISA_ENCODER(r, MEDIA_CODEC_H264);
    caminho(arq, sizeof(arq), "hls_b.mp4");
    caminho(pasta, sizeof(pasta), "hls_b_out");

    rc = gera_mp4(arq, &ARQ_COMUM, &tp);
    T_ASSERT(r, rc == 0 && tp.n == ARQ_COMUM.quadros, "nao foi possivel gerar a entrada (codigo %d, %d quadros)", rc, tp.n);

    perfil_copia(&p, CONT_HLS_FMP4);
    rc = roda_gw(arq, &p, pasta, &ev);
    T_ASSERT(r, rc == 0 && ev.done && !ev.erro, "gateway_run devolveu %d (done=%d erro=%d)", rc, ev.done, ev.erro);
    T_ASSERT(r, strstr(ev.pista, "bypass") != 0, "esperava copia sem recodificar, a pista informou: %s", ev.pista);

    nseg = le_faixa(pasta, "source", g_seg);
    T_ASSERT(r, nseg >= 2, "esperava ao menos 2 segmentos de video (um por quadro-chave), vieram %d", nseg);
    T_ASSERT(r, total_amostras(g_seg, nseg) == tp.n, "esperava %d quadros nos segmentos, vieram %d", tp.n, total_amostras(g_seg, nseg));

    // o video comeca em ZERO: antes a linha do tempo inteira andava um quadro (40 ms)
    T_ASSERT(r, g_seg[0].tfdt == 0, "o tempo-base do primeiro segmento devia ser 0, e %lld", g_seg[0].tfdt);
    T_ASSERT(r, g_seg[0].a[0].pts == 0, "o primeiro quadro devia ser apresentado em 0, e em %lld ms", g_seg[0].a[0].pts);
    T_ASSERT(r, g_seg[0].trun_ver == 1, "composicao com sinal exige trun versao 1, veio versao %d", g_seg[0].trun_ver);

    for (k = 0; k < nseg; k++)
    {
        T_ASSERT(r, g_seg[k].a[0].chave, "o segmento %d nao comeca em quadro-chave", k);
        T_ASSERT(r, g_seg[k].tfdt == g_seg[k].menor_pts,
                 "segmento %d: o tempo-base (%lld) devia ser o do primeiro quadro apresentado (%lld)", k, g_seg[k].tfdt, g_seg[k].menor_pts);
        for (i = 0; i < g_seg[k].n; i++)
        {
            if (g_seg[k].a[i].pts < g_seg[k].a[i].dts) negativos++;
            saida[tot++] = g_seg[k].a[i].pts;
        }
    }
    T_ASSERT(r, negativos > 0, "fonte com quadros B copiada devia ter amostras com composicao negativa");

    // a apresentacao de cada quadro e a da origem, sem deslocamento
    for (i = 0; i < tp.n; i++) entrada[i] = tp.pts[i];
    qsort(entrada, (size_t)tp.n, sizeof(long long), cmp_ll);
    qsort(saida, (size_t)tot, sizeof(long long), cmp_ll);
    for (i = 0; i < tp.n; i++)
        T_ASSERT(r, entrada[i] == saida[i], "tempo de apresentacao %d: origem %lld ms, saida %lld ms", i, entrada[i], saida[i]);

    naud = le_faixa(pasta, "audio", g_aud);
    T_ASSERT(r, naud >= 1, "a faixa de audio nao foi gravada");
    T_ASSERT(r, g_aud[0].tfdt == 0, "o audio devia comecar em 0, comeca na amostra %lld", g_aud[0].tfdt);

    // texto de codec do manifesto = o que esta no avcC do init
    gw_path_join(nome, sizeof(nome), pasta, "init-source.mp4");
    d = le_arquivo(nome, &n);
    T_ASSERT(r, d != 0, "init-source.mp4 nao foi gerado");
    pos = acha4(d, n, "avcC", 0);
    if (pos >= 0 && pos + 8 <= n) snprintf(esperado, sizeof(esperado), "avc1.%02X%02X%02X", d[pos + 5], d[pos + 6], d[pos + 7]);
    free(d);
    T_ASSERT(r, pos >= 0, "o init nao tem avcC");

    gw_path_join(nome, sizeof(nome), pasta, "master.m3u8");
    txt = (char*)le_arquivo(nome, &n);
    T_ASSERT(r, txt != 0, "master.m3u8 nao foi gerado");
    rc = strstr(txt, esperado) != 0;
    free(txt);
    T_ASSERT(r, rc, "master.m3u8 devia declarar %s (o que esta no avcC)", esperado);

    gw_path_join(nome, sizeof(nome), pasta, "master.mpd");
    txt = (char*)le_arquivo(nome, &n);
    T_ASSERT(r, txt != 0, "master.mpd nao foi gerado");
    rc = strstr(txt, esperado) != 0;
    free(txt);
    T_ASSERT(r, rc, "master.mpd devia declarar %s (o que esta no avcC)", esperado);
}

void teste_video_hls_cortado_recodifica(TestResult* r)
{
    char arq[512], pasta[512], saida[600], motivo[256];
    Tempos tp; Ev ev; MediaProfile p;
    int rc, nseg, naud, i, k, apresentados = ARQ_CORTADO.quadros - ARQ_CORTADO.escondidos;

    t_start(r);
    PRECISA_ENCODER(r, MEDIA_CODEC_H264);
    PRECISA_DECODER(r, MEDIA_CODEC_H264);
    caminho(arq, sizeof(arq), "hls_cortado.mp4");
    caminho(pasta, sizeof(pasta), "hls_cortado_out");

    rc = gera_mp4(arq, &ARQ_CORTADO, &tp);
    T_ASSERT(r, rc == 0 && tp.n == ARQ_CORTADO.quadros, "nao foi possivel gerar a entrada (codigo %d, %d quadros)", rc, tp.n);

    // Pedido de copia (mesma resolucao, mesmo codec) num formato de SEGMENTOS: o trecho escondido
    // nao pode aparecer, entao o gateway recodifica e descarta os quadros anteriores a zero.
    perfil_copia(&p, CONT_HLS_FMP4);
    rc = roda_gw(arq, &p, pasta, &ev);
    T_ASSERT(r, rc == 0 && ev.done && !ev.erro, "gateway_run devolveu %d (done=%d erro=%d)", rc, ev.done, ev.erro);
    T_ASSERT(r, strstr(ev.pista, "recodificado") != 0, "arquivo cortado devia ser recodificado, a pista informou: %s", ev.pista);

    nseg = le_faixa(pasta, "source", g_seg);
    T_ASSERT(r, nseg >= 1, "nenhum segmento de video foi gravado");
    T_ASSERT(r, total_amostras(g_seg, nseg) == apresentados,
             "a saida devia ter os %d quadros apresentados pela origem, tem %d (a origem guarda %d)",
             apresentados, total_amostras(g_seg, nseg), tp.n);
    T_ASSERT(r, g_seg[0].tfdt == 0 && g_seg[0].menor_pts == 0, "a saida devia comecar em 0 (tempo-base %lld, primeiro quadro %lld)", g_seg[0].tfdt, g_seg[0].menor_pts);
    for (k = 0; k < nseg; k++)
        for (i = 0; i < g_seg[k].n; i++)
            T_ASSERT(r, g_seg[k].a[i].pts >= 0, "segmento %d, amostra %d com tempo negativo (%lld ms)", k, i, g_seg[k].a[i].pts);

    // o audio do trecho escondido tambem fica de fora; sobra menos de um quadro no comeco
    naud = le_faixa(pasta, "audio", g_aud);
    T_ASSERT(r, naud >= 1, "a faixa de audio nao foi gravada");
    T_ASSERT(r, g_aud[0].tfdt >= 0 && g_aud[0].tfdt < 1024, "o audio devia comecar a menos de um quadro do zero, comeca na amostra %lld", g_aud[0].tfdt);
    T_ASSERT(r, g_aud[0].a[0].tam == AUD_BYTES, "o primeiro quadro de audio devia ser da origem, nao silencio");

    // No MP4 de arquivo unico a copia continua: ali a lista de edicao esconde o trecho.
    perfil_copia(&p, CONT_MP4_FILE);
    rc = roda_gw(arq, &p, pasta, &ev);
    T_ASSERT(r, rc == 0 && ev.done && !ev.erro, "MP4 unico: gateway_run devolveu %d (done=%d erro=%d)", rc, ev.done, ev.erro);
    T_ASSERT(r, strstr(ev.pista, "bypass") != 0, "MP4 unico de arquivo cortado devia ser copiado, a pista informou: %s", ev.pista);
    gw_path_join(saida, sizeof(saida), pasta, "output.mp4");
    T_ASSERT(r, confere_tempos(saida, &tp, motivo, sizeof(motivo))[0] == 0, "MP4 unico: tempos da copia: %s", motivo);
}

// ---- audio que entra atrasado ---------------------------------------------------------------

// Confere a faixa de audio em 'segs': silencio ate 0,5 s e, dali, os quadros da origem.
static const char* confere_silencio(const Segmento* segs, int nseg, int n_origem, char* motivo, int tam)
{
    int qn = 0, i, k, idx = 0;
    const int esperados = 22050 / 1024;   // 21 quadros; sobram 546 amostras no comeco
    motivo[0] = 0;
    aac_silence_frame(ASC_LC_ESTEREO, (int)sizeof(ASC_LC_ESTEREO), &qn);

    if (nseg < 1) { snprintf(motivo, (size_t)tam, "a faixa de audio nao foi gravada"); return motivo; }
    if (segs[0].tfdt != 22050 % 1024)
    { snprintf(motivo, (size_t)tam, "o audio devia comecar na amostra %d, comeca na %lld", 22050 % 1024, segs[0].tfdt); return motivo; }
    for (k = 0; k < nseg && !motivo[0]; k++)
        for (i = 0; i < segs[k].n; i++, idx++)
        {
            const Amostra* a = &segs[k].a[i];
            if (idx < esperados && a->tam != qn)
            { snprintf(motivo, (size_t)tam, "amostra %d devia ser silencio (%d bytes), tem %d", idx, qn, a->tam); break; }
            if (idx == esperados && (a->tam != AUD_BYTES || a->dts != 22050))
            { snprintf(motivo, (size_t)tam, "o primeiro quadro da origem devia estar na amostra 22050 com %d bytes; esta na %lld com %d", AUD_BYTES, a->dts, a->tam); break; }
            if (idx > esperados && a->tam != AUD_BYTES)
            { snprintf(motivo, (size_t)tam, "amostra %d devia ser da origem (%d bytes), tem %d", idx, AUD_BYTES, a->tam); break; }
        }
    if (!motivo[0] && idx != esperados + n_origem)
        snprintf(motivo, (size_t)tam, "esperava %d quadros de silencio + %d da origem, vieram %d no total", esperados, n_origem, idx);
    return motivo;
}

void teste_video_hls_audio_atrasado_ganha_silencio(TestResult* r)
{
    static const unsigned char ASC_HE[2]  = { 0x2B, 0x10 };   // perfil 5 (HE-AAC): fora
    static const unsigned char ASC_51[2]  = { 0x12, 0x30 };   // 6 canais (5.1)
    static const unsigned char ASC_71[2]  = { 0x12, 0x38 };   // 8 canais (7.1): fora
    static const unsigned char ASC_MONO[2] = { 0x12, 0x08 };
    char arq[512], pasta[512], motivo[256];
    Tempos tp; Ev ev; MediaProfile p;
    int64_t primeiro = -1;
    int rc, nseg, naud, qn = 0;

    t_start(r);

    // o quadro de silencio so existe para o que foi validado
    T_ASSERT(r, aac_silence_frame(ASC_LC_ESTEREO, 2, &qn) != 0 && qn == 9, "AAC-LC estereo devia ter quadro de silencio de 9 bytes (veio %d)", qn);
    T_ASSERT(r, aac_silence_frame(ASC_MONO, 2, &qn) != 0 && qn == 6, "AAC-LC mono devia ter quadro de silencio de 6 bytes (veio %d)", qn);
    T_ASSERT(r, aac_silence_frame(ASC_HE, 2, &qn) == 0, "perfil diferente de LC nao devia ter quadro de silencio");
    T_ASSERT(r, aac_silence_frame(ASC_51, 2, &qn) != 0 && qn == 26, "AAC-LC 5.1 devia ter quadro de silencio de 26 bytes (veio %d)", qn);
    T_ASSERT(r, aac_silence_frame(ASC_71, 2, &qn) == 0, "7.1 nao devia ter quadro de silencio");
    T_ASSERT(r, aac_silence_count(22050, &primeiro) == 21 && primeiro == 546, "0,5 s a 44,1 kHz: esperava 21 quadros a partir da amostra 546");
    T_ASSERT(r, aac_silence_count(1000, &primeiro) == 0 && primeiro == 1000, "menos de um quadro de atraso nao recebe silencio");

    PRECISA_ENCODER(r, MEDIA_CODEC_H264);
    caminho(arq, sizeof(arq), "hls_atrasado.mp4");
    caminho(pasta, sizeof(pasta), "hls_atrasado_out");

    rc = gera_mp4(arq, &ARQ_ATRASADO, &tp);
    T_ASSERT(r, rc == 0, "nao foi possivel gerar a entrada (codigo %d)", rc);

    perfil_copia(&p, CONT_HLS_FMP4);
    rc = roda_gw(arq, &p, pasta, &ev);
    T_ASSERT(r, rc == 0 && ev.done && !ev.erro, "gateway_run devolveu %d (done=%d erro=%d)", rc, ev.done, ev.erro);

    nseg = le_faixa(pasta, "source", g_seg);
    T_ASSERT(r, nseg >= 1 && g_seg[0].tfdt == 0, "o video devia comecar em 0 (%d segmentos, tempo-base %lld)", nseg, nseg > 0 ? g_seg[0].tfdt : -1);

    naud = le_faixa(pasta, "audio", g_aud);
    T_ASSERT(r, confere_silencio(g_aud, naud, tp.n_audio, motivo, sizeof(motivo))[0] == 0, "%s", motivo);
}

static int g_conta; static const char* g_prefixo;
static void conta_prefixo(void* user, const char* nome, int is_dir)
{ (void)user; if (!is_dir && strncmp(nome, g_prefixo, strlen(g_prefixo)) == 0) g_conta++; }

void teste_video_dash_audio_atrasado_e_2997(TestResult* r)
{
    static const Receita rc2997 = { MEDIA_CODEC_H264, 61, 30000, 1001, 0, 0, 1, 500, 30 };
    char arq[512], pasta[512], nome[600], motivo[256];
    Tempos tp; Ev ev; MediaProfile p;
    long long tl[MAXS * 4];
    char* txt; long n;
    int rc, naud, nv, na, k;

    t_start(r);
    PRECISA_ENCODER(r, MEDIA_CODEC_H264);
    PRECISA_DECODER(r, MEDIA_CODEC_H264);
    PRECISA_ENCODER(r, MEDIA_CODEC_VP9);
    caminho(arq, sizeof(arq), "dash_atrasado.mp4");
    caminho(pasta, sizeof(pasta), "dash_atrasado_out");

    rc = gera_mp4(arq, &rc2997, &tp);
    T_ASSERT(r, rc == 0, "nao foi possivel gerar a entrada (codigo %d)", rc);

    memset(&p, 0, sizeof(p));
    p.Container = CONT_DASH_WEBM; p.VideoCodec = MEDIA_CODEC_VP9; p.AudioCodec = MEDIA_CODEC_OPUS;
    p.SegmentMs = 1000; p.Mode = GW_VOD; p.Width = 160; p.Height = 120;
    rc = roda_gw(arq, &p, pasta, &ev);
    T_ASSERT(r, rc == 0 && ev.done && !ev.erro, "gateway_run devolveu %d (done=%d erro=%d)", rc, ev.done, ev.erro);

    // a faixa de audio do DASH (fMP4 separado) tambem ganha o silencio
    naud = le_faixa(pasta, "dash-audio", g_aud);
    T_ASSERT(r, confere_silencio(g_aud, naud, tp.n_audio, motivo, sizeof(motivo))[0] == 0, "%s", motivo);

    // o manifesto lista exatamente o que foi gravado, a 29,97 fps
    gw_path_join(nome, sizeof(nome), pasta, "manifest-vp9.mpd");
    txt = (char*)le_arquivo(nome, &n);
    T_ASSERT(r, txt != 0, "manifest-vp9.mpd nao foi gerado");
    nv = le_timeline(txt, 0, tl, MAXS * 4);
    g_conta = 0; g_prefixo = "chunk-vp9-"; gw_dir_list(pasta, conta_prefixo, 0);
    if (nv != g_conta || nv < 2)
    {
        free(txt);
        T_ASSERT(r, nv == g_conta, "o manifesto lista %d segmentos de video, a pasta tem %d", nv, g_conta);
        T_ASSERT(r, 0, "esperava ao menos 2 segmentos de video, vieram %d", nv);
    }

    for (k = 1; ; k++) if (le_timeline(txt, k, tl, MAXS * 4) < 0) break;   // a do audio e a ultima
    na = le_timeline(txt, k - 1, tl, MAXS * 4);
    free(txt);
    T_ASSERT(r, k >= 2, "o manifesto devia ter a lista de segmentos do audio");
    T_ASSERT(r, na == naud, "o manifesto lista %d segmentos de audio, a pasta tem %d", na, naud);
    for (k = 0; k < na; k++)
        T_ASSERT(r, tl[k] == g_aud[k].tfdt, "audio, segmento %d: o manifesto diz %lld, o arquivo comeca em %lld", k, tl[k], g_aud[k].tfdt);
}

// ---- manifestos a 29,97 fps -----------------------------------------------------------------

void teste_video_manifestos_2997(TestResult* r)
{
    static const Receita rc2997 = { MEDIA_CODEC_H264, 93, 30000, 1001, 1, 0, 1, 0, 31 };
    char arq[512], pasta[512], nome[600];
    Tempos tp; Ev ev; MediaProfile p;
    long long tv[MAXS * 4], ta[MAXS * 4];
    char* txt; const char* q; long n;
    int rc, nseg, naud, nv, na, k, extinf = 0;

    t_start(r);
    PRECISA_ENCODER(r, MEDIA_CODEC_H264);
    caminho(arq, sizeof(arq), "m2997.mp4");
    caminho(pasta, sizeof(pasta), "m2997_out");

    rc = gera_mp4(arq, &rc2997, &tp);
    T_ASSERT(r, rc == 0 && tp.n == rc2997.quadros, "nao foi possivel gerar a entrada (codigo %d, %d quadros)", rc, tp.n);

    perfil_copia(&p, CONT_HLS_FMP4);
    rc = roda_gw(arq, &p, pasta, &ev);
    T_ASSERT(r, rc == 0 && ev.done && !ev.erro, "gateway_run devolveu %d (done=%d erro=%d)", rc, ev.done, ev.erro);

    nseg = le_faixa(pasta, "source", g_seg);
    naud = le_faixa(pasta, "audio", g_aud);
    T_ASSERT(r, nseg >= 2, "esperava ao menos 2 segmentos de video, vieram %d", nseg);
    T_ASSERT(r, naud >= 1, "a faixa de audio nao foi gravada");

    gw_path_join(nome, sizeof(nome), pasta, "master.mpd");
    txt = (char*)le_arquivo(nome, &n);
    T_ASSERT(r, txt != 0, "master.mpd nao foi gerado");
    nv = le_timeline(txt, 0, tv, MAXS * 4);
    na = le_timeline(txt, 1, ta, MAXS * 4);
    free(txt);

    // cada segmento listado existe e comeca no tempo declarado
    T_ASSERT(r, nv == nseg, "o manifesto lista %d segmentos de video, a pasta tem %d", nv, nseg);
    for (k = 0; k < nv; k++)
    {
        T_ASSERT(r, tv[k] == g_seg[k].menor_pts, "video, segmento %d: o manifesto diz %lld ms, o primeiro quadro e em %lld ms", k, tv[k], g_seg[k].menor_pts);
        T_ASSERT(r, modulo(g_seg[k].tfdt - tv[k]) <= 1, "video, segmento %d: tempo-base %lld ms longe do declarado %lld ms", k, g_seg[k].tfdt, tv[k]);
    }
    T_ASSERT(r, na == naud, "o manifesto lista %d segmentos de audio, a pasta tem %d", na, naud);
    for (k = 0; k < na; k++)
        T_ASSERT(r, ta[k] == g_aud[k].tfdt, "audio, segmento %d: o manifesto diz %lld, o arquivo comeca em %lld", k, ta[k], g_aud[k].tfdt);

    gw_path_join(nome, sizeof(nome), pasta, "source.m3u8");
    txt = (char*)le_arquivo(nome, &n);
    T_ASSERT(r, txt != 0, "source.m3u8 nao foi gerado");
    for (q = txt; (q = strstr(q, "#EXTINF:")) != 0; q += 8) extinf++;
    free(txt);
    T_ASSERT(r, extinf == nseg, "a playlist lista %d segmentos, a pasta tem %d", extinf, nseg);
}

// ---- H.265 ------------------------------------------------------------------------------------

// general_level_idc lido do SPS guardado no hvcC, tirando os bytes de prevencao de emulacao.
// -1 se nao achou o SPS.
static int nivel_do_sps(const unsigned char* hvcc, long n)
{
    long p = 23; int arrays, a;
    if (n < 23) return -1;
    arrays = hvcc[22];
    for (a = 0; a < arrays && p + 3 <= n; a++)
    {
        int tipo = hvcc[p] & 0x3F, qtd = (hvcc[p + 1] << 8) | hvcc[p + 2], i;
        p += 3;
        for (i = 0; i < qtd && p + 2 <= n; i++)
        {
            int len = (hvcc[p] << 8) | hvcc[p + 1], k, zeros = 0, rn = 0;
            unsigned char rb[32];
            p += 2;
            if (p + len > n) return -1;
            if (tipo == 33)
            {
                for (k = 0; k < len && rn < 32; k++)
                {
                    if (zeros >= 2 && hvcc[p + k] == 3) { zeros = 0; continue; }
                    zeros = hvcc[p + k] == 0 ? zeros + 1 : 0;
                    rb[rn++] = hvcc[p + k];
                }
                // 2 bytes do cabecalho do NAL + 1 de ids + 11 do perfil; o 12o e o nivel
                return rn > 14 ? rb[14] : -1;
            }
            p += len;
        }
    }
    return -1;
}

void teste_video_hls_h265_init_e_codec(TestResult* r)
{
    static const Receita h265 = { MEDIA_CODEC_H265, 31, 25, 1, 0, 0, 0, 0, 30 };
    char arq[512], pasta[512], nome[600], nivel_l[16], nivel_h[16];
    Tempos tp; Ev ev; MediaProfile p;
    unsigned char* d; char* txt; long n, pos;
    int rc, nseg, nivel_cfg = -1, nivel_sps = -1, ok;
    char entrada[5] = { 0 };

    t_start(r);
    PRECISA_ENCODER(r, MEDIA_CODEC_H265);
    caminho(arq, sizeof(arq), "h265.mp4");
    caminho(pasta, sizeof(pasta), "h265_out");

    rc = gera_mp4(arq, &h265, &tp);
    T_ASSERT(r, rc == 0 && tp.n > 0, "nao foi possivel gerar a entrada H.265 (codigo %d, %d quadros)", rc, tp.n);

    perfil_copia(&p, CONT_HLS_FMP4);
    rc = roda_gw(arq, &p, pasta, &ev);
    T_ASSERT(r, rc == 0 && ev.done && !ev.erro, "gateway_run devolveu %d (done=%d erro=%d)", rc, ev.done, ev.erro);

    nseg = le_faixa(pasta, "source", g_seg);
    T_ASSERT(r, nseg >= 1 && total_amostras(g_seg, nseg) == tp.n, "esperava %d quadros nos segmentos, vieram %d", tp.n, nseg > 0 ? total_amostras(g_seg, nseg) : 0);

    // init: entrada de amostra hvc1 com hvcC -- ja saiu "avc1" com hvcC dentro
    gw_path_join(nome, sizeof(nome), pasta, "init-source.mp4");
    d = le_arquivo(nome, &n);
    T_ASSERT(r, d != 0, "init-source.mp4 nao foi gerado");
    pos = acha4(d, n, "stsd", 0);                          // versao(4) + contagem(4) + tamanho(4) + TIPO(4)
    if (pos >= 0 && pos + 20 <= n) memcpy(entrada, d + pos + 16, 4);
    pos = acha4(d, n, "hvcC", 0);
    if (pos >= 0 && pos + 4 + 23 <= n)
    {
        nivel_cfg = d[pos + 4 + 12];                       // general_level_idc do registro
        nivel_sps = nivel_do_sps(d + pos + 4, n - pos - 4);
    }
    free(d);
    T_ASSERT(r, strcmp(entrada, "hvc1") == 0, "a entrada de amostra do init de H.265 devia ser hvc1, e '%s'", entrada);
    T_ASSERT(r, pos >= 0, "o init nao tem hvcC");
    T_ASSERT(r, nivel_cfg > 0, "o hvcC saiu com nivel %d", nivel_cfg);
    T_ASSERT(r, nivel_cfg == nivel_sps, "nivel do hvcC (%d) diferente do que esta no SPS (%d)", nivel_cfg, nivel_sps);

    // texto de codec: hvc1.<perfil>.<compat>.<L|H><nivel>...
    snprintf(nivel_l, sizeof(nivel_l), ".L%d", nivel_cfg);
    snprintf(nivel_h, sizeof(nivel_h), ".H%d", nivel_cfg);
    gw_path_join(nome, sizeof(nome), pasta, "master.m3u8");
    txt = (char*)le_arquivo(nome, &n);
    T_ASSERT(r, txt != 0, "master.m3u8 nao foi gerado");
    ok = strstr(txt, "CODECS=\"hvc1.") != 0 && (strstr(txt, nivel_l) != 0 || strstr(txt, nivel_h) != 0) && strstr(txt, "avc1") == 0;
    free(txt);
    T_ASSERT(r, ok, "master.m3u8 devia declarar hvc1 com o nivel %d e nada de avc1", nivel_cfg);

    gw_path_join(nome, sizeof(nome), pasta, "master.mpd");
    txt = (char*)le_arquivo(nome, &n);
    T_ASSERT(r, txt != 0, "master.mpd nao foi gerado");
    ok = strstr(txt, "codecs=\"hvc1.") != 0 && strstr(txt, "avc1") == 0;
    free(txt);
    T_ASSERT(r, ok, "master.mpd devia declarar hvc1 e nada de avc1");
}

// ---- texto de codec do H.264 por resolucao ----------------------------------------------------

void teste_video_codec_h264_por_resolucao(TestResult* r)
{
    static const Receita simples = { MEDIA_CODEC_H264, 31, 25, 1, 0, 0, 0, 0, 30 };
    static const char* NOMES[2] = { "grande", "pequena" };
    char arq[512], pasta[512], nome[600], esperado[2][32], linha[160];
    Tempos tp; Ev ev; MediaProfile p; PipeTrack pistas[2];
    unsigned char* d; char *m3u8, *mpd; long n, pos;
    int rc, i, ok_m3u8, ok_mpd;

    t_start(r);
    esperado[0][0] = 0; esperado[1][0] = 0;
    PRECISA_ENCODER(r, MEDIA_CODEC_H264);
    PRECISA_DECODER(r, MEDIA_CODEC_H264);
    caminho(arq, sizeof(arq), "escada.mp4");
    caminho(pasta, sizeof(pasta), "escada_out");

    rc = gera_mp4(arq, &simples, &tp);
    T_ASSERT(r, rc == 0, "nao foi possivel gerar a entrada (codigo %d)", rc);

    memset(pistas, 0, sizeof(pistas));
    pistas[0].Width = 320; pistas[0].Height = 240; pistas[0].BitrateBps = 400000; pistas[0].Name = NOMES[0];
    pistas[1].Width = 160; pistas[1].Height = 120; pistas[1].BitrateBps = 150000; pistas[1].Name = NOMES[1];
    memset(&p, 0, sizeof(p));
    p.Container = CONT_HLS_FMP4; p.VideoCodec = MEDIA_CODEC_H264; p.AudioCodec = MEDIA_CODEC_AAC;
    p.SegmentMs = 1000; p.Mode = GW_VOD; p.Renditions = pistas; p.RenditionCount = 2;
    rc = roda_gw(arq, &p, pasta, &ev);
    T_ASSERT(r, rc == 0 && ev.done && !ev.erro, "gateway_run devolveu %d (done=%d erro=%d)", rc, ev.done, ev.erro);

    for (i = 0; i < 2; i++)
    {
        snprintf(linha, sizeof(linha), "init-%s.mp4", NOMES[i]);
        gw_path_join(nome, sizeof(nome), pasta, linha);
        d = le_arquivo(nome, &n);
        T_ASSERT(r, d != 0, "%s nao foi gerado", linha);
        pos = acha4(d, n, "avcC", 0);
        if (pos >= 0 && pos + 8 <= n) snprintf(esperado[i], sizeof(esperado[i]), "avc1.%02X%02X%02X", d[pos + 5], d[pos + 6], d[pos + 7]);
        free(d);
        T_ASSERT(r, pos >= 0, "%s nao tem avcC", linha);
    }

    gw_path_join(nome, sizeof(nome), pasta, "master.m3u8");
    m3u8 = (char*)le_arquivo(nome, &n);
    gw_path_join(nome, sizeof(nome), pasta, "master.mpd");
    mpd = (char*)le_arquivo(nome, &n);
    if (!m3u8 || !mpd) { free(m3u8); free(mpd); T_ASSERT(r, 0, "master.m3u8 ou master.mpd nao foi gerado"); }

    // cada resolucao declara o perfil e o nivel do PROPRIO avcC (antes saia um texto fixo)
    for (i = 0; i < 2; i++)
    {
        const char* q;
        snprintf(linha, sizeof(linha), "CODECS=\"%s\"\n%s.m3u8", esperado[i], NOMES[i]);
        ok_m3u8 = strstr(m3u8, linha) != 0;
        snprintf(linha, sizeof(linha), "<Representation id=\"%s\" codecs=\"%s\"", NOMES[i], esperado[i]);
        q = strstr(mpd, linha);
        ok_mpd = q != 0;
        if (!ok_m3u8 || !ok_mpd)
        {
            free(m3u8); free(mpd);
            T_ASSERT(r, 0, "a pista %s devia declarar %s (m3u8=%d mpd=%d)", NOMES[i], esperado[i], ok_m3u8, ok_mpd);
        }
    }
    free(m3u8); free(mpd);
}

// ---- formatos guardados na sessao -------------------------------------------------------------

void teste_video_sessao_guarda_formatos(TestResult* r)
{
    char raiz[512], id[80], dir[600], arq[700];
    char* txt; long n;
    int tem_hls, tem_dash, tem_chave;

    t_start(r);
    caminho(raiz, sizeof(raiz), "sessoes");
    gw_rmtree(raiz);
    T_ASSERT(r, frag_session_init(raiz) == 1, "frag_session_init falhou em %s", raiz);
    T_ASSERT(r, frag_session_create("teste", id, sizeof(id)) == 1, "nao foi possivel criar a sessao");
    T_ASSERT(r, frag_session_dir(id, dir, sizeof(dir)) == 1, "a sessao criada nao tem pasta");
    gw_path_join(arq, sizeof(arq), dir, "session.json");

    // o mesmo video pode ter varios formatos prontos na mesma pasta
    T_ASSERT(r, frag_session_set_format(id, "hls", "/hls/x/master.m3u8") == 1, "set_format(hls) falhou");
    T_ASSERT(r, frag_session_set_format(id, "dash", "/hls/x/master.mpd") == 1, "set_format(dash) falhou");
    T_ASSERT(r, frag_session_set_format(id, "dash-vp9", "/hls/x/manifest-vp9.mpd") == 1, "set_format(dash-vp9) falhou");

    txt = (char*)le_arquivo(arq, &n);
    T_ASSERT(r, txt != 0, "session.json nao foi gravado");
    tem_chave = strstr(txt, "\"formats\"") != 0;
    tem_hls = strstr(txt, "master.m3u8") != 0;
    tem_dash = strstr(txt, "master.mpd") != 0 && strstr(txt, "manifest-vp9.mpd") != 0;
    free(txt);
    T_ASSERT(r, tem_chave && tem_hls && tem_dash, "session.json devia guardar os tres formatos (formats=%d hls=%d dash=%d)", tem_chave, tem_hls, tem_dash);

    // gravar um formato de novo troca a url; url vazia remove so aquele
    T_ASSERT(r, frag_session_set_format(id, "hls", "/hls/x/outro.m3u8") == 1, "regravar o formato hls falhou");
    T_ASSERT(r, frag_session_set_format(id, "dash", "") == 1, "remover o formato dash falhou");
    txt = (char*)le_arquivo(arq, &n);
    T_ASSERT(r, txt != 0, "session.json sumiu");
    tem_hls = strstr(txt, "outro.m3u8") != 0 && strstr(txt, "master.m3u8") == 0;
    tem_dash = strstr(txt, "master.mpd") != 0;
    tem_chave = strstr(txt, "manifest-vp9.mpd") != 0;
    free(txt);
    T_ASSERT(r, tem_hls, "o formato hls devia ter a url nova, e so ela");
    T_ASSERT(r, !tem_dash, "o formato dash devia ter sido removido");
    T_ASSERT(r, tem_chave, "remover um formato nao pode levar os outros");

    // modo de arquivo cortado: campo proprio da sessao, que sobrevive a regravar a saida
    {
        char modo[16] = "x";
        T_ASSERT(r, frag_session_get_cut_mode(id, modo, sizeof(modo)) == 1 && modo[0] == 0, "sessao nova nao devia ter modo de arquivo cortado (tem '%s')", modo);
        T_ASSERT(r, frag_session_set_cut_mode(id, "smart") == 1, "set_cut_mode(smart) falhou");
        T_ASSERT(r, frag_session_set_cut_mode(id, "qualquer") == 0, "modo desconhecido devia ser recusado");
        T_ASSERT(r, frag_session_set_cut_mode(id, "showall") == 1 && frag_session_set_cut_mode(id, "smart") == 1, "os modos conhecidos deviam ser aceitos");
        T_ASSERT(r, frag_session_set_output(id, "hls", "h264", 320, 240, 25.0, 0) == 1, "set_output falhou");
        T_ASSERT(r, frag_session_get_cut_mode(id, modo, sizeof(modo)) == 1 && strcmp(modo, "smart") == 0, "o modo devia continuar 'smart' depois de regravar a saida (esta '%s')", modo);
        T_ASSERT(r, frag_session_set_cut_mode(id, "") == 1, "limpar o modo falhou");
        T_ASSERT(r, frag_session_get_cut_mode(id, modo, sizeof(modo)) == 1 && modo[0] == 0, "o modo devia ter sido limpo (esta '%s')", modo);
    }

    T_ASSERT(r, frag_session_delete(id) == 1, "nao foi possivel apagar a sessao de teste");
}

// ---- ultimo segmento ----------------------------------------------------------------------

void teste_video_hls_ultimo_segmento_nao_fica_minusculo(TestResult* r)
{
    // o video TERMINA num quadro-chave: sem a espera do sink, esse quadro virava um segmento so dele
    static const Receita fim_em_chave = { MEDIA_CODEC_H264, 93, 25, 1, 0, 0, 1, 0, 30, 1 };
    char arq[512], pasta[512], nome[600];
    Tempos tp; Ev ev; MediaProfile p;
    long long tv[MAXS * 4];
    char* txt; const char* q; long n;
    int rc, nseg, nv, extinf = 0;

    t_start(r);
    PRECISA_ENCODER(r, MEDIA_CODEC_H264);
    caminho(arq, sizeof(arq), "fim_chave.mp4");
    caminho(pasta, sizeof(pasta), "fim_chave_out");

    rc = gera_mp4(arq, &fim_em_chave, &tp);
    T_ASSERT(r, rc == 0, "nao foi possivel gerar a entrada (codigo %d)", rc);
    T_ASSERT(r, tp.n >= 26 && tp.chave[tp.n - 1], "a entrada devia terminar num quadro-chave a mais de 1 s do inicio (%d quadros)", tp.n);

    perfil_copia(&p, CONT_HLS_FMP4);
    rc = roda_gw(arq, &p, pasta, &ev);
    T_ASSERT(r, rc == 0 && ev.done && !ev.erro, "gateway_run devolveu %d (done=%d erro=%d)", rc, ev.done, ev.erro);

    nseg = le_faixa(pasta, "source", g_seg);
    T_ASSERT(r, nseg >= 1, "nenhum segmento de video foi gravado");
    T_ASSERT(r, total_amostras(g_seg, nseg) == tp.n, "esperava %d quadros nos segmentos, vieram %d", tp.n, total_amostras(g_seg, nseg));
    // alvo de 1 s: nenhum segmento com menos de 250 ms (7 quadros a 25 fps)
    T_ASSERT(r, g_seg[nseg - 1].n >= 7, "o ultimo segmento ficou com %d quadro(s); devia ter sido juntado ao anterior", g_seg[nseg - 1].n);

    // playlist e manifesto listam exatamente os arquivos gravados
    gw_path_join(nome, sizeof(nome), pasta, "source.m3u8");
    txt = (char*)le_arquivo(nome, &n);
    T_ASSERT(r, txt != 0, "source.m3u8 nao foi gerado");
    for (q = txt; (q = strstr(q, "#EXTINF:")) != 0; q += 8) extinf++;
    rc = strstr(txt, "#EXT-X-ENDLIST") != 0;
    free(txt);
    T_ASSERT(r, extinf == nseg && rc, "a playlist lista %d segmentos (fim=%d), a pasta tem %d", extinf, rc, nseg);

    gw_path_join(nome, sizeof(nome), pasta, "master.mpd");
    txt = (char*)le_arquivo(nome, &n);
    T_ASSERT(r, txt != 0, "master.mpd nao foi gerado");
    nv = le_timeline(txt, 0, tv, MAXS * 4);
    free(txt);
    T_ASSERT(r, nv == nseg, "o manifesto lista %d segmentos de video, a pasta tem %d", nv, nseg);
}

// ---- arquivo cortado: os outros dois modos --------------------------------------------------

static int le_playlist(const char* path, double* maior, int* target, int* segmentos);

void teste_video_hls_cortado_copia_manda_comecar_depois(TestResult* r)
{
    static const char* ARQS[3] = { "master.m3u8", "source.m3u8", "audio.m3u8" };
    char arq[512], pasta[512], nome[600];
    Tempos tp; Ev ev; MediaProfile p;
    char* txt; long n;
    int rc, nseg, naud, i, k, idx = 0, real = -1;

    t_start(r);
    PRECISA_ENCODER(r, MEDIA_CODEC_H264);
    caminho(arq, sizeof(arq), "cortado_copia.mp4");
    caminho(pasta, sizeof(pasta), "cortado_copia_out");

    rc = gera_mp4(arq, &ARQ_CORTADO, &tp);
    T_ASSERT(r, rc == 0 && tp.n == ARQ_CORTADO.quadros, "nao foi possivel gerar a entrada (codigo %d, %d quadros)", rc, tp.n);

    perfil_copia(&p, CONT_HLS_FMP4);
    p.CutFileMode = GW_CUT_COPY;
    rc = roda_gw(arq, &p, pasta, &ev);
    T_ASSERT(r, rc == 0 && ev.done && !ev.erro, "gateway_run devolveu %d (done=%d erro=%d)", rc, ev.done, ev.erro);
    T_ASSERT(r, strstr(ev.pista, "bypass") != 0 && strstr(ev.pista, "comprimido") != 0,
             "esperava copia com aviso do trecho escondido, a pista informou: %s", ev.pista);

    // tudo foi copiado, o trecho escondido inclusive, e a linha do tempo comeca em zero
    nseg = le_faixa(pasta, "source", g_seg);
    T_ASSERT(r, nseg >= 1 && total_amostras(g_seg, nseg) == tp.n, "esperava os %d quadros da origem, vieram %d", tp.n, nseg > 0 ? total_amostras(g_seg, nseg) : 0);
    T_ASSERT(r, g_seg[0].tfdt == 0 && g_seg[0].menor_pts == 0, "a copia devia comecar em 0 (tempo-base %lld, primeiro quadro %lld)", g_seg[0].tfdt, g_seg[0].menor_pts);

    // os 10 quadros escondidos (400 ms na origem) cabem em 20 ms: 2 ms cada. O primeiro quadro
    // que a origem apresenta vem logo depois, e dali em diante a cadencia e a original.
    {
        long long pts[MAXQ]; int tot = 0, escondidos = 0;
        for (k = 0; k < nseg; k++) for (i = 0; i < g_seg[k].n && tot < MAXQ; i++) pts[tot++] = g_seg[k].a[i].pts;
        qsort(pts, (size_t)tot, sizeof(long long), cmp_ll);
        for (i = 0; i < tot; i++) if (pts[i] < 20) escondidos++;
        T_ASSERT(r, escondidos == 10, "esperava os 10 quadros escondidos nos primeiros 20 ms, ha %d", escondidos);
        T_ASSERT(r, pts[10] == 20 && pts[11] == 60, "o primeiro quadro apresentado devia estar em 20 ms e o seguinte em 60 ms (estao em %lld e %lld)", pts[10], pts[11]);
        T_ASSERT(r, pts[tot - 1] == 20 + 49 * 40, "o ultimo quadro devia estar em %d ms, esta em %lld", 20 + 49 * 40, pts[tot - 1]);
    }

    // a duracao cresce so o trecho comprimido: 50 quadros apresentados (2,000 s) + 20 ms, e nao
    // os 400 ms que a origem esconde
    {
        double maior; int target, nsegs; long nn; char* t2; const char* q2; double soma = 0;
        gw_path_join(nome, sizeof(nome), pasta, "source.m3u8");
        T_ASSERT(r, le_playlist(nome, &maior, &target, &nsegs) == 0, "source.m3u8 nao foi gerado");
        t2 = (char*)le_arquivo(nome, &nn);
        for (q2 = t2; t2 && (q2 = strstr(q2, "#EXTINF:")) != 0; q2 += 8) soma += atof(q2 + 8);
        free(t2);
        T_ASSERT(r, soma > 2.015 && soma < 2.065, "a copia devia durar 2,020 s (2,000 + 0,020 do trecho comprimido), dura %.3f s", soma);
    }

    // os tres manifestos mandam comecar depois do trecho comprimido
    for (i = 0; i < 3; i++)
    {
        gw_path_join(nome, sizeof(nome), pasta, ARQS[i]);
        txt = (char*)le_arquivo(nome, &n);
        T_ASSERT(r, txt != 0, "%s nao foi gerado", ARQS[i]);
        rc = strstr(txt, "#EXT-X-START:TIME-OFFSET=0.020,PRECISE=YES") != 0;
        free(txt);
        T_ASSERT(r, rc, "%s devia mandar comecar em 0,020 s", ARQS[i]);
    }

    // o audio do trecho escondido sai fora; o primeiro quadro que fica (9 ms depois do zero da
    // origem) anda os mesmos 20 ms do video
    naud = le_faixa(pasta, "audio", g_aud);
    T_ASSERT(r, naud >= 1, "a faixa de audio nao foi gravada");
    for (k = 0; k < naud && real < 0; k++)
        for (i = 0; i < g_aud[k].n; i++, idx++)
            if (g_aud[k].a[i].tam == AUD_BYTES) { real = (int)g_aud[k].a[i].dts; break; }
    T_ASSERT(r, real == 1278, "o primeiro quadro de audio da origem devia estar na amostra 1278 (29 ms), esta na %d", real);
}

void teste_video_hls_cortado_so_primeiro_grupo(TestResult* r)
{
    // arquivo cortado COM quadros B: a emenda entre o trecho recodificado e a copia e onde os
    // tempos de decodificacao podem se atropelar
    static const Receita cortado_b = { MEDIA_CODEC_H264, 60, 25, 1, 2, 10, 1, -200, 30 };
    char arq[512], pasta[512], nome[600], entrada[5] = { 0 };
    Tempos tp; Ev ev; MediaProfile p;
    static Amostra todas[MAXQ];
    long long esperado[MAXQ], saida[MAXQ];
    unsigned char* d; char* txt; long n, pos;
    int rc, nseg, i, k, tot = 0, k1 = -1, apresentados = 0, copiados, ini_copia;

    t_start(r);
    PRECISA_ENCODER(r, MEDIA_CODEC_H264);
    PRECISA_DECODER(r, MEDIA_CODEC_H264);
    caminho(arq, sizeof(arq), "cortado_grupo.mp4");
    caminho(pasta, sizeof(pasta), "cortado_grupo_out");

    rc = gera_mp4(arq, &cortado_b, &tp);
    T_ASSERT(r, rc == 0 && tp.n == cortado_b.quadros, "nao foi possivel gerar a entrada (codigo %d, %d quadros)", rc, tp.n);
    for (i = 0; i < tp.n; i++)
    {
        if (tp.pts[i] >= 0) esperado[apresentados++] = tp.pts[i];
        if (k1 < 0 && tp.chave[i] && tp.pts[i] > 0) k1 = i;   // onde a copia comeca
    }

    perfil_copia(&p, CONT_HLS_FMP4);
    p.CutFileMode = GW_CUT_SMART;
    rc = roda_gw(arq, &p, pasta, &ev);
    T_ASSERT(r, rc == 0 && ev.done && !ev.erro, "gateway_run devolveu %d (done=%d erro=%d)", rc, ev.done, ev.erro);
    T_ASSERT(r, strstr(ev.pista, "primeiro grupo") != 0, "esperava recodificar so o primeiro grupo, a pista informou: %s", ev.pista);

    nseg = le_faixa(pasta, "source", g_seg);
    T_ASSERT(r, nseg >= 1, "nenhum segmento de video foi gravado");
    for (k = 0; k < nseg; k++) for (i = 0; i < g_seg[k].n && tot < MAXQ; i++) todas[tot++] = g_seg[k].a[i];

    // os quadros apresentados pela origem, nem um a mais, comecando em zero
    T_ASSERT(r, tot == apresentados, "a saida devia ter os %d quadros apresentados pela origem, tem %d", apresentados, tot);
    T_ASSERT(r, g_seg[0].tfdt == 0 && todas[0].pts == 0, "a saida devia comecar em 0 (tempo-base %lld, primeiro quadro %lld)", g_seg[0].tfdt, todas[0].pts);
    for (i = 0; i < tot; i++) saida[i] = todas[i].pts;
    qsort(esperado, (size_t)apresentados, sizeof(long long), cmp_ll);
    qsort(saida, (size_t)tot, sizeof(long long), cmp_ll);
    for (i = 0; i < tot; i++)
        T_ASSERT(r, esperado[i] == saida[i], "tempo de apresentacao %d: origem %lld ms, saida %lld ms", i, esperado[i], saida[i]);
    // a decodificacao nunca volta no tempo, nem na emenda
    for (i = 1; i < tot; i++)
        T_ASSERT(r, todas[i].dts > todas[i - 1].dts, "amostra %d: decodificacao em %lld ms depois de %lld ms", i, todas[i].dts, todas[i - 1].dts);

    // do primeiro quadro-chave apresentado em diante e COPIA: mesmas amostras da origem
    if (k1 > 0)
    {
        copiados = tp.n - k1; ini_copia = tot - copiados;
        T_ASSERT(r, ini_copia > 0, "nada foi recodificado (copia a partir do quadro %d de %d)", k1, tp.n);
        T_ASSERT(r, todas[ini_copia].chave && todas[ini_copia].tam > tp.tam[k1],
                 "o primeiro quadro copiado devia ser quadro-chave com os parametros da origem junto (%d bytes; a amostra da origem tem %d)",
                 todas[ini_copia].tam, tp.tam[k1]);
        for (i = 1; i < copiados; i++)
            T_ASSERT(r, todas[ini_copia + i].tam == tp.tam[k1 + i],
                     "quadro copiado %d: %d bytes na saida, %d na origem", k1 + i, todas[ini_copia + i].tam, tp.tam[k1 + i]);
    }

    // parametros dentro das amostras: entrada 'avc3' no init e no texto de codec
    gw_path_join(nome, sizeof(nome), pasta, "init-source.mp4");
    d = le_arquivo(nome, &n);
    T_ASSERT(r, d != 0, "init-source.mp4 nao foi gerado");
    pos = acha4(d, n, "stsd", 0);
    if (pos >= 0 && pos + 20 <= n) memcpy(entrada, d + pos + 16, 4);
    free(d);
    T_ASSERT(r, strcmp(entrada, "avc3") == 0, "a entrada de amostra devia ser avc3, e '%s'", entrada);

    gw_path_join(nome, sizeof(nome), pasta, "master.m3u8");
    txt = (char*)le_arquivo(nome, &n);
    T_ASSERT(r, txt != 0, "master.m3u8 nao foi gerado");
    rc = strstr(txt, "CODECS=\"avc3.") != 0 && strstr(txt, "EXT-X-START") == 0;
    free(txt);
    T_ASSERT(r, rc, "master.m3u8 devia declarar avc3 e nao mandar pular o comeco");
}

// ---- duracao declarada na playlist ----------------------------------------------------------

// Maior EXTINF da playlist e o TARGETDURATION declarado. 0 em sucesso.
static int le_playlist(const char* path, double* maior, int* target, int* segmentos)
{
    long n; char* txt = (char*)le_arquivo(path, &n); const char* q;
    *maior = 0; *target = -1; *segmentos = 0;
    if (!txt) return -1;
    q = strstr(txt, "#EXT-X-TARGETDURATION:");
    if (q) *target = atoi(q + 22);
    for (q = txt; (q = strstr(q, "#EXTINF:")) != 0; q += 8)
    { double d = atof(q + 8); if (d > *maior) *maior = d; (*segmentos)++; }
    free(txt);
    return 0;
}

void teste_video_hls_target_cobre_segmento_longo(TestResult* r)
{
    // sem pedir quadro-chave: os grupos ficam do tamanho que o encoder quiser, bem maiores que
    // o alvo de 0,5 s -- e o caso da copia de uma origem com quadros-chave espacados
    static const Receita grupos_longos = { MEDIA_CODEC_H264, 93, 25, 1, 0, 0, 1, 0, 0 };
    static const char* ARQS[2] = { "source.m3u8", "audio.m3u8" };
    char arq[512], pasta[512], nome[600];
    Tempos tp; Ev ev; MediaProfile p;
    double maior; int rc, i, target, nseg;

    t_start(r);
    PRECISA_ENCODER(r, MEDIA_CODEC_H264);
    caminho(arq, sizeof(arq), "grupos_longos.mp4");
    caminho(pasta, sizeof(pasta), "grupos_longos_out");

    rc = gera_mp4(arq, &grupos_longos, &tp);
    T_ASSERT(r, rc == 0, "nao foi possivel gerar a entrada (codigo %d)", rc);

    perfil_copia(&p, CONT_HLS_FMP4);
    p.SegmentMs = 500;   // o cabecalho declara 1 s
    rc = roda_gw(arq, &p, pasta, &ev);
    T_ASSERT(r, rc == 0 && ev.done && !ev.erro, "gateway_run devolveu %d (done=%d erro=%d)", rc, ev.done, ev.erro);

    for (i = 0; i < 2; i++)
    {
        gw_path_join(nome, sizeof(nome), pasta, ARQS[i]);
        T_ASSERT(r, le_playlist(nome, &maior, &target, &nseg) == 0 && nseg > 0, "%s nao foi gerado ou esta vazio", ARQS[i]);
        // regra do HLS: nenhum EXTINF, arredondado, passa do TARGETDURATION
        T_ASSERT(r, target >= (int)(maior + 0.5), "%s declara TARGETDURATION %d e tem segmento de %.3f s", ARQS[i], target, maior);
        if (i == 0) T_ASSERT(r, maior > 1.5, "a entrada devia render segmento de video maior que o declarado (o maior tem %.3f s)", maior);
    }
}

// ---- DASH VP9: ultimo segmento ----------------------------------------------------------------

void teste_video_dash_vp9_ultimo_segmento_nao_fica_minusculo(TestResult* r)
{
    // 51 quadros a 25 fps = 2,04 s com alvo de 1 s: sem a espera saiam 2 segmentos e um
    // terceiro de UM quadro
    static const Receita curto = { MEDIA_CODEC_H264, 51, 25, 1, 0, 0, 0, 0, 25 };
    char arq[512], pasta[512], nome[600];
    Tempos tp; Ev ev; MediaProfile p;
    long long tl[MAXS * 4];
    char* txt; long n;
    int rc, nv;

    t_start(r);
    PRECISA_ENCODER(r, MEDIA_CODEC_H264);
    PRECISA_DECODER(r, MEDIA_CODEC_H264);
    PRECISA_ENCODER(r, MEDIA_CODEC_VP9);
    caminho(arq, sizeof(arq), "vp9_curto.mp4");
    caminho(pasta, sizeof(pasta), "vp9_curto_out");

    rc = gera_mp4(arq, &curto, &tp);
    T_ASSERT(r, rc == 0 && tp.n == curto.quadros, "nao foi possivel gerar a entrada (codigo %d, %d quadros)", rc, tp.n);

    memset(&p, 0, sizeof(p));
    p.Container = CONT_DASH_WEBM; p.VideoCodec = MEDIA_CODEC_VP9; p.AudioCodec = MEDIA_CODEC_OPUS;
    p.SegmentMs = 1000; p.Mode = GW_VOD; p.Width = 160; p.Height = 120;
    rc = roda_gw(arq, &p, pasta, &ev);
    T_ASSERT(r, rc == 0 && ev.done && !ev.erro, "gateway_run devolveu %d (done=%d erro=%d)", rc, ev.done, ev.erro);

    gw_path_join(nome, sizeof(nome), pasta, "manifest-vp9.mpd");
    txt = (char*)le_arquivo(nome, &n);
    T_ASSERT(r, txt != 0, "manifest-vp9.mpd nao foi gerado");
    nv = le_timeline(txt, 0, tl, MAXS * 4);
    free(txt);
    g_conta = 0; g_prefixo = "chunk-vp9-"; gw_dir_list(pasta, conta_prefixo, 0);
    T_ASSERT(r, nv == g_conta, "o manifesto lista %d segmentos de video, a pasta tem %d", nv, g_conta);
    T_ASSERT(r, nv == 2, "2,04 s com alvo de 1 s deviam dar 2 segmentos (o quadro que sobra vai junto com o segundo), vieram %d", nv);
}

// ---- H.265: so o primeiro grupo ---------------------------------------------------------------

void teste_video_hls_cortado_so_primeiro_grupo_h265(TestResult* r)
{
    static const Receita cortado = { MEDIA_CODEC_H265, 60, 25, 1, 0, 10, 0, 0, 30 };
    char arq[512], pasta[512], nome[600], entrada[5] = { 0 };
    Tempos tp; Ev ev; MediaProfile p;
    static Amostra todas[MAXQ];
    unsigned char* d; char* txt; long n, pos;
    int rc, nseg, i, k, tot = 0, apresentados = 0;

    t_start(r);
    PRECISA_ENCODER(r, MEDIA_CODEC_H265);
    PRECISA_DECODER(r, MEDIA_CODEC_H265);
    caminho(arq, sizeof(arq), "cortado_h265.mp4");
    caminho(pasta, sizeof(pasta), "cortado_h265_out");

    rc = gera_mp4(arq, &cortado, &tp);
    T_ASSERT(r, rc == 0 && tp.n == cortado.quadros, "nao foi possivel gerar a entrada H.265 (codigo %d, %d quadros)", rc, tp.n);
    for (i = 0; i < tp.n; i++) if (tp.pts[i] >= 0) apresentados++;

    perfil_copia(&p, CONT_HLS_FMP4);
    p.CutFileMode = GW_CUT_SMART;
    rc = roda_gw(arq, &p, pasta, &ev);
    T_ASSERT(r, rc == 0 && ev.done && !ev.erro, "gateway_run devolveu %d (done=%d erro=%d)", rc, ev.done, ev.erro);
    T_ASSERT(r, strstr(ev.pista, "primeiro grupo") != 0, "esperava recodificar so o primeiro grupo, a pista informou: %s", ev.pista);

    nseg = le_faixa(pasta, "source", g_seg);
    T_ASSERT(r, nseg >= 1, "nenhum segmento de video foi gravado");
    for (k = 0; k < nseg; k++) for (i = 0; i < g_seg[k].n && tot < MAXQ; i++) todas[tot++] = g_seg[k].a[i];
    T_ASSERT(r, tot == apresentados, "a saida devia ter os %d quadros apresentados pela origem, tem %d", apresentados, tot);
    T_ASSERT(r, g_seg[0].tfdt == 0 && g_seg[0].menor_pts == 0, "a saida devia comecar em 0 (tempo-base %lld, primeiro quadro %lld)", g_seg[0].tfdt, g_seg[0].menor_pts);
    for (i = 1; i < tot; i++)
        T_ASSERT(r, todas[i].dts > todas[i - 1].dts, "amostra %d: decodificacao em %lld ms depois de %lld ms", i, todas[i].dts, todas[i - 1].dts);

    // parametros dentro das amostras: no H.265 a entrada e 'hev1'
    gw_path_join(nome, sizeof(nome), pasta, "init-source.mp4");
    d = le_arquivo(nome, &n);
    T_ASSERT(r, d != 0, "init-source.mp4 nao foi gerado");
    pos = acha4(d, n, "stsd", 0);
    if (pos >= 0 && pos + 20 <= n) memcpy(entrada, d + pos + 16, 4);
    free(d);
    T_ASSERT(r, strcmp(entrada, "hev1") == 0, "a entrada de amostra devia ser hev1, e '%s'", entrada);

    gw_path_join(nome, sizeof(nome), pasta, "master.m3u8");
    txt = (char*)le_arquivo(nome, &n);
    T_ASSERT(r, txt != 0, "master.m3u8 nao foi gerado");
    rc = strstr(txt, "CODECS=\"hev1.") != 0;
    free(txt);
    T_ASSERT(r, rc, "master.m3u8 devia declarar hev1");
}

// ---- H.265 com quadros B: decoder e emenda ----------------------------------------------------

#include "dados_hevc_b.h"

static int luma_media(const GwImage* im)
{
    long long soma = 0; int x, y;
    for (y = 0; y < im->Height; y++)
        for (x = 0; x < im->Width; x++) soma += im->Planes[0][y * im->Strides[0] + x];
    return (int)(soma / ((long long)im->Width * im->Height));
}

// Decodifica 'n' unidades (Annex-B; inicio[i]..inicio[i+1]) e devolve a luma media de cada
// quadro, na ordem em que o decoder os entrega. Devolve quantos quadros sairam; < 0 em erro.
static int decodifica_lumas(const unsigned char* dados, const int* inicio, int n, int* lumas, int max)
{
    GwVideoDec* dec = gw_vdec_open_accel(MEDIA_CODEC_H265, MEDIA_ACCEL_SOFTWARE);
    GwImage im; int i, q = 0;
    if (!dec) return -1;
    for (i = 0; i < n; i++)
    {
        gw_vdec_send(dec, dados + inicio[i], inicio[i + 1] - inicio[i]);
        while (gw_vdec_next(dec, &im) == 1) { if (q < max) lumas[q] = luma_media(&im); q++; }
    }
    gw_vdec_send(dec, 0, 0);
    while (gw_vdec_next(dec, &im) == 1) { if (q < max) lumas[q] = luma_media(&im); q++; }
    gw_vdec_close(&dec);
    return q;
}

void teste_video_decode_h265_com_quadros_b(TestResult* r)
{
    int lumas[HEVC_B_QUADROS + 8], n, k;

    t_start(r);
    PRECISA_DECODER(r, MEDIA_CODEC_H265);

    n = decodifica_lumas(HEVC_B_DADOS, HEVC_B_INICIO, HEVC_B_QUADROS, lumas, HEVC_B_QUADROS + 8);
    // nenhum quadro a menos: o decoder perdia o ultimo apresentado de cada grupo (3 grupos aqui)
    T_ASSERT(r, n == HEVC_B_QUADROS, "entraram %d quadros e sairam %d", HEVC_B_QUADROS, n);
    // e na ordem de apresentacao: o cinza de cada quadro diz a posicao dele
    for (k = 0; k < n; k++)
        T_ASSERT(r, lumas[k] >= HEVC_B_LUMA(k) - 3 && lumas[k] <= HEVC_B_LUMA(k) + 3,
                 "quadro %d fora de ordem: luma %d, esperada %d", k, lumas[k], HEVC_B_LUMA(k));
}

// Tamanho que a unidade 'i' do fluxo de teste ocupa numa amostra com os parametros dentro
// (fatias + VPS/SPS/PPS, cada NAL com 4 bytes de tamanho).
static int hevc_b_tamanho_amostra(int i)
{
    const unsigned char* d = HEVC_B_DADOS; int fim_au = HEVC_B_INICIO[i + 1], p = HEVC_B_INICIO[i], tot = 0;
    while (p + 3 < fim_au)
    {
        int ini, j, fim, tipo;
        if (!(d[p] == 0 && d[p + 1] == 0 && d[p + 2] == 1)) { p++; continue; }
        ini = p + 3; j = ini;
        while (j + 2 < fim_au && !(d[j] == 0 && d[j + 1] == 0 && (d[j + 2] == 1 || (j + 3 < fim_au && d[j + 2] == 0 && d[j + 3] == 1)))) j++;
        fim = (j + 2 < fim_au) ? j : fim_au;
        tipo = (d[ini] >> 1) & 0x3F;
        if (tipo <= 34) tot += 4 + (fim - ini);
        p = fim;
    }
    return tot;
}

void teste_video_hls_cortado_h265_quadros_b_emenda_inteira(TestResult* r)
{
    enum { ESCONDIDOS = 4, ATRASO = 2 };   // 4 quadros antes de zero; decodificacao 2 quadros a frente
    char arq[512], pasta[512], nome[600];
    Ev ev; MediaProfile p;
    H26xToLen conv; Mp4Mux* mux = 0;
    static Amostra todas[MAXQ];
    static unsigned char anexo[1 << 16];
    int inicio[MAXQ + 1], lumas[MAXQ];
    unsigned char* d; long n;
    int rc, nseg, i, k, tot = 0, apresentados = HEVC_B_QUADROS - ESCONDIDOS, k1 = 10, copiados, ini_copia, na = 0, q;
    long pos;

    t_start(r);
    PRECISA_ENCODER(r, MEDIA_CODEC_H265);
    PRECISA_DECODER(r, MEDIA_CODEC_H265);
    caminho(arq, sizeof(arq), "cortado_h265_b.mp4");
    caminho(pasta, sizeof(pasta), "cortado_h265_b_out");

    // o fluxo de teste vira um MP4 "cortado": os 4 primeiros quadros apresentados ficam antes de zero
    h26x_tl_init(&conv, 1);
    for (i = 0; i < HEVC_B_QUADROS; i++)
    {
        uint8_t* out = 0;
        int alen = h26x_tl_feed(&conv, HEVC_B_DADOS + HEVC_B_INICIO[i], HEVC_B_INICIO[i + 1] - HEVC_B_INICIO[i], &out);
        if (!mux && h26x_tl_ready(&conv)) mux = mp4_open_hevc(arq, HEVC_B_LARG, HEVC_B_ALT, conv.cfg, conv.cfg_len, 0, 0, 0, 0);
        if (mux && alen > 0)
            mp4_write_video2(mux, (long long)(HEVC_B_APRES[i] - ESCONDIDOS) * 40, (long long)(i - ATRASO - ESCONDIDOS) * 40, HEVC_B_CHAVE[i], out, alen);
    }
    if (mux) mp4_close(mux);
    h26x_tl_free(&conv);
    T_ASSERT(r, mux != 0, "nao foi possivel gravar a entrada");

    perfil_copia(&p, CONT_HLS_FMP4);
    p.CutFileMode = GW_CUT_SMART;
    rc = roda_gw(arq, &p, pasta, &ev);
    T_ASSERT(r, rc == 0 && ev.done && !ev.erro, "gateway_run devolveu %d (done=%d erro=%d)", rc, ev.done, ev.erro);
    T_ASSERT(r, strstr(ev.pista, "primeiro grupo") != 0, "esperava recodificar so o primeiro grupo, a pista informou: %s", ev.pista);

    nseg = le_faixa(pasta, "source", g_seg);
    T_ASSERT(r, nseg >= 1, "nenhum segmento de video foi gravado");
    for (k = 0; k < nseg; k++) for (i = 0; i < g_seg[k].n && tot < MAXQ; i++) todas[tot++] = g_seg[k].a[i];
    T_ASSERT(r, tot == apresentados, "a saida devia ter os %d quadros apresentados pela origem, tem %d", apresentados, tot);
    T_ASSERT(r, g_seg[0].tfdt == 0 && todas[0].pts == 0, "a saida devia comecar em 0 (tempo-base %lld, primeiro quadro %lld)", g_seg[0].tfdt, todas[0].pts);
    for (i = 1; i < tot; i++)
        T_ASSERT(r, todas[i].dts > todas[i - 1].dts, "amostra %d: decodificacao em %lld ms depois de %lld ms", i, todas[i].dts, todas[i - 1].dts);

    // do segundo grupo em diante e copia: as mesmas amostras da origem, com os parametros dentro
    copiados = HEVC_B_QUADROS - k1; ini_copia = tot - copiados;
    for (i = 0; i < copiados; i++)
        T_ASSERT(r, todas[ini_copia + i].tam == hevc_b_tamanho_amostra(k1 + i),
                 "quadro copiado %d: %d bytes na saida, %d na origem", k1 + i, todas[ini_copia + i].tam, hevc_b_tamanho_amostra(k1 + i));
    for (i = 0; i < copiados; i++)
        T_ASSERT(r, todas[ini_copia + i].pts == (long long)(HEVC_B_APRES[k1 + i] - ESCONDIDOS) * 40,
                 "quadro copiado %d: apresentacao em %lld ms, esperada %d", k1 + i, todas[ini_copia + i].pts, (HEVC_B_APRES[k1 + i] - ESCONDIDOS) * 40);

    // A EMENDA: o trecho recodificado (posicoes 4 a 9 da origem) tem de trazer os SEIS quadros,
    // cada um diferente do anterior. Com o decoder perdendo o ultimo do grupo, o sexto era uma
    // repeticao do quinto. Decodifica-se a saida e confere-se o cinza de cada um.
    T_ASSERT(r, ini_copia == 6, "esperava 6 quadros recodificados antes da copia, vieram %d", ini_copia);
    gw_path_join(nome, sizeof(nome), pasta, "source-0.m4s");
    d = le_arquivo(nome, &n);
    T_ASSERT(r, d != 0, "source-0.m4s nao foi gerado");
    pos = (long)be32(d) + 8;   // dados logo depois do moof e do cabecalho do mdat
    for (i = 0; i < ini_copia && i < g_seg[0].n; i++)
    {
        long fim = pos + g_seg[0].a[i].tam, q2 = pos;
        inicio[i] = na;
        while (q2 + 4 <= fim && na + 4 < (int)sizeof(anexo))   // tamanho de 4 bytes -> codigo de inicio
        {
            long len = (long)be32(d + q2);
            if (len <= 0 || q2 + 4 + len > fim || na + 4 + len > (long)sizeof(anexo)) break;
            anexo[na++] = 0; anexo[na++] = 0; anexo[na++] = 0; anexo[na++] = 1;
            memcpy(anexo + na, d + q2 + 4, (size_t)len); na += (int)len;
            q2 += 4 + len;
        }
        pos = fim;
    }
    inicio[i] = na;
    free(d);
    q = decodifica_lumas(anexo, inicio, ini_copia, lumas, MAXQ);
    T_ASSERT(r, q == ini_copia, "o trecho recodificado tem %d amostras e decodifica em %d quadros", ini_copia, q);
    for (i = 0; i < q; i++)
        T_ASSERT(r, lumas[i] >= HEVC_B_LUMA(ESCONDIDOS + i) - 4 && lumas[i] <= HEVC_B_LUMA(ESCONDIDOS + i) + 4,
                 "quadro recodificado %d: luma %d, esperada %d (quadro repetido ou fora de ordem)", i, lumas[i], HEVC_B_LUMA(ESCONDIDOS + i));
}

// ---- DASH AV1: tamanho dos segmentos ----------------------------------------------------------

void teste_video_dash_av1_segmentos_seguem_o_alvo(TestResult* r)
{
    // 4 s de video com alvo de 2 s. O corte so acontece em quadro-chave, e o encoder AV1 punha
    // um a cada 5 s ou mais: o video inteiro saia num segmento so.
    static const Receita quatro_s = { MEDIA_CODEC_H264, 100, 25, 1, 0, 0, 0, 0, 25 };
    char arq[512], pasta[512], nome[600];
    Tempos tp; Ev ev; MediaProfile p;
    long long tl[MAXS * 4];
    char* txt; long n;
    int rc, nv, k;

    t_start(r);
    PRECISA_ENCODER(r, MEDIA_CODEC_H264);
    PRECISA_DECODER(r, MEDIA_CODEC_H264);
    PRECISA_ENCODER(r, MEDIA_CODEC_AV1);
    caminho(arq, sizeof(arq), "av1_4s.mp4");
    caminho(pasta, sizeof(pasta), "av1_4s_out");

    rc = gera_mp4(arq, &quatro_s, &tp);
    T_ASSERT(r, rc == 0 && tp.n == quatro_s.quadros, "nao foi possivel gerar a entrada (codigo %d, %d quadros)", rc, tp.n);

    memset(&p, 0, sizeof(p));
    p.Container = CONT_DASH_WEBM; p.VideoCodec = MEDIA_CODEC_AV1; p.AudioCodec = MEDIA_CODEC_OPUS;
    p.SegmentMs = 2000; p.Mode = GW_VOD; p.Width = 160; p.Height = 120;
    rc = roda_gw(arq, &p, pasta, &ev);
    T_ASSERT(r, rc == 0 && ev.done && !ev.erro, "gateway_run devolveu %d (done=%d erro=%d)", rc, ev.done, ev.erro);

    gw_path_join(nome, sizeof(nome), pasta, "manifest-av1.mpd");
    txt = (char*)le_arquivo(nome, &n);
    T_ASSERT(r, txt != 0, "manifest-av1.mpd nao foi gerado");
    nv = le_timeline(txt, 0, tl, MAXS * 4);
    free(txt);
    g_conta = 0; g_prefixo = "chunk-av1-"; gw_dir_list(pasta, conta_prefixo, 0);
    T_ASSERT(r, nv == g_conta, "o manifesto lista %d segmentos de video, a pasta tem %d", nv, g_conta);
    T_ASSERT(r, nv >= 2, "4 s com alvo de 2 s deviam dar ao menos 2 segmentos, veio %d", nv);
    for (k = 1; k < nv; k++)
        T_ASSERT(r, tl[k] - tl[k - 1] <= 2600, "segmento %d com %lld ms para um alvo de 2000 ms", k - 1, tl[k] - tl[k - 1]);
}

// ---- o padrao fica no formato que todo player aceita ------------------------------------------

// Tipo da entrada de amostra do init (avc1, avc3, hvc1, hev1...). "" se nao achou.
static void entrada_do_init(const char* pasta, const char* nome, char out[5])
{
    char arq[600]; long n, pos; unsigned char* d;
    out[0] = 0;
    snprintf(arq, sizeof(arq), "%s/%s", pasta, nome);
    d = le_arquivo(arq, &n);
    if (!d) return;
    pos = acha4(d, n, "stsd", 0);
    if (pos >= 0 && pos + 20 <= n) { memcpy(out, d + pos + 16, 4); out[4] = 0; }
    free(d);
}

void teste_video_padrao_usa_formato_aceito_por_todos(TestResult* r)
{
    // 'avc3'/'hev1' (parametros dentro das amostras) e composicao negativa nao foram testados em
    // todos os players -- o Safari, em particular, historicamente so aceita 'hvc1'. Entao eles so
    // podem aparecer por ESCOLHA: arquivo cortado, no modo padrao, sai recodificado, com a
    // entrada classica e sem composicao negativa. Este teste segura esse padrao.
    static const Receita cortado_h265 = { MEDIA_CODEC_H265, 60, 25, 1, 0, 10, 0, 0, 30 };
    char arq[512], pasta[512], entrada[5];
    Tempos tp; Ev ev; MediaProfile p;
    int rc, nseg, i, k;

    t_start(r);
    PRECISA_ENCODER(r, MEDIA_CODEC_H264);
    PRECISA_DECODER(r, MEDIA_CODEC_H264);
    caminho(arq, sizeof(arq), "padrao_h264.mp4");
    caminho(pasta, sizeof(pasta), "padrao_h264_out");

    rc = gera_mp4(arq, &ARQ_CORTADO, &tp);
    T_ASSERT(r, rc == 0, "nao foi possivel gerar a entrada (codigo %d)", rc);
    perfil_copia(&p, CONT_HLS_FMP4);
    T_ASSERT(r, p.CutFileMode == GW_CUT_RECODE, "o modo padrao de arquivo cortado devia ser recodificar");
    rc = roda_gw(arq, &p, pasta, &ev);
    T_ASSERT(r, rc == 0 && ev.done && !ev.erro, "gateway_run devolveu %d (done=%d erro=%d)", rc, ev.done, ev.erro);

    entrada_do_init(pasta, "init-source.mp4", entrada);
    T_ASSERT(r, strcmp(entrada, "avc1") == 0, "H.264 no modo padrao devia sair com a entrada avc1, saiu '%s'", entrada);
    nseg = le_faixa(pasta, "source", g_seg);
    T_ASSERT(r, nseg >= 1, "nenhum segmento de video foi gravado");
    for (k = 0; k < nseg; k++)
        for (i = 0; i < g_seg[k].n; i++)
            T_ASSERT(r, g_seg[k].a[i].pts >= g_seg[k].a[i].dts, "segmento %d, amostra %d com composicao negativa no modo padrao", k, i);

    if (media_codec_available(MEDIA_CODEC_H265) && gw_vdec_available(MEDIA_CODEC_H265))
    {
        caminho(arq, sizeof(arq), "padrao_h265.mp4");
        caminho(pasta, sizeof(pasta), "padrao_h265_out");
        rc = gera_mp4(arq, &cortado_h265, &tp);
        T_ASSERT(r, rc == 0, "nao foi possivel gerar a entrada H.265 (codigo %d)", rc);
        perfil_copia(&p, CONT_HLS_FMP4);
        rc = roda_gw(arq, &p, pasta, &ev);
        T_ASSERT(r, rc == 0 && ev.done && !ev.erro, "H.265: gateway_run devolveu %d (done=%d erro=%d)", rc, ev.done, ev.erro);
        entrada_do_init(pasta, "init-source.mp4", entrada);
        T_ASSERT(r, strcmp(entrada, "hvc1") == 0, "H.265 no modo padrao devia sair com a entrada hvc1, saiu '%s'", entrada);
    }
}

// ---- arquivo cortado: ignorar o corte e usar a linha do tempo toda ----------------------------

void teste_video_cortado_linha_do_tempo_toda(TestResult* r)
{
    static const char* NOMES[2] = { "grande", "pequena" };
    char arq[512], pasta[512], nome[600], entrada[5];
    Tempos tp; Ev ev; MediaProfile p; PipeTrack pistas[2];
    long long pts[MAXQ];
    char* txt; long n;
    int rc, nseg, naud, i, k, tot = 0, idx = 0, real = -1;

    t_start(r);
    PRECISA_ENCODER(r, MEDIA_CODEC_H264);
    PRECISA_DECODER(r, MEDIA_CODEC_H264);
    caminho(arq, sizeof(arq), "cortado_tudo.mp4");
    caminho(pasta, sizeof(pasta), "cortado_tudo_out");

    rc = gera_mp4(arq, &ARQ_CORTADO, &tp);
    T_ASSERT(r, rc == 0 && tp.n == ARQ_CORTADO.quadros, "nao foi possivel gerar a entrada (codigo %d, %d quadros)", rc, tp.n);

    // 1) copia: os 60 quadros gravados, cada um no seu tempo, sem nada espremido nem aviso de inicio
    perfil_copia(&p, CONT_HLS_FMP4);
    p.CutFileMode = GW_CUT_SHOW_ALL;
    rc = roda_gw(arq, &p, pasta, &ev);
    T_ASSERT(r, rc == 0 && ev.done && !ev.erro, "gateway_run devolveu %d (done=%d erro=%d)", rc, ev.done, ev.erro);
    T_ASSERT(r, strstr(ev.pista, "bypass") != 0 && strstr(ev.pista, "corte do arquivo ignorado") != 0,
             "esperava copia com o corte ignorado, a pista informou: %s", ev.pista);

    nseg = le_faixa(pasta, "source", g_seg);
    T_ASSERT(r, nseg >= 1, "nenhum segmento de video foi gravado");
    for (k = 0; k < nseg; k++) for (i = 0; i < g_seg[k].n && tot < MAXQ; i++) pts[tot++] = g_seg[k].a[i].pts;
    T_ASSERT(r, tot == tp.n, "a saida devia ter os %d quadros gravados, tem %d", tp.n, tot);
    qsort(pts, (size_t)tot, sizeof(long long), cmp_ll);
    for (i = 0; i < tot; i++)
        T_ASSERT(r, pts[i] == (long long)i * 40, "quadro %d devia estar em %d ms, esta em %lld (a linha do tempo toda, na cadencia original)", i, i * 40, pts[i]);

    entrada_do_init(pasta, "init-source.mp4", entrada);
    T_ASSERT(r, strcmp(entrada, "avc1") == 0, "a copia devia manter a entrada avc1, saiu '%s'", entrada);
    gw_path_join(nome, sizeof(nome), pasta, "master.m3u8");
    txt = (char*)le_arquivo(nome, &n);
    T_ASSERT(r, txt != 0, "master.m3u8 nao foi gerado");
    rc = strstr(txt, "EXT-X-START") == 0;
    free(txt);
    T_ASSERT(r, rc, "com a linha do tempo toda nao ha por que mandar o player pular o comeco");

    // o audio anda junto: entrava 200 ms antes do zero do arquivo, entra 200 ms depois do novo zero
    naud = le_faixa(pasta, "audio", g_aud);
    T_ASSERT(r, naud >= 1, "a faixa de audio nao foi gravada");
    for (k = 0; k < naud && real < 0; k++)
        for (i = 0; i < g_aud[k].n; i++, idx++)
            if (g_aud[k].a[i].tam == AUD_BYTES) { real = (int)g_aud[k].a[i].dts; break; }
    T_ASSERT(r, real == 8820, "o primeiro quadro de audio da origem devia estar na amostra 8820 (0,2 s), esta na %d", real);

    // 2) recodificando em escada: o trecho escondido tambem entra (obedecendo o corte seriam 50)
    memset(pistas, 0, sizeof(pistas));
    pistas[0].Width = 320; pistas[0].Height = 240; pistas[0].BitrateBps = 400000; pistas[0].Name = NOMES[0];
    pistas[1].Width = 160; pistas[1].Height = 120; pistas[1].BitrateBps = 150000; pistas[1].Name = NOMES[1];
    memset(&p, 0, sizeof(p));
    p.Container = CONT_HLS_FMP4; p.VideoCodec = MEDIA_CODEC_H264; p.AudioCodec = MEDIA_CODEC_AAC;
    p.SegmentMs = 1000; p.Mode = GW_VOD; p.Renditions = pistas; p.RenditionCount = 2;
    p.CutFileMode = GW_CUT_SHOW_ALL;
    rc = roda_gw(arq, &p, pasta, &ev);
    T_ASSERT(r, rc == 0 && ev.done && !ev.erro, "escada: gateway_run devolveu %d (done=%d erro=%d)", rc, ev.done, ev.erro);
    for (i = 0; i < 2; i++)
    {
        nseg = le_faixa(pasta, NOMES[i], g_seg);
        T_ASSERT(r, nseg >= 1 && total_amostras(g_seg, nseg) == tp.n,
                 "escada, pista %s: devia ter os %d quadros gravados, tem %d", NOMES[i], tp.n, nseg > 0 ? total_amostras(g_seg, nseg) : 0);
        T_ASSERT(r, g_seg[0].tfdt == 0, "escada, pista %s: devia comecar em 0, comeca em %lld ms", NOMES[i], g_seg[0].tfdt);
    }
}

// ---- HLS em H.265 recodificado: tamanho dos segmentos ------------------------------------------

// Tipo do primeiro NAL de fatia (0..31) da primeira amostra de um segmento H.265; -1 se nao achou.
static int primeira_fatia_h265(const char* path)
{
    long n, pos, fim; unsigned char* d = le_arquivo(path, &n);
    Segmento* s = &g_aud[0];   // rascunho: o teste que chama nao usa a faixa de audio
    int tipo = -1;
    if (!d) return -1;
    if (le_segmento(path, s) == 0 && s->n > 0)
    {
        pos = (long)be32(d) + 8; fim = pos + s->a[0].tam;
        while (pos + 6 <= fim && pos + 6 <= n)
        {
            long len = (long)be32(d + pos);
            int t = (d[pos + 4] >> 1) & 0x3F;
            if (t <= 31) { tipo = t; break; }
            if (len <= 0) break;
            pos += 4 + len;
        }
    }
    free(d);
    return tipo;
}

void teste_video_hls_h265_segmentos_seguem_o_alvo(TestResult* r)
{
    // 4 s de video recodificado em H.265 com alvo de 2 s. O corte so acontece em quadro-chave, e
    // o encoder punha um a cada 10 s: o video inteiro saia num segmento so.
    static const Receita quatro_s = { MEDIA_CODEC_H264, 100, 25, 1, 0, 0, 0, 0, 25 };
    char arq[512], pasta[512], nome[600];
    Tempos tp; Ev ev; MediaProfile p;
    int rc, nseg, k, tipo;

    t_start(r);
    PRECISA_ENCODER(r, MEDIA_CODEC_H264);
    PRECISA_DECODER(r, MEDIA_CODEC_H264);
    PRECISA_ENCODER(r, MEDIA_CODEC_H265);
    caminho(arq, sizeof(arq), "h265_4s.mp4");
    caminho(pasta, sizeof(pasta), "h265_4s_out");

    rc = gera_mp4(arq, &quatro_s, &tp);
    T_ASSERT(r, rc == 0 && tp.n == quatro_s.quadros, "nao foi possivel gerar a entrada (codigo %d, %d quadros)", rc, tp.n);

    memset(&p, 0, sizeof(p));
    p.Container = CONT_HLS_FMP4; p.VideoCodec = MEDIA_CODEC_H265; p.AudioCodec = MEDIA_CODEC_AAC;
    p.SegmentMs = 2000; p.Mode = GW_VOD; p.Width = 160; p.Height = 120;
    rc = roda_gw(arq, &p, pasta, &ev);
    T_ASSERT(r, rc == 0 && ev.done && !ev.erro, "gateway_run devolveu %d (done=%d erro=%d)", rc, ev.done, ev.erro);

    nseg = le_faixa(pasta, "source", g_seg);
    T_ASSERT(r, nseg >= 1 && total_amostras(g_seg, nseg) == tp.n, "esperava %d quadros nos segmentos, vieram %d", tp.n, nseg > 0 ? total_amostras(g_seg, nseg) : 0);
    T_ASSERT(r, nseg >= 2, "4 s com alvo de 2 s deviam dar ao menos 2 segmentos, veio %d", nseg);
    for (k = 0; k < nseg; k++)
    {
        T_ASSERT(r, g_seg[k].n * 40 <= 2600, "segmento %d com %d ms para um alvo de 2000 ms", k, g_seg[k].n * 40);
        // cada segmento tem de comecar num quadro que fecha o grupo (IDR), para decodificar sozinho
        snprintf(nome, sizeof(nome), "%s/source-%d.m4s", pasta, k);
        tipo = primeira_fatia_h265(nome);
        T_ASSERT(r, tipo == 19 || tipo == 20, "o segmento %d comeca num NAL de tipo %d; devia ser IDR (19 ou 20)", k, tipo);
    }
}
