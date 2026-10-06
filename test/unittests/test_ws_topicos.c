//  Testes do WebSocket incremental (http_ws.c) e dos topicos (app_topico.c).
//
//  WebSocket: o decodificador e alimentado direto, com quadros partidos em pontos ruins.
//  Topicos: servidor de verdade (appserver_create, porta escolhida pelo sistema) e clientes
//  SSE por socket: assinatura recebe o retido e o que vem depois, o fim do topico fecha o
//  fluxo, e 50 assinantes nao custam thread.

#include "../../appserver/src/utils/net_compat.h"   // antes de tudo: winsock2 antes do windows.h
#include "testes.h"
#include "../../appserver/include/appserver.h"
#include "../../appserver/src/http/http_ws.h"
#include "../../appserver/src/net/net_servidor.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifdef _WIN32
  #include <windows.h>
  #include <tlhelp32.h>
  static void dorme_ms(int ms) { Sleep((DWORD)ms); }
  static int threads_do_processo(void)
  {
      int n = 0; DWORD pid = GetCurrentProcessId();
      HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
      THREADENTRY32 te; te.dwSize = sizeof(te);
      if (Thread32First(s, &te)) do { if (te.th32OwnerProcessID == pid) n++; } while (Thread32Next(s, &te));
      CloseHandle(s);
      return n;
  }
#else
  #include <time.h>
  static void dorme_ms(int ms) { struct timespec t = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&t, 0); }
  static int threads_do_processo(void)
  {
      FILE* f = fopen("/proc/self/status", "r"); char l[256]; int n = 0;
      while (f && fgets(l, sizeof(l), f)) if (!strncmp(l, "Threads:", 8)) n = atoi(l + 8);
      if (f) fclose(f);
      return n;
  }
#endif

// =========================================================================================
//  WebSocket
// =========================================================================================

typedef struct { char Msgs[8][256]; int N; byte Resp[256]; int NResp; } ColetaWs;

static void ws_msg(void* ctx, const byte* d, int64 n)
{
    ColetaWs* c = (ColetaWs*)ctx;
    if (c->N < 8) { int k = n < 255 ? (int)n : 255; memcpy(c->Msgs[c->N], d, (size_t)k); c->Msgs[c->N][k] = 0; c->N++; }
}
static void ws_resp(void* ctx, const byte* q, int n)
{
    ColetaWs* c = (ColetaWs*)ctx;
    if (c->NResp + n <= (int)sizeof(c->Resp)) { memcpy(c->Resp + c->NResp, q, (size_t)n); c->NResp += n; }
}

// Quadro de CLIENTE (mascarado), como o navegador manda.
static int quadro_cliente(byte* out, int fin, int op, const char* dados, int n)
{
    static const byte m[4] = { 0x11, 0x22, 0x33, 0x44 };
    int k = 0;
    out[k++] = (byte)((fin ? 0x80 : 0) | op);
    if (n <= 125) out[k++] = (byte)(0x80 | n);
    else { out[k++] = 0x80 | 126; out[k++] = (byte)(n >> 8); out[k++] = (byte)n; }
    memcpy(out + k, m, 4); k += 4;
    for (int i = 0; i < n; i++) out[k + i] = (byte)(dados[i] ^ m[i & 3]);
    return k + n;
}

