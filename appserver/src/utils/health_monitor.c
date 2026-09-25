//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Ver health_monitor.h para o contrato. Aqui ficam as decisoes de medicao.

#include "health_monitor.h"
#include "memory_pool.h"
#include "thread_handler.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#ifdef _WIN32
  #include <windows.h>
  #include <psapi.h>
  #include <pdh.h>
  #include <pdhmsg.h>
  #include <share.h>   // _SH_DENYNO para ler o log que o mem_leak_watch mantem aberto
  #include "mem_leak_watch.h"
  #pragma comment(lib, "psapi.lib")
  #pragma comment(lib, "pdh.lib")
  // NOTA: o import da pdh.dll e ESTATICO, entao ela e mapeada no processo assim que ele
  // sobe, mesmo com o monitor desligado. Tentei /DELAYLOAD por pragma aqui e o linkador
  // ignorou (nenhum aviso, e o dumpbin continua listando pdh.dll como dependencia
  // normal) -- num modulo que vira biblioteca ESTATICA a diretiva nao chegou ao link.
  // Para valer, ou o projeto do EXECUTAVEL declara DelayLoadDLLs, ou o PDH passa a ser
  // resolvido por LoadLibrary/GetProcAddress aqui dentro. Nao e urgente: mapear a DLL
  // custa endereco virtual, nao trabalho -- nenhum codigo de PDH roda com o monitor
  // desligado.
#else
  #include <unistd.h>
#endif

static xmutex_t g_lock;
static int      g_ready = 0;

/* ---- estado para os deltas de CPU ---- */
#ifdef _WIN32
static ULONGLONG g_prev_proc;   /* kernel+user do processo, em unidades de 100ns */
static ULONGLONG g_prev_wall;   /* relogio de parede, mesma unidade */
#else
static unsigned long long g_prev_proc_ticks;
static unsigned long long g_prev_wall_ticks;
#endif
static int g_cores = 1;

/* ---- janela do alarme de vazamento ----
 *
 * Amostras de blocos vivos com o instante de cada uma. A conta compara o PISO (minimo) da
 * metade mais nova com o da metade mais velha: subir de piso e memoria que entrou e nao
 * voltou. Comparar medias ou a inclinacao acusaria qualquer job de conversao, que sobe e
 * desce de propósito.
 *
 * A amostragem acontece a cada chamada de health_monitor_sample, ou seja, no ritmo de quem
 * consulta /api/health -- 1 Hz com o painel aberto. Com o painel fechado ninguem amostra e
 * a janela envelhece; por isso ela guarda o INSTANTE de cada amostra e descarta o que
 * passou do tempo, em vez de contar posicoes. */
#define LEAK_JANELA_SEG   120     /* janela inteira: duas metades de 60 s */
#define LEAK_MAX_AMOSTRAS 512
#define LEAK_PISO_OBSERVAR  200   /* piso subindo mais que isto por minuto -> observar */
#define LEAK_PISO_ALARME   1000   /* ... e mais que isto -> alarme */

typedef struct { int64 vivos; int64 t_ms; } LeakAmostra;

static LeakAmostra g_leak[LEAK_MAX_AMOSTRAS];
static int         g_leak_n;      /* quantas validas, em ordem de chegada */
static int         g_leak_ini;    /* indice da mais antiga (buffer circular) */

/* ---- estado do PDH (GPU) ---- */
#ifdef _WIN32
static PDH_HQUERY   g_pdh;
static PDH_HCOUNTER g_pdh_util;
static PDH_HCOUNTER g_pdh_mem;
static int          g_pdh_ok;
#endif


/* ============================================================================
 *  CPU
 * ========================================================================== */
#ifdef _WIN32
static ULONGLONG ft_to_u64(const FILETIME* ft)
{
    ULARGE_INTEGER u; u.LowPart = ft->dwLowDateTime; u.HighPart = ft->dwHighDateTime;
    return u.QuadPart;
}

