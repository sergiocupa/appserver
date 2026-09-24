//  Instancia unica vista de FORA: carregando modulos em tempo de execucao.
//
//  Os testes do test_xplatbase.c olham para dentro do proprio processo. Estes aqui provam o
//  que interessa para o futuro de plugin de codec:
//    POSITIVO  - um modulo que consome a biblioteca COMPARTILHADA usa o mesmo pool e o mesmo
//                registro de threads do host;
//    NEGATIVO  - um modulo que linka o xplatbase ESTATICAMENTE e DENUNCIADO. Um detector que
//                nunca foi visto acusando nao vale nada.

#include "testes.h"
#include "xplatbase.h"
#include "memory_pool.h"
#include "thread_handler.h"
#include "thread_pool.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
  #define MOD_COMPARTILHADO "mod_compartilhado.dll"
  #define MOD_DUPLICADO     "mod_duplicado.dll"
  typedef HMODULE Modulo;
  static Modulo abre(const char* nome)          { return LoadLibraryA(nome); }
  static void*  simbolo(Modulo m, const char* s){ return (void*)GetProcAddress(m, s); }
  static void   fecha(Modulo m)                 { FreeLibrary(m); }
  static void   ambiente(const char* n, const char* v) { _putenv_s(n, v); }
#else
  #include <dlfcn.h>
  #define MOD_COMPARTILHADO "./libmod_compartilhado.so"
  #define MOD_DUPLICADO     "./libmod_duplicado.so"
  typedef void* Modulo;
  static Modulo abre(const char* nome)          { return dlopen(nome, RTLD_NOW | RTLD_LOCAL); }
  static void*  simbolo(Modulo m, const char* s){ return dlsym(m, s); }
  static void   fecha(Modulo m)                 { dlclose(m); }
  static void   ambiente(const char* n, const char* v) { setenv(n, v, 1); }
#endif

typedef void (*FnAloca)(int, unsigned long long);
typedef void (*FnLibera)(int);
typedef void (*FnModulo)(char*, int);
typedef int  (*FnDuplicata)(char*, int);
typedef int  (*FnSubmete)(void (*)(void*), void*, int);
typedef void (*FnPoolDims)(int*, int*);
typedef int  (*FnThreadSobe)(void);
typedef void (*FnThreadDesce)(void);

static int g_threads_contadas;
static void conta(const Thread* t, void* ctx) { (void)t; (void)ctx; g_threads_contadas++; }

static int conta_threads(void)
{
    g_threads_contadas = 0;
    thread_enum(conta, 0);
    return g_threads_contadas;
}

// ---- POSITIVO ---------------------------------------------------------------

void teste_modulo_compartilhado_mesmo_pool(TestResult* r)
{
    Modulo m;
    FnAloca aloca; FnLibera libera; FnModulo modulo; FnDuplicata duplicata;
    MemPoolStats antes, depois;
    char do_modulo[300] = { 0 }, primeiro[300] = { 0 };
    unsigned long long alocou, liberou;

    t_start(r);

    m = abre(MOD_COMPARTILHADO);
    T_ASSERT(r, m != 0, "nao foi possivel carregar %s", MOD_COMPARTILHADO);

    aloca     = (FnAloca)simbolo(m, "mod_aloca");
    libera    = (FnLibera)simbolo(m, "mod_libera");
    modulo    = (FnModulo)simbolo(m, "mod_modulo");
    duplicata = (FnDuplicata)simbolo(m, "mod_duplicata");
    if (!aloca || !libera || !modulo || !duplicata) { fecha(m); T_ASSERT(r, 0, "exports ausentes em %s", MOD_COMPARTILHADO); }

    // 1) o modulo tem de enxergar a MESMA instancia
    modulo(do_modulo, (int)sizeof(do_modulo));
    if (duplicata(primeiro, (int)sizeof(primeiro))) { fecha(m); T_ASSERT(r, 0, "o modulo acusou duplicata (primeira em %s)", primeiro); }
    if (strcmp(do_modulo, xplat_instance_module()) != 0)
    {
        char meu[300]; snprintf(meu, sizeof(meu), "%s", xplat_instance_module());
        fecha(m);
        T_ASSERT(r, 0, "modulos diferentes: host '%s', modulo '%s'", meu, do_modulo);
    }

    // 2) o que ele aloca tem de aparecer na conta do host
    memop_get_stats(&antes);
    aloca(16, 4096);
    memop_get_stats(&depois);
    alocou = depois.alloc_count - antes.alloc_count;

    memop_get_stats(&antes);
    libera(16);
    memop_get_stats(&depois);
    liberou = depois.free_count - antes.free_count;

    fecha(m);

    T_ASSERT(r, alocou >= 16, "as alocacoes do modulo nao entraram na conta do host (delta %llu)", alocou);
    T_ASSERT(r, liberou >= 16, "as liberacoes do modulo nao entraram na conta do host (delta %llu)", liberou);
}