void teste_ws_quadros_partidos_e_juntos(TestResult* r)
{
    t_start(r);
    ColetaWs c; memset(&c, 0, sizeof(c));
    HttpWs* w = http_ws_criar(1 << 20, ws_msg, &c);
    byte buf[4096]; int n = 0;
    n += quadro_cliente(buf + n, 1, HTTP_WS_TEXTO, "primeira", 8);
    char grande[300]; memset(grande, 'g', sizeof(grande)); grande[299] = 'Z';
    n += quadro_cliente(buf + n, 1, HTTP_WS_TEXTO, grande, 300);   // tamanho de 16 bits
    // tudo junto, entregue de 1 em 1 byte
    for (int i = 0; i < n; i++) T_ASSERT(r, http_ws_receber(w, buf + i, 1, ws_resp), "recusou o byte %d", i);
    T_ASSERT(r, c.N == 2, "esperava 2 mensagens, veio %d", c.N);
    T_ASSERT(r, !strcmp(c.Msgs[0], "primeira"), "1a mensagem errada: '%s'", c.Msgs[0]);
    T_ASSERT(r, (int)strlen(c.Msgs[1]) == 255 && c.Msgs[1][0] == 'g', "2a mensagem errada");
    // dois quadros num unico pedaco
    c.N = 0; n = 0;
    n += quadro_cliente(buf + n, 1, HTTP_WS_TEXTO, "a", 1);
    n += quadro_cliente(buf + n, 1, HTTP_WS_TEXTO, "b", 1);
    T_ASSERT(r, http_ws_receber(w, buf, n, ws_resp), "recusou dois quadros juntos");
    T_ASSERT(r, c.N == 2 && !strcmp(c.Msgs[0], "a") && !strcmp(c.Msgs[1], "b"), "dois quadros num recv: veio %d", c.N);
    http_ws_destruir(w);
}

void teste_ws_fragmentada_ping_e_close(TestResult* r)
{
    t_start(r);
    ColetaWs c; memset(&c, 0, sizeof(c));
    HttpWs* w = http_ws_criar(1 << 20, ws_msg, &c);
    byte buf[512]; int n = 0;
    n += quadro_cliente(buf + n, 0, HTTP_WS_TEXTO, "par", 3);      // inicio, FIN=0
    n += quadro_cliente(buf + n, 1, HTTP_WS_PING, "oi", 2);        // controle no meio: permitido
    n += quadro_cliente(buf + n, 1, 0x0, "tes", 3);                // continuacao final
    T_ASSERT(r, http_ws_receber(w, buf, n, ws_resp), "recusou fragmentada + ping");
    T_ASSERT(r, c.N == 1 && !strcmp(c.Msgs[0], "partes"), "fragmentada remontada errada ('%s', %d)", c.N ? c.Msgs[0] : "", c.N);
    // pong: FIN|0xA, sem mascara, com o mesmo conteudo
    T_ASSERT(r, c.NResp == 4 && c.Resp[0] == 0x8A && c.Resp[1] == 2 && c.Resp[2] == 'o' && c.Resp[3] == 'i', "pong errado (%d bytes, 0x%02x 0x%02x)", c.NResp, c.Resp[0], c.Resp[1]);

    c.NResp = 0;
    n = quadro_cliente(buf, 1, HTTP_WS_FECHA, "\x03\xe8", 2);      // close 1000
    T_ASSERT(r, !http_ws_receber(w, buf, n, ws_resp), "close devia pedir o fim da conexao");
    T_ASSERT(r, c.NResp == 4 && c.Resp[0] == 0x88 && c.Resp[1] == 2, "close nao foi respondido");
    http_ws_destruir(w);

    // quadro SEM mascara vindo do cliente: invalido pelo RFC
    memset(&c, 0, sizeof(c));
    w = http_ws_criar(1 << 20, ws_msg, &c);
    byte sem[3] = { 0x81, 0x01, 'x' };
    T_ASSERT(r, !http_ws_receber(w, sem, 3, ws_resp), "aceitou quadro de cliente sem mascara");
    http_ws_destruir(w);

    // quadro do servidor: sem mascara
    ResourceBuffer q; resource_buffer_init(&q);
    http_ws_quadro(&q, HTTP_WS_TEXTO, (const byte*)"ok", 2);
    T_ASSERT(r, q.Length == 4 && (q.Data[1] & 0x80) == 0, "quadro do servidor saiu mascarado");
    resource_buffer_release(&q, true);
}

// =========================================================================================
//  Topicos, com servidor de verdade
// =========================================================================================

static AppServerInfo* g_srv;

