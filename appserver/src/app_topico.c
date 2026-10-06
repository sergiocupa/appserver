//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  CAMADA 3 (aplicacao): topicos (publica/assina, com estado retido) e jobs (trabalho longo,
//  um por chave, que publica no topico de mesmo nome).
//
//  O problema que resolvem: um processamento de video durava minutos DENTRO do handler, que
//  segurava a thread e a conexao escrevendo SSE no socket. Se o navegador recarregava, o
//  fluxo se perdia (o job continuava, ninguem mais via), e cada acompanhante custava uma
//  thread. Agora:
//
//    - o job roda na pista longa e so PUBLICA ("start", "progress", "done"...);
//    - quem quer acompanhar ASSINA: a rota e curta, retorna na hora, e a conexao fica aberta
//      no reator sem thread nenhuma;
//    - quem assina recebe primeiro o RETIDO: tudo o que acumula (start, track-done, done,
//      error) na ordem, e o ultimo valor de cada slot que substitui (progresso por pista).
//      Recarregar a pagina no meio do job mostra o estado atual e segue dali;
//    - fim do job: o topico encerra, os fluxos fecham; o retido fica RETENCAO_MS para quem
//      chegar atrasado (ex.: reconectou logo depois do "done").

#include "../include/appserver.h"
#include "http/http_canal.h"
#include "http/http_conexao.h"
#include "atomics.h"
#include "memory_pool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  static long long agora_ms(void) { return (long long)GetTickCount64(); }
#else
  #include <time.h>
  static long long agora_ms(void)
  {
      struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
      return (long long)t.tv_sec * 1000LL + t.tv_nsec / 1000000;
  }
#endif

#define RETENCAO_MS (5 * 60 * 1000)

typedef struct Item { char* Slot; char* Dados; struct Item* Prox; } Item;

typedef struct Topico
{
    char*          Nome;
    Item          *Acum, *AcumFim;   // em ordem de publicacao
    Item*          Subst;            // um por slot
    AppCanal**     Ass;
    int            NAss, CapAss;
    bool           Encerrado;
    long long      EncerradoEm;
    bool           JobRodando;
    struct Topico* Prox;
}
Topico;

struct AppJob
{
    AppServerInfo* Srv;
    char*          Chave;
    AppJobFn       Fn;
    void*          Arg;
    void         (*Liberar)(void*);
};

// ---- memoria (memory_pool do xplatbase: liberar de outra thread e suportado) -----------------

static char* copia_str(const char* s) { if (!s) return 0; size_t n = strlen(s) + 1; char* d = (char*)memop_alloc_raw(n); if (d) memcpy(d, s, n); return d; }

static void itens_libera(Item* i) { while (i) { Item* p = i->Prox; memop_free_raw(i->Slot); memop_free_raw(i->Dados); memop_free_raw(i); i = p; } }

static void retido_limpa(Topico* t)
{
    itens_libera(t->Acum); itens_libera(t->Subst);
    t->Acum = t->AcumFim = t->Subst = 0;
}

// ---- registro (sob TopicosLock) ---------------------------------------------------------------

static void fecha_assinantes(Topico* t)
{
    for (int i = 0; i < t->NAss; i++) app_canal_fechar(t->Ass[i]);
    t->NAss = 0;
}

// Tira topicos encerrados ha mais de RETENCAO_MS, sem ninguem.
static void purga(AppServerInfo* s)
{
    long long agora = agora_ms();
    Topico** pp = (Topico**)&s->Topicos;
    while (*pp)
    {
        Topico* t = *pp;
        if (t->Encerrado && !t->JobRodando && t->NAss == 0 && agora - t->EncerradoEm > RETENCAO_MS)
        {
            *pp = t->Prox;
            retido_limpa(t); memop_free_raw(t->Ass); memop_free_raw(t->Nome); memop_free_raw(t);
            continue;
        }
        pp = &t->Prox;
    }
}

static Topico* acha(AppServerInfo* s, const char* nome, bool cria)
{
    for (Topico* t = (Topico*)s->Topicos; t; t = t->Prox) if (!strcmp(t->Nome, nome)) return t;
    if (!cria) return 0;
    purga(s);
    Topico* t = (Topico*)memop_calloc_raw(1, sizeof(Topico));
    if (!t) return 0;
    t->Nome = copia_str(nome);
    t->Prox = (Topico*)s->Topicos;
    s->Topicos = t;
    return t;
}

