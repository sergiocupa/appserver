//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Quantas threads dar a um codec quando o chamador nao pede um numero.
//
//  A conta tem DUAS partes, e NENHUMA das duas e igual entre codecs:
//
//    1. REGRA DA MAQUINA -- quanto dos nucleos fisicos aquele codec aproveita. Sai de
//       varredura com video 1080p real, 60 quadros por ponto, melhor de 2-3 repeticoes,
//       em processador de 8 fisicos / 16 logicos:
//
//         H.264  ganho 1.00 / 1.70 / 1.99 / 2.26 / 2.32 / 2.69   (1,2,3,4,6,8 slices)
//                rendimento por unidade despenca cedo        -> 0.5 x fisicos
//         VP9    ganho 1.00 / 1.62 / 1.93 / 1.99 / 2.05 / 1.93   (4,8,12,16,20,24)
//                satura em 16, que e 1 x LOGICOS             -> 2.0 x fisicos
//         H.265  ganho 1.00 / 1.89 / 2.75 / 2.87 / 3.59 / 3.62   (1,2,4,6,8,12 pools)
//                de 8 para 12 rende 1%                       -> 1.0 x fisicos
//         AV1    ganho 1.00 / 3.09 / 5.87 / 8.40 / 8.47 / 8.46   (niveis 1..6)
//                satura em 4                                 -> 0.5 x fisicos
//
//    2. TETO DE GEOMETRIA -- quantos fragmentos de quadro o codec consegue tratar em
//       paralelo NAQUELA resolucao. Cada codec fragmenta de um jeito, em eixo diferente:
//
//         H.264  slices HORIZONTAIS         -> altura, em linhas de macrobloco (16 px)
//         VP9    colunas de tile + ROW_MT   -> largura (>=256 px/coluna) e altura, em
//                                             linhas de superbloco (64 px)
//         H.265  frentes de onda (WPP)      -> altura, em linhas de CTU (64 px)
//         AV1    niveis do SVT              -> nao e geometria; o codec limita em 6
//
//  As duas partes ficam DENTRO de cada codec, que e quem conhece a regra e o eixo dele.
//  Este header so oferece a contagem de nucleos e a forma de cruzar as duas.
//
//  Hyper-threading: nao entra na regra do H.264, do H.265 nem do AV1 -- duas threads
//  logicas do mesmo nucleo disputam as mesmas unidades de execucao. O VP9 e a excecao
//  medida: com ROW_MT o trabalho e fino e regular, e a saturacao caiu exatamente em
//  1 x logicos.
//
//  E header-only porque cada plugin de codec e um modulo separado que NAO linka o nucleo.
//  Sem estado, a copia por modulo nao custa nada.

#ifndef CODEC_PARALLEL_H
#define CODEC_PARALLEL_H

#include "memory_pool.h"   // toda alocacao passa pelo pool: nada de malloc solto

#ifdef _WIN32
  #include <windows.h>
#else
  #include <stdio.h>
  #include <string.h>
  #include <unistd.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Nucleos FISICOS da maquina (nunca os logicos). Cai para os logicos so se a topologia
// nao estiver disponivel.
static int codec_nucleos_fisicos(void)
{
#ifdef _WIN32
    DWORD len = 0;
    GetLogicalProcessorInformation(NULL, &len);
    if (len > 0)
    {
        SYSTEM_LOGICAL_PROCESSOR_INFORMATION* buf =
            (SYSTEM_LOGICAL_PROCESSOR_INFORMATION*)memop_alloc_raw((uint64)len);
        if (buf)
        {
            int count = 0;
            if (GetLogicalProcessorInformation(buf, &len))
            {
                DWORD n = len / (DWORD)sizeof(buf[0]);
                DWORD i;
                for (i = 0; i < n; i++)
                    if (buf[i].Relationship == RelationProcessorCore) count++;
            }
            memop_free_raw(buf);
            if (count > 0) return count;
        }
    }
    {
        SYSTEM_INFO si; GetSystemInfo(&si);
        return si.dwNumberOfProcessors > 1 ? (int)si.dwNumberOfProcessors : 1;
    }
#else
    {
        /* pares (physical id, core id) distintos no /proc/cpuinfo */
        FILE* f = fopen("/proc/cpuinfo", "r");
        if (f)
        {
            char linha[256];
            int fis[512], nucl[512], n = 0, pid = -1, cid = -1;
            while (fgets(linha, sizeof(linha), f))
            {
                if (!strncmp(linha, "physical id", 11)) sscanf(linha, "physical id : %d", &pid);
                else if (!strncmp(linha, "core id", 7))  sscanf(linha, "core id : %d", &cid);
                else if (linha[0] == '\n' && pid >= 0 && cid >= 0)
                {
                    int j, achou = 0;
                    for (j = 0; j < n; j++) if (fis[j] == pid && nucl[j] == cid) { achou = 1; break; }
                    if (!achou && n < 512) { fis[n] = pid; nucl[n] = cid; n++; }
                    pid = cid = -1;
                }
            }
            fclose(f);
            if (n > 0) return n;
        }
        {
            long n = sysconf(_SC_NPROCESSORS_ONLN);
            return n > 1 ? (int)n : 1;
        }
    }
#endif
}

// Cruza as duas contas e devolve sempre >= 1.
//
//   pedido > 0  -> o chamador mandou um numero: ele manda, sem discussao.
//   budget      -> quanto da maquina este codec pode tomar (regra dele, medida).
//   faixas > 0  -> quantos fragmentos de quadro o codec consegue tratar em paralelo
//                  NESTA resolucao (geometria dele). <= 0 significa "nao se aplica".
static int codec_threads(int pedido, int budget, int faixas)
{
    int t;
    if (pedido > 0) return pedido;

    t = budget;
    if (faixas > 0 && t > faixas) t = faixas;
    return t > 0 ? t : 1;
}

#ifdef __cplusplus
}
#endif
#endif // CODEC_PARALLEL_H
