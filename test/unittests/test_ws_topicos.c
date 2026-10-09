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
  #include <sys/stat.h>   // mkdir (pasta web do teste de Range)
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


// =========================================================================================
//  Arquivos estaticos: pedido parcial (cabecalho Range)
// =========================================================================================
// Players de video (MP4) pedem faixas para avancar no meio do arquivo (RFC 7233). Confere 206,
// Content-Range e o trecho exato nos dois tamanhos de arquivo (o pequeno sai da memoria quando
// e inteiro, o grande em blocos), varias faixas em multipart/byteranges, 416 para faixa fora do
// arquivo, os Range ignorados (200 com o arquivo inteiro, que a RFC permite) e
// "Accept-Ranges: bytes" na resposta normal.

static char g_web[512];

static AppServerInfo* servidor_web(void)
{
    static AppServerInfo* srv;
    if (srv) return srv;
#ifdef _WIN32
    char tmp[MAX_PATH]; GetTempPathA(MAX_PATH, tmp);
    snprintf(g_web, sizeof(g_web), "%sappsrv_teste_web", tmp);
    CreateDirectoryA(g_web, 0);
#else
    snprintf(g_web, sizeof(g_web), "/tmp/appsrv_teste_web");
    mkdir(g_web, 0755);
#endif
    FunctionBindList* bind = bind_list_create();
    AppServerConfig cfg = appserver_config_default();
    cfg.AgentName = "web"; cfg.Port = 0; cfg.Prefix = "api"; cfg.LogRequests = false;
    cfg.WebContentPath = g_web;
    srv = appserver_create(&cfg, bind);
    return srv;
}

static char* cria_arquivo_web(const char* nome, int tam)
{
    char cam[700]; snprintf(cam, sizeof(cam), "%s/%s", g_web, nome);
    char* d = (char*)malloc((size_t)tam);
    for (int i = 0; i < tam; i++) d[i] = (char)('A' + (i * 7 + (i >> 8)) % 26);
    FILE* f = fopen(cam, "wb"); if (f) { fwrite(d, 1, (size_t)tam, f); fclose(f); }
    return d;
}

typedef struct { int Status; long long Tam; char ContentRange[128]; char ContentType[160]; char ETag[64]; char UltimaMod[64];
                 int AceitaFaixa; char* Corpo; int NCorpo; } RespRange;

static void valor_cabecalho(const char* cab, const char* nome, char* out, int cap)
{
    out[0] = 0;
    const char* p = strstr(cab, nome); if (!p) return;
    p += strlen(nome); while (*p == ' ') p++;
    int i = 0; while (p[i] && p[i] != '\r' && i < cap - 1) { out[i] = p[i]; i++; } out[i] = 0;
}

// GET /<nome> com cabecalhos a mais (linhas "Nome: valor\r\n"; 0 = nenhum); le cabecalho e
// corpo (Content-Length; 304 nao tem corpo)
static int pede(int porta, const char* nome, const char* extras, RespRange* o)
{
    memset(o, 0, sizeof(*o));
    SOCKET k = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons((unsigned short)porta);
    if (connect(k, (struct sockaddr*)&a, sizeof(a)) != 0) { closesocket(k); return 0; }
    net_set_recv_timeout(k, 3000);
    char req[2048];
    int n = snprintf(req, sizeof(req), "GET /%s HTTP/1.1\r\nHost: t\r\n%s\r\n", nome, extras ? extras : "");
    send(k, req, n, 0);
    int cap = 4 << 20, tot = 0;
    char* b = (char*)malloc((size_t)cap + 1);
    char* fimcab = 0;
    while (tot < cap)
    {
        int r = recv(k, b + tot, cap - tot, 0);
        if (r <= 0) break;
        tot += r; b[tot] = 0;
        if (!fimcab) fimcab = strstr(b, "\r\n\r\n");
        if (fimcab)
        {
            const char* cl = strstr(b, "Content-Length:");
            long long len = cl && cl < fimcab ? atoll(cl + 15) : 0;
            if (tot - (int)(fimcab + 4 - b) >= len) break;
        }
    }
    closesocket(k);
    if (!fimcab) { free(b); return 0; }
    *fimcab = 0;
    o->Status = atoi(b + 9);
    const char* cl = strstr(b, "Content-Length:");
    o->Tam = cl ? atoll(cl + 15) : -1;
    valor_cabecalho(b, "Content-Range:", o->ContentRange, sizeof(o->ContentRange));
    valor_cabecalho(b, "Content-Type:", o->ContentType, sizeof(o->ContentType));
    valor_cabecalho(b, "ETag:", o->ETag, sizeof(o->ETag));
    valor_cabecalho(b, "Last-Modified:", o->UltimaMod, sizeof(o->UltimaMod));
    o->AceitaFaixa = strstr(b, "Accept-Ranges: bytes") != 0;
    o->NCorpo = tot - (int)(fimcab + 4 - b);
    o->Corpo = (char*)malloc((size_t)o->NCorpo + 1);
    memcpy(o->Corpo, fimcab + 4, (size_t)o->NCorpo);
    o->Corpo[o->NCorpo] = 0;
    free(b);
    return 1;
}

