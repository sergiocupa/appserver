//  Transicao entre codecs no gateway: a mesma sessao converte varias vezes seguidas, e cada
//  rodada muda o codec de saida, a resolucao e a taxa de quadros.
//
//  Por que existe: cada teste de codec, sozinho, abre o encoder, usa e fecha. O que nao
//  estava coberto e a TROCA -- fechar o SVT-AV1 e abrir o x265 no mesmo processo, trocar de
//  160x120 para 320x240, de 15 para 30 fps. E ai que sobra estado: buffer dimensionado para
//  a resolucao anterior, decoder que nao soltou o que reteve, tabela global que um codec
//  deixou apontando para memoria que ja saiu.
//
//  Duas coisas sao verificadas em cada rodada:
//    1. ela terminou sem evento de erro e produziu saida;
//    2. a saida DECODIFICA, com as dimensoes daquela rodada -- e assim que lixo aparece.
//  E, no fim, que a contabilidade de memoria volta ao mesmo lugar: se cada transicao
//  deixasse um pedaco para tras, a conta cresceria a cada volta.

#include "testes.h"
#include "media_codec.h"
#include "memory_pool.h"
#include "mux_webm.h"
#include "media_source.h"
#include "gw_decode.h"
#include "sync_gateway.h"
#include "gw_path.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define ENT_LARG 320
#define ENT_ALT  240
#define ENT_FPS   30
#define ENT_QUADROS 30

#define PASTA "unittests_tmp/matriz"

typedef struct { int done, erro, tracks; } Eventos;

static int g_chunks;

static void ao_evento(void* user, const GwEvent* e)
{
    Eventos* g = (Eventos*)user;
    switch (e->Kind)
    {
        case GW_EV_DONE:       g->done = 1;   break;
        case GW_EV_ERROR:      g->erro = 1;   break;
        case GW_EV_TRACK_DONE: g->tracks++;   break;
        default: break;
    }
}

static void conta_chunk(void* user, const char* nome, int is_dir)
{ (void)user; if (!is_dir && strncmp(nome, "chunk-", 6) == 0) g_chunks++; }

// ---- entrada ---------------------------------------------------------------

static void preenche(unsigned char* y, unsigned char* u, unsigned char* v, int quadro)
{
    int i, j;
    for (j = 0; j < ENT_ALT; j++)
        for (i = 0; i < ENT_LARG; i++)
            y[j * ENT_LARG + i] = (unsigned char)((i + j + quadro * 3) & 0xFF);
    memset(u, 128, (size_t)(ENT_LARG / 2) * (ENT_ALT / 2));
    memset(v, 128, (size_t)(ENT_LARG / 2) * (ENT_ALT / 2));
}

// WebM de entrada, no codec pedido. 0 em sucesso.
static int gera_entrada(MediaCodec codec, const char* mux_id, const char* path)
{
    MediaEncoderParams p;
    MediaEncoder* enc;
    WebmMux* mux;
    unsigned char *y, *u, *v;
    int f, ok = 0;

    memset(&p, 0, sizeof(p));
    p.Codec = codec; p.Width = ENT_LARG; p.Height = ENT_ALT;
    p.Fps = ENT_FPS; p.BitrateBps = 500000; p.Threads = 2;

    enc = media_encoder_open(&p);
    if (!enc) return -1;

    mux = webm_open(path, mux_id, ENT_LARG, ENT_ALT, ENT_FPS, 0, 0, 0, 0, 0);
    if (!mux) { enc->Close(enc); return -2; }

    y = (unsigned char*)malloc(ENT_LARG * ENT_ALT);
    u = (unsigned char*)malloc(ENT_LARG * ENT_ALT / 4);
    v = (unsigned char*)malloc(ENT_LARG * ENT_ALT / 4);

    for (f = 0; f < ENT_QUADROS; f++)
    {
        MediaFrame fr; MediaPacket pk;
        preenche(y, u, v, f);
        memset(&fr, 0, sizeof(fr));
        fr.Kind = MEDIA_KIND_VIDEO; fr.Width = ENT_LARG; fr.Height = ENT_ALT;
        fr.Y = y; fr.StrideY = ENT_LARG; fr.U = u; fr.StrideU = ENT_LARG / 2; fr.V = v; fr.StrideV = ENT_LARG / 2;
        fr.Pts = f;
        if (enc->SendFrame(enc, &fr) < 0) { ok = -3; break; }
        while (enc->ReceivePacket(enc, &pk) == 1)
            webm_write_video(mux, (long long)(f * 1000 / ENT_FPS), pk.KeyFrame, pk.Data, pk.Size);
    }
    if (ok == 0)
    {
        MediaPacket pk;
        enc->SendFrame(enc, 0);
        while (enc->ReceivePacket(enc, &pk) == 1)
            webm_write_video(mux, (long long)(ENT_QUADROS * 1000 / ENT_FPS), pk.KeyFrame, pk.Data, pk.Size);
    }

    webm_close(mux);
    enc->Close(enc);
    free(y); free(u); free(v);
    return ok;
}

