//  httpbench: mede o appserver de fora, como um cliente, e compara duas versoes.
//
//  Ele mesmo SOBE o servidor (bench_server) como processo filho a cada rodada. So assim da
//  para medir o que a arquitetura muda de verdade -- threads, CPU e memoria DO SERVIDOR --
//  alem de vazao e latencia vistas pelo cliente.
//
//  Uso:
//    httpbench --exe <bench_server> [--exe <outro>] [--rotulo A] [--rotulo B] [--env-a K=V] [--env-b K=V]
//              --web <pasta> [--rodadas N] [--saida <prefixo>] [--filtro <texto>] [--porta P]
//
//    Uma versao : mediana/min/max de cada metrica nas N rodadas.
//    Duas versoes: rodadas PAREADAS com ordem alternada (A,B / B,A / ...), efeito de
//                  Hodges-Lehmann, IC 95% por bootstrap, Wilcoxon exato + Benjamini-Hochberg
//                  e veredito com margem de nao-inferioridade por tipo de metrica.
//
//  Saida: tabela no console, <prefixo>.txt (a mesma tabela) e <prefixo>.tsv (bruto, uma
//  linha por versao x rodada x metrica).

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #ifndef _CRT_SECURE_NO_WARNINGS
  #define _CRT_SECURE_NO_WARNINGS
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #include <tlhelp32.h>
  #include <psapi.h>
  #include <intrin.h>
  #include <mmsystem.h>    // timeBeginPeriod: Sleep(1) de 1 ms, nao de 15,6
  #pragma comment(lib, "ws2_32.lib")
  #pragma comment(lib, "winmm.lib")
  #pragma comment(lib, "psapi.lib")
  typedef SOCKET Sock;
  #define SOCK_RUIM INVALID_SOCKET
  #define fecha_sock closesocket
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <sys/wait.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <signal.h>
  #include <pthread.h>
  #include <time.h>
  #include <errno.h>
  typedef int Sock;
  #define SOCK_RUIM (-1)
  #define fecha_sock close
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include "bench_estatistica.h"

// =========================================================================================
//  Plataforma: relogio, sono, threads, processo filho e amostra de recursos do processo
// =========================================================================================

#ifdef _WIN32
static long long agora_us(void)
{
    static LARGE_INTEGER f; LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (long long)(c.QuadPart / f.QuadPart) * 1000000LL + (long long)((c.QuadPart % f.QuadPart) * 1000000LL / f.QuadPart);
}
static void dorme_ms(int ms) { Sleep((DWORD)ms); }

typedef HANDLE Th;
typedef struct { void (*fn)(void*); void* arg; } ThIni;
static DWORD WINAPI th_tramp(LPVOID p) { ThIni i = *(ThIni*)p; free(p); i.fn(i.arg); return 0; }
static Th th_criar(void (*fn)(void*), void* arg)
{
    ThIni* i = (ThIni*)malloc(sizeof(ThIni)); i->fn = fn; i->arg = arg;
    return CreateThread(0, 256 * 1024, th_tramp, i, 0, 0);
}
static void th_juntar(Th t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
#else
static long long agora_us(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000000LL + t.tv_nsec / 1000;
}
static void dorme_ms(int ms) { struct timespec t = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&t, 0); }

typedef pthread_t Th;
typedef struct { void (*fn)(void*); void* arg; } ThIni;
static void* th_tramp(void* p) { ThIni i = *(ThIni*)p; free(p); i.fn(i.arg); return 0; }
static Th th_criar(void (*fn)(void*), void* arg)
{
    ThIni* i = (ThIni*)malloc(sizeof(ThIni)); i->fn = fn; i->arg = arg;
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 256 * 1024);
    pthread_t t; pthread_create(&t, &a, th_tramp, i); pthread_attr_destroy(&a);
    return t;
}
static void th_juntar(Th t) { pthread_join(t, 0); }
#endif

typedef struct
{
    int       Threads;
    long long CpuNs;      // CPU acumulada do processo (todas as threads, inclusive as que ja sairam)
    long long RssBytes;   // memoria residente
}
AmostraProc;

typedef struct
{
#ifdef _WIN32
    HANDLE H;
    DWORD  Pid;
#else
    pid_t  Pid;
#endif
    int    Vivo;
}
Proc;

#ifdef _WIN32
static double g_tsc_por_ns;   // QueryProcessCycleTime conta ciclos do TSC

static void calibra_tsc(void)
{
    long long t0 = agora_us(); unsigned long long c0 = __rdtsc();
    dorme_ms(200);
    long long t1 = agora_us(); unsigned long long c1 = __rdtsc();
    g_tsc_por_ns = (double)(c1 - c0) / ((double)(t1 - t0) * 1000.0);
}

static int proc_iniciar(Proc* p, const char* exe, int porta, const char* web)
{
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "\"%s\" %d \"%s\"", exe, porta, web);
    char dir[1024]; snprintf(dir, sizeof(dir), "%s", exe);
    char* barra = strrchr(dir, '\\'); if (!barra) barra = strrchr(dir, '/');
    if (barra) *barra = 0; else strcpy(dir, ".");

    SECURITY_ATTRIBUTES sa = { sizeof(sa), 0, TRUE };
    HANDLE nul = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, 0);
    STARTUPINFOA si; memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = nul; si.hStdError = nul; si.hStdInput = 0;
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessA(0, cmd, 0, 0, TRUE, CREATE_NO_WINDOW, 0, dir, &si, &pi);
    CloseHandle(nul);
    if (!ok) return 0;
    CloseHandle(pi.hThread);
    p->H = pi.hProcess; p->Pid = pi.dwProcessId; p->Vivo = 1;
    return 1;
}

static void proc_matar(Proc* p)
{
    if (!p->Vivo) return;
    TerminateProcess(p->H, 0);
    WaitForSingleObject(p->H, 5000);
    CloseHandle(p->H);
    p->Vivo = 0;
}

static int proc_vivo(Proc* p)
{
    DWORD c = 0;
    return p->Vivo && GetExitCodeProcess(p->H, &c) && c == STILL_ACTIVE;
}

static void proc_amostra(Proc* p, AmostraProc* a)
{
    memset(a, 0, sizeof(*a));
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (s != INVALID_HANDLE_VALUE)
    {
        THREADENTRY32 te; te.dwSize = sizeof(te);
        if (Thread32First(s, &te))
            do { if (te.th32OwnerProcessID == p->Pid) a->Threads++; } while (Thread32Next(s, &te));
        CloseHandle(s);
    }
    ULONG64 cyc = 0;
    if (QueryProcessCycleTime(p->H, &cyc)) a->CpuNs = (long long)((double)cyc / g_tsc_por_ns);
    PROCESS_MEMORY_COUNTERS pmc; memset(&pmc, 0, sizeof(pmc));
    if (GetProcessMemoryInfo(p->H, &pmc, sizeof(pmc))) a->RssBytes = (long long)pmc.WorkingSetSize;
}
#else
static void calibra_tsc(void) {}