// GET /api/assina/<topico>
static Element* rota_assina(Message* m)
{
    char top[64] = { 0 };
    if (m->Route.Count > 0)
    {
        StringX* u = (StringX*)m->Route.Items[m->Route.Count - 1];
        int n = u->Length < 63 ? (int)u->Length : 63;
        memcpy(top, u->Content, (size_t)n);
    }
    app_assinar(m, top);
    return 0;
}

// GET /api/lenta: rota CURTA que dorme g_lenta_ms (o teste controla)
static volatile int g_lenta_ms = 3;
static Element* rota_lenta(Message* m)
{
    if (g_lenta_ms > 0) dorme_ms(g_lenta_ms);
    m->Response = message_response_create_content(200, APPLICATION_JSON, "{\"ok\":1}", 8);
    return 0;
}

static AppServerInfo* servidor(void)
{
    if (g_srv) return g_srv;
    FunctionBindList* bind = bind_list_create();
    app_add_receiver(bind, "assina", rota_assina, true);
    app_add_receiver(bind, "lenta", rota_lenta, true);
    AppServerConfig cfg = appserver_config_default();
    cfg.AgentName = "teste"; cfg.Port = 0; cfg.Prefix = "api"; cfg.LogRequests = false;
    g_srv = appserver_create(&cfg, bind);
    return g_srv;
}

static SOCKET assina(int porta, const char* topico)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons((unsigned short)porta);
    if (connect(s, (struct sockaddr*)&a, sizeof(a)) != 0) { closesocket(s); return INVALID_SOCKET; }
    net_set_recv_timeout(s, 3000);
    char req[256];
    int n = snprintf(req, sizeof(req), "GET /api/assina/%s HTTP/1.1\r\nHost: t\r\nAccept: text/event-stream\r\n\r\n", topico);
    send(s, req, n, 0);
    return s;
}

// Le ate achar 'fim' no que chegou (ou esgotar o tempo). Devolve o total lido.
static int le_ate(SOCKET s, char* b, int cap, const char* fim)
{
    int t = 0; b[0] = 0;
    while (t < cap - 1)
    {
        int k = recv(s, b + t, cap - 1 - t, 0);
        if (k <= 0) break;
        t += k; b[t] = 0;
        if (fim && strstr(b, fim)) break;
    }
    return t;
}

void teste_topico_retido_e_ao_vivo(TestResult* r)
{
    t_start(r);
    AppServerInfo* s = servidor();
    T_ASSERT(r, s != 0, "servidor nao subiu");
    int porta = net_servidor_porta(s->Net);

    app_publicar(s, "teste", 0, "{\"type\":\"start\"}");
    app_publicar(s, "teste", "progress", "{\"p\":10}");
    app_publicar(s, "teste", "progress", "{\"p\":20}");   // substitui o 10

    SOCKET k = assina(porta, "teste");
    T_ASSERT(r, k != INVALID_SOCKET, "nao conectou");
    char b[4096];
    le_ate(k, b, sizeof(b), "{\"p\":20}");
    T_ASSERT(r, strstr(b, "HTTP/1.1 200") && strstr(b, "text/event-stream"), "cabecalho SSE errado: %.80s", b);
    T_ASSERT(r, strstr(b, "data: {\"type\":\"start\"}"), "retido acumulado nao chegou");
    T_ASSERT(r, strstr(b, "data: {\"p\":20}") && !strstr(b, "{\"p\":10}"), "slot devia ter so o ultimo valor");

    for (int i = 0; i < 50 && app_topico_assinantes(s, "teste") < 1; i++) dorme_ms(10);
    app_publicar(s, "teste", 0, "{\"type\":\"done\"}");
    le_ate(k, b, sizeof(b), "done");
    T_ASSERT(r, strstr(b, "data: {\"type\":\"done\"}"), "evento ao vivo nao chegou");

    app_topico_encerrar(s, "teste");
    char x[64];
    int n = recv(k, x, sizeof(x), 0);
    T_ASSERT(r, n == 0, "encerrar o topico devia fechar o fluxo (recv=%d)", n);
    closesocket(k);

    // quem chega depois do fim recebe o retido e o fluxo fecha
    k = assina(porta, "teste");
    le_ate(k, b, sizeof(b), 0);
    T_ASSERT(r, strstr(b, "{\"type\":\"done\"}") && strstr(b, "{\"type\":\"start\"}"), "atrasado nao recebeu o retido");
    closesocket(k);
}

