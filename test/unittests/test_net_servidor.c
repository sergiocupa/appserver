//  Testes do reator (appserver/src/net/net_servidor.c) com um protocolo minimo de eco, sem
//  HTTP: o que se testa aqui e so o transporte -- leitura por tarefa, ordem, pausa, fila de
//  saida, fechamento, ociosidade e o numero de threads nao crescer com as conexoes.

#include "../../appserver/src/utils/net_compat.h"   // antes de tudo: winsock2 antes do windows.h
#include "testes.h"
#include "../../appserver/src/net/net_servidor.h"
#include "../../appserver/src/utils/pista.h"
#include "atomics.h"
#include "thread_handler.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifdef _WIN32
  #include <windows.h>
  #include <tlhelp32.h>
  static unsigned long long agora_ms(void) { return GetTickCount64(); }
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
  static unsigned long long agora_ms(void)
  {
      struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
      return (unsigned long long)t.tv_sec * 1000ull + (unsigned long long)(t.tv_nsec / 1000000);
  }
  static void dorme_ms(int ms) { struct timespec t = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&t, 0); }
  static int threads_do_processo(void)
  {
      FILE* f = fopen("/proc/self/status", "r"); char l[256]; int n = 0;
      while (f && fgets(l, sizeof(l), f)) if (!strncmp(l, "Threads:", 8)) n = atoi(l + 8);
      if (f) fclose(f);
      return n;
  }
#endif

static void rede_init(void)
{
#ifdef _WIN32
    WSADATA d; WSAStartup(MAKEWORD(2, 2), &d);
#endif
}

// ---- protocolo de eco: devolve o que recebe; "P" pausa a leitura, "F" fecha ---------------

static char g_arquivo_teste[512];   // arquivo que o eco manda ao receber "A"

typedef struct { xatomic_int Abertas, Fechadas, Liberadas; xatomic_int Paralelas, MaxParalelas; NetConexao* Ultima; } Estado;
static Estado g_e;

static void* eco_abrir(NetConexao* c, void* ctx) { (void)ctx; atomic_add_inline(&g_e.Abertas, 1); g_e.Ultima = c; return (void*)1; }
static void eco_receber(NetConexao* c, const byte* d, int n)
{
    int p = atomic_add_inline(&g_e.Paralelas, 1) + 1;
    int m = atomic_get_inline(&g_e.MaxParalelas);
    if (p > m) atomic_set_inline(&g_e.MaxParalelas, p);
    if (n > 1 && d[0] == 'S') { dorme_ms(2); net_enviar(c, d, n); }   // despacho "pesado": 2 ms
    else if (n == 1 && d[0] == 'A') net_enviar_arquivo(c, g_arquivo_teste, 0, -1);
    else if (n == 1 && d[0] == 'P') net_pausar_leitura(c);
    else if (n == 1 && d[0] == 'F') net_fechar(c, true);
    else net_enviar(c, d, n);
    atomic_add_inline(&g_e.Paralelas, -1);
}
static void eco_fechar(NetConexao* c) { (void)c; atomic_add_inline(&g_e.Fechadas, 1); }
static void eco_liberar(void* ctx) { (void)ctx; atomic_add_inline(&g_e.Liberadas, 1); }
static const NetProtocolo g_eco = { eco_abrir, eco_receber, eco_fechar, eco_liberar };

static NetServidor* sobe_cfg(int ocioso_ms, int64 max_fila, int sinc_volta)
{
    memset(&g_e, 0, sizeof(g_e));
    NetConfig c; memset(&c, 0, sizeof(c));
    c.Porta = 0; c.MaxConexoes = 1000; c.OciosoMs = ocioso_ms; c.MaxFilaSaida = max_fila;
    c.MaxSincronoVolta = sinc_volta;
    return net_servidor_criar(&c, &g_eco, 0);
}

static NetServidor* sobe(int ocioso_ms, int64 max_fila) { return sobe_cfg(ocioso_ms, max_fila, 0); }

static SOCKET conecta(int porta)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons((unsigned short)porta);
    if (connect(s, (struct sockaddr*)&a, sizeof(a)) != 0) { closesocket(s); return INVALID_SOCKET; }
    net_set_recv_timeout(s, 3000);
    return s;
}

