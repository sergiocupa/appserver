//  Testes da camada de rede do servidor (appserver/src/net).
//
//  Rodam com sockets de verdade em 127.0.0.1, porta escolhida pelo sistema: nada de rede
//  externa, nada fixo que colida com um servidor ja rodando na maquina.

#include "testes.h"
#include "../../appserver/src/net/net_poll.h"
#include "thread_handler.h"
#include <string.h>

#ifdef _WIN32
  #include <windows.h>
  static unsigned long long agora_ms(void) { return GetTickCount64(); }
  static void dorme_ms(int ms) { Sleep((DWORD)ms); }
#else
  #include <time.h>
  static unsigned long long agora_ms(void)
  {
      struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
      return (unsigned long long)t.tv_sec * 1000ull + (unsigned long long)(t.tv_nsec / 1000000);
  }
  static void dorme_ms(int ms) { struct timespec t = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&t, 0); }
#endif

static void rede_init(void)
{
#ifdef _WIN32
    WSADATA d; WSAStartup(MAKEWORD(2, 2), &d);
#endif
}

// Par de sockets TCP conectados: *cli e o lado "cliente", *srv o aceito.
static int par_tcp(SOCKET* cli, SOCKET* srv)
{
    SOCKET l = socket(AF_INET, SOCK_STREAM, 0);
    if (l == INVALID_SOCKET) return 0;
    struct sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
    socklen_t n = sizeof(a);
    if (bind(l, (struct sockaddr*)&a, sizeof(a)) != 0 || listen(l, 4) != 0 ||
        getsockname(l, (struct sockaddr*)&a, &n) != 0) { closesocket(l); return 0; }
    *cli = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(*cli, (struct sockaddr*)&a, sizeof(a)) != 0) { closesocket(l); closesocket(*cli); return 0; }
    *srv = accept(l, 0, 0);
    closesocket(l);
    return *srv != INVALID_SOCKET;
}


static NetPoll* g_poll_thread;
static xthread_result_t acorda_depois(void* arg)
{
    (void)arg;
    dorme_ms(100);
    net_poll_acordar(g_poll_thread);
    return (xthread_result_t)0;
}

void teste_poll_acordar(TestResult* r)
{
    t_start(r);
    rede_init();
    NetPoll* p = net_poll_criar();
    T_ASSERT(r, p != 0, "net_poll_criar falhou");
    NetPollEvento ev[8];

    // Sem nada registrado e sem acordar: espera o tempo todo e volta 0.
    unsigned long long t0 = agora_ms();
    int n = net_poll_esperar(p, ev, 8, 80);
    unsigned long long dt = agora_ms() - t0;
    T_ASSERT(r, n == 0, "esperar sem eventos devolveu %d", n);
    T_ASSERT(r, dt >= 60, "esperar(80 ms) voltou em %llu ms, antes do tempo", dt);

    // Acordar ANTES de esperar nao se perde; varios viram um so.
    net_poll_acordar(p); net_poll_acordar(p); net_poll_acordar(p);
    t0 = agora_ms();
    n = net_poll_esperar(p, ev, 8, 5000);
    dt = agora_ms() - t0;
    T_ASSERT(r, n == 0 && dt < 1000, "acordar pendente nao fez esperar voltar (n=%d, %llu ms)", n, dt);

    // Depois de consumido, o proximo esperar volta a esperar de verdade.
    t0 = agora_ms();
    n = net_poll_esperar(p, ev, 8, 80);
    dt = agora_ms() - t0;
    T_ASSERT(r, n == 0 && dt >= 60, "acordar ficou 'grudado' (%llu ms)", dt);

    // De OUTRA thread, com o reator ja parado em esperar sem limite.
    g_poll_thread = p;
    int st = 0;
    Thread* th = thread_create(acorda_depois, 0, &st);
    T_ASSERT(r, th != 0, "thread_create falhou");
    t0 = agora_ms();
    n = net_poll_esperar(p, ev, 8, -1);
    dt = agora_ms() - t0;
    thread_join(&th);
    T_ASSERT(r, n == 0 && dt >= 50 && dt < 3000, "acordar de outra thread: n=%d em %llu ms", n, dt);

    net_poll_destruir(p);
}