static int proc_iniciar(Proc* p, const char* exe, int porta, const char* web)
{
    char ps[16]; snprintf(ps, sizeof(ps), "%d", porta);
    pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0)
    {
        int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0) { dup2(nul, 1); dup2(nul, 2); close(nul); }
        execl(exe, exe, ps, web, (char*)0);
        _exit(127);
    }
    p->Pid = pid; p->Vivo = 1;
    return 1;
}

static void proc_matar(Proc* p)
{
    if (!p->Vivo) return;
    kill(p->Pid, SIGKILL);
    waitpid(p->Pid, 0, 0);
    p->Vivo = 0;
}

static int proc_vivo(Proc* p)
{
    if (!p->Vivo) return 0;
    return waitpid(p->Pid, 0, WNOHANG) == 0;
}

static void proc_amostra(Proc* p, AmostraProc* a)
{
    memset(a, 0, sizeof(*a));
    char cam[64], lin[512];
    snprintf(cam, sizeof(cam), "/proc/%d/status", (int)p->Pid);
    FILE* f = fopen(cam, "r");
    if (f)
    {
        while (fgets(lin, sizeof(lin), f))
        {
            if (!strncmp(lin, "Threads:", 8)) a->Threads = atoi(lin + 8);
            else if (!strncmp(lin, "VmRSS:", 6)) a->RssBytes = atoll(lin + 6) * 1024LL;
        }
        fclose(f);
    }
    // utime+stime do /proc/<pid>/stat: inclui as threads que ja terminaram (a soma do
    // schedstat por task nao inclui -- e o servidor antigo cria e mata thread por mensagem).
    snprintf(cam, sizeof(cam), "/proc/%d/stat", (int)p->Pid);
    f = fopen(cam, "r");
    if (f)
    {
        char buf[1024]; size_t n = fread(buf, 1, sizeof(buf) - 1, f); buf[n] = 0; fclose(f);
        char* q = strrchr(buf, ')');
        if (q)
        {
            unsigned long long ut = 0, st = 0; int campo = 2; q++;
            char* tok = strtok(q, " ");
            while (tok)
            {
                campo++;
                if (campo == 14) ut = strtoull(tok, 0, 10);
                if (campo == 15) { st = strtoull(tok, 0, 10); break; }
                tok = strtok(0, " ");
            }
            long hz = sysconf(_SC_CLK_TCK);
            a->CpuNs = (long long)((ut + st) * (1000000000ULL / (unsigned long long)hz));
        }
    }
}
#endif

// =========================================================================================
//  Cliente HTTP minimo
// =========================================================================================

static int g_porta;

static Sock conecta(int rcvbuf)
{
    Sock s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == SOCK_RUIM) return s;
    if (rcvbuf > 0) setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&rcvbuf, sizeof(rcvbuf));
    struct sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons((unsigned short)g_porta);
    if (connect(s, (struct sockaddr*)&a, sizeof(a)) != 0) { fecha_sock(s); return SOCK_RUIM; }
    int v = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&v, sizeof(v));
    // Sem resposta em 10 s conta como erro: o benchmark nunca pode travar.
#ifdef _WIN32
    DWORD t = 10000; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof(t));
#else
    struct timeval tv = { 10, 0 }; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    return s;
}

static int envia_tudo(Sock s, const char* d, long long n)
{
    while (n > 0)
    {
        int k = send(s, d, n > (1 << 20) ? (1 << 20) : (int)n, 0);
        if (k <= 0) return 0;
        d += k; n -= k;
    }
    return 1;
}

#define CONN_CAP (256 * 1024)
typedef struct { Sock S; char* B; int Ini, Fim; } Conn;

static void conn_abre(Conn* c, int rcvbuf) { c->S = conecta(rcvbuf); if (!c->B) c->B = (char*)malloc(CONN_CAP); c->Ini = c->Fim = 0; }
static void conn_fecha(Conn* c) { if (c->S != SOCK_RUIM) fecha_sock(c->S); c->S = SOCK_RUIM; c->Ini = c->Fim = 0; }
static void conn_libera(Conn* c) { conn_fecha(c); free(c->B); c->B = 0; }

static int conn_recv(Conn* c)
{
    if (c->Ini > 0 && c->Ini == c->Fim) c->Ini = c->Fim = 0;
    if (c->Fim == CONN_CAP)
    {
        memmove(c->B, c->B + c->Ini, (size_t)(c->Fim - c->Ini));
        c->Fim -= c->Ini; c->Ini = 0;
        if (c->Fim == CONN_CAP) return 0;   // cabecalho maior que o buffer
    }
    int k = recv(c->S, c->B + c->Fim, CONN_CAP - c->Fim, 0);
    if (k <= 0) return 0;
    c->Fim += k;
    return 1;
}

static int acha_fim_cab(Conn* c)
{
    for (int i = c->Ini; i + 3 < c->Fim; i++)
        if (c->B[i] == '\r' && c->B[i + 1] == '\n' && c->B[i + 2] == '\r' && c->B[i + 3] == '\n') return i + 4;
    return -1;
}

// Le UMA resposta. corpo: bytes do corpo; copia ate cap bytes do corpo em dest (se houver).
static int le_resposta(Conn* c, int* status, long long* corpo, char* dest, int cap)
{
    int fim;
    while ((fim = acha_fim_cab(c)) < 0) if (!conn_recv(c)) return 0;

    const char* h = c->B + c->Ini;
    int hl = fim - c->Ini;
    *status = 0;
    if (hl > 12 && !strncmp(h, "HTTP/1.", 7)) *status = atoi(h + 9);
    long long cl = -1;
    for (int i = 0; i < hl; i++)
    {
        if ((i == 0 || h[i - 1] == '\n') && (h[i] == 'C' || h[i] == 'c') && hl - i > 15)
        {
            char nome[16]; memcpy(nome, h + i, 15); nome[15] = 0;
            for (int k = 0; k < 15; k++) if (nome[k] >= 'A' && nome[k] <= 'Z') nome[k] += 32;
            if (!strcmp(nome, "content-length:")) cl = atoll(h + i + 15);
        }
    }
    c->Ini = fim;
    if (cl < 0) return 0;   // o servidor sempre manda Content-Length; sem ele, algo quebrou

    long long falta = cl; int pos = 0;
    while (falta > 0)
    {
        if (c->Ini == c->Fim && !conn_recv(c)) return 0;
        int tem = c->Fim - c->Ini;
        int usa = (long long)tem < falta ? tem : (int)falta;
        if (dest && pos < cap)
        {
            int cp = usa < cap - pos ? usa : cap - pos;
            memcpy(dest + pos, c->B + c->Ini, (size_t)cp); pos += cp;
        }
        c->Ini += usa; falta -= usa;
    }
    if (dest && cap > 0) dest[pos < cap ? pos : cap - 1] = 0;
    *corpo = cl;
    return 1;
}