static int le_tudo(SOCKET s, char* b, int n)
{
    int t = 0;
    while (t < n) { int k = recv(s, b + t, n - t, 0); if (k <= 0) break; t += k; }
    return t;
}

static int espera(xatomic_int* v, int alvo, int ms)
{
    unsigned long long fim = agora_ms() + (unsigned long long)ms;
    while (agora_ms() < fim) { if (atomic_get_inline(v) >= alvo) return 1; dorme_ms(5); }
    return atomic_get_inline(v) >= alvo;
}

void teste_reator_eco_em_ordem(TestResult* r)
{
    t_start(r);
    rede_init();
    NetServidor* s = sobe(0, 8 << 20);
    T_ASSERT(r, s != 0, "servidor nao subiu");
    SOCKET k = conecta(net_servidor_porta(s));
    T_ASSERT(r, k != INVALID_SOCKET, "nao conectou");

    // 1 MB em pedacos: volta inteiro e na ordem
    enum { N = 1 << 20 };
    char* ida = (char*)malloc(N); char* volta = (char*)malloc(N);
    for (int i = 0; i < N; i++) ida[i] = (char)(i * 13 + 1);
    int enviados = 0;
    while (enviados < N) { int k2 = send(k, ida + enviados, N - enviados > 7000 ? 7000 : N - enviados, 0); if (k2 <= 0) break; enviados += k2; }
    int lidos = le_tudo(k, volta, N);
    T_ASSERT(r, lidos == N, "eco devolveu %d de %d bytes", lidos, N);
    T_ASSERT(r, memcmp(ida, volta, N) == 0, "eco fora de ordem ou corrompido");
    T_ASSERT(r, atomic_get_inline(&g_e.MaxParalelas) == 1, "AoReceber rodou em paralelo para a mesma conexao (%d)", atomic_get_inline(&g_e.MaxParalelas));
    free(ida); free(volta);

    closesocket(k);
    T_ASSERT(r, espera(&g_e.Liberadas, 1, 3000), "conexao fechada pelo cliente nao foi liberada");
    T_ASSERT(r, atomic_get_inline(&g_e.Fechadas) == 1, "AoFechar chamado %d vez(es)", atomic_get_inline(&g_e.Fechadas));
    net_servidor_parar(s);
}

// 200 conexoes paradas nao podem custar thread nenhuma.
void teste_reator_conexoes_sem_thread(TestResult* r)
{
    t_start(r);
    rede_init();
    NetServidor* s = sobe(0, 8 << 20);
    T_ASSERT(r, s != 0, "servidor nao subiu");
    dorme_ms(100);
    int t0 = threads_do_processo();
    enum { N = 200 };
    SOCKET k[N];
    for (int i = 0; i < N; i++) { k[i] = conecta(net_servidor_porta(s)); T_ASSERT(r, k[i] != INVALID_SOCKET, "conexao %d falhou", i); }
    T_ASSERT(r, espera(&g_e.Abertas, N, 3000), "so %d de %d conexoes aceitas", atomic_get_inline(&g_e.Abertas), N);
    // todas conversando uma vez: cada uma passa por uma tarefa do pool e volta ao reator
    for (int i = 0; i < N; i++) send(k[i], "ab", 2, 0);
    for (int i = 0; i < N; i++) { char b[2]; T_ASSERT(r, le_tudo(k[i], b, 2) == 2, "conexao %d nao respondeu", i); }
    int t1 = threads_do_processo();
    // o paralelo usa o pool de tarefas (threads fixas, ja existentes): nenhuma thread por conexao
    T_ASSERT(r, t1 - t0 <= 2, "threads subiram de %d para %d com %d conexoes", t0, t1, N);
    T_ASSERT(r, net_servidor_conexoes(s) == N, "servidor conta %d conexoes", net_servidor_conexoes(s));
    for (int i = 0; i < N; i++) closesocket(k[i]);
    T_ASSERT(r, espera(&g_e.Liberadas, N, 5000), "liberadas %d de %d", atomic_get_inline(&g_e.Liberadas), N);
    net_servidor_parar(s);
}

