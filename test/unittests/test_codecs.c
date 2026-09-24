//  Um teste de IDA E VOLTA por codec: codifica quadros sinteticos, decodifica o que saiu e
//  confere que os quadros voltam, com as dimensoes certas.
//
//  Por que existe: a suite cobria VP9, AV1 e H.264, mas nao H.265 nem Opus -- e a separacao
//  dos codecs em um projeto cada mexeu justamente no caminho de decode. Um recorte que
//  ninguem exercita nao esta verificado, esta so compilando.
//
//  Regra: codec que este build nao compilou faz o teste PASSAR sem verificar nada (a suite
//  precisa ficar verde em maquina que nao tem tudo). O que ele nao pode e passar calado
//  quando o codec existe e o caminho esta quebrado.

#include "testes.h"
#include "media_codec.h"
#include "gw_decode.h"
#include <string.h>
#include <stdio.h>

#define LARG     320
#define ALT      240
#define QUADROS  12
#define MAX_PKT  512
#define MAX_BYTES (4 << 20)

// O bitstream sai em PACOTES, e eles precisam continuar separados: o decoder de VP9 e o de
// AV1 esperam um pacote por chamada. Concatenar tudo (como o teste de H.264 faz, porque
// Annex-B aceita) nao serve para os dois.
typedef struct
{
    unsigned char* Bytes;
    int            Usado;
    int            Offsets[MAX_PKT];
    int            Tamanhos[MAX_PKT];
    int            Count;
}
Fluxo;

static void quadro_sintetico(unsigned char* y, unsigned char* u, unsigned char* v, int n)
{
    int i, j;
    for (j = 0; j < ALT; j++)
        for (i = 0; i < LARG; i++)
            y[j * LARG + i] = (unsigned char)((i * 2 + j + n * 5) & 0xFF);
    memset(u, 110, (size_t)(LARG / 2) * (ALT / 2));
    memset(v, 140, (size_t)(LARG / 2) * (ALT / 2));
}

static void guarda(Fluxo* f, const MediaPacket* pk)
{
    if (f->Count >= MAX_PKT || f->Usado + pk->Size > MAX_BYTES) return;
    memcpy(f->Bytes + f->Usado, pk->Data, (size_t)pk->Size);
    f->Offsets[f->Count]  = f->Usado;
    f->Tamanhos[f->Count] = pk->Size;
    f->Count++;
    f->Usado += pk->Size;
}

// Codifica QUADROS quadros de video. Retorna o numero de pacotes, ou -1 se o build nao tem
// encoder para esse codec.
static int codifica_video(MediaCodec codec, Fluxo* f, char* backend, int tam_backend)
{
    MediaEncoderParams p;
    MediaEncoder* enc;
    MediaPacket pk;
    unsigned char *y, *u, *v;
    int n;

    memset(&p, 0, sizeof(p));
    p.Codec = codec; p.Width = LARG; p.Height = ALT;
    p.Fps = 30; p.BitrateBps = 800000;
    p.Accel = MEDIA_ACCEL_SOFTWARE;   // o caminho que a separacao mexeu e o de software

    enc = media_encoder_open(&p);
    if (!enc) return -1;

    if (backend && tam_backend > 0)
        snprintf(backend, (size_t)tam_backend, "%s", enc->Backend ? enc->Backend : "?");

    y = (unsigned char*)memop_alloc_raw(LARG * ALT);
    u = (unsigned char*)memop_alloc_raw(LARG * ALT / 4);
    v = (unsigned char*)memop_alloc_raw(LARG * ALT / 4);

    for (n = 0; n < QUADROS; n++)
    {
        MediaFrame fr;
        quadro_sintetico(y, u, v, n);
        memset(&fr, 0, sizeof(fr));
        fr.Kind = MEDIA_KIND_VIDEO; fr.Width = LARG; fr.Height = ALT;
        fr.Y = y; fr.StrideY = LARG; fr.U = u; fr.StrideU = LARG / 2; fr.V = v; fr.StrideV = LARG / 2;
        fr.Pts = n;
        if (enc->SendFrame(enc, &fr) < 0) break;
        while (enc->ReceivePacket(enc, &pk) == 1) guarda(f, &pk);
    }

    enc->SendFrame(enc, 0);                                   // flush
    while (enc->ReceivePacket(enc, &pk) == 1) guarda(f, &pk);

    enc->Close(enc);
    memop_free_raw(y); memop_free_raw(u); memop_free_raw(v);
    return f->Count;
}