void teste_topico_assinantes_sem_thread(TestResult* r)
{
    t_start(r);
    AppServerInfo* s = servidor();
    T_ASSERT(r, s != 0, "servidor nao subiu");
    int porta = net_servidor_porta(s->Net);

    enum { N = 50 };
    SOCKET k[N];
    k[0] = assina(porta, "vivo");                      // aquece: a 1a conexao pode criar coisas
    for (int i = 0; i < 100 && app_topico_assinantes(s, "vivo") < 1; i++) dorme_ms(10);
    dorme_ms(100);
    int t0 = threads_do_processo();
    for (int i = 1; i < N; i++) k[i] = assina(porta, "vivo");
    for (int i = 0; i < 300 && app_topico_assinantes(s, "vivo") < N; i++) dorme_ms(10);
    T_ASSERT(r, app_topico_assinantes(s, "vivo") == N, "assinantes: %d de %d", app_topico_assinantes(s, "vivo"), N);
    int t1 = threads_do_processo();
    // o paralelo usa o pool de tarefas (threads fixas): nenhuma thread por assinante
    T_ASSERT(r, t1 - t0 <= 1, "%d assinantes: threads de %d para %d", N, t0, t1);

    app_publicar(s, "vivo", 0, "{\"todos\":1}");
    char b[1024];
    int receberam = 0;
    for (int i = 0; i < N; i++)
    {
        le_ate(k[i], b, sizeof(b), "{\"todos\":1}");
        if (strstr(b, "data: {\"todos\":1}")) receberam++;
    }
    T_ASSERT(r, receberam == N, "evento chegou a %d de %d assinantes", receberam, N);

    // clientes saem: na proxima publicacao eles somem do topico
    for (int i = 0; i < N; i++) closesocket(k[i]);
    dorme_ms(200);
    app_publicar(s, "vivo", 0, "x");
    dorme_ms(100);
    app_publicar(s, "vivo", 0, "y");
    T_ASSERT(r, app_topico_assinantes(s, "vivo") == 0, "assinantes que sairam continuam no topico (%d)", app_topico_assinantes(s, "vivo"));
}


// ---- rota curta que demora: aprendida e tirada do reator -----------------------------------

static int pede_lenta(SOCKET k)
{
    static const char req[] = "GET /api/lenta HTTP/1.1\r\nHost: t\r\n\r\n";
    send(k, req, (int)sizeof(req) - 1, 0);
    char b[1024];
    le_ate(k, b, sizeof(b), "{\"ok\":1}");
    return strstr(b, "HTTP/1.1 200") && strstr(b, "{\"ok\":1}");
}

void teste_rota_lenta_aprendida(TestResult* r)
{
    t_start(r);
    AppServerInfo* s = servidor();
    T_ASSERT(r, s != 0, "servidor nao subiu");
    T_ASSERT(r, app_rota_lenta(s, "lenta") == 0, "rota comecou marcada como lenta");
    SOCKET k = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons((unsigned short)net_servidor_porta(s->Net));
    T_ASSERT(r, connect(k, (struct sockaddr*)&a, sizeof(a)) == 0, "nao conectou");
    net_set_recv_timeout(k, 3000);

    g_lenta_ms = 3;
    for (int i = 0; i < 3; i++) T_ASSERT(r, pede_lenta(k), "resposta %d errada", i);
    T_ASSERT(r, app_rota_lenta(s, "lenta") == 1, "3 execucoes de 3 ms e a rota nao foi aprendida como lenta");
    // marcada: continua respondendo (agora fora do reator), na mesma conexao, em ordem
    for (int i = 0; i < 3; i++) T_ASSERT(r, pede_lenta(k), "resposta %d errada depois de marcada", i);

    g_lenta_ms = 0;   // ficou rapida: o contador desce e a marca sai
    for (int i = 0; i < 10; i++) T_ASSERT(r, pede_lenta(k), "resposta rapida %d errada", i);
    T_ASSERT(r, app_rota_lenta(s, "lenta") == 0, "rota voltou a ser rapida e continua marcada");
    T_ASSERT(r, app_rota_lenta(s, "nao-existe") == -1, "rota inexistente devia dar -1");
    closesocket(k);
}