static double cpu_percent_win(void)
{
    FILETIME cr, ex, kt, ut, agora;
    if (!GetProcessTimes(GetCurrentProcess(), &cr, &ex, &kt, &ut)) return 0.0;
    GetSystemTimeAsFileTime(&agora);

    ULONGLONG proc = ft_to_u64(&kt) + ft_to_u64(&ut);
    ULONGLONG wall = ft_to_u64(&agora);

    double pct = 0.0;
    if (g_prev_wall != 0 && wall > g_prev_wall)
    {
        /* As duas grandezas estao na MESMA unidade (100ns), entao a razao ja e a fracao
         * de um nucleo. Dividir por g_cores leva para "porcentagem da maquina", que e o
         * que um painel de saude precisa mostrar. */
        double dproc = (double)(proc - g_prev_proc);
        double dwall = (double)(wall - g_prev_wall);
        pct = (dproc / dwall) * 100.0 / (double)g_cores;
    }
    g_prev_proc = proc;
    g_prev_wall = wall;

    if (pct < 0.0)   pct = 0.0;
    if (pct > 100.0) pct = 100.0;
    return pct;
}
#else
static double cpu_percent_posix(void)
{
    /* utime+stime estao em clock ticks em /proc/self/stat (campos 14 e 15). */
    FILE* f = fopen("/proc/self/stat", "r");
    if (!f) return 0.0;

    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return 0.0;
    buf[n] = 0;

    /* O campo 2 (comm) vem entre parenteses e pode conter espacos: comeca depois do
     * ULTIMO ')'. Ignorar isso e o erro classico de quem faz sscanf direto aqui. */
    char* p = strrchr(buf, ')');
    if (!p) return 0.0;
    p++;

    unsigned long long utime = 0, stime = 0;
    int campo = 2;                       /* ja passamos por pid e comm */
    while (*p)
    {
        while (*p == ' ') p++;
        if (!*p) break;
        campo++;
        if (campo == 14) utime = strtoull(p, &p, 10);
        else if (campo == 15) { stime = strtoull(p, &p, 10); break; }
        else while (*p && *p != ' ') p++;
    }

    long hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0) hz = 100;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    unsigned long long wall = (unsigned long long)ts.tv_sec * (unsigned long long)hz
                            + (unsigned long long)ts.tv_nsec * (unsigned long long)hz / 1000000000ULL;
    unsigned long long proc = utime + stime;

    double pct = 0.0;
    if (g_prev_wall_ticks != 0 && wall > g_prev_wall_ticks)
    {
        double dproc = (double)(proc - g_prev_proc_ticks);
        double dwall = (double)(wall - g_prev_wall_ticks);
        pct = (dproc / dwall) * 100.0 / (double)g_cores;
    }
    g_prev_proc_ticks = proc;
    g_prev_wall_ticks = wall;

    if (pct < 0.0)   pct = 0.0;
    if (pct > 100.0) pct = 100.0;
    return pct;
}
#endif


/* ============================================================================
 *  Memoria do processo
 * ========================================================================== */
static void mem_processo(uint64* rss, uint64* virt)
{
    *rss = 0; *virt = 0;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX pmc;
    memset(&pmc, 0, sizeof(pmc));
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc)))
    {
        *rss  = (uint64)pmc.WorkingSetSize;
        *virt = (uint64)pmc.PrivateUsage;
    }
#else
    FILE* f = fopen("/proc/self/statm", "r");
    if (f)
    {
        unsigned long long total = 0, resident = 0;
        if (fscanf(f, "%llu %llu", &total, &resident) == 2)
        {
            long pg = sysconf(_SC_PAGESIZE);
            if (pg <= 0) pg = 4096;
            *virt = (uint64)total    * (uint64)pg;
            *rss  = (uint64)resident * (uint64)pg;
        }
        fclose(f);
    }
#endif
}


/* ============================================================================
 *  GPU (so Windows, via PDH)
 * ==========================================================================
 *  MAXIMO entre tipos de engine, nao soma.
 *
 *  O contador vem por INSTANCIA, e cada instancia e um par (processo, engine):
 *      pid_1234_luid_0x...._phys_0_eng_0_engtype_3D
 *  Um mesmo processo aparece em 3D, Copy, VideoDecode, VideoEncode... Somar tudo passa
 *  facil de 100%, porque as engines trabalham em paralelo. O Gerenciador de Tarefas
 *  resolve somando DENTRO de cada tipo e mostrando o maior entre os tipos; e a mesma
 *  conta feita aqui.
 *
 *  Filtramos pelo NOSSO pid, para ficar coerente com CPU e memoria, que sao do processo.
 *  Se a GPU estiver ocupada por outro programa, este painel mostra 0 -- e correto: ele
 *  responde pela saude deste servidor.
 *
 *  As engines de video ganham um campo proprio porque e o que o codec_hw usa (encode por
 *  hardware via Media Foundation).
 */
#ifdef _WIN32

#define GPU_MAX_TIPOS 16

typedef struct { char nome[32]; double soma; } EngTipo;