// ---- uma rodada do gateway --------------------------------------------------

typedef struct
{
    MediaCodec Saida;
    Container  Cont;
    int        Larg, Alt, Fps;
}
Passo;

// Roda o gateway com esse passo. Devolve o codigo do gateway; preenche os eventos.
static int roda(const char* entrada, const Passo* pa, const char* pasta, Eventos* ev)
{
    MediaSource* src;
    MediaProfile p;
    GwFeedback fb;
    int rc;

    gw_rmtree(pasta);
    gw_mkdir_p(pasta);

    src = source_file_open(entrada);
    if (!src) return -1;

    memset(&p, 0, sizeof(p));
    p.Container  = pa->Cont;
    p.VideoCodec = pa->Saida;
    p.SegmentMs  = 1000;
    p.Mode       = GW_VOD;
    p.Width = pa->Larg; p.Height = pa->Alt;
    p.Fps   = pa->Fps;

    memset(ev, 0, sizeof(*ev));
    memset(&fb, 0, sizeof(fb));
    fb.Fn = ao_evento; fb.User = ev;

    g_chunks = 0;
    rc = gateway_run(src, &p, pasta, &fb, 0);
    src->Close(src);
    gw_dir_list(pasta, conta_chunk, 0);
    return rc;
}

// Decodifica o output.mp4 daquela rodada. Devolve quantos quadros sairam (-1 se nem abriu)
// e as dimensoes do ultimo. So faz sentido para o sink de MP4 (arquivo unico).
static int decodifica_saida(const char* pasta, int* larg, int* alt)
{
    char path[512];
    MediaSource* src;
    MediaStreamInfo st[4];
    GwVideoDec* dec;
    GwPacket pkt;
    GwImage img;
    int nst = 0, n = 0;

    *larg = 0; *alt = 0;
    snprintf(path, sizeof(path), "%s/output.mp4", pasta);

    src = source_file_open(path);
    if (!src) return -1;

    src->Info(src, st, 4, &nst);
    if (nst <= 0) { src->Close(src); return -1; }

    dec = gw_vdec_open(st[0].Codec);
    if (!dec) { src->Close(src); return -1; }

    while (src->Read(src, &pkt) == 1)
    {
        if (gw_vdec_send(dec, pkt.Data, pkt.Size) < 0) continue;
        while (gw_vdec_next(dec, &img) == 1) { n++; *larg = img.Width; *alt = img.Height; }
    }
    gw_vdec_send(dec, 0, 0);
    while (gw_vdec_next(dec, &img) == 1) { n++; *larg = img.Width; *alt = img.Height; }

    gw_vdec_close(&dec);
    src->Close(src);
    return n;
}

static long long vivos(void)
{
    MemPoolStats s;
    memop_get_stats(&s);
    return (long long)s.alloc_count - (long long)s.free_count;
}