// Tira da lista os assinantes cuja conexao caiu (o envio falhou ou a conexao fechou).
static void poda(Topico* t)
{
    int j = 0;
    for (int i = 0; i < t->NAss; i++)
    {
        if (app_canal_ativo(t->Ass[i])) t->Ass[j++] = t->Ass[i];
        else app_canal_fechar(t->Ass[i]);
    }
    t->NAss = j;
}

static bool envia_item(AppCanal* k, const Item* it) { return app_canal_evento(k, 0, it->Dados); }

// ---- entrega -------------------------------------------------------------------------------
// O evento vem formatado uma vez; para cada assinante e um net_enviar: um send() que nao
// bloqueia (o que o socket nao aceitar vai para a fila daquele cliente, que o reator escoa).
//
// A mesma regra do reator: quem publica entrega ele mesmo ate Config.SseVoltaUs. Poucos
// assinantes cabem nisso e nenhuma thread acorda (bateria). Passou do tempo: o custo medido
// por envio diz quantos assinantes cabem numa pista (limitado por SseMaxPorPista), o resto e
// dividido em pistas e vai ao pool -- e quem publica continua pegando pistas tambem.
//
// So retorna com TODAS as pistas entregues, ainda sob TopicosLock: o proximo evento do topico
// nao comeca antes deste acabar (a ordem por assinante se mantem), nenhum canal e fechado no
// meio de um envio, e nao se forma fila de eventos: com muitos assinantes, quem publica espera
// a entrega (pressao para tras), nao empilha.
//
// Tarefa que so roda depois que as pistas acabaram nao toca em canal nem no evento: pega
// nada e solta a referencia. Por isso a Entrega vive no heap, contada.

#define SSE_AJUDANTES_MAX 8   // tarefas por evento, alem de quem publica

#ifdef _WIN32
  static long long agora_us(void)
  {
      static LARGE_INTEGER f; LARGE_INTEGER c;
      if (!f.QuadPart) QueryPerformanceFrequency(&f);
      QueryPerformanceCounter(&c);
      return (long long)(c.QuadPart / f.QuadPart) * 1000000LL + (long long)((c.QuadPart % f.QuadPart) * 1000000LL / f.QuadPart);
  }
  static void cede(void) { SwitchToThread(); }
#else
  #include <sched.h>
  static long long agora_us(void)
  {
      struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
      return (long long)t.tv_sec * 1000000LL + t.tv_nsec / 1000;
  }
  static void cede(void) { sched_yield(); }
#endif

typedef struct Entrega
{
    xmutex_t    Lock;
    AppCanal**  Ass;        // valido enquanto quem publica nao retorna (segura TopicosLock)
    int         N, K;       // assinantes; tamanho da pista
    int         Prox;       // primeiro assinante ainda sem pista
    int         EmCurso;    // pistas sendo entregues agora
    int         Refs;       // quem publica + tarefas que ainda nao sairam
    const byte* D;
    int         Tam;
}
Entrega;

static bool pega_pista(Entrega* e, int* ini, int* fim)
{
    thread_mutex_lock_inline(&e->Lock);
    bool ok = e->Prox < e->N;
    if (ok) { *ini = e->Prox; *fim = e->Prox + e->K < e->N ? e->Prox + e->K : e->N; e->Prox = *fim; e->EmCurso++; }
    thread_mutex_unlock_inline(&e->Lock);
    return ok;
}

static void entrega_pistas(Entrega* e)
{
    int ini, fim;
    while (pega_pista(e, &ini, &fim))
    {
        for (int i = ini; i < fim; i++) http_canal_enviar_pronto(e->Ass[i], e->D, e->Tam);
        thread_mutex_lock_inline(&e->Lock);
        e->EmCurso--;
        thread_mutex_unlock_inline(&e->Lock);
    }
}

static void solta(Entrega* e)
{
    thread_mutex_lock_inline(&e->Lock);
    int r = --e->Refs;
    thread_mutex_unlock_inline(&e->Lock);
    if (r == 0) { thread_mutex_destroy_inline(&e->Lock); memop_free_raw(e); }
}

static void tarefa_pista(void* arg) { Entrega* e = (Entrega*)arg; entrega_pistas(e); solta(e); }

