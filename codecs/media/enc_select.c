//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  A cascata de selecao. Ver enc_select.h.
//
//  O que existe neste build NAO esta mais escrito aqui: vem dos DESCRITORES que cada
//  projeto de codec exporta (codec_plugin.h). Este arquivo so DECIDE -- percorre os
//  backends na ordem de tentativa (hardware antes de software), calcula o degrau efetivo
//  nesta maquina e abre o primeiro que atender.
//
//  Antes, a tabela daqui carregava h264_encoder_open, x265, svt-av1, a faixa de SIMD de
//  cada lib... Codec novo mexia neste arquivo. Agora nao mexe.

#include "enc_select.h"
#include "codec_plugin.h"
#include <string.h>
#include <stdio.h>


static const char* isa_name(int r)
{
    switch (r)
    {
        case ISA_SSE2:   return "SSE2";
        case ISA_SSSE3:  return "SSSE3";
        case ISA_SSE41:  return "SSE4.1";
        case ISA_AVX:    return "AVX";
        case ISA_AVX2:   return "AVX2";
        case ISA_AVX512: return "AVX-512";
        default:         return "?";
    }
}

static int cpu_isa_rank(void)
{
    const CpuFeatures* f = cpu_features();
    if (f->avx512) return ISA_AVX512;
    if (f->avx2)   return ISA_AVX2;
    if (f->avx)    return ISA_AVX;
    if (f->sse41)  return ISA_SSE41;
    if (f->ssse3)  return ISA_SSSE3;
    if (f->sse2)   return ISA_SSE2;
    return ISA_NONE;
}

// Degrau efetivo de um backend de software NESTA maquina.
static EncTier sw_tier(int x64_min, int x64_max, int arm64_neon, char* detail, int size)
{
    if (enc_current_arch() == ENC_ARCH_ARM64)
    {
        if (arm64_neon && cpu_features()->neon) { snprintf(detail, size, "NEON"); return ENC_TIER_SIMD; }
        snprintf(detail, size, "C puro");
        return ENC_TIER_THREADS;
    }

    int cpu = cpu_isa_rank();
    if (x64_min != ISA_NONE && cpu >= x64_min)
    {
        int use = cpu < x64_max ? cpu : x64_max;   // o que a CPU tem, limitado ao que a lib usa
        snprintf(detail, size, "%s", isa_name(use));
        return ENC_TIER_SIMD;
    }
    if (x64_min != ISA_NONE) snprintf(detail, size, "C puro (CPU sem %s)", isa_name(x64_min));
    else                     snprintf(detail, size, "C puro");
    return ENC_TIER_THREADS;
}

static int for_this_platform(unsigned os, unsigned arch)
{
    return (os & enc_current_os()) && (arch & enc_current_arch());
}

static void set_backend(MediaEncoder* e, const char* name, EncTier tier, const char* detail)
{
    e->Backend = name;
    e->Tier = (int)tier;
    snprintf(e->BackendDetail, sizeof(e->BackendDetail), "%s", detail ? detail : "");
}

MediaEncoder* media_encoder_open(const MediaEncoderParams* p)
{
    if (!p) return 0;

    // Abrir E carregar sao a mesma chamada: se o codec mora num modulo, ele entra aqui.
    codec_plugin_ensure(p->Codec);

    for (int i = 0, n = codec_plugin_count(); i < n; i++)
    {
        const CodecPlugin* b = codec_plugin_at(i);
        char detail[96] = { 0 };
        MediaEncoder* e;
        EncTier tier;

        if (b->Codec != p->Codec || !(b->Role & CODEC_ROLE_ENCODE)) continue;
        if (!b->Compiled || !b->EncoderOpen)                        continue;
        if (!for_this_platform(b->Os, b->Arch))                     continue;
        if (b->Hardware  && p->Accel == MEDIA_ACCEL_SOFTWARE)       continue;
        if (!b->Hardware && p->Accel == MEDIA_ACCEL_HARDWARE)       continue;

        if (b->Hardware) tier = ENC_TIER_HARDWARE;
        else             tier = sw_tier(b->IsaMin, b->IsaMax, b->Neon, detail, sizeof(detail));

        e = b->EncoderOpen(p, detail, sizeof(detail));

        // Degrau que nao abriu -- nao existe aqui, nao aceita esta resolucao, ou esgotou
        // sessoes (a GeForce limita quantos NVENC rodam juntos) -- cai para o proximo.
        if (!e) continue;

        set_backend(e, b->Name, tier, detail);
        // Se este backend veio de um modulo carregado, o modulo fica preso ate o Close.
        codec_use_track_encoder(codec_plugin_set_of(b), e);
        return e;
    }
    return 0;
}

static void info_fill(EncBackendInfo* o, const char* name, MediaCodec codec, EncTier tier,
                      unsigned os, unsigned arch, int avail, const char* why, const char* detail)
{
    memset(o, 0, sizeof(*o));
    o->Name = name; o->Codec = codec; o->Tier = tier; o->Os = os; o->Arch = arch;
    o->Available = avail; o->Why = why;
    snprintf(o->Detail, sizeof(o->Detail), "%s", detail ? detail : "");
}

// Percorre os descritores no papel pedido e descreve cada um: degrau efetivo nesta
// maquina, se da para usar aqui e agora, e por que nao quando for o caso.
// As duas listas publicas sao a MESMA funcao -- antes eram duas tabelas separadas (g_enc e
// g_dec), que podiam divergir uma da outra.
static int lista_por_papel(MediaCodec codec, int role, EncBackendInfo* out, int max)
{
    int n = 0;
    codec_plugin_ensure(codec);      // a lista tem de mostrar tambem o que vem de modulo
    for (int i = 0, total = codec_plugin_count(); i < total; i++)
    {
        const CodecPlugin* b = codec_plugin_at(i);
        char detail[96] = { 0 };
        EncTier tier;
        int avail = 0;
        const char* why = 0;

        if (!(b->Role & role)) continue;
        if (codec != MEDIA_CODEC_NONE && b->Codec != codec) continue;

        if (b->Hardware) tier = ENC_TIER_HARDWARE;
        else
        {
            tier = sw_tier(b->IsaMin, b->IsaMax, b->Neon, detail, sizeof(detail));
            if (role == CODEC_ROLE_DECODE && b->DecDetail)
                snprintf(detail, sizeof(detail), "%s", b->DecDetail);
        }

        if (!b->Compiled)                            why = "nao compilado neste build";
        else if (!for_this_platform(b->Os, b->Arch)) why = "nao roda nesta plataforma";
        else if (b->Probe)
        {
            if (b->Probe(b->Codec, detail, sizeof(detail)) > 0) avail = 1;
            else
            {
                why = (role == CODEC_ROLE_ENCODE)
                    ? "nenhum encoder de hardware para este codec nesta maquina"
                    : "sem decode por GPU para este codec nesta maquina";
                if (b->Note) snprintf(detail, sizeof(detail), "%s", b->Note);
            }
        }
        else avail = 1;

        if (n < max) info_fill(&out[n], b->Name, b->Codec, tier, b->Os, b->Arch, avail, why, detail);
        n++;
    }
    return n;
}

int media_encoder_list(MediaCodec codec, EncBackendInfo* out, int max)
{ return lista_por_papel(codec, CODEC_ROLE_ENCODE, out, max); }

int media_decoder_list(MediaCodec codec, EncBackendInfo* out, int max)
{ return lista_por_papel(codec, CODEC_ROLE_DECODE, out, max); }
