//  Servidor MINIMO para medir o appserver: so as rotas que o httpbench exercita, sem codec,
//  sem gateway, sem painel. O que ele mede e o servidor (recepcao, despacho, resposta), nao
//  as aplicacoes em cima dele.
//
//  Uso: bench_server <porta> <pasta_web>
//
//  Rotas:
//    GET  /api/ping    {"ok":1}
//    POST /api/eco     {"bytes":N}      N = tamanho do corpo recebido
//    GET  /api/lento   dorme 200 ms e responde {"ok":1}  (rota longa no meio das curtas)
//    GET  /api/sse     SSE (assinatura do topico "relogio"): "data: <relogio_us>" a cada 100 ms
//    GET  /<arquivo>   estatico da pasta web
//
//  O relogio do SSE e o MESMO do httpbench (QPC no Windows, CLOCK_MONOTONIC no Linux),
//  valido entre processos: o cliente mede o atraso de entrega subtraindo.

#include "appserver.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <windows.h>
  static long long agora_us(void)
  {
      static LARGE_INTEGER f; LARGE_INTEGER c;
      if (!f.QuadPart) QueryPerformanceFrequency(&f);
      QueryPerformanceCounter(&c);
      return (long long)(c.QuadPart / f.QuadPart) * 1000000LL + (long long)((c.QuadPart % f.QuadPart) * 1000000LL / f.QuadPart);
  }
  static void dorme_ms(int ms) { Sleep((DWORD)ms); }
#else
  #include <time.h>
  static long long agora_us(void)
  {
      struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
      return (long long)t.tv_sec * 1000000LL + t.tv_nsec / 1000;
  }
  static void dorme_ms(int ms) { struct timespec t = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&t, 0); }
#endif


static void responde(Message* m, const char* json)
{
    m->Response = message_response_create_content(200, APPLICATION_JSON, (char*)json, (int)strlen(json));
}

static Element* rota_ping(Message* m)
{
    responde(m, "{\"ok\":1}");
    return 0;
}

static Element* rota_eco(Message* m)
{
    char r[64];
    snprintf(r, sizeof(r), "{\"bytes\":%lld}", (long long)m->ContentLength);
    responde(m, r);
    return 0;
}

static Element* rota_lento(Message* m)
{
    dorme_ms(200);
    responde(m, "{\"ok\":1}");
    return 0;
}

// SSE por ASSINATURA: a rota e curta (retorna na hora) e a conexao fica aberta no reator,
// sem thread. Um unico publicador (a main, abaixo) manda o relogio a cada 100 ms para todos
// os assinantes do topico. No servidor antigo cada assinante era um handler segurando uma
// thread num laco de sleep + send.
static Element* rota_sse(Message* m)
{
    app_assinar(m, "relogio");
    return 0;
}


int main(int argc, char** argv)
{
    if (argc < 3) { fprintf(stderr, "uso: bench_server <porta> <pasta_web>\n"); return 2; }

    FunctionBindList* bind = bind_list_create();
    app_add_receiver(bind, "ping",  rota_ping,  true);
    app_add_receiver(bind, "eco",   rota_eco,   true);
    app_add_receiver_longa(bind, "lento", rota_lento);   // dorme 200 ms: pista longa
    app_add_receiver(bind, "sse",   rota_sse,   true);   // curta: so assina

    AppServerConfig cfg = appserver_config_default();
    cfg.AgentName      = "bench";
    cfg.Port           = atoi(argv[1]);
    cfg.Prefix         = "api";
    cfg.WebContentPath = argv[2];
    cfg.MaxClients     = 2048;
    // BENCH_PERFIL=economia|performance: perfil do pool (o httpbench mede os dois). Sem a
    // variavel, o padrao da biblioteca (nao mexe no pool).
    {
        const char* p = getenv("BENCH_PERFIL");
        if (p && (p[0] == 'e' || p[0] == 'E')) cfg.PerfilPool = POOL_PERFIL_ECONOMIA;
        else if (p && (p[0] == 'p' || p[0] == 'P')) cfg.PerfilPool = POOL_PERFIL_PERFORMANCE;
    }

    // BENCH_SSE_VOLTA_US / BENCH_SSE_MAX_PISTA: entrega SSE (0 = em serie). Sem a variavel, o padrao.
    {
        const char* v = getenv("BENCH_SSE_VOLTA_US");
        if (v) cfg.SseVoltaUs = atoi(v);
        v = getenv("BENCH_SSE_MAX_PISTA");
        if (v) cfg.SseMaxPorPista = atoi(v);
    }

    AppServerInfo* s = appserver_create(&cfg, bind);
    if (!s) return 1;
    fflush(stdout);
    // BENCH_PUB_LOG=1: a cada 50 publicacoes com assinantes, o tempo de app_publicar (que so
    // retorna com o evento entregue a todos) -- o custo da entrega medido no proprio servidor.
    const char* plog = getenv("BENCH_PUB_LOG");
    int pms = getenv("BENCH_PUB_MS") ? atoi(getenv("BENCH_PUB_MS")) : 100;
    long long dur[50]; int nd = 0;
    // publicador do relogio (slot "t": so o ultimo valor fica retido)
    for (;;)
    {
        char ev[32];
        long long t0 = agora_us();
        snprintf(ev, sizeof(ev), "%lld", t0);
        app_publicar(s, "relogio", "t", ev);
        if (plog)
        {
            int ass = app_topico_assinantes(s, "relogio");
            if (ass > 0) dur[nd++] = agora_us() - t0;
            if (nd == 50)
            {
                for (int i = 1; i < nd; i++) for (int j = i; j > 0 && dur[j] < dur[j - 1]; j--) { long long x = dur[j]; dur[j] = dur[j - 1]; dur[j - 1] = x; }
                fprintf(stderr, "[pub] assinantes=%d p50=%lld p90=%lld max=%lld us pistas=%d\n", ass, dur[25], dur[45], dur[49], app_topico_pistas(s));
                fflush(stderr);
                nd = 0;
            }
        }
        dorme_ms(pms);
    }
}
