//  Medicao de encode H.264 com video REAL.
//
//  Existe para responder uma pergunta so: ligar o threading do openh264 acelera?
//  O threading estava desligado no codec_h264.c com um comentario culpando um crash no
//  h264_recv -- mas a causa raiz daquele crash era outra (ABI do openh264, campo rPsnr da
//  2.6.0 contra a DLL 2.5.1), e ja foi corrigida. Entao a premissa precisa ser remedida.
//
//  O que e cronometrado: SOMENTE as chamadas de encode. O decode do arquivo de entrada
//  acontece no mesmo laco, mas fica fora do relogio -- senao o custo do decode, que nao
//  muda entre as rodadas, diluiria a diferenca que se quer enxergar.
//
//  Nao e teste de regressao: se o arquivo nao estiver na maquina, ele se marca como PULADO.

#include "testes.h"
#include "media_codec.h"
#include "media_source.h"
#include "gw_decode.h"
#include "memory_pool.h"
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
  #include <windows.h>
  static double agora_ms(void)
  {
      LARGE_INTEGER f, t;
      QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t);
      return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
  }
#else
  #include <time.h>
  static double agora_ms(void)
  {
      struct timespec ts;
      clock_gettime(CLOCK_MONOTONIC, &ts);
      return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
  }
#endif

#define ENTRADA   "E:\\videos\\8293503-hd_1920_1080_30fps.mp4"
#define MAX_QUADROS 150          // ~10 s a 30 fps: amostra grande o bastante para estabilizar

typedef struct
{
    MediaCodec Codec;
    int    Quadros;
    int    Pacotes;
    double MsEncode;             // so o encode
    long long Bytes;             // bitstream produzido (para ver se a compressao mudou)
    char   Backend[96];
    unsigned char* Fluxo;        // copia do bitstream, para conferir que decodifica
    int    FluxoUsado;
    int    FluxoCap;
    int    Off[512];             // fronteira de cada pacote: VP9/AV1 decodificam um a um
    int    Tam[512];
    int    NPkt;
} Medida;

static void guarda_fluxo(Medida* m, const MediaPacket* pk)
{
    if (!m->Fluxo) { m->FluxoCap = 32 << 20; m->Fluxo = (unsigned char*)memop_alloc_raw((uint64)m->FluxoCap); }
    if (!m->Fluxo || m->FluxoUsado + pk->Size > m->FluxoCap) return;
    if (m->NPkt >= 512) return;
    memcpy(m->Fluxo + m->FluxoUsado, pk->Data, (size_t)pk->Size);
    m->Off[m->NPkt] = m->FluxoUsado;
    m->Tam[m->NPkt] = pk->Size;
    m->NPkt++;
    m->FluxoUsado += pk->Size;
}

// Decodifica o que o encoder produziu. E aqui que o crash historico aparecia: com varias
// slices ha mais NALs por camada, e era exatamente o que o h264_recv lia errado.
static int decodifica_de_volta(const Medida* m, int* larg, int* alt)
{
    GwVideoDec* d;
    GwImage img;
    int n = 0, i;

    *larg = 0; *alt = 0;
    if (!m->Fluxo || m->NPkt <= 0) return -1;

    d = gw_vdec_open(m->Codec);
    if (!d) return -1;

    // UM pacote por vez. O H.264 toleraria o fluxo inteiro de uma vez (Annex-B se
    // autodelimita), mas VP9 e AV1 nao: cada chamada tem de receber um quadro.
    for (i = 0; i < m->NPkt; i++)
    {
        if (gw_vdec_send(d, m->Fluxo + m->Off[i], m->Tam[i]) < 0) continue;
        while (gw_vdec_next(d, &img) == 1) { n++; *larg = img.Width; *alt = img.Height; }
    }
    gw_vdec_send(d, 0, 0);
    while (gw_vdec_next(d, &img) == 1) { n++; *larg = img.Width; *alt = img.Height; }

    gw_vdec_close(&d);
    return n;
}

