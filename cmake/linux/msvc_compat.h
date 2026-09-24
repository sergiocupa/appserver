//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Compatibilidade das funcoes "seguras" da CRT do MSVC para o build LINUX.
//
//  Este header e FORCADO (-include) pelo CMake so nos alvos do projeto e so fora do
//  Windows. O codigo usa ~100 chamadas como sprintf_s/fopen_s/_stricmp -- renomeacoes
//  mecanicas, que editadas uma a uma virariam ruido e risco de erro. Centralizar aqui deixa
//  o build Windows (MSVC) exatamente como esta.
//
//  O que e ESTRUTURAL (sockets, threads, headers do Windows) NAO fica aqui: esses pontos tem
//  #ifdef explicito nos proprios fontes, onde quem le o codigo enxerga a diferenca.

#ifndef APPSERVER_MSVC_COMPAT_H
#define APPSERVER_MSVC_COMPAT_H

#ifndef _WIN32

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <limits.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifndef _ERRNO_T_DEFINED
#define _ERRNO_T_DEFINED
typedef int errno_t;
#endif

#ifndef _TRUNCATE
#define _TRUNCATE ((size_t)-1)
#endif
#ifndef MAX_PATH
#define MAX_PATH PATH_MAX
#endif
#ifndef CP_UTF8
#define CP_UTF8 65001
#endif
#ifndef UINT_PTR
#define UINT_PTR uintptr_t
#endif

// Convencoes de chamada do Windows. No x86-64 Linux ha uma so, entao somem. As funcoes de
// thread do servidor usam WINAPI para casar com o xthread_func_t da xplatbase, que no ramo
// Linux nao tem convencao nenhuma.
#ifndef WINAPI
#define WINAPI
#endif
#ifndef __stdcall
#define __stdcall
#endif
#ifndef __cdecl
#define __cdecl
#endif

// ---- formatacao / strings ----------------------------------------------------
// sprintf_s(buf, tamanho, fmt, ...) tem a mesma forma do snprintf. A diferenca de semantica
// (o _s aborta em estouro; o snprintf trunca) so aparece num bug que ja existiria no Windows.
#define sprintf_s   snprintf
#define vsprintf_s  vsnprintf
#define _snprintf   snprintf
#define _vsnprintf  vsnprintf
#define fprintf_s   fprintf
#define sscanf_s    sscanf
#define fscanf_s    fscanf

#define _stricmp    strcasecmp
#define _strnicmp   strncasecmp
#define _strdup     strdup

static inline errno_t strcpy_s(char* dst, size_t size, const char* src)
{
    if (!dst || size == 0) return EINVAL;
    if (!src) { dst[0] = 0; return EINVAL; }
    size_t n = strlen(src);
    if (n >= size) { dst[0] = 0; return ERANGE; }
    memcpy(dst, src, n + 1);
    return 0;
}

static inline errno_t strncpy_s(char* dst, size_t size, const char* src, size_t count)
{
    if (!dst || size == 0) return EINVAL;
    if (!src) { dst[0] = 0; return EINVAL; }
    size_t n = strnlen(src, count == _TRUNCATE ? size - 1 : count);
    if (n >= size) n = size - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
    return 0;
}

static inline errno_t strcat_s(char* dst, size_t size, const char* src)
{
    if (!dst || !src) return EINVAL;
    size_t d = strnlen(dst, size);
    if (d >= size) return EINVAL;
    return strcpy_s(dst + d, size - d, src);
}

static inline errno_t memcpy_s(void* dst, size_t size, const void* src, size_t count)
{
    if (!dst || !src || count > size) return EINVAL;
    memcpy(dst, src, count);
    return 0;
}

// ---- arquivos ------------------------------------------------------------------
static inline errno_t fopen_s(FILE** fp, const char* path, const char* mode)
{
    if (!fp) return EINVAL;
    *fp = fopen(path, mode);
    return *fp ? 0 : errno;
}

#define _fseeki64(f, off, origin) fseeko((f), (off_t)(off), (origin))
#define _ftelli64(f)              ((int64_t)ftello(f))
#define _access                   access
#define _unlink                   unlink
#define _rmdir                    rmdir
#define _getcwd                   getcwd
#define _chdir                    chdir
#define _mkdir(path)              mkdir((path), 0755)

// ---- ambiente / tempo -----------------------------------------------------------
// _dupenv_s devolve uma copia que o chamador libera com free(): strdup cumpre o contrato.
static inline errno_t _dupenv_s(char** out, size_t* len, const char* name)
{
    if (!out) return EINVAL;
    const char* v = getenv(name);
    *out = v ? strdup(v) : NULL;
    if (len) *len = v ? strlen(v) + 1 : 0;
    return 0;
}

// Ordem dos argumentos do MSVC: (struct tm* destino, const time_t* origem).
static inline errno_t gmtime_s(struct tm* dst, const time_t* t)    { return gmtime_r(t, dst)    ? 0 : errno; }
static inline errno_t localtime_s(struct tm* dst, const time_t* t) { return localtime_r(t, dst) ? 0 : errno; }

static inline uint64_t GetTickCount64(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

#ifndef Sleep
#define Sleep(ms) usleep((useconds_t)(ms) * 1000u)
#endif

// Win32 de arquivo, usado em pontos isolados: mesmo contrato (TRUE = sucesso).
#define DeleteFileA(path) (unlink(path) == 0)

// Console do Windows: no terminal Linux o UTF-8 ja e o padrao.
#define SetConsoleOutputCP(cp) ((void)(cp))
#define SetConsoleCP(cp)       ((void)(cp))

#endif  // !_WIN32
#endif  // APPSERVER_MSVC_COMPAT_H