// =========================================================================================
//  Carga: N threads repetindo a mesma requisicao por um tempo
// =========================================================================================

typedef struct
{
    const char* Req; long long ReqLen;
    int         NovaConexao;
    int         StatusEsperado;
    long long   Inicio, Fim;     // mede so entre Inicio e Fim (antes disso e aquecimento)
    long long*  Lat; int NLat, CapLat;
    long long   Bytes;           // corpo recebido (medido)
    long long   NTotal;          // respostas certas, INCLUSIVE no aquecimento (base do CPU por req)
    int         Erros;
    Th          T;
}
Trab;

static void trab_loop(void* p)
{
    Trab* t = (Trab*)p;
    Conn c; memset(&c, 0, sizeof(c)); c.S = SOCK_RUIM;
    for (;;)
    {
        long long t0 = agora_us();
        if (t0 >= t->Fim) break;
        if (c.S == SOCK_RUIM) conn_abre(&c, 0);
        int st = 0; long long corpo = 0;
        int ok = c.S != SOCK_RUIM && envia_tudo(c.S, t->Req, t->ReqLen) && le_resposta(&c, &st, &corpo, 0, 0) && st == t->StatusEsperado;
        long long t1 = agora_us();
        if (ok) t->NTotal++;
        if (t0 >= t->Inicio)
        {
            if (!ok) t->Erros++;
            else
            {
                if (t->NLat == t->CapLat) { t->CapLat = t->CapLat ? t->CapLat * 2 : 4096; t->Lat = (long long*)realloc(t->Lat, (size_t)t->CapLat * sizeof(long long)); }
                t->Lat[t->NLat++] = t1 - t0;
                t->Bytes += corpo;
            }
        }
        if (!ok) { conn_fecha(&c); dorme_ms(1); }
        else if (t->NovaConexao) conn_fecha(&c);
    }
    conn_libera(&c);
}

static Proc* g_proc;   // servidor em teste (amostras de CPU e memoria em volta de cada carga)

typedef struct { Trab* T; int N; long long MedidoUs; AmostraProc A0; } Carga;

static void carga_inicia(Carga* g, int n, const char* req, long long len, int nova, int status, int aquece_ms, int mede_ms)
{
    g->N = n; g->T = (Trab*)calloc((size_t)n, sizeof(Trab));
    memset(&g->A0, 0, sizeof(g->A0));
    if (g_proc) proc_amostra(g_proc, &g->A0);
    long long ini = agora_us() + (long long)aquece_ms * 1000;
    g->MedidoUs = (long long)mede_ms * 1000;
    for (int i = 0; i < n; i++)
    {
        Trab* t = &g->T[i];
        t->Req = req; t->ReqLen = len; t->NovaConexao = nova; t->StatusEsperado = status;
        t->Inicio = ini; t->Fim = ini + g->MedidoUs;
        t->T = th_criar(trab_loop, t);
    }
}

typedef struct { double Rps, MBps, P50, P99, Max, CpuUsReq, RssCresceMB; int Erros; long long N; } ResCarga;

static int cmp_ll(const void* a, const void* b) { long long x = *(const long long*)a, y = *(const long long*)b; return x < y ? -1 : x > y; }

static double pct(const long long* v, long long n, double q)
{
    if (n <= 0) return 0;
    long long i = (long long)(q * (double)(n - 1) + 0.5);
    return (double)v[i];
}

static ResCarga carga_termina(Carga* g)
{
    ResCarga r; memset(&r, 0, sizeof(r));
    long long n = 0, bytes = 0, total = 0;
    for (int i = 0; i < g->N; i++) { th_juntar(g->T[i].T); n += g->T[i].NLat; bytes += g->T[i].Bytes; total += g->T[i].NTotal; r.Erros += g->T[i].Erros; }
    if (g_proc)
    {
        // CPU DO SERVIDOR por requisicao atendida: o custo de cada uma, independente de quantos
        // nucleos a maquina tem. E o crescimento de memoria no periodo (vazamento aparece aqui).
        AmostraProc a1; proc_amostra(g_proc, &a1);
        r.CpuUsReq    = total ? (double)(a1.CpuNs - g->A0.CpuNs) / 1000.0 / (double)total : 0;
        r.RssCresceMB = (double)(a1.RssBytes - g->A0.RssBytes) / (1024.0 * 1024.0);
    }
    long long* v = (long long*)malloc((size_t)(n ? n : 1) * sizeof(long long));
    long long k = 0;
    for (int i = 0; i < g->N; i++) { memcpy(v + k, g->T[i].Lat, (size_t)g->T[i].NLat * sizeof(long long)); k += g->T[i].NLat; free(g->T[i].Lat); }
    qsort(v, (size_t)n, sizeof(long long), cmp_ll);
    double seg = (double)g->MedidoUs / 1e6;
    r.N = n; r.Rps = (double)n / seg; r.MBps = (double)bytes / (1024.0 * 1024.0) / seg;
    r.P50 = pct(v, n, 0.50); r.P99 = pct(v, n, 0.99); r.Max = n ? (double)v[n - 1] : 0;
    free(v); free(g->T);
    return r;
}

static ResCarga carga(int n, const char* req, long long len, int nova, int aquece_ms, int mede_ms)
{
    Carga g; carga_inicia(&g, n, req, len, nova, 200, aquece_ms, mede_ms);
    return carga_termina(&g);
}

// =========================================================================================
//  Registro das metricas
// =========================================================================================

#define MAX_VERSOES 2
#define MAX_METRICAS 160
#define MAX_RODADAS EST_MAX_AMOSTRAS

typedef struct
{
    char   Nome[64];
    char   Unid[12];
    int    Dir;        // +1 maior e melhor, -1 menor e melhor
    double Eps;        // soma antes da razao: metricas que podem ser 0 (threads, CPU parado)
    double Margem;     // nao-inferioridade (fracao)
    double V[MAX_VERSOES][MAX_RODADAS];
    int    Tem[MAX_VERSOES][MAX_RODADAS];
}
Metrica;

static Metrica g_met[MAX_METRICAS];
static int     g_nmet;
static int     g_ver, g_rod;         // versao e rodada em andamento
static const char* g_filtro;

