//  Modulo auxiliar do teste POSITIVO: uma biblioteca carregada em tempo de execucao que
//  consome o xplatbase COMPARTILHADO. Se a instancia for mesmo unica, o que este modulo
//  aloca aparece na contabilidade lida pelo host, e a thread que ele cria aparece no
//  registro de threads do host.
//
//  E o cenario do plugin de codec, reduzido ao essencial.

#include "xplatbase.h"
#include "memory_pool.h"
#include "thread_handler.h"
#include "thread_pool.h"
#include "thread_wait.h"
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
  #define EXPORTA __declspec(dllexport)
#else
  #define EXPORTA __attribute__((visibility("default")))
#endif

#define MAX_BLOCOS 64

static void*  g_blocos[MAX_BLOCOS];
static Thread* g_thread;
static xwait_t g_sai;              // sinal para a thread terminar
static volatile int g_encerrar;

EXPORTA void mod_aloca(int quantos, unsigned long long tamanho)
{
    int i;
    for (i = 0; i < quantos && i < MAX_BLOCOS; i++) g_blocos[i] = memop_alloc_raw((uint64)tamanho);
}

EXPORTA void mod_libera(int quantos)
{
    int i;
    for (i = 0; i < quantos && i < MAX_BLOCOS; i++) { memop_free_raw(g_blocos[i]); g_blocos[i] = 0; }
}

EXPORTA void mod_modulo(char* out, int tam)
{
    snprintf(out, (size_t)tam, "%s", xplat_instance_module());
}

EXPORTA int mod_duplicata(char* primeiro, int tam)
{
    XplatInstanceInfo first;
    int dup = xplat_instance_check(&first);
    if (dup && primeiro) snprintf(primeiro, (size_t)tam, "%s", first.Module);
    return dup;
}

// ---- thread que fica VIVA ---------------------------------------------------
// O teste anterior criava e encerrava a thread antes de contar, e o registro voltava ao
// mesmo numero -- nao provava nada. Aqui a thread espera o sinal, entao da para contar
// com ela viva.

static xthread_result_t corpo(void* arg)
{
    (void)arg;
    while (!g_encerrar)
    {
        thread_wait_prepare_inline(&g_sai);
        if (g_encerrar) break;
        thread_wait_sleep_for_inline(&g_sai, 20000);
    }
    return (xthread_result_t)0;
}

EXPORTA int mod_thread_sobe(void)
{
    int st = 0;
    if (g_thread) return 1;
    g_encerrar = 0;
    memset((void*)&g_sai, 0, sizeof(g_sai));
    g_thread = thread_create(corpo, 0, &st);
    return g_thread != 0;
}

EXPORTA void mod_thread_desce(void)
{
    if (!g_thread) return;
    g_encerrar = 1;
    thread_wait_wake_inline(&g_sai);
    thread_join(&g_thread);
    g_thread = 0;
}

// ---- pool de TAREFAS --------------------------------------------------------
// A outra metade da pergunta: memoria E task. O host passa a propria funcao; se o pool for
// o mesmo, ela roda em um worker que o host conhece.

EXPORTA int mod_submete(void (*fn)(void*), void* arg, int quantas)
{
    int i, aceitas = 0;
    for (i = 0; i < quantas; i++) if (pool_submit((pool_task_fn)fn, arg)) aceitas++;
    return aceitas;
}

EXPORTA void mod_pool_dims(int* workers, int* core)
{
    pool_dims(workers, core);
}