// Pausa: dado que chega depois nao e lido ate retomar.
void teste_reator_pausa_e_retoma(TestResult* r)
{
    t_start(r);
    rede_init();
    NetServidor* s = sobe(0, 8 << 20);
    SOCKET k = conecta(net_servidor_porta(s));
    T_ASSERT(r, espera(&g_e.Abertas, 1, 2000), "nao abriu");
    send(k, "P", 1, 0);
    dorme_ms(100);
    send(k, "xyz", 3, 0);
    char b[8];
    net_set_recv_timeout(k, 300);
    int n = recv(k, b, sizeof(b), 0);
    T_ASSERT(r, n <= 0, "leu com a conexao pausada (%d bytes voltaram)", n);
    net_retomar_leitura(g_e.Ultima);
    net_set_recv_timeout(k, 3000);
    n = le_tudo(k, b, 3);
    T_ASSERT(r, n == 3 && !memcmp(b, "xyz", 3), "depois de retomar nao ecoou (%d)", n);
    closesocket(k);
    net_servidor_parar(s);
}

// Cliente que nao le: a fila de saida passa do limite e a conexao cai (sem crescer a memoria).
void teste_reator_cliente_que_nao_le_cai(TestResult* r)
{
    t_start(r);
    rede_init();
    NetServidor* s = sobe(0, 256 * 1024);
    SOCKET k = socket(AF_INET, SOCK_STREAM, 0);
    int rb = 4096; setsockopt(k, SOL_SOCKET, SO_RCVBUF, (const char*)&rb, sizeof(rb));
    struct sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons((unsigned short)net_servidor_porta(s));
    T_ASSERT(r, connect(k, (struct sockaddr*)&a, sizeof(a)) == 0, "nao conectou");
    T_ASSERT(r, espera(&g_e.Abertas, 1, 2000), "nao abriu");
    // o servidor ecoa tudo; o cliente manda muito e nao le nada
    char* bloco = (char*)malloc(64 * 1024); memset(bloco, 'w', 64 * 1024);
    net_set_nonblocking(k, 1);
    unsigned long long fim = agora_ms() + 5000;
    while (agora_ms() < fim && atomic_get_inline(&g_e.Fechadas) == 0) { send(k, bloco, 64 * 1024, 0); dorme_ms(2); }
    free(bloco);
    T_ASSERT(r, atomic_get_inline(&g_e.Fechadas) == 1, "cliente que nao le nao foi desconectado");
    closesocket(k);
    net_servidor_parar(s);
}

void teste_reator_fecha_ocioso(TestResult* r)
{
    t_start(r);
    rede_init();
    NetServidor* s = sobe(1200, 8 << 20);
    SOCKET k = conecta(net_servidor_porta(s));
    T_ASSERT(r, espera(&g_e.Abertas, 1, 2000), "nao abriu");
    unsigned long long t0 = agora_ms();
    T_ASSERT(r, espera(&g_e.Fechadas, 1, 5000), "conexao ociosa nao foi fechada");
    unsigned long long dt = agora_ms() - t0;
    T_ASSERT(r, dt >= 1000 && dt < 4000, "fechou em %llu ms (ocioso = 1200 ms, varredura de 1 s)", dt);
    char b[4];
    T_ASSERT(r, recv(k, b, sizeof(b), 0) <= 0, "cliente nao viu o fechamento");
    closesocket(k);
    net_servidor_parar(s);
}


// ---- sincrono x assincrono por demanda -----------------------------------------------------

