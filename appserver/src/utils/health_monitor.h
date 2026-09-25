//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Saude do PROCESSO servidor: CPU, memoria e GPU, num formato que a UI consome.
//
//  Tudo aqui e do processo, nao da maquina. Um painel de saude de servidor precisa
//  responder "quanto ESTE servidor esta gastando", nao "quanto a maquina esta ocupada" --
//  senao um build rodando em outra janela aparece como carga do servidor.
//
//  CPU e GPU sao medidas por DELTA entre duas amostras, entao este modulo guarda a amostra
//  anterior. A primeira chamada depois do init nao tem com o que comparar e devolve 0.
//
//  GPU: so Windows, via contadores PDH ("\GPU Engine(*)\Utilization Percentage"), que
//  existem a partir do Windows 10 1709 e valem para qualquer fabricante. No Linux nao ha
//  equivalente generico -- cada fabricante expoe de um jeito (NVML na NVIDIA,
//  gpu_busy_percent no sysfs da amdgpu) -- entao Available fica 0 e a UI mostra "n/d".
//  Ver health_monitor.c para por que a conta e o MAXIMO entre tipos de engine, e nao a soma.

#ifndef HEALTH_MONITOR_H
#define HEALTH_MONITOR_H

#ifdef __cplusplus
extern "C" {
#endif

#include "xplatbase.h"

typedef struct HealthSample
{
    /* ---- CPU (do processo) ---- */
    double  CpuPercent;        /* 0..100, ja dividido pelos nucleos logicos */
    int     CpuCores;          /* nucleos logicos da maquina */

    /* ---- Memoria do processo, vista pelo SO ---- */
    uint64  RssBytes;          /* working set (Win) / VmRSS (Linux) */
    uint64  VirtualBytes;

    /* ---- Memoria do pool do xplatbase ----
     * E este o sinal que importa neste projeto: o pool e por onde passa toda alocacao.
     * LiveBlocks = alloc_count - free_count. O numero absoluto nao diz muito (conta o
     * processo inteiro); o que acusa vazamento e a TENDENCIA entre amostras. */
    int64   PoolLiveBlocks;
    uint64  PoolAllocCount;
    uint64  PoolFreeCount;
    uint64  PoolReservedBytes; /* segmentos de 4MB reservados do SO agora */
    uint64  PoolCachedChunks;  /* chunks de 64KB livres no cache global */
    uint64  PoolPurgeCount;

    /* ---- GPU ---- */
    int     GpuAvailable;      /* 0 = nao ha fonte de GPU nesta plataforma/maquina */
    double  GpuPercent;        /* 0..100 */
    double  GpuEncodePercent;  /* so as engines de video (encode/decode) */
    uint64  GpuMemoryBytes;    /* memoria dedicada em uso pelo processo; 0 = desconhecida */

    /* ---- Vazamento ----
     *
     *  ALARME BARATO, de tempo de execucao. Nao suspende thread nenhuma e nao percorre o
     *  pool: e so a leitura de memop_get_stats() que ja acontece aqui, guardada numa
     *  janela. A varredura cara (mem_leak_watch_scan_now) continua sendo sob demanda --
     *  este alarme serve para dizer QUANDO vale a pena pagar por ela.
     *
     *  O sinal NAO e a inclinacao dos blocos vivos. Durante um job de conversao eles sobem
     *  e voltam, e uma inclinacao positiva ali e trabalho normal, nao vazamento. O que
     *  acusa acumulo e o PISO subir: o minimo de blocos vivos da janela recente maior que
     *  o minimo da janela anterior significa que memoria entrou e nao voltou. */
    int     LeakLevel;          /* 0 = ok, 1 = observar, 2 = alarme */
    int64   LeakFloorDelta;     /* quanto o piso de blocos vivos subiu entre as duas metades */
    int64   LeakFloorPerMin;    /* o mesmo, normalizado por minuto */
    int     LeakWindowSec;      /* quanto tempo a janela ja cobre (cheia = 2 x METADE) */
    int     LeakWatchAvailable; /* mem_leak_watch so existe no Windows */
}
HealthSample;

/* Idempotentes. O init abre a consulta de PDH (Windows) e tira a amostra de referencia. */
void    health_monitor_init(void);
void    health_monitor_shutdown(void);

/* Preenche 'out'. Thread-safe. Devolve false so se 'out' for nulo. */
boolean health_monitor_sample(HealthSample* out);

/* Dispara mem_leak_watch_scan_now() e devolve o FIM do mem_leak_watch.log.
 * O buffer e do chamador. Devolve o numero de bytes escritos (0 se nao ha log).
 *
 * E caro de proposito e nao deve ir em timer: o scan suspende as threads uma a uma.
 * Serve para o botao "varrer agora" do painel, e nada mais. */
int     health_monitor_leak_scan(char* out, int out_size);

#ifdef __cplusplus
}
#endif
#endif /* HEALTH_MONITOR_H */