static int engtype_da_instancia(const char* inst, char* out, int out_size)
{
    const char* m = strstr(inst, "engtype_");
    if (!m) return 0;
    m += 8;
    int i = 0;
    while (m[i] && i < out_size - 1) { out[i] = m[i]; i++; }
    out[i] = 0;
    return i > 0;
}

static int instancia_e_nossa(const char* inst, DWORD pid)
{
    char alvo[32];
    snprintf(alvo, sizeof(alvo), "pid_%lu_", (unsigned long)pid);
    return strncmp(inst, alvo, strlen(alvo)) == 0;
}

static void gpu_amostra_win(HealthSample* out)
{
    out->GpuAvailable = 0;
    if (!g_pdh_ok) return;

    if (PdhCollectQueryData(g_pdh) != ERROR_SUCCESS) return;

    DWORD pid = GetCurrentProcessId();
    out->GpuAvailable = 1;

    /* ---- utilizacao ---- */
    {
        DWORD tam = 0, count = 0;
        PDH_STATUS st = PdhGetFormattedCounterArrayA(g_pdh_util, PDH_FMT_DOUBLE, &tam, &count, NULL);
        if (st == PDH_MORE_DATA && tam > 0)
        {
            PDH_FMT_COUNTERVALUE_ITEM_A* itens = (PDH_FMT_COUNTERVALUE_ITEM_A*)memop_alloc_raw(tam);
            if (itens)
            {
                if (PdhGetFormattedCounterArrayA(g_pdh_util, PDH_FMT_DOUBLE, &tam, &count, itens) == ERROR_SUCCESS)
                {
                    EngTipo tipos[GPU_MAX_TIPOS];
                    int nt = 0;
                    double video = 0.0;

                    for (DWORD i = 0; i < count; i++)
                    {
                        const char* inst = itens[i].szName;
                        if (!inst || !instancia_e_nossa(inst, pid)) continue;

                        char et[32];
                        if (!engtype_da_instancia(inst, et, sizeof(et))) continue;

                        double v = itens[i].FmtValue.doubleValue;
                        if (v < 0.0) v = 0.0;

                        if (strstr(et, "Video")) video += v;

                        int achou = -1;
                        for (int k = 0; k < nt; k++) if (strcmp(tipos[k].nome, et) == 0) { achou = k; break; }
                        if (achou < 0 && nt < GPU_MAX_TIPOS)
                        {
                            achou = nt++;
                            snprintf(tipos[achou].nome, sizeof(tipos[achou].nome), "%s", et);
                            tipos[achou].soma = 0.0;
                        }
                        if (achou >= 0) tipos[achou].soma += v;
                    }

                    double maior = 0.0;
                    for (int k = 0; k < nt; k++) if (tipos[k].soma > maior) maior = tipos[k].soma;

                    out->GpuPercent       = maior > 100.0 ? 100.0 : maior;
                    out->GpuEncodePercent = video > 100.0 ? 100.0 : video;
                }
                memop_free_raw(itens);
            }
        }
    }

    /* ---- memoria dedicada ---- */
    {
        DWORD tam = 0, count = 0;
        PDH_STATUS st = PdhGetFormattedCounterArrayA(g_pdh_mem, PDH_FMT_LARGE, &tam, &count, NULL);
        if (st == PDH_MORE_DATA && tam > 0)
        {
            PDH_FMT_COUNTERVALUE_ITEM_A* itens = (PDH_FMT_COUNTERVALUE_ITEM_A*)memop_alloc_raw(tam);
            if (itens)
            {
                if (PdhGetFormattedCounterArrayA(g_pdh_mem, PDH_FMT_LARGE, &tam, &count, itens) == ERROR_SUCCESS)
                {
                    uint64 soma = 0;
                    for (DWORD i = 0; i < count; i++)
                    {
                        const char* inst = itens[i].szName;
                        if (!inst || !instancia_e_nossa(inst, pid)) continue;
                        LONGLONG v = itens[i].FmtValue.largeValue;
                        if (v > 0) soma += (uint64)v;
                    }
                    out->GpuMemoryBytes = soma;
                }
                memop_free_raw(itens);
            }
        }
    }
}