static void met(const char* nome, const char* unid, int dir, double eps, double margem, double v)
{
    int i;
    for (i = 0; i < g_nmet; i++) if (!strcmp(g_met[i].Nome, nome)) break;
    if (i == g_nmet)
    {
        if (g_nmet == MAX_METRICAS) return;
        Metrica* m = &g_met[g_nmet++]; memset(m, 0, sizeof(*m));
        snprintf(m->Nome, sizeof(m->Nome), "%s", nome); snprintf(m->Unid, sizeof(m->Unid), "%s", unid);
        m->Dir = dir; m->Eps = eps; m->Margem = margem;
    }
    g_met[i].V[g_ver][g_rod] = v;
    g_met[i].Tem[g_ver][g_rod] = 1;
}

// Atalhos com as margens do plano: vazao 5%, p50 10%, p99 15%, recursos 10%.
static void met_carga(const char* cen, ResCarga r, int com_mbps)
{
    char n[64];
    if (com_mbps) { snprintf(n, sizeof(n), "%s.MBps", cen); met(n, "MB/s", +1, 0, 0.05, r.MBps); }
    else          { snprintf(n, sizeof(n), "%s.rps",  cen); met(n, "req/s", +1, 0, 0.05, r.Rps); }
    snprintf(n, sizeof(n), "%s.p50", cen);  met(n, "us", -1, 1, 0.10, r.P50);
    snprintf(n, sizeof(n), "%s.p99", cen);  met(n, "us", -1, 1, 0.15, r.P99);
    snprintf(n, sizeof(n), "%s.erros", cen); met(n, "n", -1, 1, 0.10, r.Erros);
    snprintf(n, sizeof(n), "%s.cpu_us_req", cen);    met(n, "us", -1, 1, 0.10, r.CpuUsReq);
    snprintf(n, sizeof(n), "%s.rss_cresce_MB", cen); met(n, "MB", -1, 1, 0.10, r.RssCresceMB);
}

// --filtro aceita varios trechos separados por virgula: "sse,lento" roda os dois.
static int roda(const char* cen)
{
    if (!g_filtro) return 1;
    const char* f = g_filtro;
    while (*f)
    {
        const char* v = strchr(f, ',');
        size_t n = v ? (size_t)(v - f) : strlen(f);
        char t[64]; if (n >= sizeof(t)) n = sizeof(t) - 1;
        memcpy(t, f, n); t[n] = 0;
        if (n && strstr(cen, t)) return 1;
        if (!v) break;
        f = v + 1;
    }
    return 0;
}

// =========================================================================================
//  Cenarios
// =========================================================================================

static char*  g_post1m; static long long g_post1m_len;
static char   g_req_ping[] = "GET /api/ping HTTP/1.1\r\nHost: bench\r\n\r\n";
static char   g_req_ping_close[] = "GET /api/ping HTTP/1.1\r\nHost: bench\r\nConnection: close\r\n\r\n";
static char   g_req_64k[] = "GET /bench_64k.bin HTTP/1.1\r\nHost: bench\r\n\r\n";
static char   g_req_4m[]  = "GET /bench_4m.bin HTTP/1.1\r\nHost: bench\r\n\r\n";
static char   g_req_lento[] = "GET /api/lento HTTP/1.1\r\nHost: bench\r\n\r\n";

#define LEN(s) ((long long)sizeof(s) - 1)

static void cen_ping_c1(void)   { met_carga("ping_ka_c1",  carga(1,  g_req_ping, LEN(g_req_ping), 0, 300, 2000), 0); }
static void cen_ping_nova(void) { met_carga("ping_nova_c8", carga(8, g_req_ping_close, LEN(g_req_ping_close), 1, 300, 2000), 0); }
static void cen_64k(void)       { met_carga("estatico_64k_c8", carga(8, g_req_64k, LEN(g_req_64k), 0, 300, 2000), 1); }
static void cen_4m(void)        { met_carga("estatico_4m_c4",  carga(4, g_req_4m,  LEN(g_req_4m),  0, 300, 2000), 1); }

// Tambem mede o que SOBRA ligado depois da carga: CPU com o servidor parado.
static void cen_ping_c32(void)
{
    met_carga("ping_ka_c32", carga(32, g_req_ping, LEN(g_req_ping), 0, 300, 2000), 0);
    AmostraProc x0, x1;
    dorme_ms(1000);
    proc_amostra(g_proc, &x0); dorme_ms(3000); proc_amostra(g_proc, &x1);
    met("ping_ka_c32.cpu_parado_depois_ms_3s", "ms", -1, 5, 0.10, (double)(x1.CpuNs - x0.CpuNs) / 1e6);
    met("ping_ka_c32.threads_depois", "n", -1, 1, 0.10, x1.Threads);
}

static void cen_post(void)
{
    ResCarga r = carga(4, g_post1m, g_post1m_len, 0, 300, 2000);
    r.MBps = r.Rps * (double)(1 << 20) / (1024.0 * 1024.0);   // o que conta e o corpo ENVIADO
    met_carga("post_1m_c4", r, 1);
}

// 20 requisicoes num unico send; as respostas tem de voltar na ordem (HTTP/1.1 exige).
static void cen_pipeline(void)
{
    enum { POR_LOTE = 20, LOTES = 100 };
    char* lote = (char*)malloc(64 * 1024);
    int n = 0;
    for (int k = 1; k <= POR_LOTE; k++)
    {
        n += snprintf(lote + n, 64 * 1024 - n, "POST /api/eco HTTP/1.1\r\nHost: bench\r\nContent-Type: application/octet-stream\r\nContent-Length: %d\r\n\r\n", k);
        memset(lote + n, 'x', (size_t)k); n += k;
    }
    Conn c; memset(&c, 0, sizeof(c)); c.S = SOCK_RUIM;
    int certos = 0;
    long long* lat = (long long*)malloc(LOTES * sizeof(long long)); int nl = 0;
    for (int l = 0; l < LOTES; l++)
    {
        if (c.S == SOCK_RUIM) conn_abre(&c, 0);
        long long t0 = agora_us();
        int ok = c.S != SOCK_RUIM && envia_tudo(c.S, lote, n);
        for (int k = 1; ok && k <= POR_LOTE; k++)
        {
            int st; long long cl; char corpo[64];
            if (!le_resposta(&c, &st, &cl, corpo, sizeof(corpo)) || st != 200) { ok = 0; break; }
            char esp[64]; snprintf(esp, sizeof(esp), "{\"bytes\":%d}", k);
            if (strcmp(corpo, esp) != 0) ok = 0;
        }
        if (ok) { certos++; lat[nl++] = agora_us() - t0; }
        else conn_fecha(&c);
    }
    conn_libera(&c);
    qsort(lat, (size_t)nl, sizeof(long long), cmp_ll);
    met("pipeline.ordem_ok", "%", +1, 1, 0.01, 100.0 * certos / LOTES);
    met("pipeline.lote_p50", "us", -1, 1, 0.10, pct(lat, nl, 0.5));
    free(lat); free(lote);
}