static void entrega(AppServerInfo* s, AppCanal** ass, int n, const byte* d, int tam)
{
    int orc = s->Config.SseVoltaUs;
    long long t0 = agora_us();
    int i = 0;
    while (i < n)
    {
        http_canal_enviar_pronto(ass[i++], d, tam);
        if (orc > 0 && agora_us() - t0 >= orc) break;
    }
    if (i >= n) return;

    // assinantes por pista: os que cabem no mesmo orcamento, pelo custo medido ate aqui
    long long gasto = agora_us() - t0;
    int k = gasto > 0 ? (int)((long long)orc * i / gasto) : i;
    int max = s->Config.SseMaxPorPista;
    if (max > 0 && k > max) k = max;
    if (k < 1) k = 1;
    int resto = n - i;
    int ajudantes = (resto + k - 1) / k - 1;   // quem publica tambem pega uma pista
    if (ajudantes > SSE_AJUDANTES_MAX) ajudantes = SSE_AJUDANTES_MAX;
    Entrega* e = ajudantes > 0 ? (Entrega*)memop_calloc_raw(1, sizeof(Entrega)) : 0;
    if (!e)
    {
        for (; i < n; i++) http_canal_enviar_pronto(ass[i], d, tam);
        return;
    }
    thread_mutex_init_inline(&e->Lock);
    e->Ass = ass + i; e->N = resto; e->K = k; e->D = d; e->Tam = tam;
    e->Refs = 1 + ajudantes;
    ThreadPool* p = s->Config.Pool;
    int foram = 0;
    for (int a = 0; a < ajudantes; a++)
        if (p ? pool_submit_relative(p, tarefa_pista, e) : pool_submit(tarefa_pista, e)) foram++;
    if (foram < ajudantes)
    {
        thread_mutex_lock_inline(&e->Lock);
        e->Refs -= ajudantes - foram;
        thread_mutex_unlock_inline(&e->Lock);
    }
    atomic_add_inline(&s->SsePistas, foram);

    entrega_pistas(e);
    // as pistas que outras threads pegaram ainda podem estar enviando: espera (sao curtas,
    // no maximo um orcamento cada)
    for (int volta = 0;; volta++)
    {
        thread_mutex_lock_inline(&e->Lock);
        bool fim = e->EmCurso == 0;
        thread_mutex_unlock_inline(&e->Lock);
        if (fim) break;
        if (volta < 64) xcpu_pause(); else cede();
    }
    solta(e);
}

// ---- API ------------------------------------------------------------------------------------

void app_publicar(AppServerInfo* s, const char* topico, const char* slot, const char* dados)
{
    if (!s || !topico || !dados) return;
    thread_mutex_lock_inline(&s->TopicosLock);
    Topico* t = acha(s, topico, true);
    if (t && !t->Encerrado)
    {
        // retem
        Item* it = 0;
        if (slot)
        {
            for (it = t->Subst; it; it = it->Prox) if (!strcmp(it->Slot, slot)) break;
            if (it) { memop_free_raw(it->Dados); it->Dados = copia_str(dados); }
            else { it = (Item*)memop_calloc_raw(1, sizeof(Item)); if (it) { it->Slot = copia_str(slot); it->Dados = copia_str(dados); it->Prox = t->Subst; t->Subst = it; } }
        }
        else
        {
            it = (Item*)memop_calloc_raw(1, sizeof(Item));
            if (it) { it->Dados = copia_str(dados); if (t->AcumFim) t->AcumFim->Prox = it; else t->Acum = it; t->AcumFim = it; }
        }
        // entrega: o evento e formatado UMA vez e o mesmo buffer vai a todos (net_enviar nao
        // bloqueia: o que o socket de um cliente lento nao aceitar fica na fila dele)
        if (t->NAss > 0)
        {
            ResourceBuffer ev; resource_buffer_init(&ev);
            http_canal_formata(&ev, 0, dados);
            entrega(s, t->Ass, t->NAss, ev.Data, ev.Length);
            resource_buffer_release(&ev, true);
            poda(t);
        }
    }
    thread_mutex_unlock_inline(&s->TopicosLock);
}