// Um cliente, uma requisicao por vez: nunca ha outra conexao esperando, entao tudo roda no
// proprio reator, sem nenhuma tarefa no pool.
void teste_reator_sincrono_com_um_cliente(TestResult* r)
{
    t_start(r);
    rede_init();
    NetServidor* s = sobe(0, 8 << 20);
    T_ASSERT(r, s != 0, "servidor nao subiu");
    SOCKET k = conecta(net_servidor_porta(s));
    T_ASSERT(r, k != INVALID_SOCKET, "nao conectou");
    long long i0, t0; net_servidor_contadores(s, &i0, &t0);
    enum { N = 300 };
    for (int i = 0; i < N; i++)
    {
        char ida[32], volta[32];
        int n = snprintf(ida, sizeof(ida), "msg-%04d", i);
        send(k, ida, n, 0);
        T_ASSERT(r, le_tudo(k, volta, n) == n && !memcmp(ida, volta, (size_t)n), "eco %d errado", i);
    }
    long long i1, t1; net_servidor_contadores(s, &i1, &t1);
    T_ASSERT(r, t1 - t0 == 0, "um cliente sequencial gerou %lld tarefa(s) no pool", t1 - t0);
    T_ASSERT(r, i1 - i0 > 0, "nenhuma leitura sincrona (%lld)", i1 - i0);   // uma leitura pode atender mais de uma mensagem
    closesocket(k);
    net_servidor_parar(s);
}

typedef struct { int Porta; int Id; int Erros; char Prefixo; } ClienteEco;

static xthread_result_t cliente_eco(void* arg)
{
    ClienteEco* c = (ClienteEco*)arg;
    SOCKET k = conecta(c->Porta);
    if (k == INVALID_SOCKET) { c->Erros = 1000; return (xthread_result_t)0; }
    for (int i = 0; i < 200; i++)
    {
        char ida[48], volta[48];
        int n = snprintf(ida, sizeof(ida), "%cc%02d-m%04d-xxxxxxxx", c->Prefixo, c->Id, i);
        send(k, ida, n, 0);
        if (le_tudo(k, volta, n) != n || memcmp(ida, volta, (size_t)n)) c->Erros++;
    }
    closesocket(k);
    return (xthread_result_t)0;
}

static int roda_clientes(NetServidor* s, int N, char prefixo, long long* tarefas)
{
    ClienteEco cs[16]; Thread* th[16];
    if (N > 16) N = 16;
    long long i0, t0; net_servidor_contadores(s, &i0, &t0);
    for (int i = 0; i < N; i++)
    {
        int st = 0;
        cs[i].Porta = net_servidor_porta(s); cs[i].Id = i; cs[i].Erros = 0; cs[i].Prefixo = prefixo;
        th[i] = thread_create(cliente_eco, &cs[i], &st);
    }
    int erros = 0;
    for (int i = 0; i < N; i++) { thread_join(&th[i]); erros += cs[i].Erros; }
    long long i1, t1; net_servidor_contadores(s, &i1, &t1);
    *tarefas = t1 - t0;
    return erros;
}

// MaxSincronoVolta = 4 (configuravel): ate 4 clientes simultaneos com despacho LEVE cabem na
// volta do reator -- quase nenhuma leitura vai para as threads de leitura.
void teste_reator_disputa_leve_fica_no_reator(TestResult* r)
{
    t_start(r);
    rede_init();
    NetServidor* s = sobe_cfg(0, 8 << 20, 4);
    T_ASSERT(r, s != 0, "servidor nao subiu");
    long long tarefas = 0;
    int erros = roda_clientes(s, 4, 'm', &tarefas);
    T_ASSERT(r, erros == 0, "%d eco(s) errado(s) ou fora de ordem", erros);
    // 4 x 200 = 800 mensagens; soluco ocasional do sistema pode estourar a volta
    T_ASSERT(r, tarefas * 10 < 800, "4 clientes leves geraram %lld tarefas para 800 mensagens", tarefas);
    net_servidor_parar(s);
}

// 16 clientes com despacho PESADO (2 ms cada): a volta estoura o orcamento e o resto vai para
// o pool, em paralelo -- sem trocar a ordem dentro de cada conexao.
void teste_reator_paralelo_sob_disputa(TestResult* r)
{
    t_start(r);
    rede_init();
    NetServidor* s = sobe(0, 8 << 20);
    T_ASSERT(r, s != 0, "servidor nao subiu");
    long long tarefas = 0;
    int erros = roda_clientes(s, 16, 'S', &tarefas);
    T_ASSERT(r, erros == 0, "%d eco(s) errado(s) ou fora de ordem", erros);
    T_ASSERT(r, tarefas > 0, "despacho pesado com 16 clientes e nenhuma tarefa no pool");
    T_ASSERT(r, atomic_get_inline(&g_e.MaxParalelas) > 1, "nenhum despacho rodou em paralelo");
    net_servidor_parar(s);
}


