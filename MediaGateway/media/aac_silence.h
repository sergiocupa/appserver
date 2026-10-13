//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Quadro AAC-LC de SILENCIO (1024 amostras), pronto, por configuracao de canais.
//
//  Para que: faixa de audio que entra ATRASADA na origem (edicao vazia no MP4 -- o audio
//  comeca, por exemplo, 0,5 s depois do video). Num formato de segmentos a faixa de audio
//  comecaria em 0,5 s, e o player abre onde as duas faixas ja tem dados: o comeco do video
//  nao aparecia. Preenchendo o intervalo com silencio, as duas faixas comecam juntas.
//
//  O quadro nao depende da taxa de amostragem, so do perfil (LC) e dos canais, e nao exige
//  encoder de AAC -- que o projeto nao tem. Os bytes de 1 a 5 canais sao os que os players
//  usam para tapar buracos de audio; cada um foi decodificado (50 quadros, 44,1 e 48 kHz)
//  conferindo 1024 amostras por quadro e pico zero.
//  5.1 (6 canais): o quadro que os players usam poe o canal de graves como um canal comum
//  (SCE de instancia 1), e o decoder avisa que teve de remapear. O daqui foi montado com os
//  mesmos elementos, trocando esse ultimo pelo elemento certo (LFE): SCE + CPE + CPE + LFE.
//  Decodifica como 5.1, sem aviso, a 32, 44,1, 48 e 96 kHz. Fora do que esta aqui (7.1,
//  HE-AAC, taxa explicita), nada e preenchido.

#pragma once
#include <stdint.h>

#define AAC_SILENCE_SAMPLES 1024

// asc = AudioSpecificConfig da faixa. Devolve o quadro e o tamanho em *size; NULL se o perfil
// nao e AAC-LC ou a configuracao de canais nao esta entre 1 e 6.
static inline const uint8_t* aac_silence_frame(const uint8_t* asc, int asc_len, int* size)
{
    static const uint8_t C1[] = { 0x00,0xc8,0x00,0x80,0x23,0x80 };
    static const uint8_t C2[] = { 0x21,0x00,0x49,0x90,0x02,0x19,0x00,0x23,0x80 };
    static const uint8_t C3[] = { 0x00,0xc8,0x00,0x80,0x20,0x84,0x01,0x26,0x40,0x08,0x64,0x00,0x8e };
    static const uint8_t C4[] = { 0x00,0xc8,0x00,0x80,0x20,0x84,0x01,0x26,0x40,0x08,0x64,0x00,0x80,0x2c,0x80,0x08,0x02,0x38 };
    static const uint8_t C5[] = { 0x00,0xc8,0x00,0x80,0x20,0x84,0x01,0x26,0x40,0x08,0x64,0x00,0x82,0x30,0x04,0x99,0x00,0x21,0x90,0x02,0x38 };
    static const uint8_t C6[] = { 0x00,0xc8,0x00,0x80,0x20,0x84,0x01,0x26,0x40,0x08,0x64,0x00,0x82,0x30,0x04,0x99,0x00,0x21,0x90,0x02,0x18,0x32,0x00,0x20,0x08,0xe0 };

    if (size) *size = 0;
    if (!asc || asc_len < 2) return 0;
    int perfil = asc[0] >> 3;                                  // audioObjectType: 2 = AAC-LC
    int taxa   = ((asc[0] & 0x07) << 1) | (asc[1] >> 7);       // 15 = taxa explicita (outro leiaute)
    int canais = (asc[1] >> 3) & 0x0F;
    if (perfil != 2 || taxa == 15) return 0;

    const uint8_t* q = 0; int n = 0;
    switch (canais)
    {
        case 1: q = C1; n = (int)sizeof(C1); break;
        case 2: q = C2; n = (int)sizeof(C2); break;
        case 3: q = C3; n = (int)sizeof(C3); break;
        case 4: q = C4; n = (int)sizeof(C4); break;
        case 5: q = C5; n = (int)sizeof(C5); break;
        case 6: q = C6; n = (int)sizeof(C6); break;
        default: return 0;
    }
    if (size) *size = n;
    return q;
}

// Quantos quadros de silencio cabem antes de uma faixa que comeca em 'inicio' (amostras), e
// onde o primeiro comeca: os quadros terminam EXATAMENTE em 'inicio', para o audio de verdade
// manter o tempo da fonte. Sobra no comeco menos de um quadro (23 ms a 44,1 kHz).
static inline int64_t aac_silence_count(int64_t inicio, int64_t* primeiro)
{
    int64_t n = inicio > 0 ? inicio / AAC_SILENCE_SAMPLES : 0;
    if (primeiro) *primeiro = inicio - n * AAC_SILENCE_SAMPLES;
    return n;
}
