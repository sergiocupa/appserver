//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Pista: ver pista.h.

#include "pista.h"
#include "xplatbase.h"   // thread_create (thread gerenciada: o memory_pool conhece) e xmutex_t
#include <stdlib.h>

#ifdef _WIN32
  typedef CONDITION_VARIABLE PistaCond;
  static void cond_init(PistaCond* c)              { InitializeConditionVariable(c); }
  static void cond_espera(PistaCond* c, xmutex_t* m) { SleepConditionVariableCS(c, m, INFINITE); }
  static void cond_acorda(PistaCond* c)            { WakeConditionVariable(c); }
#else
  #include <pthread.h>
  typedef pthread_cond_t PistaCond;
  static void cond_init(PistaCond* c)              { pthread_cond_init(c, 0); }
  static void cond_espera(PistaCond* c, xmutex_t* m) { pthread_cond_wait(c, m); }
  static void cond_acorda(PistaCond* c)            { pthread_cond_signal(c); }
#endif

typedef struct Trabalho { void (*Fn)(void*); void* Arg; struct Trabalho* Prox; } Trabalho;

struct Pista
{
    xmutex_t  Lock;
    PistaCond Tem;          // ha trabalho na fila
    Trabalho *Cab, *Cauda;
    int       NaFila;
    int       Threads, Livres, Max;
};

static xthread_result_t pista_thread(void* arg)
{
    Pista* p = (Pista*)arg;
    thread_mutex_lock_inline(&p->Lock);
    for (;;)
    {
        while (!p->Cab)
        {
            p->Livres++;
            cond_espera(&p->Tem, &p->Lock);   // dorme no sistema: zero CPU ate chegar trabalho
            p->Livres--;
        }
        Trabalho* t = p->Cab;
        p->Cab = t->Prox;
        if (!p->Cab) p->Cauda = 0;
        p->NaFila--;
        thread_mutex_unlock_inline(&p->Lock);

        t->Fn(t->Arg);
        memop_free_raw(t);

        thread_mutex_lock_inline(&p->Lock);
    }
    return (xthread_result_t)0;   // nao chega: as threads vivem com o processo
}

Pista* pista_criar(int max_threads)
{
    Pista* p = (Pista*)memop_calloc_raw(1, sizeof(Pista));
    if (!p) return 0;
    thread_mutex_init_inline(&p->Lock);
    cond_init(&p->Tem);
    p->Max = max_threads > 0 ? max_threads : 1;
    return p;
}

bool pista_submeter(Pista* p, void (*fn)(void*), void* arg)
{
    Trabalho* t = (Trabalho*)memop_alloc_raw(sizeof(Trabalho));
    if (!t) return false;
    t->Fn = fn; t->Arg = arg; t->Prox = 0;

    thread_mutex_lock_inline(&p->Lock);
    if (p->Cauda) p->Cauda->Prox = t; else p->Cab = t;
    p->Cauda = t;
    p->NaFila++;
    // Thread livre: acorda uma. Nenhuma livre e ainda cabe: cria (sob demanda).
    bool criar = p->Livres == 0 && p->Threads < p->Max;
    if (criar) p->Threads++;
    else cond_acorda(&p->Tem);
    thread_mutex_unlock_inline(&p->Lock);

    if (criar)
    {
        int st = 0;
        if (!thread_create(pista_thread, p, &st))
        {
            thread_mutex_lock_inline(&p->Lock);
            p->Threads--;
            bool sem_ninguem = p->Threads == 0;
            thread_mutex_unlock_inline(&p->Lock);
            // sem nenhuma thread o trabalho ficaria parado para sempre: avisa quem submeteu
            if (sem_ninguem)
            {
                thread_mutex_lock_inline(&p->Lock);
                // tira o trabalho que acabou de entrar (e o ultimo da fila)
                Trabalho** pp = &p->Cab; Trabalho* ant = 0;
                while (*pp && *pp != t) { ant = *pp; pp = &(*pp)->Prox; }
                if (*pp) { *pp = t->Prox; if (p->Cauda == t) p->Cauda = ant; p->NaFila--; }
                thread_mutex_unlock_inline(&p->Lock);
                memop_free_raw(t);
                return false;
            }
        }
    }
    return true;
}

int pista_threads(Pista* p) { thread_mutex_lock_inline(&p->Lock); int n = p->Threads; thread_mutex_unlock_inline(&p->Lock); return n; }
int pista_na_fila(Pista* p) { thread_mutex_lock_inline(&p->Lock); int n = p->NaFila; thread_mutex_unlock_inline(&p->Lock); return n; }