// ---- pista longa: threads proprias que dormem --------------------------------------------

static xatomic_int g_pista_feitos, g_pista_juntas, g_pista_max;

static void trabalho_pista(void* arg)
{
    (void)arg;
    int j = atomic_add_inline(&g_pista_juntas, 1) + 1;
    int m = atomic_get_inline(&g_pista_max);
    if (j > m) atomic_set_inline(&g_pista_max, j);
    dorme_ms(30);
    atomic_add_inline(&g_pista_juntas, -1);
    atomic_add_inline(&g_pista_feitos, 1);
}

void teste_pista_sob_demanda_e_reuso(TestResult* r)
{
    t_start(r);
    atomic_set_inline(&g_pista_feitos, 0); atomic_set_inline(&g_pista_juntas, 0); atomic_set_inline(&g_pista_max, 0);
    // conta so as threads da pista: a contagem do processo inteiro pega threads de outros testes terminando
    Pista* p = pista_criar(4);
    T_ASSERT(r, p != 0, "pista_criar falhou");
    T_ASSERT(r, pista_threads(p) == 0, "pista criou thread sem trabalho nenhum");

    for (int i = 0; i < 20; i++) T_ASSERT(r, pista_submeter(p, trabalho_pista, 0), "submeter %d falhou", i);
    T_ASSERT(r, espera(&g_pista_feitos, 20, 5000), "so %d de 20 trabalhos feitos", atomic_get_inline(&g_pista_feitos));
    T_ASSERT(r, pista_threads(p) == 4, "com max 4 a pista criou %d thread(s)", pista_threads(p));
    T_ASSERT(r, atomic_get_inline(&g_pista_max) <= 4, "%d trabalhos ao mesmo tempo com max 4", atomic_get_inline(&g_pista_max));
    T_ASSERT(r, atomic_get_inline(&g_pista_max) >= 2, "trabalhos nao rodaram em paralelo");

    // segunda leva: as mesmas threads, nenhuma nova
    for (int i = 0; i < 8; i++) pista_submeter(p, trabalho_pista, 0);
    T_ASSERT(r, espera(&g_pista_feitos, 28, 5000), "segunda leva incompleta");
    T_ASSERT(r, pista_threads(p) == 4, "segunda leva criou thread nova (%d)", pista_threads(p));
    T_ASSERT(r, pista_na_fila(p) == 0, "sobrou trabalho na fila");
}


// ---- reator parado nao acorda ----------------------------------------------------------------

// Com conexoes abertas e paradas, sem nada a vencer no intervalo, o reator dorme direto: nada
// de acordar a cada segundo "para ver". Num aparelho com bateria cada acordada conta.
void teste_reator_parado_nao_acorda(TestResult* r)
{
    t_start(r);
    rede_init();
    NetServidor* s = sobe(60000, 8 << 20);   // ociosidade de 60 s: nada vence no teste
    T_ASSERT(r, s != 0, "servidor nao subiu");
    SOCKET k[3];
    for (int i = 0; i < 3; i++) k[i] = conecta(net_servidor_porta(s));
    T_ASSERT(r, espera(&g_e.Abertas, 3, 2000), "conexoes nao abriram");
    dorme_ms(200);
    long long a0 = net_servidor_acordadas(s);
    dorme_ms(2500);
    long long a1 = net_servidor_acordadas(s);
    T_ASSERT(r, a1 - a0 == 0, "parado por 2,5 s, o reator acordou %lld vez(es)", a1 - a0);

    // e continua atendendo na hora
    send(k[0], "oi", 2, 0);
    char b[2];
    T_ASSERT(r, le_tudo(k[0], b, 2) == 2 && !memcmp(b, "oi", 2), "depois de dormir nao respondeu");
    for (int i = 0; i < 3; i++) closesocket(k[i]);
    net_servidor_parar(s);
}


// ---- arquivo pela fila de saida (sendfile no Linux, blocos no Windows) ------------------------

