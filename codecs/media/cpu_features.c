//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Deteccao de conjuntos de instrucao SIMD, para o seletor de backend decidir se um
//  encoder de software roda no degrau SIMD ou no THREADS.
//
//  AVX/AVX2/AVX512 exigem DUAS checagens: o bit da CPU (CPUID) e o SO salvando os
//  registradores largos na troca de contexto (OSXSAVE + XGETBV). So o CPUID diria "tem
//  AVX" numa VM ou num SO que nao preserva os registradores YMM -- e a primeira
//  instrucao AVX derrubaria o processo com instrucao ilegal.

#include "enc_select.h"

#if defined(_M_X64) || defined(__x86_64__)
  #define CPUF_X86 1
  #ifdef _MSC_VER
    #include <intrin.h>
    static void cpuid(int leaf, int sub, int r[4]) { __cpuidex(r, leaf, sub); }
    static unsigned long long xgetbv0(void) { return _xgetbv(0); }
  #else
    #include <cpuid.h>
    static void cpuid(int leaf, int sub, int r[4])
    { unsigned a, b, c, d; __cpuid_count(leaf, sub, a, b, c, d); r[0] = (int)a; r[1] = (int)b; r[2] = (int)c; r[3] = (int)d; }
    static unsigned long long xgetbv0(void)
    { unsigned lo, hi; __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0)); return ((unsigned long long)hi << 32) | lo; }
  #endif
#endif

static CpuFeatures g_cpu;
static int         g_cpu_done;

static void detect(CpuFeatures* f)
{
#ifdef CPUF_X86
    int r[4];
    cpuid(0, 0, r);
    int max_leaf = r[0];

    cpuid(1, 0, r);
    f->sse2  = (r[3] >> 26) & 1;
    f->ssse3 = (r[2] >> 9)  & 1;
    f->sse41 = (r[2] >> 19) & 1;

    int osxsave = (r[2] >> 27) & 1;
    int avx_cpu = (r[2] >> 28) & 1;
    unsigned long long xcr0 = osxsave ? xgetbv0() : 0;
    int os_ymm  = (xcr0 & 0x6) == 0x6;     // SSE + AVX state salvos pelo SO
    int os_zmm  = (xcr0 & 0xE6) == 0xE6;   // + opmask e ZMM (AVX-512)

    f->avx = avx_cpu && os_ymm;
    if (max_leaf >= 7)
    {
        cpuid(7, 0, r);
        f->avx2   = f->avx && ((r[1] >> 5) & 1);
        f->avx512 = os_zmm && ((r[1] >> 16) & 1);   // AVX512F
    }
#elif defined(_M_ARM64) || defined(__aarch64__)
    f->neon = 1;   // Advanced SIMD e obrigatorio no AArch64
#endif
}

const CpuFeatures* cpu_features(void)
{
    // Corrida benigna: duas threads podem detectar ao mesmo tempo, mas escrevem o mesmo
    // resultado. Nao vale um lock para isso.
    if (!g_cpu_done) { CpuFeatures f = { 0 }; detect(&f); g_cpu = f; g_cpu_done = 1; }
    return &g_cpu;
}

unsigned enc_current_os(void)
{
#ifdef _WIN32
    return ENC_OS_WINDOWS;
#else
    return ENC_OS_LINUX;
#endif
}

unsigned enc_current_arch(void)
{
#if defined(_M_ARM64) || defined(__aarch64__)
    return ENC_ARCH_ARM64;
#else
    return ENC_ARCH_X64;
#endif
}

const char* enc_tier_name(EncTier t)
{
    switch (t)
    {
        case ENC_TIER_HARDWARE: return "hardware";
        case ENC_TIER_GPU:      return "gpu";
        case ENC_TIER_SIMD:     return "simd";
        case ENC_TIER_THREADS:  return "threads";
        default:                return "?";
    }
}
