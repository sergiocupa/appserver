//  Testes do pipeline de midia, em C.
//
//  Vieram dos executaveis soltos que existiam antes (webmtest, mkvtest, gwtest). O que eles
//  provam continua o mesmo; o que muda e que agora cada caso aparece no Gerenciador de Testes
//  e pode ser executado ou depurado isoladamente.
//
//  Todos sao AUTOCONTIDOS: cada um gera o que precisa numa pasta de trabalho propria, em vez
//  de depender de arquivo deixado por outro teste. Era assim que o gwtest dependia do
//  webmtest ter rodado antes.

#include "testes.h"
#include "media_codec.h"
#include "mux_webm.h"
#include "media_source.h"
#include "gw_decode.h"
#include "sync_gateway.h"
#include "gw_path.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define LARG  320
#define ALT   240
#define FPS    30
#define QUADROS 30

#define PASTA "unittests_tmp"

static void caminho(char* out, int tam, const char* nome)
{
    gw_mkdir_p(PASTA);
    snprintf(out, (size_t)tam, "%s/%s", PASTA, nome);
}

// Imagem sintetica: gradiente que muda a cada quadro, para o encoder ter o que comprimir.
static void preenche(unsigned char* y, unsigned char* u, unsigned char* v, int quadro)
{
    int i, j;
    for (j = 0; j < ALT; j++)
        for (i = 0; i < LARG; i++)
            y[j * LARG + i] = (unsigned char)((i + j + quadro * 3) & 0xFF);
    memset(u, 128, (size_t)(LARG / 2) * (ALT / 2));
    memset(v, 128, (size_t)(LARG / 2) * (ALT / 2));
}

// Gera um arquivo WebM com QUADROS quadros no codec pedido. Devolve 0 em sucesso.
static int gera_webm(MediaCodec codec, const char* mux_id, const char* path, int* muxados)
{
    MediaEncoderParams p;
    MediaEncoder* enc;
    WebmMux* mux;
    unsigned char *y, *u, *v;
    int f, ok = 0;

    *muxados = 0;
    memset(&p, 0, sizeof(p));
    p.Codec = codec; p.Width = LARG; p.Height = ALT; p.Fps = FPS; p.BitrateBps = 500000; p.Threads = 2;

    enc = media_encoder_open(&p);
    if (!enc) return -1;

    mux = webm_open(path, mux_id, LARG, ALT, FPS, 0, 0, 0, 0, 0);
    if (!mux) { enc->Close(enc); return -2; }

    y = (unsigned char*)malloc(LARG * ALT);
    u = (unsigned char*)malloc(LARG * ALT / 4);
    v = (unsigned char*)malloc(LARG * ALT / 4);

    for (f = 0; f < QUADROS; f++)
    {
        MediaFrame fr; MediaPacket pk;
        preenche(y, u, v, f);
        memset(&fr, 0, sizeof(fr));
        fr.Kind = MEDIA_KIND_VIDEO; fr.Width = LARG; fr.Height = ALT;
        fr.Y = y; fr.StrideY = LARG; fr.U = u; fr.StrideU = LARG / 2; fr.V = v; fr.StrideV = LARG / 2;
        fr.Pts = f;
        if (enc->SendFrame(enc, &fr) < 0) { ok = -3; break; }
        while (enc->ReceivePacket(enc, &pk) == 1)
        {
            webm_write_video(mux, (long long)(f * 1000 / FPS), pk.KeyFrame, pk.Data, pk.Size);
            (*muxados)++;
        }
    }

    if (ok == 0)
    {
        MediaPacket pk;
        enc->SendFrame(enc, 0);   // drena: sem isto faltam os ultimos quadros
        while (enc->ReceivePacket(enc, &pk) == 1)
        {
            webm_write_video(mux, (long long)(QUADROS * 1000 / FPS), pk.KeyFrame, pk.Data, pk.Size);
            (*muxados)++;
        }
    }

    webm_close(mux);
    enc->Close(enc);
    free(y); free(u); free(v);
    return ok;
}