void teste_reator_envia_arquivo(TestResult* r)
{
    t_start(r);
    rede_init();
#ifdef _WIN32
    char tmp[MAX_PATH]; GetTempPathA(MAX_PATH, tmp);
    snprintf(g_arquivo_teste, sizeof(g_arquivo_teste), "%sappsrv_teste_arquivo.bin", tmp);
#else
    snprintf(g_arquivo_teste, sizeof(g_arquivo_teste), "/tmp/appsrv_teste_arquivo.bin");
#endif
    enum { TAM = 3 * 1024 * 1024 + 123 };
    char* dado = (char*)malloc(TAM);
    for (int i = 0; i < TAM; i++) dado[i] = (char)(i * 31 + 7);
    FILE* f = fopen(g_arquivo_teste, "wb");
    T_ASSERT(r, f != 0, "nao criou %s", g_arquivo_teste);
    fwrite(dado, 1, TAM, f); fclose(f);

    NetServidor* s = sobe(0, 8 << 20);
    T_ASSERT(r, s != 0, "servidor nao subiu");
    SOCKET k = conecta(net_servidor_porta(s));
    send(k, "A", 1, 0);
    char* lido = (char*)malloc(TAM);
    int n = le_tudo(k, lido, TAM);
    T_ASSERT(r, n == TAM, "recebeu %d de %d bytes", n, TAM);
    T_ASSERT(r, memcmp(lido, dado, TAM) == 0, "conteudo recebido difere do arquivo");
    // e a conexao segue viva depois do arquivo
    send(k, "ok", 2, 0);
    char b[2];
    T_ASSERT(r, le_tudo(k, b, 2) == 2 && !memcmp(b, "ok", 2), "depois do arquivo a conexao nao respondeu");
    closesocket(k);
    net_servidor_parar(s);
    free(lido); free(dado);
    remove(g_arquivo_teste);
}


// 4 clientes baixando o MESMO arquivo grande ao mesmo tempo: o envio passa do orcamento do
// reator e vai para tarefas de envio no pool (Windows; no Linux o arquivo sai por sendfile no
// reator). Cada cliente tem de receber o arquivo exato: um so escoa cada conexao, na ordem.
typedef struct { int Porta; const char* Esperado; int Tam; int Ok; } ClienteArq;
static xthread_result_t cliente_arquivo(void* arg)
{
    ClienteArq* a = (ClienteArq*)arg;
    SOCKET k = conecta(a->Porta);
    if (k == INVALID_SOCKET) return (xthread_result_t)0;
    send(k, "A", 1, 0);
    char* lido = (char*)malloc((size_t)a->Tam);
    int n = le_tudo(k, lido, a->Tam);
    a->Ok = n == a->Tam && memcmp(lido, a->Esperado, (size_t)a->Tam) == 0;
    free(lido);
    closesocket(k);
    return (xthread_result_t)0;
}

void teste_reator_arquivos_simultaneos(TestResult* r)
{
    t_start(r);
    rede_init();
#ifdef _WIN32
    char tmp[MAX_PATH]; GetTempPathA(MAX_PATH, tmp);
    snprintf(g_arquivo_teste, sizeof(g_arquivo_teste), "%sappsrv_teste_arquivo3.bin", tmp);
#else
    snprintf(g_arquivo_teste, sizeof(g_arquivo_teste), "/tmp/appsrv_teste_arquivo3.bin");
#endif
    enum { TAM = 3 * 1024 * 1024 + 777, N = 4 };
    char* dado = (char*)malloc(TAM);
    for (int i = 0; i < TAM; i++) dado[i] = (char)(i * 29 + (i >> 11));
    FILE* f = fopen(g_arquivo_teste, "wb");
    T_ASSERT(r, f != 0, "nao criou %s", g_arquivo_teste);
    fwrite(dado, 1, TAM, f); fclose(f);

    NetServidor* s = sobe(0, 8 << 20);
    T_ASSERT(r, s != 0, "servidor nao subiu");
    long long e0 = net_servidor_escoamentos(s);
    for (int volta = 0; volta < 3; volta++)
    {
        ClienteArq cs[N]; Thread* th[N];
        for (int i = 0; i < N; i++) { int st = 0; cs[i].Porta = net_servidor_porta(s); cs[i].Esperado = dado; cs[i].Tam = TAM; cs[i].Ok = 0; th[i] = thread_create(cliente_arquivo, &cs[i], &st); }
        for (int i = 0; i < N; i++) thread_join(&th[i]);
        for (int i = 0; i < N; i++) T_ASSERT(r, cs[i].Ok, "volta %d, cliente %d: arquivo recebido difere do original", volta, i);
    }
    long long escoa = net_servidor_escoamentos(s) - e0;
    printf("  tarefas de envio: %lld\n", escoa);
#ifdef _WIN32
    T_ASSERT(r, escoa > 0, "4 arquivos grandes ao mesmo tempo e nenhuma tarefa de envio (o reator mandou tudo)");
#endif
    net_servidor_parar(s);
    free(dado);
    remove(g_arquivo_teste);
}