static void cen_ociosas(void)
{
    enum { N = 200 };
    Sock s[N];
    AmostraProc a0, a1, a2;
    proc_amostra(g_proc, &a0);
    for (int i = 0; i < N; i++) s[i] = conecta(0);
    dorme_ms(500);
    proc_amostra(g_proc, &a1);
    dorme_ms(3000);
    proc_amostra(g_proc, &a2);
    ResCarga r = carga(1, g_req_ping, LEN(g_req_ping), 0, 200, 1000);
    int abertas = 0;
    for (int i = 0; i < N; i++) if (s[i] != SOCK_RUIM) { abertas++; fecha_sock(s[i]); }
    met("ociosas_200.abertas",        "n",  +1, 1, 0.01, abertas);
    met("ociosas_200.threads_delta",  "n",  -1, 1, 0.10, a1.Threads - a0.Threads);
    met("ociosas_200.cpu_ms_3s",      "ms", -1, 5, 0.10, (double)(a2.CpuNs - a1.CpuNs) / 1e6);
    met("ociosas_200.rss_delta_MB",   "MB", -1, 1, 0.10, (double)(a2.RssBytes - a0.RssBytes) / (1024.0 * 1024.0));
    met("ociosas_200.ping_p50",       "us", -1, 1, 0.10, r.P50);
    dorme_ms(500);
}

// 50 assinantes SSE lidos por UMA thread com poll. Com uma thread por assinante, os 50
// acordavam juntos a cada evento e a fila do escalonador do CLIENTE entrava na latencia
// medida (quando o servidor manda para todos no mesmo instante, como num publica/assina).
#ifdef _WIN32
  typedef WSAPOLLFD PollFd;
  #define poll_sys(v, n, t) WSAPoll((v), (ULONG)(n), (t))
#else
  #include <poll.h>
  typedef struct pollfd PollFd;
  #define poll_sys(v, n, t) poll((v), (nfds_t)(n), (t))
#endif