// ---- perfil do pool: economia com pedidos de performance (jobs) ------------------------------

static volatile int g_perfil_no_job = -1;
static void job_le_perfil(AppJob* job, void* arg) { (void)job; (void)arg; g_perfil_no_job = pool_perfil_atual(); dorme_ms(50); }

void teste_perfil_economia_e_jobs(TestResult* r)
{
    t_start(r);
    int original = pool_perfil_atual();
    FunctionBindList* bind = bind_list_create();
    AppServerConfig cfg = appserver_config_default();
    cfg.AgentName = "perfil"; cfg.Port = 0; cfg.Prefix = "api"; cfg.LogRequests = false;
    cfg.PerfilPool = POOL_PERFIL_ECONOMIA;
    AppServerInfo* s = appserver_create(&cfg, bind);
    T_ASSERT(r, s != 0, "servidor nao subiu");
    T_ASSERT(r, pool_perfil_atual() == POOL_PERFIL_ECONOMIA, "Config.PerfilPool nao aplicou economia");

    // pedidos aninhados: so o ultimo fim devolve economia
    app_perfil_performance_inicio(s);
    app_perfil_performance_inicio(s);
    T_ASSERT(r, pool_perfil_atual() == POOL_PERFIL_PERFORMANCE, "pedido de performance nao aplicou");
    app_perfil_performance_fim(s);
    T_ASSERT(r, pool_perfil_atual() == POOL_PERFIL_PERFORMANCE, "voltou a economia com um pedido ainda ativo");
    app_perfil_performance_fim(s);
    T_ASSERT(r, pool_perfil_atual() == POOL_PERFIL_ECONOMIA, "ultimo fim nao devolveu economia");

    // job: performance enquanto roda, economia depois
    g_perfil_no_job = -1;
    T_ASSERT(r, app_job_iniciar(s, "perfil/job", job_le_perfil, 0, 0), "job nao iniciou");
    for (int i = 0; i < 300 && app_job_rodando(s, "perfil/job"); i++) dorme_ms(10);
    T_ASSERT(r, g_perfil_no_job == POOL_PERFIL_PERFORMANCE, "durante o job o pool estava em %d", g_perfil_no_job);
    for (int i = 0; i < 100 && pool_perfil_atual() != POOL_PERFIL_ECONOMIA; i++) dorme_ms(10);
    T_ASSERT(r, pool_perfil_atual() == POOL_PERFIL_ECONOMIA, "depois do job o pool nao voltou a economia");

    pool_perfil(original);   // o pool global e do processo de testes inteiro
}


// ---- entrega SSE em pistas ----------------------------------------------------------------
// Orcamento grande: tudo na thread de quem publica, nenhuma pista. Orcamento de 1 us e pista
// de 4: quase toda entrega vira pistas no pool -- o caso que pode errar ordem ou perder evento.

static AppServerInfo* servidor_sse(int volta_us, int max_pista)
{
    FunctionBindList* bind = bind_list_create();
    app_add_receiver(bind, "assina", rota_assina, true);
    AppServerConfig cfg = appserver_config_default();
    cfg.AgentName = "sse"; cfg.Port = 0; cfg.Prefix = "api"; cfg.LogRequests = false;
    cfg.SseVoltaUs = volta_us; cfg.SseMaxPorPista = max_pista;
    return appserver_create(&cfg, bind);
}

static AppServerInfo* g_sse_pistas;
static AppServerInfo* sse_pistas(void) { if (!g_sse_pistas) g_sse_pistas = servidor_sse(1, 4); return g_sse_pistas; }