void teste_modulo_compartilhado_mesmo_registro_de_threads(TestResult* r)
{
    Modulo m;
    FnThreadSobe sobe; FnThreadDesce desce;
    int antes, com_thread, depois, ok;

    t_start(r);

    m = abre(MOD_COMPARTILHADO);
    T_ASSERT(r, m != 0, "nao foi possivel carregar %s", MOD_COMPARTILHADO);

    sobe  = (FnThreadSobe)simbolo(m, "mod_thread_sobe");
    desce = (FnThreadDesce)simbolo(m, "mod_thread_desce");
    if (!sobe || !desce) { fecha(m); T_ASSERT(r, 0, "exports de thread ausentes"); }

    // A thread fica VIVA enquanto contamos: a versao anterior deste teste encerrava antes
    // de contar, o numero voltava ao mesmo e nao provava nada.
    antes = conta_threads();
    ok = sobe();
    com_thread = conta_threads();
    desce();
    depois = conta_threads();

    fecha(m);

    T_ASSERT(r, ok, "o modulo nao conseguiu criar a thread");
    T_ASSERT(r, com_thread > antes,
             "thread criada no modulo nao apareceu no registro do host (%d -> %d)", antes, com_thread);
    T_ASSERT(r, depois <= antes + 1, "a thread do modulo nao saiu do registro (%d -> %d)", antes, depois);
}

// ---- pool de tarefas --------------------------------------------------------
// Memoria era metade da pergunta; esta e a outra. A tarefa e do HOST, quem submete e o
// MODULO: se o pool fosse outro, ou nao rodaria, ou rodaria em thread que o host desconhece.

#define MAX_IDS 64

/* Nao ha "id de thread" na API; o registro guarda o handle nativo (Thread.Thr). Entao a
   comparacao e feita no termo de cada sistema. */
#ifdef _WIN32
  typedef DWORD IdNativo;
  static IdNativo id_atual(void)                   { return GetCurrentThreadId(); }
  static int     id_do_registro(const Thread* t, IdNativo id) { return GetThreadId(t->Thr) == id; }
#else
  typedef pthread_t IdNativo;
  static IdNativo id_atual(void)                   { return pthread_self(); }
  static int     id_do_registro(const Thread* t, IdNativo id) { return pthread_equal(t->Thr, id) != 0; }
#endif

static volatile long g_executadas;
static IdNativo      g_ids[MAX_IDS];
static xmutex_t      g_mtx_ids;
static int           g_mtx_pronto;

static void tarefa_do_host(void* arg)
{
    (void)arg;
    thread_mutex_lock(&g_mtx_ids);
    if (g_executadas < MAX_IDS) g_ids[g_executadas] = id_atual();
    g_executadas++;
    thread_mutex_unlock(&g_mtx_ids);
}

static IdNativo g_procurado;
static int      g_achou;
static void procura(const Thread* t, void* ctx)
{
    (void)ctx;
    if (t && id_do_registro(t, g_procurado)) g_achou = 1;
}

void teste_modulo_usa_mesmo_pool_de_tarefas(TestResult* r)
{
    Modulo m;
    FnSubmete submete; FnPoolDims dims;
    int w_host = 0, c_host = 0, w_mod = -1, c_mod = -1, aceitas, i, desconhecidas = 0;
    const int QUANTAS = 32;

    t_start(r);

    m = abre(MOD_COMPARTILHADO);
    T_ASSERT(r, m != 0, "nao foi possivel carregar %s", MOD_COMPARTILHADO);

    submete = (FnSubmete)simbolo(m, "mod_submete");
    dims    = (FnPoolDims)simbolo(m, "mod_pool_dims");
    if (!submete || !dims) { fecha(m); T_ASSERT(r, 0, "exports de pool ausentes"); }

    if (!g_mtx_pronto) { thread_mutex_init(&g_mtx_ids); g_mtx_pronto = 1; }
    g_executadas = 0;

    pool_dims(&w_host, &c_host);
    dims(&w_mod, &c_mod);

    aceitas = submete(tarefa_do_host, 0, QUANTAS);
    pool_wait_idle();

    // as threads que executaram tem de estar no registro do HOST
    for (i = 0; i < (int)g_executadas && i < MAX_IDS; i++)
    {
        g_procurado = g_ids[i]; g_achou = 0;
        thread_enum(procura, 0);
        if (!g_achou) desconhecidas++;
    }

    fecha(m);

    T_ASSERT(r, w_mod == w_host && c_mod == c_host,
             "o modulo enxerga outro pool (host %d/%d, modulo %d/%d)", w_host, c_host, w_mod, c_mod);
    T_ASSERT(r, aceitas == QUANTAS, "o pool recusou tarefas submetidas pelo modulo (%d de %d)", aceitas, QUANTAS);
    T_ASSERT(r, (int)g_executadas == QUANTAS,
             "tarefas submetidas pelo modulo nao executaram (%d de %d)", (int)g_executadas, QUANTAS);
    T_ASSERT(r, desconhecidas == 0,
             "%d tarefa(s) rodaram em thread fora do registro do host", desconhecidas);
}