bool app_assinar(Message* request, const char* topico)
{
    if (!request || !request->Client || !topico) return false;
    AppServerInfo* s = request->Client->Server;
    AppCanal* k = app_canal_sse(request);
    if (!k) return false;
    http_canal_destacar(request);   // o topico guarda o canal: o handler pode retornar

    thread_mutex_lock_inline(&s->TopicosLock);
    Topico* t = acha(s, topico, true);
    if (!t) { thread_mutex_unlock_inline(&s->TopicosLock); app_canal_fechar(k); return false; }
    // estado retido: o que acumula, em ordem, depois o ultimo de cada slot
    for (Item* it = t->Acum; it; it = it->Prox) envia_item(k, it);
    for (Item* it = t->Subst; it; it = it->Prox) envia_item(k, it);
    if (t->Encerrado)
    {
        thread_mutex_unlock_inline(&s->TopicosLock);
        app_canal_fechar(k);   // ja acabou: quem chegou atrasado recebe o retido e o fim
        return true;
    }
    if (t->NAss == t->CapAss)
    {
        int cap = t->CapAss ? t->CapAss * 2 : 8;
        AppCanal** n = (AppCanal**)memop_realloc_raw(t->Ass, (size_t)cap * sizeof(AppCanal*));
        if (!n) { thread_mutex_unlock_inline(&s->TopicosLock); app_canal_fechar(k); return false; }
        t->Ass = n; t->CapAss = cap;
    }
    t->Ass[t->NAss++] = k;
    thread_mutex_unlock_inline(&s->TopicosLock);
    return true;
}

void app_topico_encerrar(AppServerInfo* s, const char* topico)
{
    thread_mutex_lock_inline(&s->TopicosLock);
    Topico* t = acha(s, topico, false);
    if (t && !t->Encerrado)
    {
        t->Encerrado = true;
        t->EncerradoEm = agora_ms();
        fecha_assinantes(t);
    }
    thread_mutex_unlock_inline(&s->TopicosLock);
}

int app_topico_assinantes(AppServerInfo* s, const char* topico)
{
    thread_mutex_lock_inline(&s->TopicosLock);
    Topico* t = acha(s, topico, false);
    if (t) poda(t);
    int n = t ? t->NAss : 0;
    thread_mutex_unlock_inline(&s->TopicosLock);
    return n;
}

int app_topico_pistas(AppServerInfo* s) { return s ? atomic_get_inline(&s->SsePistas) : 0; }

// ---- jobs -----------------------------------------------------------------------------------

const char* app_job_chave(AppJob* job) { return job ? job->Chave : 0; }

void app_job_publicar(AppJob* job, const char* slot, const char* dados)
{
    if (job) app_publicar(job->Srv, job->Chave, slot, dados);
}

bool app_job_rodando(AppServerInfo* s, const char* chave)
{
    thread_mutex_lock_inline(&s->TopicosLock);
    Topico* t = acha(s, chave, false);
    bool r = t && t->JobRodando;
    thread_mutex_unlock_inline(&s->TopicosLock);
    return r;
}

static void job_roda(void* arg)
{
    AppJob* j = (AppJob*)arg;
    AppServerInfo* s = j->Srv;
    // job e trabalho pesado (video): com o servidor em economia, o pool fica em performance
    // enquanto ele roda (contado: varios jobs ao mesmo tempo nao se atropelam)
    bool perf = s->Config.JobsEmPerformance;
    if (perf) app_perfil_performance_inicio(s);
    j->Fn(j, j->Arg);
    if (perf) app_perfil_performance_fim(s);

    thread_mutex_lock_inline(&s->TopicosLock);
    Topico* t = acha(s, j->Chave, false);
    if (t) t->JobRodando = false;
    thread_mutex_unlock_inline(&s->TopicosLock);
    app_topico_encerrar(s, j->Chave);

    if (j->Liberar) j->Liberar(j->Arg);
    memop_free_raw(j->Chave);
    memop_free_raw(j);
}

bool app_job_iniciar(AppServerInfo* s, const char* chave, AppJobFn fn, void* arg, void (*liberar)(void*))
{
    if (!s || !chave || !fn) { if (liberar) liberar(arg); return false; }
    thread_mutex_lock_inline(&s->TopicosLock);
    Topico* t = acha(s, chave, true);
    if (!t || t->JobRodando)
    {
        thread_mutex_unlock_inline(&s->TopicosLock);
        if (liberar) liberar(arg);
        return false;   // um por chave: quem pediu de novo so assina o que ja esta rodando
    }
    // rodada nova: o retido da anterior sai (quem ja assinava continua, agora vendo esta)
    retido_limpa(t);
    t->Encerrado = false;
    t->JobRodando = true;
    thread_mutex_unlock_inline(&s->TopicosLock);

    AppJob* j = (AppJob*)memop_calloc_raw(1, sizeof(AppJob));
    j->Srv = s; j->Chave = copia_str(chave); j->Fn = fn; j->Arg = arg; j->Liberar = liberar;
    if (!http_pista_longa(s, job_roda, j)) job_roda(j);
    return true;
}