// Le o arquivo de volta pelo mesmo caminho que o gateway usa e decodifica.
static int demux_e_decodifica(const char* path, int* pacotes, int* decodificados)
{
    MediaSource* src;
    MediaStreamInfo st[4];
    GwVideoDec* dec;
    GwPacket pkt;
    int nst = 0;

    *pacotes = 0; *decodificados = 0;

    src = source_file_open(path);
    if (!src) return -1;

    src->Info(src, st, 4, &nst);
    if (nst <= 0) { src->Close(src); return -2; }

    dec = gw_vdec_open(st[0].Codec);
    if (!dec) { src->Close(src); return -3; }

    while (src->Read(src, &pkt) == 1)
    {
        GwImage img;
        (*pacotes)++;
        if (gw_vdec_send(dec, pkt.Data, pkt.Size) < 0) continue;
        while (gw_vdec_next(dec, &img) == 1) (*decodificados)++;
    }

    gw_vdec_send(dec, 0, 0);   // drena
    {
        GwImage img;
        while (gw_vdec_next(dec, &img) == 1) (*decodificados)++;
    }

    gw_vdec_close(&dec);
    src->Close(src);
    return 0;
}

static void roundtrip(TestResult* r, MediaCodec codec, const char* mux_id, const char* arquivo)
{
    char path[512];
    int muxados = 0, pacotes = 0, decodificados = 0, rc;

    t_start(r);

    T_ASSERT(r, media_codec_available(codec),   "encoder de %s nao compilado neste build", media_codec_name(codec));
    T_ASSERT(r, media_decoder_available(codec), "decoder de %s nao compilado neste build", media_codec_name(codec));

    caminho(path, sizeof(path), arquivo);

    rc = gera_webm(codec, mux_id, path, &muxados);
    T_ASSERT(r, rc == 0, "falha ao gerar o webm de %s (codigo %d)", media_codec_name(codec), rc);
    T_ASSERT(r, muxados >= QUADROS, "esperava ao menos %d pacotes muxados, saíram %d", QUADROS, muxados);

    rc = demux_e_decodifica(path, &pacotes, &decodificados);
    T_ASSERT(r, rc == 0, "falha no demux/decode (codigo %d)", rc);
    T_ASSERT(r, pacotes >= QUADROS, "esperava ao menos %d pacotes no demux, vieram %d", QUADROS, pacotes);
    T_ASSERT(r, decodificados == QUADROS, "esperava %d quadros decodificados, vieram %d", QUADROS, decodificados);
}

void teste_webm_roundtrip_vp9(TestResult* r) { roundtrip(r, MEDIA_CODEC_VP9, "V_VP9", "rt_vp9.webm"); }
void teste_webm_roundtrip_av1(TestResult* r) { roundtrip(r, MEDIA_CODEC_AV1, "V_AV1", "rt_av1.webm"); }

// ---- gateway ponta a ponta --------------------------------------------------

typedef struct { int done, erro, tracks; long long vivos; } EventosGw;

static void ao_evento(void* user, const GwEvent* e)
{
    EventosGw* g = (EventosGw*)user;
    switch (e->Kind)
    {
        case GW_EV_DONE:       g->done = 1; break;
        case GW_EV_ERROR:      g->erro = 1; break;
        case GW_EV_TRACK_DONE: g->tracks++; break;
        case GW_EV_MEM:        g->vivos = (long long)e->MemAlloc - (long long)e->MemFree; break;
        default: break;
    }
}

static int g_chunks;
static void conta_chunk(void* user, const char* nome, int is_dir)
{ (void)user; if (!is_dir && strncmp(nome, "chunk-", 6) == 0) g_chunks++; }

// Roda o gateway sobre um webm de entrada e devolve o balanco de memoria da rodada.
static int roda_gateway(const char* entrada, MediaCodec saida, Container container,
                        const char* pasta_saida, EventosGw* ev)
{
    MediaSource* src;
    MediaProfile p;
    GwFeedback fb;
    int rc;

    gw_rmtree(pasta_saida);
    gw_mkdir_p(pasta_saida);

    src = source_file_open(entrada);
    if (!src) return -1;

    memset(&p, 0, sizeof(p));
    p.Container = container;
    p.VideoCodec = saida;
    p.SegmentMs = 1000;
    p.Mode = GW_VOD;
    p.Width = 160; p.Height = 120;   // forca escala: exercita decode -> escala -> encode
    p.Fps = 15;                      // forca reamostragem de taxa

    memset(ev, 0, sizeof(*ev));
    memset(&fb, 0, sizeof(fb));
    fb.Fn = ao_evento; fb.User = ev;

    g_chunks = 0;
    rc = gateway_run(src, &p, pasta_saida, &fb, 0);
    src->Close(src);
    gw_dir_list(pasta_saida, conta_chunk, 0);
    return rc;
}