// ---- NEGATIVO ---------------------------------------------------------------

void teste_duplicata_e_denunciada(TestResult* r)
{
    Modulo m;
    FnDuplicata verifica;
    char primeiro[300] = { 0 };
    int dup;

    t_start(r);

    // O xplatbase se inicializa na CARGA do modulo, entao a politica precisa estar definida
    // ANTES: sem isto, a copia duplicada abortaria o processo (o padrao em Debug).
    ambiente("XPLATBASE_DUPLICATE_FATAL", "0");

    m = abre(MOD_DUPLICADO);
    T_ASSERT(r, m != 0, "nao foi possivel carregar %s", MOD_DUPLICADO);

    verifica = (FnDuplicata)simbolo(m, "dup_verifica");
    if (!verifica) { fecha(m); T_ASSERT(r, 0, "export 'dup_verifica' ausente"); }

    dup = verifica(primeiro, (int)sizeof(primeiro));
    fecha(m);

    T_ASSERT(r, dup == 1,
             "a SEGUNDA instancia passou despercebida (o modulo reportou vir de '%s')", primeiro);
    T_ASSERT(r, primeiro[0] != '\0', "a denuncia nao informou de onde veio a primeira instancia");
}

// ---- ciclo -------------------------------------------------------------------
// Carregar e descarregar repetidamente e o que expoe acumulo: thread que nao morre,
// handle que nao fecha, memoria que nao volta. Foi assim que apareceram, nesta preparacao,
// o travamento do executor de testes e a morte no FreeLibrary.

static unsigned handles_do_processo(void)
{
#ifdef _WIN32
    DWORD n = 0;
    return GetProcessHandleCount(GetCurrentProcess(), &n) ? (unsigned)n : 0u;
#else
    return 0u;   /* no Linux a contagem equivalente nao e necessaria para este teste */
#endif
}

void teste_ciclo_carrega_descarrega(TestResult* r)
{
    const int CICLOS = 50;
    MemPoolStats antes, depois;
    long long vivos_antes, vivos_depois;
    int threads_antes, threads_depois, i;
    unsigned handles_antes, handles_depois;

    t_start(r);

    /* uma carga antes de medir: a primeira traz custo unico (cache do carregador) */
    { Modulo m = abre(MOD_COMPARTILHADO); if (m) fecha(m); }

    memop_get_stats(&antes);
    vivos_antes   = (long long)antes.alloc_count - (long long)antes.free_count;
    threads_antes = conta_threads();
    handles_antes = handles_do_processo();

    for (i = 0; i < CICLOS; i++)
    {
        Modulo m = abre(MOD_COMPARTILHADO);
        FnAloca aloca; FnLibera libera;
        if (!m) T_ASSERT(r, 0, "falhou ao carregar o modulo no ciclo %d", i);
        aloca  = (FnAloca)simbolo(m, "mod_aloca");
        libera = (FnLibera)simbolo(m, "mod_libera");
        if (aloca && libera) { aloca(8, 2048); libera(8); }
        fecha(m);
    }

    memop_get_stats(&depois);
    vivos_depois   = (long long)depois.alloc_count - (long long)depois.free_count;
    threads_depois = conta_threads();
    handles_depois = handles_do_processo();

    T_ASSERT(r, threads_depois <= threads_antes + 1,
             "threads cresceram em %d ciclos: %d -> %d", CICLOS, threads_antes, threads_depois);
    T_ASSERT(r, vivos_depois <= vivos_antes + 16,
             "memoria viva cresceu em %d ciclos: %lld -> %lld", CICLOS, vivos_antes, vivos_depois);
    if (handles_antes > 0)
        T_ASSERT(r, handles_depois <= handles_antes + 8,
                 "handles cresceram em %d ciclos: %u -> %u", CICLOS, handles_antes, handles_depois);
}