static int pede_faixa(int porta, const char* nome, const char* faixa, RespRange* o)
{
    char ex[512] = "";
    if (faixa) snprintf(ex, sizeof(ex), "Range: %s\r\n", faixa);
    return pede(porta, nome, ex, o);
}

// Confere um corpo multipart/byteranges: k partes, cada uma com Content-Range "bytes a-b/total"
// e exatamente os bytes do arquivo; fronteira final no fim. 0 = ok; senao, a parte que falhou.
static int confere_multipart(const RespRange* o, const char* dado, const long long* ini, const long long* fim, int k, long long total, char* msg, int cap)
{
    const char* bd = strstr(o->ContentType, "boundary=");
    if (strncmp(o->ContentType, "multipart/byteranges", 20) || !bd) { snprintf(msg, cap, "Content-Type '%s'", o->ContentType); return -1; }
    char delim[96]; snprintf(delim, sizeof(delim), "\r\n--%s", bd + 9);
    const char* p = o->Corpo; const char* fimc = o->Corpo + o->NCorpo;
    for (int i = 0; i < k; i++)
    {
        if ((size_t)(fimc - p) < strlen(delim) || memcmp(p, delim, strlen(delim))) { snprintf(msg, cap, "parte %d: sem fronteira", i); return i + 1; }
        const char* hc = p + strlen(delim) + 2;
        const char* fh = strstr(hc, "\r\n\r\n");
        if (!fh) { snprintf(msg, cap, "parte %d: sem fim de cabecalho", i); return i + 1; }
        char cr[128] = "", esp[128];
        const char* q = strstr(hc, "Content-Range:");
        if (q && q < fh) { q += 14; while (*q == ' ') q++; int j = 0; while (q[j] != '\r' && j < 127) { cr[j] = q[j]; j++; } cr[j] = 0; }
        snprintf(esp, sizeof(esp), "bytes %lld-%lld/%lld", ini[i], fim[i], total);
        if (strcmp(cr, esp)) { snprintf(msg, cap, "parte %d: Content-Range '%s' (esperado '%s')", i, cr, esp); return i + 1; }
        long long nb = fim[i] - ini[i] + 1;
        const char* d = fh + 4;
        if (d + nb > fimc || memcmp(d, dado + ini[i], (size_t)nb)) { snprintf(msg, cap, "parte %d: trecho diferente do arquivo", i); return i + 1; }
        p = d + nb;
    }
    char fin[100]; snprintf(fin, sizeof(fin), "%s--\r\n", delim);
    if ((size_t)(fimc - p) != strlen(fin) || memcmp(p, fin, strlen(fin))) { snprintf(msg, cap, "fronteira final ausente ou sobra no corpo"); return k + 1; }
    if (o->Tam != o->NCorpo) { snprintf(msg, cap, "Content-Length %lld, corpo %d", o->Tam, o->NCorpo); return k + 2; }
    return 0;
}