static void gpu_init_win(void)
{
    g_pdh_ok = 0;
    if (PdhOpenQueryA(NULL, 0, &g_pdh) != ERROR_SUCCESS) return;

    /* Os contadores de GPU so existem a partir do Windows 10 1709. Em versao mais antiga
     * o AddCounter falha e o painel simplesmente mostra "n/d" -- nao e erro. */
    if (PdhAddCounterA(g_pdh, "\\GPU Engine(*)\\Utilization Percentage", 0, &g_pdh_util) != ERROR_SUCCESS)
    {
        PdhCloseQuery(g_pdh); g_pdh = NULL; return;
    }
    if (PdhAddCounterA(g_pdh, "\\GPU Process Memory(*)\\Dedicated Usage", 0, &g_pdh_mem) != ERROR_SUCCESS)
    {
        g_pdh_mem = NULL;   /* utilizacao sozinha ja serve */
    }

    /* Contador de taxa precisa de DUAS coletas para ter valor. Esta e a primeira. */
    PdhCollectQueryData(g_pdh);
    g_pdh_ok = 1;
}
#endif /* _WIN32 */


/* ============================================================================
 *  Alarme de vazamento (barato: so contadores, nada de suspender thread)
 * ========================================================================== */
static int64 agora_ms(void)
{
#ifdef _WIN32
    return (int64)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64)ts.tv_sec * 1000 + (int64)ts.tv_nsec / 1000000;
#endif
}

static void leak_registra(int64 vivos, HealthSample* out)
{
    int64 t = agora_ms();

    /* entra a nova amostra */
    if (g_leak_n < LEAK_MAX_AMOSTRAS)
    {
        int pos = (g_leak_ini + g_leak_n) % LEAK_MAX_AMOSTRAS;
        g_leak[pos].vivos = vivos; g_leak[pos].t_ms = t;
        g_leak_n++;
    }
    else
    {
        g_leak[g_leak_ini].vivos = vivos; g_leak[g_leak_ini].t_ms = t;
        g_leak_ini = (g_leak_ini + 1) % LEAK_MAX_AMOSTRAS;
    }

    /* sai o que passou da janela */
    while (g_leak_n > 0)
    {
        LeakAmostra* velha = &g_leak[g_leak_ini];
        if (t - velha->t_ms <= (int64)LEAK_JANELA_SEG * 1000) break;
        g_leak_ini = (g_leak_ini + 1) % LEAK_MAX_AMOSTRAS;
        g_leak_n--;
    }

    if (g_leak_n < 2) return;

    int64 t_ini = g_leak[g_leak_ini].t_ms;
    int64 span  = t - t_ini;
    out->LeakWindowSec = (int)(span / 1000);

    /* Só vale a pena decidir com a janela razoavelmente cheia. Antes disso o piso da
     * metade velha ainda e o proprio inicio do processo, e qualquer aquecimento normal
     * passaria por vazamento. */
    if (span < (int64)LEAK_JANELA_SEG * 1000 / 2) return;

    int64 meio = t_ini + span / 2;
    int64 piso_velho = 0, piso_novo = 0;
    int   tem_velho = 0, tem_novo = 0;

    for (int i = 0; i < g_leak_n; i++)
    {
        LeakAmostra* a = &g_leak[(g_leak_ini + i) % LEAK_MAX_AMOSTRAS];
        if (a->t_ms <= meio)
        {
            if (!tem_velho || a->vivos < piso_velho) { piso_velho = a->vivos; tem_velho = 1; }
        }
        else
        {
            if (!tem_novo || a->vivos < piso_novo) { piso_novo = a->vivos; tem_novo = 1; }
        }
    }
    if (!tem_velho || !tem_novo) return;

    int64 delta = piso_novo - piso_velho;
    double minutos = (double)span / 2.0 / 60000.0;   /* cada metade */
    if (minutos <= 0.0) return;

    out->LeakFloorDelta  = delta;
    out->LeakFloorPerMin = (int64)((double)delta / minutos);

    if (out->LeakFloorPerMin >= LEAK_PISO_ALARME)        out->LeakLevel = 2;
    else if (out->LeakFloorPerMin >= LEAK_PISO_OBSERVAR) out->LeakLevel = 1;
    else                                                 out->LeakLevel = 0;
}


/* ============================================================================
 *  API
 * ========================================================================== */
void health_monitor_init(void)
{
    if (g_ready) return;

    thread_mutex_init_inline(&g_lock);

#ifdef _WIN32
    {
        SYSTEM_INFO si; GetSystemInfo(&si);
        g_cores = si.dwNumberOfProcessors > 0 ? (int)si.dwNumberOfProcessors : 1;
    }
    g_prev_proc = 0; g_prev_wall = 0;
    gpu_init_win();
    cpu_percent_win();     /* amostra de referencia: a proxima ja tem delta */
#else
    {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        g_cores = n > 0 ? (int)n : 1;
    }
    g_prev_proc_ticks = 0; g_prev_wall_ticks = 0;
    cpu_percent_posix();
#endif

    g_ready = 1;
}