// Decodifica a entrada e encoda cada quadro, cronometrando so o encode.
static int mede(MediaCodec codec, int threads, int larg, int alt, int fps, int limite, Medida* m)
{
    MediaSource* src;
    MediaStreamInfo st[4];
    GwVideoDec* dec;
    GwPacket pkt;
    GwImage img;
    MediaEncoderParams p;
    MediaEncoder* enc;
    int nst = 0;

    memset(m, 0, sizeof(*m));
    m->Codec = codec;

    src = source_file_open(ENTRADA);
    if (!src) return -1;
    src->Info(src, st, 4, &nst);
    if (nst <= 0) { src->Close(src); return -1; }

    dec = gw_vdec_open(st[0].Codec);
    if (!dec) { src->Close(src); return -1; }

    memset(&p, 0, sizeof(p));
    p.Codec = codec;
    p.Width = larg; p.Height = alt; p.Fps = fps;
    p.BitrateBps = 6000000;
    p.Threads = threads;
    p.Accel = MEDIA_ACCEL_SOFTWARE;   // a pergunta e sobre o encoder de software

    enc = media_encoder_open(&p);
    if (!enc) { gw_vdec_close(&dec); src->Close(src); return -1; }
    snprintf(m->Backend, sizeof(m->Backend), "%s", enc->Backend ? enc->Backend : "?");

    while (m->Quadros < limite && src->Read(src, &pkt) == 1)
    {
        if (gw_vdec_send(dec, pkt.Data, pkt.Size) < 0) continue;
        while (m->Quadros < limite && gw_vdec_next(dec, &img) == 1)
        {
            MediaFrame fr; MediaPacket out;
            double t0;

            if (img.Width != larg || img.Height != alt) continue;   // so o tamanho pedido

            memset(&fr, 0, sizeof(fr));
            fr.Kind = MEDIA_KIND_VIDEO; fr.Width = larg; fr.Height = alt;
            fr.Y = img.Planes[0]; fr.StrideY = img.Strides[0];
            fr.U = img.Planes[1]; fr.StrideU = img.Strides[1];
            fr.V = img.Planes[2]; fr.StrideV = img.Strides[2];
            fr.Pts = m->Quadros;

            t0 = agora_ms();
            if (enc->SendFrame(enc, &fr) == 0)
                while (enc->ReceivePacket(enc, &out) == 1) { m->Pacotes++; m->Bytes += out.Size; guarda_fluxo(m, &out); }
            m->MsEncode += agora_ms() - t0;

            m->Quadros++;
        }
    }

    {
        MediaPacket out;
        double t0 = agora_ms();
        enc->SendFrame(enc, 0);
        while (enc->ReceivePacket(enc, &out) == 1) { m->Pacotes++; m->Bytes += out.Size; guarda_fluxo(m, &out); }
        m->MsEncode += agora_ms() - t0;
    }

    enc->Close(enc);
    gw_vdec_close(&dec);
    src->Close(src);
    return m->Quadros > 0 ? 0 : -1;
}

static void relata(const char* rotulo, const Medida* m)
{
    double fps = m->MsEncode > 0 ? (m->Quadros * 1000.0 / m->MsEncode) : 0;
    printf("  %-14s %4d quadros | encode %8.1f ms | %6.1f fps | %6.2f MB\n",
           rotulo, m->Quadros, m->MsEncode, fps, (double)m->Bytes / (1024.0 * 1024.0));
    fflush(stdout);
}

