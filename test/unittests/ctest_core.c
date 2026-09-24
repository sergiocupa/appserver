#include "ctest_core.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void t_start(TestResult* r)
{
    if (!r) return;
    r->ok = 1;
    r->skipped = 0;
    r->msg[0] = '\0';
}

void t_failf(TestResult* r, const char* file, int line, const char* fmt, ...)
{
    char detalhe[384];
    va_list ap;
    const char* base;

    if (!r) return;
    va_start(ap, fmt);
    vsnprintf(detalhe, sizeof(detalhe), fmt, ap);
    va_end(ap);

    // So o nome do arquivo: o caminho inteiro empurra a mensagem para fora da tela.
    base = strrchr(file, '\\');
    if (!base) base = strrchr(file, '/');
    base = base ? base + 1 : file;

    r->ok = 0;
    snprintf(r->msg, sizeof(r->msg), "%s(%d): %s", base, line, detalhe);
}

void t_skipf(TestResult* r, const char* fmt, ...)
{
    va_list ap;
    if (!r) return;
    va_start(ap, fmt);
    vsnprintf(r->msg, sizeof(r->msg), fmt, ap);
    va_end(ap);
    r->ok = 1;          // pulado nao e falha
    r->skipped = 1;
}