// O corpo comum das tres matrizes. 'passos' e percorrido DUAS vezes: a primeira aquece
// (primeira abertura de cada codec aloca tabela, pool, etc.), a segunda tem de repetir o
// mesmo resultado e o mesmo balanco de memoria.
static void percorre(TestResult* r, const char* entrada, const Passo* passos, int n)
{
    long long depois_da_1a = 0, depois_da_2a = 0;
    int volta, i;

    for (volta = 0; volta < 2; volta++)
    {
        for (i = 0; i < n; i++)
        {
            const Passo* pa = &passos[i];
            const char* nome = media_codec_name(pa->Saida);
            Eventos ev;
            char pasta[512];
            int rc;

            if (!media_codec_available(pa->Saida)) continue;   // build sem esse encoder

            snprintf(pasta, sizeof(pasta), "%s/p%d", PASTA, i);
            rc = roda(entrada, pa, pasta, &ev);

            T_ASSERT(r, rc == 0,
                     "volta %d, passo %d (%s %dx%d @%dfps): gateway devolveu %d",
                     volta + 1, i, nome, pa->Larg, pa->Alt, pa->Fps, rc);
            T_ASSERT(r, !ev.erro,
                     "volta %d, passo %d (%s %dx%d @%dfps): o gateway emitiu evento de ERRO",
                     volta + 1, i, nome, pa->Larg, pa->Alt, pa->Fps);
            T_ASSERT(r, ev.done,
                     "volta %d, passo %d (%s %dx%d @%dfps): terminou sem evento de conclusao",
                     volta + 1, i, nome, pa->Larg, pa->Alt, pa->Fps);

            if (pa->Cont == CONT_MP4_FILE)
            {
                // Arquivo unico: da para decodificar e conferir que o que saiu e video de
                // verdade, na resolucao daquela rodada. E aqui que lixo de uma transicao
                // aparece.
                int larg = 0, alt = 0;
                int quadros = decodifica_saida(pasta, &larg, &alt);
                T_ASSERT(r, quadros > 0,
                         "volta %d, passo %d (%s %dx%d @%dfps): a saida nao decodificou",
                         volta + 1, i, nome, pa->Larg, pa->Alt, pa->Fps);
                T_ASSERT(r, larg == pa->Larg && alt == pa->Alt,
                         "volta %d, passo %d (%s): pedi %dx%d e a saida decodificou %dx%d",
                         volta + 1, i, nome, pa->Larg, pa->Alt, larg, alt);
            }
            else
            {
                T_ASSERT(r, g_chunks > 0,
                         "volta %d, passo %d (%s %dx%d @%dfps): nenhum segmento foi escrito",
                         volta + 1, i, nome, pa->Larg, pa->Alt, pa->Fps);
            }
        }

        if (volta == 0) depois_da_1a = vivos();
        else            depois_da_2a = vivos();
    }

    // Uma volta inteira a mais nao pode deixar sobra. Folga pequena para a contabilidade
    // do proprio teste (caminhos, listagem de diretorio).
    T_ASSERT(r, depois_da_2a - depois_da_1a <= 32,
             "a segunda volta deixou %lld alocacoes vivas a mais que a primeira (%lld -> %lld)",
             depois_da_2a - depois_da_1a, depois_da_1a, depois_da_2a);
}

// ---- as tres matrizes -------------------------------------------------------

// Troca o CODEC de saida a cada rodada, mantendo resolucao e fps.
void teste_gateway_alterna_codec(TestResult* r)
{
    static const Passo PASSOS[] =
    {
        { MEDIA_CODEC_H264, CONT_MP4_FILE,  160, 120, 15 },
        { MEDIA_CODEC_VP9,  CONT_DASH_WEBM, 160, 120, 15 },
        { MEDIA_CODEC_H265, CONT_MP4_FILE,  160, 120, 15 },
        { MEDIA_CODEC_AV1,  CONT_DASH_WEBM, 160, 120, 15 },
        { MEDIA_CODEC_H264, CONT_MP4_FILE,  160, 120, 15 },   // volta ao primeiro
    };
    char entrada[512];

    t_start(r);
    if (!media_codec_available(MEDIA_CODEC_VP9))
        T_SKIP(r, "sem encoder de VP9 para gerar a entrada");

    gw_mkdir_p(PASTA);
    snprintf(entrada, sizeof(entrada), "%s/entrada_vp9.webm", PASTA);
    T_ASSERT(r, gera_entrada(MEDIA_CODEC_VP9, "V_VP9", entrada) == 0, "falhou ao gerar a entrada");

    percorre(r, entrada, PASSOS, (int)(sizeof(PASSOS) / sizeof(PASSOS[0])));
}

