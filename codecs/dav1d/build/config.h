/*
 * config.h do dav1d ESCRITO A MAO (substitui o que o meson geraria).
 *
 * Por que existe: o dav1d so tem build por meson, que por sua vez exige Python. Como o
 * resto dos codecs deste repo e compilado por .vcxproj (Windows) e CMake (Linux), gerar
 * este header a mao evita uma quarta ferramenta no ciclo. As decisoes que o meson tomaria
 * sondando o compilador estao resolvidas aqui por #ifdef -- todas sao conhecidas para os
 * alvos que o projeto suporta (MSVC/x64, gcc-clang/x64, gcc-clang/aarch64).
 *
 * Se um dia o dav1d for atualizado, conferir contra include/dav1d/version.h e contra a
 * lista de simbolos usada pelos fontes:
 *     grep -rhoE '\b(ARCH_[A-Z0-9_]+|HAVE_[A-Z0-9_]+|CONFIG_[A-Z0-9_]+)\b' src include | sort -u
 *
 * ASSEMBLY: ligado em x86-64 (HAVE_ASM=1; os .asm de src/x86 sao montados pelo NASM com
 * build/config.asm). Em ARM64 o build continua C puro (HAVE_ASM=0): os .S de ARM nao sao
 * montados.
 */

#ifndef DAV1D_CONFIG_H
#define DAV1D_CONFIG_H

/* ---- arquitetura ------------------------------------------------------- */
#if defined(_M_X64) || defined(__x86_64__)
  #define ARCH_X86         1
  #define ARCH_X86_64      1
  #define ARCH_X86_32      0
  #define ARCH_AARCH64     0
  #define ARCH_ARM         0
#elif defined(_M_IX86) || defined(__i386__)
  #define ARCH_X86         1
  #define ARCH_X86_64      0
  #define ARCH_X86_32      1
  #define ARCH_AARCH64     0
  #define ARCH_ARM         0
#elif defined(_M_ARM64) || defined(__aarch64__)
  #define ARCH_X86         0
  #define ARCH_X86_64      0
  #define ARCH_X86_32      0
  #define ARCH_AARCH64     1
  #define ARCH_ARM         0
#elif defined(_M_ARM) || defined(__arm__)
  #define ARCH_X86         0
  #define ARCH_X86_64      0
  #define ARCH_X86_32      0
  #define ARCH_AARCH64     0
  #define ARCH_ARM         1
#else
  #define ARCH_X86         0
  #define ARCH_X86_64      0
  #define ARCH_X86_32      0
  #define ARCH_AARCH64     0
  #define ARCH_ARM         0
#endif

/* Arquiteturas que este repo nao alveja: zeradas explicitamente para os #if dos fontes. */
#define ARCH_PPC64LE       0
#define ARCH_RISCV         0
#define ARCH_LOONGARCH     0
#define ARCH_LOONGARCH64   0

/* ---- profundidade de bits ---------------------------------------------- */
/* Os dois caminhos entram na lib: os 13 *_tmpl.c sao compilados duas vezes, com
 * -DBITDEPTH=8 e -DBITDEPTH=16. Estes dois flags dizem ao codigo NAO-templated que
 * ambos existem. */
#define CONFIG_8BPC        1
#define CONFIG_16BPC       1

/* ---- assembly ----------------------------------------------------------- */
// Assembly LIGADO so em x86-64: os .asm sao montados pelo NASM (codecs.vcxproj). No ARM64
// continua C puro, porque os .S de ARM nao sao montados neste build.
#if ARCH_X86_64
  #define HAVE_ASM         1
#else
  #define HAVE_ASM         0
#endif
#define TRIM_DSP_FUNCTIONS 0

/* Extensoes de assembly ARM: sem asm, nenhuma se aplica. */
#define HAVE_AS_ARCH_DIRECTIVE           0
#define HAVE_AS_ARCHEXT_DOTPROD_DIRECTIVE 0
#define HAVE_AS_ARCHEXT_I8MM_DIRECTIVE    0
#define HAVE_AS_ARCHEXT_SVE_DIRECTIVE     0
#define HAVE_AS_ARCHEXT_SVE2_DIRECTIVE    0
#define HAVE_AS_FUNC                      0
#define HAVE_PRIVATE_EXTERN               0

/* ---- log ---------------------------------------------------------------- */
#define CONFIG_LOG         1

/* ---- plataforma --------------------------------------------------------- */
#ifdef _WIN32
  /* MSVC: sem <unistd.h>; alocacao alinhada vem de _aligned_malloc, que o dav1d ja
   * trata quando nenhuma das tres opcoes abaixo esta definida. */
  #define HAVE_UNISTD_H              0
  #define HAVE_ALIGNED_ALLOC         0
  #define HAVE_POSIX_MEMALIGN        0
  #define HAVE_MEMALIGN              0
  #define HAVE_DLSYM                 0
  #define HAVE_GETAUXVAL             0
  #define HAVE_ELF_AUX_INFO          0
  #define HAVE_PTHREAD_GETAFFINITY_NP 0
  #define HAVE_PTHREAD_SETNAME_NP    0
  #define HAVE_PTHREAD_SET_NAME_NP   0
  #define HAVE_PTHREAD_NP_H          0
#else
  #define HAVE_UNISTD_H              1
  #define HAVE_ALIGNED_ALLOC         1
  #define HAVE_POSIX_MEMALIGN        1
  #define HAVE_MEMALIGN              0
  #define HAVE_DLSYM                 1
  #define HAVE_PTHREAD_GETAFFINITY_NP 1
  #define HAVE_PTHREAD_SETNAME_NP    1
  #define HAVE_PTHREAD_SET_NAME_NP   0
  #define HAVE_PTHREAD_NP_H          0
  #if defined(__linux__)
    #define HAVE_GETAUXVAL           1
    #define HAVE_ELF_AUX_INFO        0
  #else
    #define HAVE_GETAUXVAL           0
    #define HAVE_ELF_AUX_INFO        1
  #endif
#endif

#endif /* DAV1D_CONFIG_H */