void teste_estatico_range(TestResult* r)
{
    t_start(r);
    AppServerInfo* s = servidor_web();
    T_ASSERT(r, s != 0, "servidor nao subiu");
    int porta = net_servidor_porta(s->Net);
    enum { PEQ = 100000, GRD = 1500000 };
    char* peq = cria_arquivo_web("range_pequeno.bin", PEQ);
    char* grd = cria_arquivo_web("range_grande.bin", GRD);

    // 206: o trecho exato
    struct { const char* Arq; const char* Faixa; long long Ini, Fim, Total; const char* Dado; } casos[] = {
        { "range_pequeno.bin", "bytes=1000-1999",     1000,      1999,    PEQ, peq },
        { "range_pequeno.bin", "bytes=-100",          PEQ - 100, PEQ - 1, PEQ, peq },
        { "range_pequeno.bin", "bytes=99000-200000",  99000,     PEQ - 1, PEQ, peq },   // fim alem do arquivo: corta
        { "range_grande.bin",  "bytes=700000-700999", 700000,    700999,  GRD, grd },
        { "range_grande.bin",  "bytes=1400000-",      1400000,   GRD - 1, GRD, grd },
        { "range_grande.bin",  "bytes=0-0",           0,         0,       GRD, grd },
        { "range_pequeno.bin", "bytes=0-9,200000-",   0,         9,       PEQ, peq },   // so uma das faixas cabe: 206 simples
    };
    for (int i = 0; i < (int)(sizeof(casos) / sizeof(casos[0])); i++)
    {
        RespRange o;
        T_ASSERT(r, pede_faixa(porta, casos[i].Arq, casos[i].Faixa, &o), "%s %s: sem resposta", casos[i].Arq, casos[i].Faixa);
        long long n = casos[i].Fim - casos[i].Ini + 1;
        char cr[128]; snprintf(cr, sizeof(cr), "bytes %lld-%lld/%lld", casos[i].Ini, casos[i].Fim, casos[i].Total);
        T_ASSERT(r, o.Status == 206, "%s %s: status %d (esperado 206)", casos[i].Arq, casos[i].Faixa, o.Status);
        T_ASSERT(r, !strcmp(o.ContentRange, cr), "%s %s: Content-Range '%s' (esperado '%s')", casos[i].Arq, casos[i].Faixa, o.ContentRange, cr);
        T_ASSERT(r, o.Tam == n && o.NCorpo == (int)n, "%s %s: corpo %d / Content-Length %lld (esperado %lld)", casos[i].Arq, casos[i].Faixa, o.NCorpo, o.Tam, n);
        T_ASSERT(r, !memcmp(o.Corpo, casos[i].Dado + casos[i].Ini, (size_t)n), "%s %s: trecho diferente do arquivo", casos[i].Arq, casos[i].Faixa);
        free(o.Corpo);
    }

    // 206 multipart/byteranges: varias faixas, nos dois tamanhos (com espaco depois da virgula)
    {
        long long pi[] = { 0, 20, PEQ - 5 }, pf[] = { 9, 29, PEQ - 1 };
        long long gi[] = { 100, 1400000 }, gf[] = { 199, 1400099 };
        struct { const char* Arq; const char* Faixa; const char* Dado; long long* Ini; long long* Fim; int K; long long Total; } mp[] = {
            { "range_pequeno.bin", "bytes=0-9,20-29,-5",            peq, pi, pf, 3, PEQ },
            { "range_grande.bin",  "bytes=100-199, 1400000-1400099", grd, gi, gf, 2, GRD },
        };
        for (int i = 0; i < 2; i++)
        {
            RespRange o; char msg[200];
            T_ASSERT(r, pede_faixa(porta, mp[i].Arq, mp[i].Faixa, &o), "%s %s: sem resposta", mp[i].Arq, mp[i].Faixa);
            T_ASSERT(r, o.Status == 206 && !o.ContentRange[0], "%s %s: status %d, Content-Range no topo '%s' (esperado 206 sem)", mp[i].Arq, mp[i].Faixa, o.Status, o.ContentRange);
            int e = confere_multipart(&o, mp[i].Dado, mp[i].Ini, mp[i].Fim, mp[i].K, mp[i].Total, msg, sizeof(msg));
            T_ASSERT(r, e == 0, "%s %s: %s", mp[i].Arq, mp[i].Faixa, msg);
            free(o.Corpo);
        }
    }

    // 416: nenhuma faixa dentro do arquivo
    const char* fora[] = { "bytes=200000-", "bytes=200000-,300000-300010" };
    RespRange o;
    char esp[64]; snprintf(esp, sizeof(esp), "bytes */%d", PEQ);
    for (int i = 0; i < 2; i++)
    {
        T_ASSERT(r, pede_faixa(porta, "range_pequeno.bin", fora[i], &o), "%s: sem resposta", fora[i]);
        T_ASSERT(r, o.Status == 416 && !strcmp(o.ContentRange, esp) && o.Tam == 0, "%s: status %d, Content-Range '%s', Content-Length %lld (esperado 416, '%s', 0)", fora[i], o.Status, o.ContentRange, o.Tam, esp);
        free(o.Corpo);
    }

    // ignoradas -> 200 com o arquivo inteiro: outra unidade, sintaxe invalida, mais de 16 faixas,
    // faixas sobrepostas pedindo mais que o arquivo
    const char* ignoradas[] = { "itens=1-2", "bytes=5-2", "bytes=0-0,1-1,2-2,3-3,4-4,5-5,6-6,7-7,8-8,9-9,10-10,11-11,12-12,13-13,14-14,15-15,16-16", "bytes=0-,0-" };
    for (int i = 0; i < 4; i++)
    {
        T_ASSERT(r, pede_faixa(porta, "range_pequeno.bin", ignoradas[i], &o), "%s: sem resposta", ignoradas[i]);
        T_ASSERT(r, o.Status == 200 && o.NCorpo == PEQ && !memcmp(o.Corpo, peq, PEQ), "%s: status %d, %d bytes (esperado 200 com o arquivo inteiro)", ignoradas[i], o.Status, o.NCorpo);
        free(o.Corpo);
    }

    // resposta normal anuncia que aceita faixas (o navegador so busca no video se vir isso)
    const char* arqs[] = { "range_pequeno.bin", "range_grande.bin" };
    int tams[] = { PEQ, GRD };
    for (int i = 0; i < 2; i++)
    {
        T_ASSERT(r, pede_faixa(porta, arqs[i], 0, &o), "%s sem Range: sem resposta", arqs[i]);
        T_ASSERT(r, o.Status == 200 && o.NCorpo == tams[i], "%s sem Range: status %d, %d bytes", arqs[i], o.Status, o.NCorpo);
        T_ASSERT(r, o.AceitaFaixa, "%s: resposta 200 sem 'Accept-Ranges: bytes'", arqs[i]);
        free(o.Corpo);
    }

    free(peq); free(grd);
    char cam[700];
    snprintf(cam, sizeof(cam), "%s/range_pequeno.bin", g_web); remove(cam);
    snprintf(cam, sizeof(cam), "%s/range_grande.bin", g_web);  remove(cam);
}

