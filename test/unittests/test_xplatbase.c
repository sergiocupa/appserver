//  Testes do xplatbase, em C.
//
//  O que eles guardam: a partir da preparacao de instancia unica, TODO modulo do processo
//  usa o mesmo pool de memoria e o mesmo pool de tarefas. Se alguem linkar o xplatbase
//  estaticamente de novo, o primeiro teste aqui falha -- e essa e a ideia.

#include "testes.h"
#include "xplatbase.h"
#include "memory_pool.h"
#include "thread_pool.h"
#include "atomics.h"
#include <string.h>

void teste_instancia_unica(TestResult* r)
{
    XplatInstanceInfo primeira;
    t_start(r);

    T_ASSERT(r, xplat_instance_check(&primeira) == 0,
             "ha outra instancia do xplatbase no processo (a primeira veio de %s)", primeira.Module);
}

void teste_instancia_vem_da_compartilhada(TestResult* r)
{
    const char* modulo;
    t_start(r);

    modulo = xplat_instance_module();
    T_ASSERT(r, modulo != 0 && modulo[0] != '\0', "modulo da instancia nao identificado");

    // Se isto falhar, o teste esta rodando contra uma copia ESTATICA -- o que derruba a
    // garantia inteira assim que houver plugin no processo.
    T_ASSERT(r, strstr(modulo, "Xplatbase.dll") != 0 || strstr(modulo, "libxplatbase.so") != 0,
             "esperava a biblioteca compartilhada; a instancia veio de '%s'", modulo);
}

void teste_memoria_contabiliza_alocacao(TestResult* r)
{
    MemPoolStats antes, depois;
    void* blocos[8];
    int i;

    t_start(r);

    memop_get_stats(&antes);
    for (i = 0; i < 8; i++) blocos[i] = memop_alloc_raw(4096);
    memop_get_stats(&depois);

    for (i = 0; i < 8; i++) T_ASSERT(r, blocos[i] != 0, "alocacao %d devolveu nulo", i);
    T_ASSERT(r, depois.alloc_count - antes.alloc_count >= 8,
             "esperava ao menos 8 alocacoes contabilizadas, vieram %llu",
             (unsigned long long)(depois.alloc_count - antes.alloc_count));

    memop_get_stats(&antes);
    for (i = 0; i < 8; i++) memop_free_raw(blocos[i]);
    memop_get_stats(&depois);

    T_ASSERT(r, depois.free_count - antes.free_count >= 8,
             "esperava ao menos 8 liberacoes contabilizadas, vieram %llu",
             (unsigned long long)(depois.free_count - antes.free_count));
}

void teste_memoria_escreve_e_le(TestResult* r)
{
    const uint64 tamanho = 64 * 1024;   // acima da classe pequena: outro caminho do pool
    unsigned char* p;
    uint64 i;

    t_start(r);

    p = (unsigned char*)memop_alloc_raw(tamanho);
    T_ASSERT(r, p != 0, "alocacao de %llu bytes devolveu nulo", (unsigned long long)tamanho);

    for (i = 0; i < tamanho; i++) p[i] = (unsigned char)(i & 0xFF);
    for (i = 0; i < tamanho; i++)
    {
        if (p[i] != (unsigned char)(i & 0xFF))
        {
            memop_free_raw(p);
            T_ASSERT(r, 0, "byte %llu voltou %u, esperava %u",
                     (unsigned long long)i, (unsigned)p[i], (unsigned)(i & 0xFF));
        }
    }
    memop_free_raw(p);
}

// ---- pool de tarefas --------------------------------------------------------

static xatomic_int g_contador;

static void tarefa_incrementa(void* arg)
{
    (void)arg;
    atomic_add_inline(&g_contador, 1);
}

void teste_pool_executa_tarefas(TestResult* r)
{
    ThreadPool* pool;
    const int total = 64;
    int i;

    t_start(r);

    atomic_set_inline(&g_contador, 0);

    pool = pool_create_relative(2);
    T_ASSERT(r, pool != 0, "nao foi possivel criar o pool de tarefas");

    for (i = 0; i < total; i++) pool_submit_relative(pool, tarefa_incrementa, 0);
    pool_wait_idle_relative(pool);

    i = atomic_get_inline(&g_contador);
    pool_destroy_relative(pool);

    T_ASSERT(r, i == total, "esperava %d tarefas executadas, foram %d", total, i);
}