static int abre_assinantes(AppServerInfo* s, const char* topico, SOCKET* k, int n)
{
    int porta = net_servidor_porta(s->Net);
    for (int i = 0; i < n; i++) k[i] = assina(porta, topico);
    for (int i = 0; i < 300 && app_topico_assinantes(s, topico) < n; i++) dorme_ms(10);
    return app_topico_assinantes(s, topico);
}

// Le o fluxo ate 'fim' e extrai a sequencia de eventos "data: <letra><numero>".
static int le_sequencia(SOCKET k, const char* fim, char* letras, int* nums, int cap)
{
    static char b[64 * 1024];
    le_ate(k, b, sizeof(b), fim);
    int n = 0;
    for (char* p = strstr(b, "data: "); p && n < cap; p = strstr(p + 6, "data: "))
    {
        letras[n] = p[6];
        nums[n] = atoi(p + 7);
        n++;
    }
    return n;
}

void teste_sse_poucos_assinantes_sem_pista(TestResult* r)
{
    t_start(r);
    AppServerInfo* s = servidor_sse(100000, 64);   // 100 ms de orcamento: cabe tudo
    T_ASSERT(r, s != 0, "servidor nao subiu");
    enum { N = 10, EV = 20 };
    SOCKET k[N];
    T_ASSERT(r, abre_assinantes(s, "poucos", k, N) == N, "assinantes nao entraram");
    for (int e = 0; e < EV; e++) { char d[16]; snprintf(d, sizeof(d), "a%d", e); app_publicar(s, "poucos", 0, d); }
    T_ASSERT(r, app_topico_pistas(s) == 0, "orcamento sobrando e mesmo assim %d pistas foram ao pool", app_topico_pistas(s));
    for (int i = 0; i < N; i++)
    {
        char l[64]; int v[64];
        int n = le_sequencia(k[i], "a19\n", l, v, 64);
        T_ASSERT(r, n == EV, "assinante %d recebeu %d de %d eventos", i, n, EV);
        for (int e = 0; e < EV; e++) T_ASSERT(r, l[e] == 'a' && v[e] == e, "assinante %d: evento %d fora de ordem (%c%d)", i, e, l[e], v[e]);
        closesocket(k[i]);
    }
}

void teste_sse_pistas_ordem_e_todos(TestResult* r)
{
    t_start(r);
    AppServerInfo* s = sse_pistas();
    T_ASSERT(r, s != 0, "servidor nao subiu");
    enum { N = 64, EV = 30 };
    SOCKET k[N];
    T_ASSERT(r, abre_assinantes(s, "ordem", k, N) == N, "assinantes nao entraram");
    int p0 = app_topico_pistas(s);
    for (int e = 0; e < EV; e++) { char d[16]; snprintf(d, sizeof(d), "a%d", e); app_publicar(s, "ordem", 0, d); }
    T_ASSERT(r, app_topico_pistas(s) > p0, "orcamento de 1 us e nenhuma pista foi ao pool");
    for (int i = 0; i < N; i++)
    {
        char l[64]; int v[64];
        int n = le_sequencia(k[i], "a29\n", l, v, 64);
        T_ASSERT(r, n == EV, "assinante %d recebeu %d de %d eventos", i, n, EV);
        for (int e = 0; e < EV; e++) T_ASSERT(r, l[e] == 'a' && v[e] == e, "assinante %d: evento %d fora de ordem (%c%d)", i, e, l[e], v[e]);
        closesocket(k[i]);
    }
}