// N assinantes (50, 200, 1000): com muitos, a entrega do servidor passa do orcamento de quem
// publica e vai em pistas. A latencia medida inclui a leitura do proprio cliente (uma thread
// percorrendo N sockets): igual para as duas versoes comparadas.
static void cen_sse_n(int N, const char* nome)
{
    static const char req[] = "GET /api/sse HTTP/1.1\r\nHost: bench\r\nAccept: text/event-stream\r\n\r\n";
    Conn* c = (Conn*)calloc((size_t)N, sizeof(Conn));
    PollFd* pf = (PollFd*)calloc((size_t)N, sizeof(PollFd));
    int* primeiro = (int*)calloc((size_t)N, sizeof(int));
    int ok = 0;
    char mn[64];
    // O 1o evento de cada assinante NAO entra na latencia: ele chega grudado no cabecalho (e
    // so seria contado quando o 2o chegasse, 100 ms depois) ou e o estado retido do topico
    // (valor ja publicado antes da assinatura). Entrega ao vivo e do 2o em diante.
    for (int i = 0; i < N; i++) primeiro[i] = 1;
    long long* lat = 0; long long nlat = 0, cap = 0;
    AmostraProc a0, a1, a2;
    proc_amostra(g_proc, &a0);

    for (int i = 0; i < N; i++)
    {
        conn_abre(&c[i], 0);
        if (c[i].S != SOCK_RUIM) envia_tudo(c[i].S, req, LEN(req));
    }
    // cabecalhos
    for (int i = 0; i < N; i++)
    {
        int fc = -1;
        while (c[i].S != SOCK_RUIM && (fc = acha_fim_cab(&c[i])) < 0) if (!conn_recv(&c[i])) break;
        if (fc >= 0 && strncmp(c[i].B + c[i].Ini, "HTTP/1.1 200", 12) == 0) { c[i].Ini = fc; ok++; }
        else conn_fecha(&c[i]);
        pf[i].fd = c[i].S; pf[i].events = POLLIN; pf[i].revents = 0;
    }

    long long t0 = agora_us();
    long long t_a1 = t0 + 700000LL, t_a2 = t0 + 3700000LL, fim = t0 + 4000000LL;
    int tem_a1 = 0, tem_a2 = 0;
    while (agora_us() < fim)
    {
        long long agora = agora_us();
        if (!tem_a1 && agora >= t_a1) { proc_amostra(g_proc, &a1); tem_a1 = 1; }
        if (!tem_a2 && agora >= t_a2) { proc_amostra(g_proc, &a2); tem_a2 = 1; }
        for (int i = 0; i < N; i++) { pf[i].fd = c[i].S; pf[i].events = c[i].S != SOCK_RUIM ? POLLIN : 0; pf[i].revents = 0; }
        int n = poll_sys(pf, N, 20);
        if (n <= 0) continue;
        for (int i = 0; i < N; i++)
        {
            if (!(pf[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            if (!conn_recv(&c[i])) { conn_fecha(&c[i]); continue; }
            long long quando = agora_us();   // o evento chegou agora (esta leitura)
            for (;;)
            {
                char* fimv = 0;
                for (int k = c[i].Ini; k + 1 < c[i].Fim; k++) if (c[i].B[k] == '\n' && c[i].B[k + 1] == '\n') { fimv = c[i].B + k; break; }
                if (!fimv) break;
                char* d = strstr(c[i].B + c[i].Ini, "data: ");
                if (d && d < fimv && primeiro[i]) primeiro[i] = 0;
                else if (d && d < fimv)
                {
                    if (nlat == cap) { cap = cap ? cap * 2 : 4096; lat = (long long*)realloc(lat, (size_t)cap * sizeof(long long)); }
                    lat[nlat++] = quando - atoll(d + 6);
                }
                c[i].Ini = (int)(fimv - c[i].B) + 2;
            }
        }
    }
    if (!tem_a1) proc_amostra(g_proc, &a1);
    if (!tem_a2) proc_amostra(g_proc, &a2);
    for (int i = 0; i < N; i++) conn_libera(&c[i]);
    if (lat) qsort(lat, (size_t)nlat, sizeof(long long), cmp_ll);
    snprintf(mn, sizeof(mn), "%s.assinantes_ok", nome); met(mn, "n",  +1, 1, 0.01, ok);
    snprintf(mn, sizeof(mn), "%s.threads_delta", nome); met(mn, "n",  -1, 1, 0.10, a1.Threads - a0.Threads);
    snprintf(mn, sizeof(mn), "%s.cpu_ms_3s", nome);     met(mn, "ms", -1, 5, 0.10, (double)(a2.CpuNs - a1.CpuNs) / 1e6);
    snprintf(mn, sizeof(mn), "%s.entrega_p50", nome);   met(mn, "us", -1, 1, 0.10, pct(lat, nlat, 0.5));
    snprintf(mn, sizeof(mn), "%s.entrega_p99", nome);   met(mn, "us", -1, 1, 0.15, pct(lat, nlat, 0.99));
    free(lat); free(c); free(pf); free(primeiro);
    dorme_ms(500);   // o servidor antigo so percebe a saida no proximo envio (ate 100 ms)
}

static void cen_sse(void)      { cen_sse_n(50, "sse_50"); }
static void cen_sse_200(void)  { cen_sse_n(200, "sse_200"); }
static void cen_sse_1000(void) { cen_sse_n(1000, "sse_1000"); }

// Rotas longas (200 ms) ocupando o servidor enquanto pings curtos chegam.
static void cen_lento(void)
{
    Carga lentos; carga_inicia(&lentos, 16, g_req_lento, LEN(g_req_lento), 0, 200, 0, 2600);
    dorme_ms(300);
    ResCarga r = carga(4, g_req_ping, LEN(g_req_ping), 0, 0, 2000);
    ResCarga rl = carga_termina(&lentos);
    met_carga("lento_mix.ping", r, 0);
    met("lento_mix.lento_rps", "req/s", +1, 0, 0.05, rl.Rps);
}

// Clientes que pedem 4 MB e NAO leem: o servidor tem de aguentar sem travar os outros e sem
// segurar memoria sem limite.
static void cen_cliente_lento(void)
{
    enum { N = 20 };
    Sock s[N];
    AmostraProc a0, a1;
    proc_amostra(g_proc, &a0);
    for (int i = 0; i < N; i++) { s[i] = conecta(4096); if (s[i] != SOCK_RUIM) envia_tudo(s[i], g_req_4m, LEN(g_req_4m)); }
    dorme_ms(1000);
    proc_amostra(g_proc, &a1);
    ResCarga r = carga(4, g_req_ping, LEN(g_req_ping), 0, 200, 2000);
    for (int i = 0; i < N; i++) if (s[i] != SOCK_RUIM) fecha_sock(s[i]);
    met("cliente_lento.threads_delta", "n",  -1, 1, 0.10, a1.Threads - a0.Threads);
    met("cliente_lento.rss_delta_MB",  "MB", -1, 1, 0.10, (double)(a1.RssBytes - a0.RssBytes) / (1024.0 * 1024.0));
    met_carga("cliente_lento.ping", r, 0);
    dorme_ms(500);
}

// Upload grande: vazao e o PICO de memoria do servidor durante o envio.
typedef struct { volatile int Para; long long Pico; long long Base; Th T; } Amostrador;
static void amostrador_loop(void* p)
{
    Amostrador* a = (Amostrador*)p;
    while (!a->Para)
    {
        AmostraProc x; proc_amostra(g_proc, &x);
        if (x.RssBytes > a->Pico) a->Pico = x.RssBytes;
        dorme_ms(20);
    }
}

static void cen_upload(void)
{
    const long long TOTAL = 200LL << 20;
    AmostraProc a0; proc_amostra(g_proc, &a0);
    Amostrador am; memset(&am, 0, sizeof(am)); am.Base = a0.RssBytes; am.Pico = a0.RssBytes;
    am.T = th_criar(amostrador_loop, &am);

    Conn c; memset(&c, 0, sizeof(c)); conn_abre(&c, 0);
    char cab[256];
    int n = snprintf(cab, sizeof(cab), "POST /api/eco HTTP/1.1\r\nHost: bench\r\nContent-Type: application/octet-stream\r\nContent-Length: %lld\r\n\r\n", TOTAL);
    char* bloco = (char*)malloc(1 << 20); memset(bloco, 'u', 1 << 20);
    long long t0 = agora_us();
    int ok = c.S != SOCK_RUIM && envia_tudo(c.S, cab, n);
    for (long long f = TOTAL; ok && f > 0; f -= (1 << 20)) ok = envia_tudo(c.S, bloco, f > (1 << 20) ? (1 << 20) : f);
    int st = 0; long long cl = 0; char corpo[64] = { 0 };
    ok = ok && le_resposta(&c, &st, &cl, corpo, sizeof(corpo)) && st == 200;
    long long t1 = agora_us();
    am.Para = 1; th_juntar(am.T);
    char esp[64]; snprintf(esp, sizeof(esp), "{\"bytes\":%lld}", TOTAL);
    ok = ok && !strcmp(corpo, esp);
    conn_libera(&c); free(bloco);
    met("upload_200m.ok",           "n",    +1, 1, 0.01, ok);
    met("upload_200m.MBps",         "MB/s", +1, 0, 0.05, ok ? 200.0 / ((double)(t1 - t0) / 1e6) : 0);
    met("upload_200m.rss_pico_MB",  "MB",   -1, 1, 0.10, (double)(am.Pico - am.Base) / (1024.0 * 1024.0));
    dorme_ms(300);
}

// =========================================================================================
//  Orquestracao
// =========================================================================================

static int espera_porta(int ms)
{
    long long fim = agora_us() + (long long)ms * 1000;
    while (agora_us() < fim)
    {
        Sock s = conecta(0);
        if (s != SOCK_RUIM) { fecha_sock(s); return 1; }
        dorme_ms(50);
    }
    return 0;
}

// Cada cenario num processo NOVO. No servidor antigo a memoria cresce a cada requisicao (heap
// de thread morta no memory_pool) e o processo cai depois de algumas dezenas de milhares:
// rodando tudo num processo so, um cenario media o estrago do anterior -- e depois de cair,
// os seguintes nao mediam nada. Isolado, cada cenario mede so a si mesmo, e o crescimento
// aparece na metrica rss_cresce_MB de cada um.
typedef struct { const char* Nome; void (*Fn)(void); } Cenario;

static void cen_processo(void)
{
    AmostraProc a0, a1;
    proc_amostra(g_proc, &a0); dorme_ms(3000); proc_amostra(g_proc, &a1);
    met("processo.threads_parado",    "n",  -1, 1, 0.10, a0.Threads);
    met("processo.rss_parado_MB",     "MB", -1, 1, 0.10, (double)a0.RssBytes / (1024.0 * 1024.0));
    met("processo.cpu_parado_ms_3s",  "ms", -1, 5, 0.10, (double)(a1.CpuNs - a0.CpuNs) / 1e6);
}

static const Cenario CENARIOS[] = {
    { "processo",        cen_processo },
    { "ping_ka_c1",      cen_ping_c1 },
    { "ping_ka_c32",     cen_ping_c32 },
    { "ping_nova_c8",    cen_ping_nova },
    { "estatico_64k_c8", cen_64k },
    { "estatico_4m_c4",  cen_4m },
    { "post_1m_c4",      cen_post },
    { "pipeline",        cen_pipeline },
    { "ociosas_200",     cen_ociosas },
    { "sse_50",          cen_sse },
    { "sse_200",         cen_sse_200 },
    { "sse_1000",        cen_sse_1000 },
    { "lento_mix",       cen_lento },
    { "cliente_lento",   cen_cliente_lento },
    { "upload_200m",     cen_upload },
};

static int g_porta_base, g_porta_seq;

static int rodada(const char* exe, const char* web)
{
    const char* dbg = getenv("HTTPBENCH_DEBUG");
    int falhas = 0;
    for (int i = 0; i < (int)(sizeof(CENARIOS) / sizeof(CENARIOS[0])); i++)
    {
        const Cenario* c = &CENARIOS[i];
        if (!roda(c->Nome)) continue;

        Proc p; memset(&p, 0, sizeof(p));
        g_porta = g_porta_base + (g_porta_seq++ % 500);   // porta nova: nada de TIME_WAIT da anterior
        g_proc = &p;
        if (!proc_iniciar(&p, exe, g_porta, web)) { fprintf(stderr, "nao subiu: %s\n", exe); return 0; }
        if (!espera_porta(15000)) { fprintf(stderr, "porta %d nao abriu\n", g_porta); proc_matar(&p); falhas++; continue; }
        dorme_ms(300);

        c->Fn();

        char n[64]; snprintf(n, sizeof(n), "%s.sobreviveu", c->Nome);
        int vivo = proc_vivo(&p);
        met(n, "n", +1, 1, 0.01, vivo);
        if (dbg && dbg[0] == '1')
        {
            AmostraProc a; proc_amostra(&p, &a);
            fprintf(stderr, "  [debug] %-16s vivo=%d threads=%d rss=%.1f MB cpu=%.0f ms\n",
                    c->Nome, vivo, a.Threads, (double)a.RssBytes / (1024.0 * 1024.0), (double)a.CpuNs / 1e6);
        }
        proc_matar(&p);
        g_proc = 0;
    }
    return falhas == 0;
}

static void cria_arquivo(const char* dir, const char* nome, long long tam)
{
    char cam[1024]; snprintf(cam, sizeof(cam), "%s/%s", dir, nome);
    FILE* f = fopen(cam, "rb");
    if (f) { fseek(f, 0, SEEK_END); long long t = ftell(f); fclose(f); if (t == tam) return; }
    f = fopen(cam, "wb");
    if (!f) { fprintf(stderr, "nao criou %s\n", cam); exit(2); }
    char b[65536];
    for (int i = 0; i < (int)sizeof(b); i++) b[i] = (char)(i * 31 + 7);
    for (long long f2 = tam; f2 > 0; f2 -= sizeof(b)) fwrite(b, 1, f2 > (long long)sizeof(b) ? sizeof(b) : (size_t)f2, f);
    fclose(f);
}

// ---- saida -----------------------------------------------------------------------------

static FILE* g_txt;
static void out(const char* fmt, ...)
{
    va_list a;
    va_start(a, fmt); vprintf(fmt, a); va_end(a);
    if (g_txt) { va_start(a, fmt); vfprintf(g_txt, fmt, a); va_end(a); }
}

static double mediana_de(Metrica* m, int v, int nrod, double* mn, double* mx)
{
    double x[MAX_RODADAS]; int n = 0;
    for (int r = 0; r < nrod; r++) if (m->Tem[v][r]) x[n++] = m->V[v][r];
    if (!n) { *mn = *mx = 0; return 0; }
    *mn = *mx = x[0];
    for (int i = 1; i < n; i++) { if (x[i] < *mn) *mn = x[i]; if (x[i] > *mx) *mx = x[i]; }
    return est_mediana(x, n);
}

static void relatorio_um(const char* rot, int nrod)
{
    out("\n%s: %d rodada(s). Mediana [min .. max].\n\n", rot, nrod);
    out("%-34s %14s %14s %14s  %s\n", "metrica", "mediana", "min", "max", "unid");
    for (int i = 0; i < g_nmet; i++)
    {
        Metrica* m = &g_met[i]; double mn, mx;
        double md = mediana_de(m, 0, nrod, &mn, &mx);
        out("%-34s %14.1f %14.1f %14.1f  %s\n", m->Nome, md, mn, mx, m->Unid);
    }
}

static int cmp_d(const void* a, const void* b) { double x = *(const double*)a, y = *(const double*)b; return x < y ? -1 : x > y; }

static void relatorio_par(const char* ra, const char* rb, int nrod)
{
    double p[MAX_METRICAS]; int sig[MAX_METRICAS];
    double hl[MAX_METRICAS], lo[MAX_METRICAS], hi[MAX_METRICAS];
    int nd[MAX_METRICAS];
    for (int i = 0; i < g_nmet; i++)
    {
        Metrica* m = &g_met[i];
        double d[MAX_RODADAS]; int n = 0;
        for (int r = 0; r < nrod; r++)
            if (m->Tem[0][r] && m->Tem[1][r])
            {
                double a = m->V[0][r] + m->Eps, b = m->V[1][r] + m->Eps;
                if (a <= 0) a = 1e-9;
                if (b <= 0) b = 1e-9;
                d[n++] = log(b / a);
            }
        nd[i] = n;
        if (n < 2) { p[i] = 1; hl[i] = lo[i] = hi[i] = 0; continue; }
        hl[i] = est_hodges_lehmann(d, n);
        p[i] = est_wilcoxon_p(d, n);
        // IC 95% do efeito por bootstrap das rodadas
        enum { B = 2000 };
        static double bs[B];
        EstRng rng = { 0x9E3779B97F4A7C15ULL ^ (uint64_t)(i + 1) };
        for (int b = 0; b < B; b++)
        {
            double s[MAX_RODADAS];
            for (int k = 0; k < n; k++) s[k] = d[est_rng_int(&rng, n)];
            bs[b] = est_hodges_lehmann(s, n);
        }
        qsort(bs, B, sizeof(double), cmp_d);
        lo[i] = bs[(int)(0.025 * B)]; hi[i] = bs[(int)(0.975 * B) - 1];
    }
    est_benjamini_hochberg(p, g_nmet, 0.05, sig);

    out("\nComparacao pareada: A = %s, B = %s, %d rodada(s) com ordem alternada.\n", ra, rb, nrod);
    out("Efeito = B/A (Hodges-Lehmann), IC 95%% por bootstrap; p = Wilcoxon exato; * = significativo com BH (q=5%%).\n");
    out("Veredito: MELHOR/PIOR exige IC inteiro de um lado e BH; 'nao pior' = pior limite do IC dentro da margem.\n\n");
    out("%-34s %12s %12s %9s %21s %8s  %-12s %s\n", "metrica", "A (med)", "B (med)", "efeito", "IC 95%", "p", "veredito", "unid");
    for (int i = 0; i < g_nmet; i++)
    {
        Metrica* m = &g_met[i]; double mn, mx;
        double ma = mediana_de(m, 0, nrod, &mn, &mx), mb = mediana_de(m, 1, nrod, &mn, &mx);
        double ef = (exp(hl[i]) - 1) * 100, el = (exp(lo[i]) - 1) * 100, eh = (exp(hi[i]) - 1) * 100;
        // "pior" no sentido da metrica: maior e melhor -> pior e cair
        double pior_lim = m->Dir > 0 ? -el : eh;          // quanto, no maximo, pode ter piorado (%)
        int melhor = m->Dir > 0 ? (lo[i] > 0) : (hi[i] < 0);
        int pior   = m->Dir > 0 ? (hi[i] < 0) : (lo[i] > 0);
        const char* v;
        if (nd[i] < 2)                     v = "sem dados";
        else if (melhor && sig[i])         v = "MELHOR";
        else if (pior && sig[i])           v = pior_lim > m->Margem * 100 ? "PIOR" : "pior<margem";
        else if (pior_lim <= m->Margem * 100) v = "nao pior";
        else                               v = "inconclusivo";
        char ic[48]; snprintf(ic, sizeof(ic), "[%+.1f%% .. %+.1f%%]", el, eh);
        out("%-34s %12.1f %12.1f %+8.1f%% %21s %7.3f%s  %-12s %s\n", m->Nome, ma, mb, ef, ic, p[i], sig[i] ? "*" : " ", v, m->Unid);
    }
}

// --env-a / --env-b K=V: variavel de ambiente so do servidor daquela versao (o MESMO binario em
// duas configuracoes). Aplicada antes de subir o processo de cada rodada; a da outra sai.
#define MAX_ENV 4
static const char* g_env[MAX_VERSOES][MAX_ENV];
static int         g_nenv[MAX_VERSOES];

static void env_poe(const char* kv, int liga)
{
    char k[128]; const char* ig = strchr(kv, '=');
    int n = ig ? (int)(ig - kv) : (int)strlen(kv);
    if (n <= 0 || n >= (int)sizeof(k)) return;
    memcpy(k, kv, (size_t)n); k[n] = 0;
#ifdef _WIN32
    char b[512];
    snprintf(b, sizeof(b), "%s=%s", k, liga && ig ? ig + 1 : "");
    _putenv(b);
#else
    if (liga && ig) setenv(k, ig + 1, 1); else unsetenv(k);
#endif
}

static void env_da_versao(int v)
{
    for (int o = 0; o < MAX_VERSOES; o++) if (o != v) for (int i = 0; i < g_nenv[o]; i++) env_poe(g_env[o][i], 0);
    for (int i = 0; i < g_nenv[v]; i++) env_poe(g_env[v][i], 1);
}

int main(int argc, char** argv)
{
    const char* exe[MAX_VERSOES] = { 0 }; const char* rot[MAX_VERSOES] = { "A", "B" };
    int nexe = 0, nrot = 0, nrod = 5, porta = 19000;
    const char* web = 0; const char* saida = "httpbench";
    for (int i = 1; i + 1 < argc; i += 2)
    {
        if (!strcmp(argv[i], "--exe") && nexe < MAX_VERSOES) exe[nexe++] = argv[i + 1];
        else if (!strcmp(argv[i], "--rotulo") && nrot < MAX_VERSOES) rot[nrot++] = argv[i + 1];
        else if (!strcmp(argv[i], "--web")) web = argv[i + 1];
        else if (!strcmp(argv[i], "--rodadas")) nrod = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--saida")) saida = argv[i + 1];
        else if (!strcmp(argv[i], "--filtro")) g_filtro = argv[i + 1];
        else if (!strcmp(argv[i], "--porta")) porta = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--env-a") && g_nenv[0] < MAX_ENV) g_env[0][g_nenv[0]++] = argv[i + 1];
        else if (!strcmp(argv[i], "--env-b") && g_nenv[1] < MAX_ENV) g_env[1][g_nenv[1]++] = argv[i + 1];
    }
    if (!nexe || !web) { fprintf(stderr, "uso: httpbench --exe <srv> [--exe <srv2>] --web <pasta> [--rodadas N] [--saida prefixo] [--filtro txt]\n"); return 2; }
    if (nrod < 1) nrod = 1;
    if (nrod > MAX_RODADAS) nrod = MAX_RODADAS;

#ifdef _WIN32
    WSADATA wd; WSAStartup(MAKEWORD(2, 2), &wd);
    timeBeginPeriod(1);
#else
    signal(SIGPIPE, SIG_IGN);
#endif
    calibra_tsc();
    g_porta_base = porta;
    cria_arquivo(web, "bench_64k.bin", 64 * 1024);
    cria_arquivo(web, "bench_4m.bin", 4 * 1024 * 1024);

    long long ncab = 0;
    char cab[256];
    ncab = snprintf(cab, sizeof(cab), "POST /api/eco HTTP/1.1\r\nHost: bench\r\nContent-Type: application/octet-stream\r\nContent-Length: %d\r\n\r\n", 1 << 20);
    g_post1m_len = ncab + (1 << 20);
    g_post1m = (char*)malloc((size_t)g_post1m_len);
    memcpy(g_post1m, cab, (size_t)ncab); memset(g_post1m + ncab, 'p', 1 << 20);

    char cam[1024];
    snprintf(cam, sizeof(cam), "%s.txt", saida); g_txt = fopen(cam, "w");

    for (int r = 0; r < nrod; r++)
    {
        for (int k = 0; k < nexe; k++)
        {
            int v = (r % 2 == 0) ? k : nexe - 1 - k;   // ordem alternada entre rodadas
            g_ver = v; g_rod = r;
            long long t0 = agora_us();
            printf("rodada %d/%d  %s ... ", r + 1, nrod, rot[v]); fflush(stdout);
            env_da_versao(v);
            if (!rodada(exe[v], web)) { printf("FALHOU\n"); continue; }
            printf("%.0f s\n", (double)(agora_us() - t0) / 1e6);
        }
    }

    if (nexe == 1) relatorio_um(rot[0], nrod);
    else relatorio_par(rot[0], rot[1], nrod);

    snprintf(cam, sizeof(cam), "%s.tsv", saida);
    FILE* t = fopen(cam, "w");
    if (t)
    {
        fprintf(t, "versao\trodada\tmetrica\tvalor\tunid\n");
        for (int i = 0; i < g_nmet; i++)
            for (int v = 0; v < nexe; v++)
                for (int r = 0; r < nrod; r++)
                    if (g_met[i].Tem[v][r]) fprintf(t, "%s\t%d\t%s\t%.3f\t%s\n", rot[v], r + 1, g_met[i].Nome, g_met[i].V[v][r], g_met[i].Unid);
        fclose(t);
    }
    if (g_txt) fclose(g_txt);
    return 0;
}