// Cliente que PARA de ler no meio do arquivo por mais que NET_ARQ_SOLTA_MS, duas vezes: o
// servidor solta o bloco em memoria e o rele depois (Windows; no Linux e sendfile). O que
// chega tem de ser o arquivo exato, sem buraco nem repeticao.
void teste_reator_envia_arquivo_cliente_parado(TestResult* r)
{
    t_start(r);
    rede_init();
#ifdef _WIN32
    char tmp[MAX_PATH]; GetTempPathA(MAX_PATH, tmp);
    snprintf(g_arquivo_teste, sizeof(g_arquivo_teste), "%sappsrv_teste_arquivo2.bin", tmp);
#else
    snprintf(g_arquivo_teste, sizeof(g_arquivo_teste), "/tmp/appsrv_teste_arquivo2.bin");
#endif
    enum { TAM = 3 * 1024 * 1024 + 4567 };
    char* dado = (char*)malloc(TAM);
    for (int i = 0; i < TAM; i++) dado[i] = (char)(i * 13 + (i >> 9));
    FILE* f = fopen(g_arquivo_teste, "wb");
    T_ASSERT(r, f != 0, "nao criou %s", g_arquivo_teste);
    fwrite(dado, 1, TAM, f); fclose(f);

    NetServidor* s = sobe(0, 8 << 20);
    T_ASSERT(r, s != 0, "servidor nao subiu");
    SOCKET k = conecta(net_servidor_porta(s));
    send(k, "A", 1, 0);
    char* lido = (char*)malloc(TAM);
    int pedacos[3] = { 100000, 1000000, TAM - 1100000 };
    int pos = 0;
    for (int i = 0; i < 3; i++)
    {
        if (i > 0) dorme_ms(1200);   // parado: o bloco e solto
        int n = le_tudo(k, lido + pos, pedacos[i]);
        T_ASSERT(r, n == pedacos[i], "trecho %d: recebeu %d de %d bytes", i, n, pedacos[i]);
        pos += n;
    }
    T_ASSERT(r, memcmp(lido, dado, TAM) == 0, "conteudo recebido difere do arquivo (bloco solto e relido errado)");
    send(k, "ok", 2, 0);
    char b[2];
    T_ASSERT(r, le_tudo(k, b, 2) == 2 && !memcmp(b, "ok", 2), "depois do arquivo a conexao nao respondeu");
    closesocket(k);
    net_servidor_parar(s);
    free(lido); free(dado);
    remove(g_arquivo_teste);
}


// Muitos clientes LEVES (16, acima de MaxSincronoVolta): o excedente de cada volta vai para o
// pool -- o servidor usa os outros nucleos em vez de virar um nucleo so.
void teste_reator_muitos_clientes_usam_o_pool(TestResult* r)
{
    t_start(r);
    rede_init();
    NetServidor* s = sobe(0, 8 << 20);
    T_ASSERT(r, s != 0, "servidor nao subiu");
    long long tarefas = 0;
    int erros = roda_clientes(s, 16, 'm', &tarefas);
    T_ASSERT(r, erros == 0, "%d eco(s) errado(s) ou fora de ordem", erros);
    T_ASSERT(r, tarefas > 0, "16 clientes simultaneos e nenhuma tarefa no pool");
    net_servidor_parar(s);
}
