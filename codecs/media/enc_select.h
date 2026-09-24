//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Selecao de backend de encode POR RECURSO DISPONIVEL.
//
//  Ordem de preferencia (a mais performatica primeiro):
//     1. HARDWARE  bloco de encode fixo (NVENC/QSV/AMF/Qualcomm via Media Foundation)
//     2. GPU       encode por shader/compute -- nenhuma lib da pilha faz isso hoje; o
//                  degrau existe para quando entrar uma (o NVENC NAO e este degrau:
//                  e bloco fixo dentro da GPU, entao conta como HARDWARE)
//     3. SIMD      encoder de software com assembly/intrinsics compilado E a CPU tendo
//                  o conjunto de instrucoes
//     4. THREADS   encoder de software em C puro, so com paralelismo
//
//  Cada backend DECLARA onde roda (Windows/Linux, x64/ARM64) e e SONDADO em runtime.
//  media_encoder_open() tenta na ordem acima e cai para o proximo quando um degrau nao
//  existe, nao suporta o codec, ou falha ao abrir (ex.: limite de sessoes do NVENC).

#pragma once
#include "media_codec.h"

typedef enum
{
    ENC_TIER_HARDWARE = 0,
    ENC_TIER_GPU      = 1,
    ENC_TIER_SIMD     = 2,
    ENC_TIER_THREADS  = 3
} EncTier;

// Mascaras de plataforma/arquitetura que um backend declara suportar.
#define ENC_OS_WINDOWS   0x1
#define ENC_OS_LINUX     0x2
#define ENC_ARCH_X64     0x1
#define ENC_ARCH_ARM64   0x2

typedef struct
{
    const char* Name;        // identificador estavel: "mf-hw", "openh264", "x265", ...
    char        Detail[96];  // o que foi achado (ex.: "NVIDIA H.264 Encoder MFT", "AVX2")
    MediaCodec  Codec;
    EncTier     Tier;        // degrau EFETIVO nesta maquina (SIMD vira THREADS se a CPU nao tiver a ISA)
    unsigned    Os, Arch;    // onde o backend roda (declarado)
    int         Available;   // 1 = pode abrir aqui e agora
    const char* Why;         // motivo quando Available == 0
} EncBackendInfo;

// Lista TODOS os backends conhecidos, na ORDEM EM QUE SERIAM TENTADOS. Os que nao
// valem aqui (outra plataforma, nao compilado, sem hardware) vem com Available=0 e o
// motivo em Why -- a lista serve tambem de matriz de suporte Windows/Linux/x64/ARM64.
// Preenche ate 'max'; retorna quantos existem.
// codec == MEDIA_CODEC_NONE lista todos.
int media_encoder_list(MediaCodec codec, EncBackendInfo* out, int max);

// Mesmo formato, para os DECODERS. Hoje so ha degraus de software (ver enc_select.c).
int media_decoder_list(MediaCodec codec, EncBackendInfo* out, int max);

const char* enc_tier_name(EncTier t);

// Plataforma deste binario, nas mesmas mascaras.
unsigned enc_current_os(void);
unsigned enc_current_arch(void);

// ---- deteccao de CPU -------------------------------------------------------
// Fica na camada de codecs (e nao na xplatbase) porque so quem decide SIMD precisa dela;
// se outro modulo passar a precisar, e candidata natural a subir para a xplatbase.
typedef struct
{
    int sse2, ssse3, sse41, avx, avx2, avx512;   // x86-64 (avx*: ja checado o suporte do SO)
    int neon;                                     // ARM64 (sempre presente no AArch64)
} CpuFeatures;

const CpuFeatures* cpu_features(void);