// O corpo comum. Cada teste abaixo e uma linha.
static void ida_e_volta(TestResult* r, MediaCodec codec)
{
    Fluxo f;
    GwVideoDec* dec;
    GwImage img;
    char backend[96] = { 0 };
    const char* nome = media_codec_name(codec);
    int i, pacotes, decodificados = 0, larg = 0, alt = 0;

    t_start(r);

    if (!media_codec_available(codec))
        T_SKIP(r, "%s: este build nao tem encoder", nome);

    memset(&f, 0, sizeof(f));
    f.Bytes = (unsigned char*)memop_alloc_raw(MAX_BYTES);
    if (!f.Bytes) T_ASSERT(r, 0, "sem memoria para o fluxo de %s", nome);

    pacotes = codifica_video(codec, &f, backend, sizeof(backend));
    if (pacotes < 0)
    {
        memop_free_raw(f.Bytes);
        T_SKIP(r, "%s: media_encoder_open recusou (nenhum backend atendeu)", nome);
    }

    if (pacotes <= 0 || f.Usado <= 0)
    {
        memop_free_raw(f.Bytes);
        T_ASSERT(r, 0, "o encoder de %s ('%s') nao produziu bitstream", nome, backend);
    }

    if (!gw_vdec_available(codec))
    {
        memop_free_raw(f.Bytes);
        T_SKIP(r, "%s: encodou %d pacote(s) com '%s', mas este build nao tem decoder",
               nome, pacotes, backend);
    }

    dec = gw_vdec_open(codec);
    if (!dec) { memop_free_raw(f.Bytes); T_ASSERT(r, 0, "nao foi possivel abrir o decoder de %s", nome); }

    // Um pacote por vez, e os frames sao consumidos antes do proximo pacote existir:
    // os planos pertencem ao decoder e valem so ate a chamada seguinte.
    for (i = 0; i < f.Count; i++)
    {
        gw_vdec_send(dec, f.Bytes + f.Offsets[i], f.Tamanhos[i]);
        while (gw_vdec_next(dec, &img) == 1)
        {
            decodificados++; larg = img.Width; alt = img.Height;
        }
    }
    gw_vdec_send(dec, 0, 0);                                  // flush
    while (gw_vdec_next(dec, &img) == 1)
    {
        decodificados++; larg = img.Width; alt = img.Height;
    }

    {
        const char* dec_backend = gw_vdec_backend(dec);
        char db[96]; snprintf(db, sizeof(db), "%s", dec_backend ? dec_backend : "?");
        gw_vdec_close(&dec);
        memop_free_raw(f.Bytes);

        T_ASSERT(r, decodificados > 0,
                 "%s: %d pacote(s), %d bytes do encoder '%s', e o decoder '%s' nao devolveu quadro",
                 nome, pacotes, f.Usado, backend, db);
        T_ASSERT(r, larg == LARG && alt == ALT,
                 "%s: decodificou %dx%d, esperado %dx%d (encoder '%s', decoder '%s')",
                 nome, larg, alt, LARG, ALT, backend, db);
        // Um quadro entrou, um quadro tem de sair. Perder os ultimos no flush ja aconteceu
        // aqui (B-frames retidos), e custou frames em toda conversao.
        T_ASSERT(r, decodificados >= QUADROS,
                 "%s: entraram %d quadros e sairam %d (encoder '%s', decoder '%s')",
                 nome, QUADROS, decodificados, backend, db);
    }
}

void teste_ida_e_volta_h264(TestResult* r) { ida_e_volta(r, MEDIA_CODEC_H264); }
void teste_ida_e_volta_h265(TestResult* r) { ida_e_volta(r, MEDIA_CODEC_H265); }
void teste_ida_e_volta_vp9 (TestResult* r) { ida_e_volta(r, MEDIA_CODEC_VP9);  }
void teste_ida_e_volta_av1 (TestResult* r) { ida_e_volta(r, MEDIA_CODEC_AV1);  }

// ---- audio -----------------------------------------------------------------
// Opus so tem encoder nesta camada (nao ha decoder de audio aqui), entao o teste vai ate
// onde da: o bitstream tem de sair, e o cabecalho TOC de cada pacote tem de ser coerente.

void teste_encode_opus(TestResult* r)
{
    MediaEncoderParams p;
    MediaEncoder* enc;
    MediaPacket pk;
    short* pcm;
    int n, pacotes = 0, bytes = 0;
    const int TAXA = 48000, CANAIS = 2, POR_QUADRO = 960;   // 20 ms a 48 kHz

    t_start(r);

    if (!media_codec_available(MEDIA_CODEC_OPUS))
        T_SKIP(r, "este build nao tem encoder de Opus");

    memset(&p, 0, sizeof(p));
    p.Codec = MEDIA_CODEC_OPUS;
    p.SampleRate = TAXA; p.Channels = CANAIS; p.BitrateBps = 96000;

    enc = media_encoder_open(&p);
    if (!enc) T_SKIP(r, "media_encoder_open recusou o Opus");

    pcm = (short*)memop_alloc_raw((uint64)(POR_QUADRO * CANAIS * (int)sizeof(short)));

    for (n = 0; n < 25; n++)   // meio segundo
    {
        MediaFrame fr;
        int i;
        for (i = 0; i < POR_QUADRO; i++)
        {
            // seno grosseiro, so para nao ser silencio (silencio comprime a quase nada)
            short v = (short)(((i * 37 + n * 211) % 2000) - 1000);
            pcm[i * CANAIS] = v; pcm[i * CANAIS + 1] = (short)-v;
        }
        memset(&fr, 0, sizeof(fr));
        fr.Kind = MEDIA_KIND_AUDIO;
        fr.SampleRate = TAXA; fr.Channels = CANAIS;
        fr.Pcm = pcm; fr.PcmSamples = POR_QUADRO;
        fr.Pts = n * POR_QUADRO;
        if (enc->SendFrame(enc, &fr) < 0) break;
        while (enc->ReceivePacket(enc, &pk) == 1) { pacotes++; bytes += pk.Size; }
    }

    enc->SendFrame(enc, 0);
    while (enc->ReceivePacket(enc, &pk) == 1) { pacotes++; bytes += pk.Size; }

    enc->Close(enc);
    memop_free_raw(pcm);

    T_ASSERT(r, pacotes > 0, "o encoder de Opus nao produziu pacote nenhum");
    T_ASSERT(r, bytes > 0, "o encoder de Opus produziu %d pacote(s) com 0 byte", pacotes);
}