// Validadores e pedidos condicionais (RFC 7232): ETag e Last-Modified nas respostas; 304 para
// If-None-Match (comparacao fraca) e If-Modified-Since; If-Range so entrega a faixa se o arquivo
// e o mesmo (etiqueta forte ou data igual) -- senao, o arquivo inteiro. Nos dois tamanhos (o
// pequeno sai da memoria, o grande em blocos).
void teste_estatico_condicional(TestResult* r)
{
    t_start(r);
    AppServerInfo* s = servidor_web();
    T_ASSERT(r, s != 0, "servidor nao subiu");
    int porta = net_servidor_porta(s->Net);
    enum { PEQ = 50000, GRD = 1200000 };
    char* dados[2] = { cria_arquivo_web("cond_pequeno.bin", PEQ), cria_arquivo_web("cond_grande.bin", GRD) };
    const char* arqs[2] = { "cond_pequeno.bin", "cond_grande.bin" };
    int tams[2] = { PEQ, GRD };
    for (int a = 0; a < 2; a++)
    {
        RespRange o; char ex[512], etag[64], mod[64];
        T_ASSERT(r, pede(porta, arqs[a], 0, &o), "%s: sem resposta", arqs[a]);
        T_ASSERT(r, o.Status == 200 && o.ETag[0] == '"' && o.UltimaMod[0], "%s: status %d, ETag '%s', Last-Modified '%s' (esperado 200 com os dois)", arqs[a], o.Status, o.ETag, o.UltimaMod);
        strcpy(etag, o.ETag); strcpy(mod, o.UltimaMod); free(o.Corpo);

        // 304: o cliente ja tem esta versao
        const char* iguais[] = { "If-None-Match: %s\r\n", "If-None-Match: \"outra\", W/%s\r\n", "If-None-Match: *\r\n", "If-Modified-Since: %s\r\n" };
        for (int i = 0; i < 4; i++)
        {
            snprintf(ex, sizeof(ex), iguais[i], i == 3 ? mod : etag);
            T_ASSERT(r, pede(porta, arqs[a], ex, &o), "%s %s: sem resposta", arqs[a], ex);
            T_ASSERT(r, o.Status == 304 && o.NCorpo == 0 && !strcmp(o.ETag, etag), "%s com '%.*s': status %d, %d bytes, ETag '%s' (esperado 304 sem corpo)", arqs[a], (int)strlen(ex) - 2, ex, o.Status, o.NCorpo, o.ETag);
            free(o.Corpo);
        }
        // 200: versao diferente (ou data anterior a modificacao)
        const char* difs[] = { "If-None-Match: \"outra\"\r\n", "If-Modified-Since: Sun, 06 Nov 1994 08:49:37 GMT\r\n", "If-Modified-Since: data-estranha\r\n" };
        for (int i = 0; i < 3; i++)
        {
            T_ASSERT(r, pede(porta, arqs[a], difs[i], &o), "%s %s: sem resposta", arqs[a], difs[i]);
            T_ASSERT(r, o.Status == 200 && o.NCorpo == tams[a] && !memcmp(o.Corpo, dados[a], (size_t)tams[a]), "%s com '%s': status %d, %d bytes (esperado 200 inteiro)", arqs[a], difs[i], o.Status, o.NCorpo);
            free(o.Corpo);
        }
        // If-Range: mesma versao -> 206 com a faixa; outra -> 200 com o arquivo inteiro
        const char* ir_ok[] = { etag, mod };
        for (int i = 0; i < 2; i++)
        {
            snprintf(ex, sizeof(ex), "Range: bytes=10-19\r\nIf-Range: %s\r\n", ir_ok[i]);
            T_ASSERT(r, pede(porta, arqs[a], ex, &o), "%s If-Range %s: sem resposta", arqs[a], ir_ok[i]);
            T_ASSERT(r, o.Status == 206 && o.NCorpo == 10 && !memcmp(o.Corpo, dados[a] + 10, 10), "%s If-Range '%s': status %d, %d bytes (esperado 206 com 10)", arqs[a], ir_ok[i], o.Status, o.NCorpo);
            free(o.Corpo);
        }
        char fraca[80]; snprintf(fraca, sizeof(fraca), "W/%s", etag);
        const char* ir_nao[] = { "\"outra\"", fraca, "Sun, 06 Nov 1994 08:49:37 GMT" };
        for (int i = 0; i < 3; i++)
        {
            snprintf(ex, sizeof(ex), "Range: bytes=10-19\r\nIf-Range: %s\r\n", ir_nao[i]);
            T_ASSERT(r, pede(porta, arqs[a], ex, &o), "%s If-Range %s: sem resposta", arqs[a], ir_nao[i]);
            T_ASSERT(r, o.Status == 200 && o.NCorpo == tams[a], "%s If-Range '%s': status %d, %d bytes (esperado 200 inteiro)", arqs[a], ir_nao[i], o.Status, o.NCorpo);
            free(o.Corpo);
        }
    }
    free(dados[0]); free(dados[1]);
    char cam[700];
    snprintf(cam, sizeof(cam), "%s/cond_pequeno.bin", g_web); remove(cam);
    snprintf(cam, sizeof(cam), "%s/cond_grande.bin", g_web);  remove(cam);
}