void teste_poll_leitura_e_desarme(TestResult* r)
{
    t_start(r);
    rede_init();
    SOCKET cli, srv;
    T_ASSERT(r, par_tcp(&cli, &srv), "nao conseguiu montar o par TCP em loopback");
    net_set_nonblocking(srv, 1);

    NetPoll* p = net_poll_criar();
    T_ASSERT(r, p != 0, "net_poll_criar falhou");
    int marca = 42;
    NetPollEvento ev[8];

    T_ASSERT(r, net_poll_definir(p, srv, NET_POLL_LER, &marca), "definir LER falhou");
    int n = net_poll_esperar(p, ev, 8, 50);
    T_ASSERT(r, n == 0, "LER sem dado nenhum reportou %d evento(s)", n);

    send(cli, "abc", 3, 0);
    n = net_poll_esperar(p, ev, 8, 2000);
    T_ASSERT(r, n == 1, "dado chegou e esperar devolveu %d", n);
    T_ASSERT(r, ev[0].Dado == &marca, "Dado do evento nao e o registrado");
    T_ASSERT(r, (ev[0].Eventos & NET_POLL_LER) != 0, "evento sem LER (0x%x)", ev[0].Eventos);

    // Desarmado: o dado continua la, sem ler, e nada e reportado.
    T_ASSERT(r, net_poll_definir(p, srv, 0, &marca), "desarmar falhou");
    n = net_poll_esperar(p, ev, 8, 80);
    T_ASSERT(r, n == 0, "desarmado ainda reportou %d evento(s)", n);

    // Rearmado: por nivel, o mesmo dado nao lido volta a ser reportado.
    T_ASSERT(r, net_poll_definir(p, srv, NET_POLL_LER, &marca), "rearmar falhou");
    n = net_poll_esperar(p, ev, 8, 2000);
    T_ASSERT(r, n == 1 && (ev[0].Eventos & NET_POLL_LER), "rearmado nao reportou o dado pendente (n=%d)", n);

    char b[16];
    int lidos = recv(srv, b, sizeof(b), 0);
    T_ASSERT(r, lidos == 3 && memcmp(b, "abc", 3) == 0, "leu %d bytes", lidos);

    // Nada mais para ler: recv nao bloqueia, volta would-block.
    lidos = recv(srv, b, sizeof(b), 0);
    T_ASSERT(r, lidos < 0 && net_last_error_would_block(), "recv sem dado nao voltou would-block (%d)", lidos);

    // O outro lado fecha: aparece como evento, e o recv devolve 0.
    closesocket(cli);
    n = net_poll_esperar(p, ev, 8, 2000);
    T_ASSERT(r, n == 1 && (ev[0].Eventos & (NET_POLL_LER | NET_POLL_FIM)), "fechamento nao virou evento (n=%d)", n);
    lidos = recv(srv, b, sizeof(b), 0);
    T_ASSERT(r, lidos == 0, "depois do fechamento recv devolveu %d", lidos);

    net_poll_remover(p, srv);
    n = net_poll_esperar(p, ev, 8, 50);
    T_ASSERT(r, n == 0, "removido ainda reportou %d evento(s)", n);

    closesocket(srv);
    net_poll_destruir(p);
}

void teste_poll_escrita_e_varios(TestResult* r)
{
    t_start(r);
    rede_init();
    enum { N = 20 };
    SOCKET cli[N], srv[N];
    int marca[N];
    NetPoll* p = net_poll_criar();
    T_ASSERT(r, p != 0, "net_poll_criar falhou");

    for (int i = 0; i < N; i++)
    {
        T_ASSERT(r, par_tcp(&cli[i], &srv[i]), "par TCP %d falhou", i);
        net_set_nonblocking(srv[i], 1);
        marca[i] = i;
        T_ASSERT(r, net_poll_definir(p, srv[i], NET_POLL_ESCREVER, &marca[i]), "definir %d falhou", i);
    }

    // Socket recem-conectado tem espaco no buffer: todos prontos para escrever.
    NetPollEvento ev[64];
    int n = net_poll_esperar(p, ev, 64, 2000);
    T_ASSERT(r, n == N, "esperava %d prontos para escrever, veio %d", N, n);
    int visto[N] = { 0 };
    for (int k = 0; k < n; k++)
    {
        int i = *(int*)ev[k].Dado;
        T_ASSERT(r, i >= 0 && i < N && (ev[k].Eventos & NET_POLL_ESCREVER), "evento de escrita invalido");
        visto[i]++;
    }
    for (int i = 0; i < N; i++) T_ASSERT(r, visto[i] == 1, "socket %d apareceu %d vez(es)", i, visto[i]);

    // So um lendo: so ele aparece, mesmo com os outros registrados (trocados para LER).
    for (int i = 0; i < N; i++) net_poll_definir(p, srv[i], NET_POLL_LER, &marca[i]);
    send(cli[7], "x", 1, 0);
    n = net_poll_esperar(p, ev, 64, 2000);
    T_ASSERT(r, n == 1 && *(int*)ev[0].Dado == 7, "esperava so o 7 (n=%d)", n);

    // max menor que os prontos: o resto nao se perde, vem na proxima espera.
    for (int i = 0; i < N; i++) send(cli[i], "y", 1, 0);
    dorme_ms(50);
    n = net_poll_esperar(p, ev, 5, 2000);
    T_ASSERT(r, n == 5, "com max=5 devolveu %d", n);
    n = net_poll_esperar(p, ev, 64, 2000);
    T_ASSERT(r, n == N, "os que nao couberam sumiram: segunda espera devolveu %d de %d", n, N);

    for (int i = 0; i < N; i++) { net_poll_remover(p, srv[i]); closesocket(srv[i]); closesocket(cli[i]); }
    net_poll_destruir(p);
}