void teste_bench_h264_threads(TestResult* r)
{
    // 1 vem primeiro porque e a referencia dos ganhos. 0 pede o PADRAO do proprio codec,
    // que hoje e metade dos nucleos fisicos -- esta na grade para provar que o padrao
    // realmente entra em vigor, e nao so quando alguem passa um numero.
    static const int GRADE[] = { 1, 0, 2, 4, 6, 8 };
    const int N = (int)(sizeof(GRADE) / sizeof(GRADE[0]));
    Medida m[8];
    double base = 0;
    FILE* f;
    int i;

    t_start(r);

    f = fopen(ENTRADA, "rb");
    if (!f) T_SKIP(r, "video de referencia nao esta nesta maquina: %s", ENTRADA);
    fclose(f);

    if (!media_codec_available(MEDIA_CODEC_H264))
        T_SKIP(r, "este build nao tem encoder de H.264");

    printf("\n=== encode H.264 1920x1080, video real, %d quadros ===\n", MAX_QUADROS);
    printf("  %-9s %10s %9s %8s %8s %9s  %s\n",
           "threads", "encode ms", "fps", "ganho", "por thr", "MB", "decode de volta");
    fflush(stdout);

    for (i = 0; i < N; i++)
    {
        char rotulo[16];
        int larg = 0, alt = 0, dec, efetivo;
        double ganho, porthr;

        if (mede(MEDIA_CODEC_H264, GRADE[i], 1920, 1080, 30, MAX_QUADROS, &m[i]) != 0)
            T_ASSERT(r, 0, "falhou ao medir com %d thread(s)", GRADE[i]);

        if (GRADE[i] == 1) base = m[i].MsEncode;

        efetivo = GRADE[i] > 0 ? GRADE[i] : 1;   // para o "por thread" do default: ver nota
        ganho   = (base > 0 && m[i].MsEncode > 0) ? base / m[i].MsEncode : 0;
        porthr  = ganho / (double)efetivo;

        if (GRADE[i] == 0) snprintf(rotulo, sizeof(rotulo), "default");
        else               snprintf(rotulo, sizeof(rotulo), "%d", GRADE[i]);

        dec = decodifica_de_volta(&m[i], &larg, &alt);

        printf("  %-9s %10.1f %9.1f %7.2fx %7.2fx %9.2f  %d quadros %dx%d\n",
               rotulo, m[i].MsEncode, m[i].Quadros * 1000.0 / m[i].MsEncode,
               ganho, GRADE[i] > 0 ? porthr : 0.0,
               (double)m[i].Bytes / (1024.0 * 1024.0), dec, larg, alt);
        fflush(stdout);

        // cada contagem de slices produz um bitstream diferente: todos tem de decodificar
        T_ASSERT(r, dec > 0, "a saida com '%s' NAO decodificou", rotulo);
        T_ASSERT(r, larg == 1920 && alt == 1080,
                 "com '%s' a saida decodificou em %dx%d", rotulo, larg, alt);
        T_ASSERT(r, m[i].Quadros == m[0].Quadros,
                 "com '%s' processou %d quadros, esperado %d",
                 rotulo, m[i].Quadros, m[0].Quadros);
    }

    printf("\n");
    fflush(stdout);
    for (i = 0; i < N; i++) if (m[i].Fluxo) memop_free_raw(m[i].Fluxo);

    // O padrao tem de estar valendo: a rodada "default" nao pode ficar no ritmo de 1 thread.
    // Sem isso, um erro que desligasse o padrao passaria despercebido -- que e exatamente
    // o estado em que este codec ficou por meses.
    T_ASSERT(r, m[1].MsEncode < m[0].MsEncode * 0.8,
             "o padrao nao acelerou: default %.0f ms contra %.0f ms de 1 thread",
             m[1].MsEncode, m[0].MsEncode);
}


// ============================================================================
//  Comparacao ANTES x DEPOIS da calibracao de nucleos, para TODOS os codecs.
//
//  "antes" e o que cada codec fazia quando o chamador nao passava Threads, no codigo
//  anterior a calibracao:
//     H.264  threading DESLIGADO            -> equivale a pedir 1
//     VP9    g_threads e tile columns = 4   -> equivale a pedir 4
//     H.265  default do preset do x265      -> nao foi alterado
//     AV1    default do SVT-AV1             -> nao foi alterado
//  "depois" e sempre Threads = 0, ou seja: deixa a regra decidir.
//
//  Para H.265 e AV1 as duas colunas sao a MESMA configuracao de proposito -- eles nao
//  foram calibrados, e a coluna serve justamente para mostrar isso, em vez de sugerir
//  um ganho que nao existe.
// ============================================================================

#define BENCH_QUADROS_CODEC 90     // 3 s a 30 fps: o AV1 a 1080p e lento, e sao 8 rodadas

typedef struct { MediaCodec Codec; const char* Nome; int Antes; int Calibrado; } AlvoBench;