void teste_gateway_dash_vp9(TestResult* r)
{
    char entrada[512], saida[512], mpd[512];
    EventosGw ev;
    int muxados = 0, rc;

    t_start(r);
    T_ASSERT(r, media_codec_available(MEDIA_CODEC_VP9), "encoder VP9 nao compilado neste build");

    caminho(entrada, sizeof(entrada), "gw_vp9.webm");
    caminho(saida,   sizeof(saida),   "gw_out_dash");

    rc = gera_webm(MEDIA_CODEC_VP9, "V_VP9", entrada, &muxados);
    T_ASSERT(r, rc == 0, "nao foi possivel preparar a entrada (codigo %d)", rc);

    rc = roda_gateway(entrada, MEDIA_CODEC_VP9, CONT_DASH_WEBM, saida, &ev);
    T_ASSERT(r, rc == 0,   "gateway_run devolveu %d", rc);
    T_ASSERT(r, ev.done,   "o gateway nao emitiu o evento de conclusao");
    T_ASSERT(r, !ev.erro,  "o gateway reportou erro");
    T_ASSERT(r, g_chunks > 0, "nenhum chunk foi gravado");

    gw_path_join(mpd, sizeof(mpd), saida, "manifest.mpd");
    T_ASSERT(r, gw_file_exists(mpd), "manifest.mpd nao foi gerado");
}

void teste_gateway_hls_h264(TestResult* r)
{
    char entrada[512], saida[512];
    EventosGw ev;
    int muxados = 0, rc;

    t_start(r);
    T_ASSERT(r, media_codec_available(MEDIA_CODEC_VP9), "encoder VP9 nao compilado neste build");

    caminho(entrada, sizeof(entrada), "gw_vp9_hls.webm");
    caminho(saida,   sizeof(saida),   "gw_out_hls");

    rc = gera_webm(MEDIA_CODEC_VP9, "V_VP9", entrada, &muxados);
    T_ASSERT(r, rc == 0, "nao foi possivel preparar a entrada (codigo %d)", rc);

    // HLS nao passa pelo caminho segmento-paralelo: exercita o percurso por quadro,
    // com decode e drenagem proprios.
    rc = roda_gateway(entrada, MEDIA_CODEC_H264, CONT_HLS_FMP4, saida, &ev);
    T_ASSERT(r, rc == 0,  "gateway_run devolveu %d", rc);
    T_ASSERT(r, ev.done,  "o gateway nao emitiu o evento de conclusao");
    T_ASSERT(r, !ev.erro, "o gateway reportou erro");
}

void teste_gateway_memoria_nao_cresce(TestResult* r)
{
    char entrada[512], saida[512];
    EventosGw ev;
    long long primeira = 0;
    int muxados = 0, rc, rodada;

    t_start(r);
    T_ASSERT(r, media_codec_available(MEDIA_CODEC_VP9), "encoder VP9 nao compilado neste build");

    caminho(entrada, sizeof(entrada), "gw_vp9_mem.webm");
    caminho(saida,   sizeof(saida),   "gw_out_mem");

    rc = gera_webm(MEDIA_CODEC_VP9, "V_VP9", entrada, &muxados);
    T_ASSERT(r, rc == 0, "nao foi possivel preparar a entrada (codigo %d)", rc);

    // O que importa nao e o valor de "vivos", e sim ele NAO CRESCER de uma rodada para a
    // outra: e assim que acumulo entre sessoes aparece.
    for (rodada = 1; rodada <= 3; rodada++)
    {
        rc = roda_gateway(entrada, MEDIA_CODEC_VP9, CONT_DASH_WEBM, saida, &ev);
        T_ASSERT(r, rc == 0 && ev.done && !ev.erro, "rodada %d falhou (codigo %d)", rodada, rc);

        if (rodada == 1) primeira = ev.vivos;
        else T_ASSERT(r, ev.vivos <= primeira,
                      "memoria viva cresceu: rodada 1 = %lld, rodada %d = %lld",
                      primeira, rodada, ev.vivos);
    }
}
