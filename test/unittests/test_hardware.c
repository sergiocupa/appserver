//  Testes que dependem de HARDWARE (encoder por hardware e decode por GPU), em C.
//
//  Regra desta suite: eles NAO podem falhar numa maquina sem o equipamento. O que a maquina
//  nao tem, o teste anota e passa -- caso contrario a suite ficaria vermelha em qualquer
//  lugar que nao fosse esta estacao, e uma suite que vive vermelha nao e lida.
//  O que eles garantem, onde ha hardware: que a cascata (hardware -> GPU -> SIMD -> threads)
//  entrega um backend utilizavel e que o que ele produz volta a ser decodificado.

#include "testes.h"
#include "media_codec.h"
#include "enc_select.h"
#include "gw_decode.h"
#include <string.h>
#include <stdio.h>

#define LARG 320
#define ALT  240
#define QUADROS 12

static void quadro_sintetico(unsigned char* y, unsigned char* u, unsigned char* v, int n)
{
    int i, j;
    for (j = 0; j < ALT; j++)
        for (i = 0; i < LARG; i++)
            y[j * LARG + i] = (unsigned char)((i * 2 + j + n * 5) & 0xFF);
    memset(u, 110, (size_t)(LARG / 2) * (ALT / 2));
    memset(v, 140, (size_t)(LARG / 2) * (ALT / 2));
}

// Codifica QUADROS quadros em H.264 com a aceleracao pedida. Devolve o numero de pacotes,
// ou -1 se nao houver encoder para aquele degrau.
static int codifica_h264(int accel, char* backend, int tam_backend, unsigned char* saida, int* tam_saida)
{
    MediaEncoderParams p;
    MediaEncoder* enc;
    unsigned char *y, *u, *v;
    int n, pacotes = 0, usado = 0;

    memset(&p, 0, sizeof(p));
    p.Codec = MEDIA_CODEC_H264; p.Width = LARG; p.Height = ALT;
    p.Fps = 30; p.BitrateBps = 800000; p.Accel = accel;

    enc = media_encoder_open(&p);
    if (!enc) return -1;

    if (backend && tam_backend > 0)
        snprintf(backend, (size_t)tam_backend, "%s", enc->Backend ? enc->Backend : "?");

    y = (unsigned char*)memop_alloc_raw(LARG * ALT);
    u = (unsigned char*)memop_alloc_raw(LARG * ALT / 4);
    v = (unsigned char*)memop_alloc_raw(LARG * ALT / 4);

    for (n = 0; n < QUADROS; n++)
    {
        MediaFrame fr; MediaPacket pk;
        quadro_sintetico(y, u, v, n);
        memset(&fr, 0, sizeof(fr));
        fr.Kind = MEDIA_KIND_VIDEO; fr.Width = LARG; fr.Height = ALT;
        fr.Y = y; fr.StrideY = LARG; fr.U = u; fr.StrideU = LARG / 2; fr.V = v; fr.StrideV = LARG / 2;
        fr.Pts = n;
        if (enc->SendFrame(enc, &fr) < 0) break;
        while (enc->ReceivePacket(enc, &pk) == 1)
        {
            if (saida && tam_saida && usado + pk.Size <= *tam_saida)
            { memcpy(saida + usado, pk.Data, (size_t)pk.Size); usado += pk.Size; }
            pacotes++;
        }
    }

    {
        MediaPacket pk;
        enc->SendFrame(enc, 0);
        while (enc->ReceivePacket(enc, &pk) == 1)
        {
            if (saida && tam_saida && usado + pk.Size <= *tam_saida)
            { memcpy(saida + usado, pk.Data, (size_t)pk.Size); usado += pk.Size; }
            pacotes++;
        }
    }

    enc->Close(enc);
    memop_free_raw(y); memop_free_raw(u); memop_free_raw(v);
    if (tam_saida) *tam_saida = usado;
    return pacotes;
}

void teste_encoder_hardware_h264(TestResult* r)
{
    EncBackendInfo lista[8];
    char backend[96] = { 0 };
    int n, i, tem_hw = 0, pacotes, tam = 0;

    t_start(r);

    n = media_encoder_list(MEDIA_CODEC_H264, lista, 8);
    for (i = 0; i < n && i < 8; i++)
        if (lista[i].Tier == ENC_TIER_HARDWARE && lista[i].Available) tem_hw = 1;

    if (!tem_hw) T_SKIP(r, "esta maquina nao tem encoder de H.264 por hardware");

    pacotes = codifica_h264(MEDIA_ACCEL_HARDWARE, backend, sizeof(backend), 0, &tam);
    T_ASSERT(r, pacotes > 0,
             "a lista anuncia encoder por hardware, mas ele nao produziu pacote (backend '%s')", backend);
}

void teste_decode_do_que_o_encoder_produziu(TestResult* r)
{
    static unsigned char fluxo[1 << 20];
    GwVideoDec* dec;
    GwImage img;
    char backend[96] = { 0 };
    int tam = (int)sizeof(fluxo), pacotes, decodificados = 0;
    int ultima_larg = 0, ultima_alt = 0;

    t_start(r);

    // AUTO: pega o melhor degrau que existir (hardware, GPU, SIMD ou threads).
    pacotes = codifica_h264(MEDIA_ACCEL_AUTO, backend, sizeof(backend), fluxo, &tam);
    if (pacotes < 0) T_SKIP(r, "este build nao tem encoder de H.264");

    T_ASSERT(r, pacotes > 0 && tam > 0, "o encoder '%s' nao produziu bitstream", backend);

    dec = gw_vdec_open(MEDIA_CODEC_H264);
    T_ASSERT(r, dec != 0, "nao foi possivel abrir o decoder de H.264");

    // A largura/altura tem de ser guardadas DENTRO do laco: a ultima chamada devolve 0 e
    // nao toca na imagem, entao ler depois do laco pega lixo (foi o que este teste fez na
    // primeira versao, e acusou 0x0 com o decode funcionando).
    gw_vdec_send(dec, fluxo, tam);
    while (gw_vdec_next(dec, &img) == 1) { decodificados++; ultima_larg = img.Width; ultima_alt = img.Height; }
    gw_vdec_send(dec, 0, 0);
    while (gw_vdec_next(dec, &img) == 1) { decodificados++; ultima_larg = img.Width; ultima_alt = img.Height; }

    {
        const char* dec_backend = gw_vdec_backend(dec);
        int larg = ultima_larg, alt = ultima_alt;
        gw_vdec_close(&dec);

        T_ASSERT(r, decodificados > 0,
                 "nada foi decodificado (encoder '%s', decoder '%s', %d bytes)",
                 backend, dec_backend ? dec_backend : "?", tam);
        T_ASSERT(r, larg == LARG && alt == ALT,
                 "resolucao decodificada %dx%d, esperava %dx%d", larg, alt, LARG, ALT);
    }
}