void teste_bench_todos_codecs(TestResult* r)
{
    static const AlvoBench ALVOS[] = {
        // "Antes" e o valor que o codec usava antes da calibracao, passado como pedido
        // explicito. H.264 (single-slice) e VP9 (4 fixo) sao reproduziveis assim.
        //
        // H.265 e AV1 NAO sao: o antigo "Threads = 0" nao setava nada -- deixava o pool do
        // preset do x265 e o SVT se dimensionar sozinho --, e hoje 0 e justamente o que
        // aciona a regra. O 1 abaixo e uma BASE de comparacao (paralelismo minimo), nao o
        // comportamento antigo; por isso os dois entram marcados.
        { MEDIA_CODEC_H264, "H.264", 1, 1 },
        { MEDIA_CODEC_VP9,  "VP9",   4, 1 },
        { MEDIA_CODEC_H265, "H.265", 1, 0 },
        { MEDIA_CODEC_AV1,  "AV1",   1, 0 },
    };
    const int N = (int)(sizeof(ALVOS) / sizeof(ALVOS[0]));
    FILE* f;
    int i;

    t_start(r);

    f = fopen(ENTRADA, "rb");
    if (!f) T_SKIP(r, "video de referencia nao esta nesta maquina: %s", ENTRADA);
    fclose(f);

    printf("\n=== ANTES x DEPOIS da calibracao de nucleos ===\n");
    printf("    video real 1920x1080, %d quadros, so o encode cronometrado\n\n",
           BENCH_QUADROS_CODEC);
    printf("  %-7s %12s %12s %8s   %s\n", "codec", "antes (ms)", "depois (ms)", "ganho", "decode de volta");
    fflush(stdout);

    for (i = 0; i < N; i++)
    {
        Medida antes, depois;
        int la = 0, aa = 0, ld = 0, ad = 0, da, dd;
        double ganho;

        if (!media_codec_available(ALVOS[i].Codec))
        {
            printf("  %-7s %12s %12s %8s   (encoder nao compilado)\n", ALVOS[i].Nome, "-", "-", "-");
            fflush(stdout);
            continue;
        }

        printf("  %-7s  medindo 'antes'...", ALVOS[i].Nome); fflush(stdout);
        if (mede(ALVOS[i].Codec, ALVOS[i].Antes, 1920, 1080, 30, BENCH_QUADROS_CODEC, &antes) != 0)
        { printf(" FALHOU\n"); fflush(stdout); T_ASSERT(r, 0, "%s: falhou a rodada 'antes'", ALVOS[i].Nome); }

        printf(" %.0f ms | medindo 'depois'...", antes.MsEncode); fflush(stdout);
        if (mede(ALVOS[i].Codec, 0, 1920, 1080, 30, BENCH_QUADROS_CODEC, &depois) != 0)
        { printf(" FALHOU\n"); fflush(stdout); T_ASSERT(r, 0, "%s: falhou a rodada 'depois'", ALVOS[i].Nome); }
        printf(" %.0f ms\n", depois.MsEncode); fflush(stdout);

        da = decodifica_de_volta(&antes,  &la, &aa);
        dd = decodifica_de_volta(&depois, &ld, &ad);
        ganho = depois.MsEncode > 0 ? antes.MsEncode / depois.MsEncode : 0;

        printf("  %-7s %12.1f %12.1f %7.2fx   %d / %d quadros%s\n\n",
               ALVOS[i].Nome, antes.MsEncode, depois.MsEncode, ganho, da, dd,
               ALVOS[i].Calibrado ? ""
                                  : "   <- coluna antes = paralelismo minimo, nao o default antigo");
        fflush(stdout);

        if (antes.Fluxo)  memop_free_raw(antes.Fluxo);
        if (depois.Fluxo) memop_free_raw(depois.Fluxo);

        // o ganho e o resultado, nao a afirmacao: o que se exige e que as duas rodadas
        // tenham processado o mesmo material e que a saida continue utilizavel
        T_ASSERT(r, antes.Quadros == depois.Quadros,
                 "%s: rodadas com quantidades diferentes (%d vs %d)",
                 ALVOS[i].Nome, antes.Quadros, depois.Quadros);
        T_ASSERT(r, depois.Pacotes > 0, "%s: a rodada 'depois' nao produziu bitstream", ALVOS[i].Nome);
    }

    printf("\n");
    fflush(stdout);
}


// ============================================================================
//  Varredura de paralelismo, codec por codec.
//
//  Cada codec paraleliza de um jeito DIFERENTE, e o parametro Threads significa coisas
//  diferentes em cada um. Por isso a grade nao e a mesma:
//
//    H.264 (openh264)  numero de SLICES horizontais. Faixas de linhas de macrobloco.
//    VP9   (libvpx)    g_threads, com ROW_MT ligado; as colunas de tile saem da largura
//                      (>= 256 px por coluna, potencia de 2 -- 1080p comporta 4).
//    H.265 (x265)      tamanho do pool de threads, com WPP (frentes de onda em linhas de
//                      CTU). frame-parallel continua DESLIGADO no codigo -- medir ele
//                      exigiria alterar o encoder, e esta varredura nao altera nada.
//    AV1   (SVT-AV1)   level_of_parallelism, que e um NIVEL de 1 a 6, nao uma contagem
//                      de threads. Valores acima de 6 sao limitados pelo proprio codec.
//
//  Cada ponto roda DUAS vezes e vale o MENOR tempo. A bancada tem variacao de ate 16%
//  entre execucoes identicas -- medido --, e o menor tempo e o menos contaminado por
//  outra coisa rodando na maquina. Com uma rodada so, a ordem entre pontos vizinhos
//  inverte de execucao para execucao, como ja aconteceu aqui.
// ============================================================================

#define VARRE_QUADROS  60     // 2 s a 30 fps; sao ~48 rodadas de encode no total
#define VARRE_REPS      2