// Metade dos assinantes sai; as pistas seguem entregando ao resto e os que sairam somem.
void teste_sse_pistas_assinantes_saem(TestResult* r)
{
    t_start(r);
    AppServerInfo* s = sse_pistas();
    T_ASSERT(r, s != 0, "servidor nao subiu");
    enum { N = 48, EV = 20 };
    SOCKET k[N];
    T_ASSERT(r, abre_assinantes(s, "saem", k, N) == N, "assinantes nao entraram");
    for (int i = 0; i < N; i += 2) { closesocket(k[i]); k[i] = INVALID_SOCKET; }
    for (int e = 0; e < EV; e++) { char d[16]; snprintf(d, sizeof(d), "a%d", e); app_publicar(s, "saem", 0, d); dorme_ms(2); }
    for (int i = 1; i < N; i += 2)
    {
        char l[64]; int v[64];
        int n = le_sequencia(k[i], "a19\n", l, v, 64);
        T_ASSERT(r, n == EV, "assinante %d recebeu %d de %d eventos", i, n, EV);
        for (int e = 0; e < EV; e++) T_ASSERT(r, v[e] == e, "assinante %d: evento %d fora de ordem (%d)", i, e, v[e]);
    }
    for (int i = 0; i < 100 && app_topico_assinantes(s, "saem") != N / 2; i++) { app_publicar(s, "saem", 0, "x"); dorme_ms(10); }
    T_ASSERT(r, app_topico_assinantes(s, "saem") == N / 2, "assinantes: %d (esperado %d)", app_topico_assinantes(s, "saem"), N / 2);
    for (int i = 1; i < N; i += 2) closesocket(k[i]);
}

// Dois publicadores ao mesmo tempo (tarefas do pool), com pistas: cada assinante recebe os
// dois fluxos inteiros, cada um em ordem, e TODOS na mesma ordem global.
static AppServerInfo* g_pub_srv;
static xatomic_int    g_pub_fim;
static void publicador(void* arg)
{
    char letra = (char)(intptr_t)arg;
    for (int e = 0; e < 40; e++) { char d[16]; snprintf(d, sizeof(d), "%c%d", letra, e); app_publicar(g_pub_srv, "dois", 0, d); }
    atomic_add_inline(&g_pub_fim, 1);
}

void teste_sse_pistas_dois_publicadores(TestResult* r)
{
    t_start(r);
    AppServerInfo* s = sse_pistas();
    T_ASSERT(r, s != 0, "servidor nao subiu");
    enum { N = 32, EV = 80 };
    SOCKET k[N];
    T_ASSERT(r, abre_assinantes(s, "dois", k, N) == N, "assinantes nao entraram");
    g_pub_srv = s; atomic_set_inline(&g_pub_fim, 0);
    T_ASSERT(r, pool_submit(publicador, (void*)(intptr_t)'a') && pool_submit(publicador, (void*)(intptr_t)'b'), "publicadores nao subiram");
    for (int i = 0; i < 500 && atomic_get_inline(&g_pub_fim) < 2; i++) dorme_ms(10);
    T_ASSERT(r, atomic_get_inline(&g_pub_fim) == 2, "publicadores nao terminaram");
    app_publicar(s, "dois", 0, "z0");   // marca de fim
    char l0[128]; int v0[128];
    int n0 = 0;
    for (int i = 0; i < N; i++)
    {
        char l[128]; int v[128];
        int n = le_sequencia(k[i], "z0\n", l, v, 128);
        T_ASSERT(r, n == EV + 1, "assinante %d recebeu %d de %d eventos", i, n, EV + 1);
        int pa = -1, pb = -1;
        for (int e = 0; e < EV; e++)
        {
            if (l[e] == 'a') { T_ASSERT(r, v[e] == pa + 1, "assinante %d: a%d depois de a%d", i, v[e], pa); pa = v[e]; }
            else if (l[e] == 'b') { T_ASSERT(r, v[e] == pb + 1, "assinante %d: b%d depois de b%d", i, v[e], pb); pb = v[e]; }
            else T_ASSERT(r, 0, "assinante %d: evento inesperado %c", i, l[e]);
        }
        if (i == 0) { memcpy(l0, l, sizeof(l0)); memcpy(v0, v, sizeof(v0)); n0 = n; }
        else for (int e = 0; e < n0; e++) T_ASSERT(r, l[e] == l0[e] && v[e] == v0[e], "assinante %d viu ordem diferente do 0 na posicao %d", i, e);
        closesocket(k[i]);
    }
}