// Mantem o codec e troca RESOLUCAO e FPS. O caminho decode -> escala -> encode e
// redimensionado a cada rodada; buffer que sobreviva de uma para a outra aparece aqui.
void teste_gateway_alterna_resolucao_e_fps(TestResult* r)
{
    static const Passo PASSOS[] =
    {
        { MEDIA_CODEC_H264, CONT_MP4_FILE, 160, 120, 15 },
        { MEDIA_CODEC_H264, CONT_MP4_FILE, 320, 240, 30 },
        { MEDIA_CODEC_H264, CONT_MP4_FILE,  96,  72, 10 },
        { MEDIA_CODEC_H264, CONT_MP4_FILE, 240, 176, 24 },
        { MEDIA_CODEC_H264, CONT_MP4_FILE, 160, 120, 15 },   // volta a primeira
    };
    char entrada[512];

    t_start(r);
    if (!media_codec_available(MEDIA_CODEC_VP9))
        T_SKIP(r, "sem encoder de VP9 para gerar a entrada");
    if (!media_codec_available(MEDIA_CODEC_H264))
        T_SKIP(r, "sem encoder de H.264");

    gw_mkdir_p(PASTA);
    snprintf(entrada, sizeof(entrada), "%s/entrada_vp9.webm", PASTA);
    T_ASSERT(r, gera_entrada(MEDIA_CODEC_VP9, "V_VP9", entrada) == 0, "falhou ao gerar a entrada");

    percorre(r, entrada, PASSOS, (int)(sizeof(PASSOS) / sizeof(PASSOS[0])));
}

// Troca o codec de ENTRADA: o decoder e que muda (libvpx -> dav1d -> libvpx), com o mesmo
// encoder do outro lado.
void teste_gateway_alterna_entrada(TestResult* r)
{
    static const Passo PASSO = { MEDIA_CODEC_H264, CONT_MP4_FILE, 160, 120, 15 };
    char ent_vp9[512], ent_av1[512], pasta[512];
    const char* entradas[4];
    int i, volta;
    long long antes = 0, depois = 0;

    t_start(r);
    if (!media_codec_available(MEDIA_CODEC_VP9) || !media_codec_available(MEDIA_CODEC_AV1))
        T_SKIP(r, "precisa de encoder de VP9 e de AV1 para gerar as duas entradas");
    if (!media_codec_available(MEDIA_CODEC_H264))
        T_SKIP(r, "sem encoder de H.264 para a saida");

    gw_mkdir_p(PASTA);
    snprintf(ent_vp9, sizeof(ent_vp9), "%s/ent_vp9.webm", PASTA);
    snprintf(ent_av1, sizeof(ent_av1), "%s/ent_av1.webm", PASTA);
    T_ASSERT(r, gera_entrada(MEDIA_CODEC_VP9, "V_VP9", ent_vp9) == 0, "falhou ao gerar a entrada VP9");
    T_ASSERT(r, gera_entrada(MEDIA_CODEC_AV1, "V_AV1", ent_av1) == 0, "falhou ao gerar a entrada AV1");

    entradas[0] = ent_vp9; entradas[1] = ent_av1;
    entradas[2] = ent_av1; entradas[3] = ent_vp9;

    snprintf(pasta, sizeof(pasta), "%s/ent", PASTA);

    for (volta = 0; volta < 2; volta++)
    {
        for (i = 0; i < 4; i++)
        {
            Eventos ev;
            int larg = 0, alt = 0, quadros, rc;

            rc = roda(entradas[i], &PASSO, pasta, &ev);
            T_ASSERT(r, rc == 0 && !ev.erro && ev.done,
                     "volta %d, entrada %d (%s): rc=%d erro=%d done=%d",
                     volta + 1, i, i == 0 || i == 3 ? "VP9" : "AV1", rc, ev.erro, ev.done);

            quadros = decodifica_saida(pasta, &larg, &alt);
            T_ASSERT(r, quadros > 0,
                     "volta %d, entrada %d: a saida nao decodificou", volta + 1, i);
            T_ASSERT(r, larg == PASSO.Larg && alt == PASSO.Alt,
                     "volta %d, entrada %d: saida %dx%d, esperado %dx%d",
                     volta + 1, i, larg, alt, PASSO.Larg, PASSO.Alt);
        }
        if (volta == 0) antes = vivos(); else depois = vivos();
    }

    T_ASSERT(r, depois - antes <= 32,
             "trocar o decoder de entrada deixou %lld alocacoes vivas a mais (%lld -> %lld)",
             depois - antes, antes, depois);
}