typedef struct
{
    MediaCodec  Codec;
    const char* Nome;
    const char* Knob;         // o que Threads significa NESTE codec
    int         Grade[16];
    int         N;
} Varredura;

void teste_bench_varredura(TestResult* r)
{
    static const Varredura ALVOS[] = {
        { MEDIA_CODEC_H264, "H.264", "slices horizontais",
          { 1, 2, 3, 4, 6, 8 }, 6 },
        // O VP9 leva a grade mais longa: foi o unico que nao saturou na faixa curta --
        // so parou de render em 16, que e 1 x nucleos LOGICOS nesta maquina.
        { MEDIA_CODEC_VP9,  "VP9",   "g_threads (ROW_MT + tiles)",
          { 4, 8, 12, 16, 20, 24, 32 }, 7 },
        { MEDIA_CODEC_H265, "H.265", "pool de threads (WPP)",
          { 1, 2, 4, 6, 8, 12 }, 6 },
        { MEDIA_CODEC_AV1,  "AV1",   "level_of_parallelism (1..6)",
          { 1, 2, 3, 4, 5, 6 }, 6 },
    };
    const int NA = (int)(sizeof(ALVOS) / sizeof(ALVOS[0]));
    FILE* f;
    int a;

    t_start(r);

    f = fopen(ENTRADA, "rb");
    if (!f) T_SKIP(r, "video de referencia nao esta nesta maquina: %s", ENTRADA);
    fclose(f);

    printf("\n########## VARREDURA DE PARALELISMO ##########\n");
    printf("video real 1920x1080, %d quadros por ponto, %d repeticoes (vale o menor tempo)\n",
           VARRE_QUADROS, VARRE_REPS);
    printf("so o encode e cronometrado; toda saida e decodificada de volta\n");
    fflush(stdout);

    for (a = 0; a < NA; a++)
    {
        double base = 0;
        int i;

        if (!media_codec_available(ALVOS[a].Codec))
        {
            printf("\n=== %s: encoder nao compilado neste build ===\n", ALVOS[a].Nome);
            fflush(stdout);
            continue;
        }

        printf("\n=== %s  --  Threads = %s ===\n", ALVOS[a].Nome, ALVOS[a].Knob);
        printf("  %-8s %11s %9s %8s %8s %9s  %s\n",
               "valor", "encode ms", "fps", "ganho", "por unid", "MB", "decode");
        fflush(stdout);

        for (i = 0; i < ALVOS[a].N; i++)
        {
            Medida melhor; int rep, larg = 0, alt = 0, dec = 0;
            double ganho, porunid;

            memset(&melhor, 0, sizeof(melhor));

            printf("  %-8d medindo", ALVOS[a].Grade[i]); fflush(stdout);
            for (rep = 0; rep < VARRE_REPS; rep++)
            {
                Medida m;
                if (mede(ALVOS[a].Codec, ALVOS[a].Grade[i], 1920, 1080, 30, VARRE_QUADROS, &m) != 0)
                { printf(" FALHOU\n"); fflush(stdout); T_ASSERT(r, 0, "%s: falhou em %d", ALVOS[a].Nome, ALVOS[a].Grade[i]); }
                printf(" %.0f", m.MsEncode); fflush(stdout);

                if (melhor.Quadros == 0 || m.MsEncode < melhor.MsEncode)
                {
                    if (melhor.Fluxo) memop_free_raw(melhor.Fluxo);
                    melhor = m;
                }
                else if (m.Fluxo) memop_free_raw(m.Fluxo);
            }

            dec = decodifica_de_volta(&melhor, &larg, &alt);
            if (i == 0) base = melhor.MsEncode;
            ganho   = melhor.MsEncode > 0 ? base / melhor.MsEncode : 0;
            porunid = ganho / (double)ALVOS[a].Grade[i];

            printf("\r  %-8d %11.1f %9.1f %7.2fx %7.2fx %9.2f  %d/%d %dx%d      \n",
                   ALVOS[a].Grade[i], melhor.MsEncode,
                   melhor.Quadros * 1000.0 / melhor.MsEncode, ganho, porunid,
                   (double)melhor.Bytes / (1024.0 * 1024.0), dec, melhor.Quadros, larg, alt);
            fflush(stdout);

            T_ASSERT(r, dec > 0, "%s em %d: a saida nao decodificou", ALVOS[a].Nome, ALVOS[a].Grade[i]);
            if (melhor.Fluxo) memop_free_raw(melhor.Fluxo);
        }
    }

    printf("\n########## fim da varredura ##########\n\n");
    fflush(stdout);
}