void health_monitor_shutdown(void)
{
    if (!g_ready) return;
#ifdef _WIN32
    if (g_pdh) { PdhCloseQuery(g_pdh); g_pdh = NULL; }
    g_pdh_ok = 0;
#endif
    thread_mutex_destroy_inline(&g_lock);
    g_ready = 0;
}

boolean health_monitor_sample(HealthSample* out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));

    if (!g_ready) health_monitor_init();

    thread_mutex_lock_inline(&g_lock);

    out->CpuCores = g_cores;
#ifdef _WIN32
    out->CpuPercent = cpu_percent_win();
    gpu_amostra_win(out);
    out->LeakWatchAvailable = 1;
#else
    out->CpuPercent = cpu_percent_posix();
    out->GpuAvailable = 0;          /* sem fonte generica no Linux; ver o header */
    out->LeakWatchAvailable = 0;    /* mem_leak_watch ainda e so Windows */
#endif

    mem_processo(&out->RssBytes, &out->VirtualBytes);

    {
        MemPoolStats s;
        memop_get_stats(&s);
        out->PoolAllocCount    = s.alloc_count;
        out->PoolFreeCount     = s.free_count;
        out->PoolLiveBlocks    = (int64)s.alloc_count - (int64)s.free_count;
        out->PoolReservedBytes = s.os_reserved_bytes;
        out->PoolCachedChunks  = s.cached_chunks;
        out->PoolPurgeCount    = s.purge_count;

        leak_registra(out->PoolLiveBlocks, out);
    }

    thread_mutex_unlock_inline(&g_lock);
    return true;
}

/* Caminho do mem_leak_watch.log.
 *
 * O xplatbase grava AO LADO DO EXECUTAVEL (mlw_default_log_path monta a partir do
 * GetModuleFileName), nao no diretorio de trabalho. Abrir "mem_leak_watch.log" pelo
 * caminho relativo achava nada quando o processo roda com o cwd em outro lugar -- que e
 * justamente o caso do appservertester, cujo cwd e test/appservertester enquanto o exe
 * esta em x64/Debug. O painel entao dizia "a varredura nao produziu log" com a varredura
 * tendo funcionado e escrito 166 linhas. */
static int leak_log_path(char* out, int out_size)
{
#ifdef _WIN32
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)out_size);
    if (n == 0 || n >= (DWORD)out_size) return 0;
    char* barra = strrchr(out, '\\');
    if (!barra) return 0;
    barra[1] = 0;
    if ((int)strlen(out) + 20 >= out_size) return 0;
    snprintf(out + strlen(out), (size_t)out_size - strlen(out), "%s", "mem_leak_watch.log");
    return 1;
#else
    (void)out; (void)out_size;
    return 0;
#endif
}

int health_monitor_leak_scan(char* out, int out_size)
{
    if (!out || out_size <= 1) return 0;
    out[0] = 0;

#ifdef _WIN32
    mem_leak_watch_scan_now();

    /* O mem_leak_watch so escreve em arquivo; nao ha API para ler o ultimo resultado.
     * Entao lemos o FIM do log, que e onde a varredura recem-disparada acabou de cair. */
    {
        char  path[MAX_PATH * 2];
        FILE* f;
        long  tam, quer, desde;
        size_t lidos;

        if (!leak_log_path(path, (int)sizeof(path))) return 0;

        /* _fsopen com _SH_DENYNO, e nao fopen_s: no MSVC o fopen_s abre em modo EXCLUSIVO,
         * e o mem_leak_watch mantem o proprio log aberto para append o tempo todo. O fonte
         * do xplatbase ja alerta para essa mesma pegadinha. */
        f = _fsopen(path, "rb", _SH_DENYNO);
        if (!f) return 0;

        fseek(f, 0, SEEK_END);
        tam   = ftell(f);
        quer  = out_size - 1;
        desde = tam > quer ? tam - quer : 0;
        fseek(f, desde, SEEK_SET);

        lidos = fread(out, 1, (size_t)(tam - desde), f);
        fclose(f);
        out[lidos] = 0;

        /* Cortar pelo tamanho deixa a primeira linha pela metade. Descarta ela, senao o
         * painel abre com meia pilha de chamada, sem comeco. */
        if (desde > 0)
        {
            char* q = strchr(out, '\n');
            if (q && *(q + 1))
            {
                size_t resto = lidos - (size_t)(q + 1 - out);
                memmove(out, q + 1, resto);
                out[resto] = 0;
                lidos = resto;
            }
        }
        return (int)lidos;
    }
#else
    (void)out_size;
    return 0;
#endif
}
