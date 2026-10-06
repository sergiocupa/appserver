//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Reator + tarefas no pool. Ver net_servidor.h.
//
//  Estados de leitura de uma conexao (so o reator muda):
//
//      ARMADO  --bytes chegaram-->  TAREFA  --tarefa terminou-->  ARMADO
//                                     |
//                                     +--protocolo pediu pausa--> PAUSADO --retomar--> ARMADO
//
//  Em TAREFA e PAUSADO a leitura esta fora do poll. A conexao so e encerrada (socket
//  fechado, AoFechar) fora de TAREFA: assim AoFechar nunca corre junto com AoReceber, e o
//  socket nunca e fechado debaixo de um recv.
//
//  Vida da memoria: Refs conta o reator (1, ate encerrar), cada tarefa na fila/rodando,
//  cada comando pendente e cada net_segurar. Quem leva a zero chama AoLiberar e libera.

// fopen/snprintf padrao C: o SDL do MSVC os trata como erro (C4996); o codigo e portavel.
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "net_servidor.h"
#include "net_poll.h"
#include "atomics.h"
#include "memory_pool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #define NET_TLS __declspec(thread)
  static long long agora_ms(void) { return (long long)GetTickCount64(); }
  static long long agora_us(void)
  {
      static LARGE_INTEGER f; LARGE_INTEGER c;
      if (!f.QuadPart) QueryPerformanceFrequency(&f);
      QueryPerformanceCounter(&c);
      return (long long)(c.QuadPart / f.QuadPart) * 1000000LL + (long long)((c.QuadPart % f.QuadPart) * 1000000LL / f.QuadPart);
  }
#else
  #include <time.h>
  #include <signal.h>
  #if defined(__linux__)
    #include <sys/sendfile.h>
  #endif
  #define NET_TLS __thread
  static long long agora_ms(void)
  {
      struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
      return (long long)t.tv_sec * 1000LL + t.tv_nsec / 1000000;
  }
  static long long agora_us(void)
  {
      struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
      return (long long)t.tv_sec * 1000000LL + t.tv_nsec / 1000;
  }
#endif

#define NET_LER_BLOCO        (64 * 1024)
#define NET_LER_MAX_TAREFA   (256 * 1024)          // por tarefa: as outras conexoes tambem tem vez
#define NET_ARQ_BLOCO        (256 * 1024)
// Envio de arquivo PARADO (o cliente nao le) ha este tempo: o bloco em memoria e solto e
// relido do arquivo quando o socket voltar a aceitar. Medido no Windows: o bloco por conexao
// parada era ~0,35 MB cada; bloco menor resolveria a memoria mas custava -39% de vazao no
// arquivo de 4 MB (e maior nao ganhava nada). So o Windows usa bloco: o Linux usa sendfile.
#define NET_ARQ_SOLTA_MS     500
#define NET_DRENO_MS         2000
#define NET_DRENO_MAX        (16LL * 1024 * 1024)
#define NET_DEBUG_MS         1000     // so com APPSERVER_DEBUG_REATOR=1: imprime contadores
#define NET_SEM_PRAZO        0x7fffffffffffffffLL

// Modo sincrono (o reator le e despacha ali mesmo): orcamento de bytes por vez. Passou disto
// (upload grande), o resto vai para uma tarefa. Nao ha orcamento de TEMPO: um despacho lento
// so atrapalha se houver outra conexao esperando -- e isso o reator ve na volta seguinte (mais
// de uma pronta -> paralelo). Medido: com janela de tempo, soluco de 0,5-0,9 ms do Windows
// (15 em 3000) mandava 73% das leituras para o pool sem ninguem mais esperando.
// Rota curta que e lenta de verdade, sempre, a camada HTTP aprende e manda direto ao pool.
#define NET_INLINE_BYTES     (64 * 1024)
#define NET_INLINE_US        500      // so diagnostico: conta despachos acima disto

// Balanco CPU x paralelismo de uma volta do reator: ele mesmo atende ate MaxSincronoVolta
// conexoes prontas (padrao 1) dentro de NET_VOLTA_US; as outras vao, em paralelo, para o
// POOL DE TAREFAS do xplatbase. O custo de energia do pool (giro dos workers antes de dormir)
// e ajustado pelo PERFIL do pool (pool_perfil: performance x economia), nao aqui -- o
// servidor nao tem mecanismo de paralelismo proprio para isso. Medido antes de chegar aqui:
//   - tudo em serie no reator: CPU baixa, mas um nucleo so -- com 32 clientes a vazao caia
//     37-83% e a latencia dobrava com 4 clientes;
//   - threads proprias que dormem (sem giro): CPU baixa, mas acordar uma thread por tarefa
//     derrubava a vazao com 32 clientes no Linux (105 mil -> 19 mil req/s).
#define NET_VOLTA_US         1000
#define NET_SINCRONO_VOLTA   1

// SAIDA com o mesmo balanco da leitura: o reator escoa a fila de uma conexao ate NET_VOLTA_US;
// passou disso e o socket ainda aceita (arquivo, resposta grande), o resto vai para uma TAREFA
// DE ENVIO no pool. Medido no Windows, 4 downloads de 4 MB ao mesmo tempo: o reator ficava
// 91-100% ocupado lendo o arquivo e copiando para o socket, um cliente atras do outro (p99
// +22-32% sobre o servidor antigo, que tinha uma thread por cliente). Uma conexao e escoada
// por um so de cada vez (Escoando): os bytes saem na ordem. No Linux o arquivo sai por
// sendfile, que fica no reator (barato, e so ele bloqueia o SIGPIPE).
#define NET_ESCOA_TAREFA_US  5000     // por tarefa de envio: as outras conexoes tambem tem vez
enum { ESC_OK, ESC_ERRO, ESC_ORCAMENTO, ESC_DEVOLVE };

enum { EST_ARMADO, EST_TAREFA, EST_PAUSADO };
enum { CMD_FIM_TAREFA, CMD_FIM_LEITURA, CMD_REAVALIA, CMD_RETOMAR, CMD_RELOGIO };

typedef struct NoSaida
{
    struct NoSaida* Prox;
    int     Arquivo;          // 0 = bytes em Dados; 1 = trecho de arquivo
    int     Tam, Off;         // bytes (ou bloco lido do arquivo) e quanto ja saiu
    FILE*   Arq;
    int64   Pos;              // posicao no arquivo (sendfile)
    int64   Restante;         // do arquivo, ainda nao lido
    char*   Bloco;            // bloco do arquivo em envio
    char    Dados[1];
}
NoSaida;

struct NetConexao
{
    NetServidor*   Srv;
    SOCKET         S;
    int            Id;
    void*          Ctx;
    char           Remoto[64], Local[64];
    xatomic_int    Refs;
    xatomic_int    Ocupado;
    volatile int   PausaPedida;

    // reator
    int            Estado;
    unsigned       Interesse;
    bool           Encerrada;
    bool           FimLeitura;     // o outro lado fechou o envio (recv devolveu 0)
    bool           Drenando;       // envio fechado, descartando o que ainda chega
    long long      DrenoFim;
    int64          Drenado;
    volatile long long Atividade;  // ms da ultima leitura
    struct NetConexao *Prox, *Ant;
    struct NetConexao *ProxMorta;

    // saida (Lock)
    xmutex_t       Lock;
    NoSaida       *Cab, *Cauda;
    int64          Pendente;
    long long      UltimoEnvio;    // ms do ultimo byte que o socket aceitou (bloco parado: NET_ARQ_SOLTA_MS)
    bool           Escoando;       // tarefa de envio rodando: so ela escoa a fila (o reator nao arma escrita)
    bool           Fechada;        // socket fechado: nada mais sai
    bool           FechaPedido;    // fechar ja (erro, fila estourada, net_fechar)
    bool           FechaDepois;    // fechar depois de escoar e de terminar o que esta em andamento
};

typedef struct { NetConexao* C; int Tipo; } Cmd;

struct NetServidor
{
    NetConfig     Cfg;
    char          Recusa[512];
    int           RecusaLen;
    NetProtocolo  Proto;
    void*         Ctx;
    SOCKET        Escuta;
    int           Porta;
    NetPoll*      Poll;
    Thread*       Reator;
    volatile int  Parar;
    ThreadPool*   Pool;

    xmutex_t      CmdLock;
    Cmd*          Cmds;  int NCmd, CapCmd;
    Cmd*          Proc;  int CapProc;       // lote em processamento (so o reator)

    NetConexao*   Lista;
    int           NConex;
    NetConexao*   Mortas;

    // sincrono x assincrono (so o reator mexe)
    char*         BufInline;      // buffer de leitura do modo sincrono
    int           TarefasVivas;   // leituras no pool ainda nao devolvidas
    xatomic_int   ContInline, ContTarefas;   // diagnostico: leituras de cada modo
    xatomic_int   ContEscoa;                 // diagnostico: tarefas de envio
    // diagnostico: POR QUE foi para o pool (APPSERVER_DEBUG_REATOR=1 imprime a cada 1 s)
    long long     MotivoQtd, MotivoTempo, MotivoBytes, InlineLentos;
    bool          Debug;
    long long     MaxInlineUs;

    // prazos (ms; NET_SEM_PRAZO = nenhum). O reator dorme ate o menor deles, ou para sempre.
    long long     PrazoVarre;     // conexao ociosa mais antiga / fim de dreno (pode ser cedo: e so revisto)
    long long     PrazoTick;      // pedido pelo protocolo (AoTick)
    bool          TickPedido;     // net_pedir_relogio
    xatomic_int   Acordadas;
};

static void prazo_varre(NetServidor* s, long long t) { if (t < s->PrazoVarre) s->PrazoVarre = t; }

static xatomic_int g_total;
static char g_marca_escuta;   // Dado do socket de escuta no poll

int net_conexoes_total(void) { return atomic_get_inline(&g_total); }

// ---- referencias -------------------------------------------------------------------------

void net_segurar(NetConexao* c) { atomic_add_inline(&c->Refs, 1); }

static void fila_libera(NetConexao* c)
{
    NoSaida* n = c->Cab;
    while (n)
    {
        NoSaida* p = n->Prox;
        if (n->Arq) fclose(n->Arq);
        memop_free_raw(n->Bloco);
        memop_free_raw(n);
        n = p;
    }
    c->Cab = c->Cauda = 0;
    c->Pendente = 0;
}

void net_soltar(NetConexao* c)
{
    if (atomic_add_inline(&c->Refs, -1) != 1) return;   // devolve o valor ANTERIOR
    NetServidor* s = c->Srv;
    if (c->Ctx && s->Proto.AoLiberar) s->Proto.AoLiberar(c->Ctx);
    fila_libera(c);
    thread_mutex_destroy_inline(&c->Lock);
    memop_free_raw(c);
}

// ---- comandos para o reator --------------------------------------------------------------

static void posta(NetServidor* s, NetConexao* c, int tipo)
{
    net_segurar(c);   // o comando segura a conexao ate ser processado
    thread_mutex_lock_inline(&s->CmdLock);
    if (s->NCmd == s->CapCmd)
    {
        int cap = s->CapCmd ? s->CapCmd * 2 : 256;
        Cmd* n = (Cmd*)memop_realloc_raw(s->Cmds, (size_t)cap * sizeof(Cmd));
        if (!n) { thread_mutex_unlock_inline(&s->CmdLock); net_soltar(c); return; }
        s->Cmds = n; s->CapCmd = cap;
    }
    s->Cmds[s->NCmd].C = c;
    s->Cmds[s->NCmd].Tipo = tipo;
    s->NCmd++;
    thread_mutex_unlock_inline(&s->CmdLock);
    net_poll_acordar(s->Poll);
}

// ---- acessores ---------------------------------------------------------------------------

void*       net_contexto(NetConexao* c) { return c->Ctx; }
const char* net_remoto(NetConexao* c)   { return c->Remoto; }
const char* net_local(NetConexao* c)    { return c->Local; }
int         net_id(NetConexao* c)       { return c->Id; }
int         net_servidor_porta(NetServidor* s) { return s->Porta; }
int         net_servidor_conexoes(NetServidor* s) { return s->NConex; }

void net_ocupar(NetConexao* c, int delta)
{
    int antes = atomic_add_inline(&c->Ocupado, delta);
    if (delta < 0 && antes + delta == 0) posta(c->Srv, c, CMD_REAVALIA);   // pode estar esperando para fechar
}

void net_pausar_leitura(NetConexao* c)  { c->PausaPedida = 1; }

void net_retomar_leitura(NetConexao* c)
{
    c->PausaPedida = 0;
    posta(c->Srv, c, CMD_RETOMAR);
}

// ---- saida -------------------------------------------------------------------------------

// Tenta escoar a fila. Chamar com Lock. prazo_us > 0: para ao passar dele (ESC_ORCAMENTO) com
// o socket ainda aceitando. tarefa: chamada de uma tarefa de envio (no Linux, arquivo volta
// ao reator: ESC_DEVOLVE). ESC_OK = fila vazia ou socket cheio; ESC_ERRO = socket deu erro.
static int escoa_locked(NetConexao* c, long long prazo_us, bool tarefa)
{
    (void)tarefa;
    while (c->Cab)
    {
        if (prazo_us > 0 && agora_us() > prazo_us) return ESC_ORCAMENTO;
        NoSaida* n = c->Cab;
        const char* p; int falta;
#if defined(__linux__)
        if (n->Arquivo)
        {
            // sendfile: o kernel manda do cache de paginas direto para o socket -- sem bloco em
            // memoria, sem copia, sem o reator ler o disco. So o reator chama isto, e o SIGPIPE
            // esta bloqueado nele: sendfile nao aceita MSG_NOSIGNAL. Tarefa devolve ao reator.
            if (tarefa) return ESC_DEVOLVE;
            while (n->Restante > 0)
            {
                off_t off = (off_t)n->Pos;
                size_t quer = n->Restante > (1 << 20) ? (size_t)(1 << 20) : (size_t)n->Restante;
                ssize_t k = sendfile(c->S, fileno(n->Arq), &off, quer);
                if (k > 0) { n->Pos += k; n->Restante -= k; continue; }
                if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return ESC_OK;   // socket cheio
                if (k < 0 && errno == EINTR) continue;
                return ESC_ERRO;   // erro, ou o arquivo encurtou (k == 0): resposta truncada, fecha
            }
            goto proximo;
        }
#endif
        if (n->Arquivo)
        {
            if (n->Off == n->Tam)
            {
                if (n->Restante == 0) goto proximo;
                if (!n->Bloco) { n->Bloco = (char*)memop_alloc_raw(NET_ARQ_BLOCO); if (!n->Bloco) return ESC_ERRO; }
                int quer = n->Restante > NET_ARQ_BLOCO ? NET_ARQ_BLOCO : (int)n->Restante;
                size_t lidos = fread(n->Bloco, 1, (size_t)quer, n->Arq);
                // arquivo encurtou no meio do envio: o Content-Length ja foi, a resposta
                // ficaria truncada -- so resta fechar a conexao
                if (lidos == 0) return ESC_ERRO;
                n->Tam = (int)lidos; n->Off = 0; n->Restante -= (int64)lidos;
            }
            p = n->Bloco + n->Off; falta = n->Tam - n->Off;
        }
        else
        {
            p = n->Dados + n->Off; falta = n->Tam - n->Off;
        }
        while (falta > 0)
        {
            int k = send(c->S, p, falta, NET_SEND_FLAGS);
            if (k > 0)
            {
                p += k; falta -= k; n->Off += k; if (!n->Arquivo) c->Pendente -= k; c->UltimoEnvio = agora_ms();
                if (falta > 0 && prazo_us > 0 && agora_us() > prazo_us) return ESC_ORCAMENTO;
                continue;
            }
            if (k < 0 && net_last_error_would_block()) return ESC_OK;   // socket cheio: o reator avisa
            if (k < 0 && WSAGetLastError() == WSAEINTR) continue;
            return ESC_ERRO;
        }
        if (n->Arquivo && n->Restante > 0) continue;   // proximo bloco do mesmo arquivo
    proximo:
        c->Cab = n->Prox;
        if (!c->Cab) c->Cauda = 0;
        if (n->Arq) fclose(n->Arq);
        memop_free_raw(n->Bloco);
        memop_free_raw(n);
    }
    return ESC_OK;
}

// Solta o bloco de um envio de arquivo parado: volta no arquivo o que nao saiu, para reler
// quando o socket aceitar de novo. Chamar com Lock. Sem conseguir voltar, mantem o bloco.
static void solta_bloco_locked(NoSaida* n)
{
    int falta = n->Tam - n->Off;
    if (falta > 0 && fseek(n->Arq, -(long)falta, SEEK_CUR) != 0) return;
    n->Restante += falta;
    n->Tam = n->Off = 0;
    memop_free_raw(n->Bloco);
    n->Bloco = 0;
}

static void enfileira_locked(NetConexao* c, NoSaida* n)
{
    n->Prox = 0;
    if (c->Cauda) c->Cauda->Prox = n; else c->Cab = n;
    c->Cauda = n;
}

bool net_enviar(NetConexao* c, const void* dados, int n)
{
    if (n <= 0) return true;
    const char* p = (const char*)dados;
    bool acorda = false, ok = true;

    thread_mutex_lock_inline(&c->Lock);
    if (c->Fechada || c->FechaPedido) { thread_mutex_unlock_inline(&c->Lock); return false; }

    // Fila vazia: tenta mandar direto, sem copiar. O comum (resposta pequena, socket com
    // espaco) termina aqui, sem passar pelo reator.
    if (!c->Cab)
    {
        while (n > 0)
        {
            int k = send(c->S, p, n, NET_SEND_FLAGS);
            if (k > 0) { p += k; n -= k; continue; }
            if (k < 0 && net_last_error_would_block()) break;
            if (k < 0 && WSAGetLastError() == WSAEINTR) continue;
            c->FechaPedido = true; ok = false; acorda = true;
            break;
        }
    }

    if (ok && n > 0)
    {
        if (c->Pendente + n > c->Srv->Cfg.MaxFilaSaida)
        {
            // Cliente que nao le (ou le muito devagar): segurar tudo em memoria derruba o
            // servidor. A conexao cai; ele reconecta se quiser.
            c->FechaPedido = true; ok = false; acorda = true;
        }
        else
        {
            NoSaida* no = (NoSaida*)memop_alloc_raw(sizeof(NoSaida) + (size_t)n);
            if (!no) { c->FechaPedido = true; ok = false; acorda = true; }
            else
            {
                memset(no, 0, sizeof(NoSaida));
                memcpy(no->Dados, p, (size_t)n);
                no->Tam = n;
                bool estava_vazia = c->Cab == 0;
                enfileira_locked(c, no);
                c->Pendente += n;
                acorda = estava_vazia;   // o reator precisa armar a escrita
            }
        }
    }
    thread_mutex_unlock_inline(&c->Lock);
    if (acorda) posta(c->Srv, c, CMD_REAVALIA);
    return ok;
}

bool net_enviar_arquivo(NetConexao* c, const char* caminho, int64 inicio, int64 tam)
{
    FILE* f = fopen(caminho, "rb");
    if (!f) return false;
#ifdef _WIN32
    _fseeki64(f, 0, SEEK_END); int64 total = _ftelli64(f);
    _fseeki64(f, inicio, SEEK_SET);
#else
    fseeko(f, 0, SEEK_END); int64 total = (int64)ftello(f);
    fseeko(f, (off_t)inicio, SEEK_SET);
#endif
    if (inicio < 0 || inicio > total) { fclose(f); return false; }
    if (tam < 0 || inicio + tam > total) tam = total - inicio;

    NoSaida* no = (NoSaida*)memop_calloc_raw(1, sizeof(NoSaida));
    if (!no) { fclose(f); return false; }
    no->Arquivo = 1; no->Arq = f; no->Pos = inicio; no->Restante = tam;

    bool ok = true, acorda = false;
    thread_mutex_lock_inline(&c->Lock);
    if (c->Fechada || c->FechaPedido) ok = false;
    else
    {
        enfileira_locked(c, no);
        acorda = true;   // arquivo sai pela fila, em blocos: reator, ou tarefa de envio se for grande
    }
    thread_mutex_unlock_inline(&c->Lock);
    if (!ok) { fclose(f); memop_free_raw(no); return false; }
    if (acorda) posta(c->Srv, c, CMD_REAVALIA);
    return true;
}

void net_fechar(NetConexao* c, bool depois_de_enviar)
{
    thread_mutex_lock_inline(&c->Lock);
    if (depois_de_enviar) c->FechaDepois = true; else c->FechaPedido = true;
    thread_mutex_unlock_inline(&c->Lock);
    posta(c->Srv, c, CMD_REAVALIA);
}

// ---- tarefa de envio (pool) --------------------------------------------------------------

// Escoa a fila fora do reator, ate o socket encher, a fila acabar ou NET_ESCOA_TAREFA_US; e
// devolve a conexao ao reator (que rearma a escrita, continua ou fecha). Enquanto ela roda,
// Escoando impede o reator de escoar a mesma conexao: os bytes saem na ordem.
static void tarefa_escoa(void* arg)
{
    NetConexao* c = (NetConexao*)arg;
    thread_mutex_lock_inline(&c->Lock);
    int r = c->Fechada ? ESC_OK : escoa_locked(c, agora_us() + NET_ESCOA_TAREFA_US, true);
    if (r == ESC_ERRO) c->FechaPedido = true;
    c->Escoando = false;
    thread_mutex_unlock_inline(&c->Lock);
    posta(c->Srv, c, CMD_REAVALIA);
    net_soltar(c);   // a referencia da tarefa
}

static void submete_escoa(NetServidor* s, NetConexao* c)
{
    net_segurar(c);
    atomic_add_inline(&s->ContEscoa, 1);
    bool ok = s->Pool ? pool_submit_relative(s->Pool, tarefa_escoa, c) : pool_submit(tarefa_escoa, c);
    if (!ok) tarefa_escoa(c);   // pool recusou: escoa aqui mesmo (degradado, mas nao perde)
}

long long net_servidor_escoamentos(NetServidor* s) { return atomic_get_inline(&s->ContEscoa); }

// ---- reator: interesse, encerramento -----------------------------------------------------

static void atualiza_interesse(NetServidor* s, NetConexao* c)
{
    if (c->Encerrada) return;
    unsigned in = 0;
    if (c->Estado == EST_ARMADO && !c->FimLeitura) in |= NET_POLL_LER;
    thread_mutex_lock_inline(&c->Lock);
    if (c->Cab && !c->Fechada && !c->Escoando) in |= NET_POLL_ESCREVER;
    thread_mutex_unlock_inline(&c->Lock);
    if (in != c->Interesse)
    {
        net_poll_definir(s->Poll, c->S, in, c);
        c->Interesse = in;
    }
}

static void encerra(NetServidor* s, NetConexao* c)
{
    if (c->Encerrada) return;
    c->Encerrada = true;
    net_poll_remover(s->Poll, c->S);

    thread_mutex_lock_inline(&c->Lock);
    c->Fechada = true;
    fila_libera(c);
    closesocket(c->S);
    thread_mutex_unlock_inline(&c->Lock);

    if (c->Ant) c->Ant->Prox = c->Prox; else s->Lista = c->Prox;
    if (c->Prox) c->Prox->Ant = c->Ant;
    c->Prox = c->Ant = 0;
    s->NConex--;
    atomic_add_inline(&g_total, -1);

    if (c->Ctx && s->Proto.AoFechar) s->Proto.AoFechar(c);

    // A referencia do reator so e solta no fim do lote de eventos: um evento posterior do
    // mesmo lote ainda pode apontar para esta conexao.
    c->ProxMorta = s->Mortas;
    s->Mortas = c;
}

// Decide o que fazer com a conexao depois de qualquer mudanca. So no reator.
static void reavalia(NetServidor* s, NetConexao* c)
{
    if (c->Encerrada) return;
    thread_mutex_lock_inline(&c->Lock);
    int r = ESC_OK;
    if (c->Fechada) r = ESC_ERRO;
    else if (!c->Escoando) r = escoa_locked(c, agora_us() + NET_VOLTA_US, false);   // tarefa de envio rodando: ela escoa
    if (r == ESC_ERRO) c->FechaPedido = true;
    bool envia_fora = r == ESC_ORCAMENTO;   // muito a mandar: o resto vai para uma tarefa
    if (envia_fora) c->Escoando = true;
    bool escoando = c->Escoando;
    bool vazia   = c->Cab == 0;
    bool ja      = c->FechaPedido || c->Fechada;
    bool depois  = c->FechaDepois;
    bool bloco   = !escoando && c->Cab && c->Cab->Arquivo && c->Cab->Bloco;   // arquivo com bloco esperando o socket
    long long ult = c->UltimoEnvio;
    thread_mutex_unlock_inline(&c->Lock);
    if (bloco) prazo_varre(s, ult + NET_ARQ_SOLTA_MS);
    if (envia_fora) submete_escoa(s, c);

    if (ja)
    {
        if (c->Estado != EST_TAREFA && !escoando) { encerra(s, c); return; }
        // tarefa (de leitura ou de envio) rodando: sai do poll e espera ela devolver, ai encerra
        if (c->Interesse) { net_poll_definir(s->Poll, c->S, 0, c); c->Interesse = 0; }
        return;
    }
    if (depois && vazia && !escoando && atomic_get_inline(&c->Ocupado) == 0 && c->Estado != EST_TAREFA)
    {
        if (c->FimLeitura) { encerra(s, c); return; }
        if (!c->Drenando)
        {
            shutdown(c->S, SD_SEND);
            c->Drenando = true;
            c->DrenoFim = agora_ms() + NET_DRENO_MS;
            prazo_varre(s, c->DrenoFim);
            c->Estado = EST_ARMADO;   // le (e descarta) ate o cliente fechar
        }
    }
    atualiza_interesse(s, c);
}

// ---- tarefa de leitura (pool) ------------------------------------------------------------

// ---- leitura -----------------------------------------------------------------------------
//
// O reator decide a cada volta, pelo TEMPO:
//
//   SINCRONO (o normal): as conexoes prontas sao lidas e despachadas (AoReceber) pelo proprio
//     reator, uma depois da outra, enquanto a volta couber em NET_VOLTA_US. Sem tarefa, sem
//     acordar outra thread: com requisicoes curtas, mesmo varios clientes cabem aqui.
//   ASSINCRONO (por demanda): o que nao couber no orcamento da volta -- muito trabalho de uma
//     vez -- vai para tarefas do pool, em paralelo.
//
// O pool so trabalha quando o volume justifica. Nos dois modos continua valendo: uma conexao
// e lida por um so de cada vez, e os bytes chegam ao protocolo na ordem.

enum { LEU_VAZIO, LEU_FIM, LEU_ORCAMENTO };

// Le o que houver (sem bloquear) e entrega ao protocolo, ate esvaziar, o outro lado fechar,
// ou acabar o orcamento (bytes; tempo, se prazo_us > 0).
static int le_conexao(NetServidor* s, NetConexao* c, char* buf, int64 max_bytes, long long prazo_us)
{
    int64 total = 0;
    for (;;)
    {
        int k = recv(c->S, buf, NET_LER_BLOCO, 0);
        if (k > 0)
        {
            c->Atividade = agora_ms();
            if (c->Drenando)
            {
                c->Drenado += k;
                if (c->Drenado > NET_DRENO_MAX) return LEU_FIM;
                continue;
            }
            s->Proto.AoReceber(c, (const byte*)buf, k);
            total += k;
            if (c->PausaPedida || c->FechaPedido) return LEU_VAZIO;
            if (total >= max_bytes) return LEU_ORCAMENTO;          // ainda pode haver dado
            if (prazo_us > 0 && agora_us() > prazo_us) return LEU_ORCAMENTO;
            continue;
        }
        if (k == 0) return LEU_FIM;
        if (net_last_error_would_block()) return LEU_VAZIO;
        if (WSAGetLastError() == WSAEINTR) continue;
        return LEU_FIM;   // erro de conexao
    }
}

static NET_TLS char* t_buf;
static NET_TLS int   t_reator;   // 1 na thread do reator

bool net_no_reator(void) { return t_reator != 0; }

static void tarefa_leitura(void* arg)
{
    NetConexao* c = (NetConexao*)arg;
    NetServidor* s = c->Srv;
    if (!t_buf) t_buf = (char*)memop_alloc_raw(NET_LER_BLOCO);
    int r = t_buf ? le_conexao(s, c, t_buf, NET_LER_MAX_TAREFA, 0) : LEU_FIM;
    // LEU_ORCAMENTO: ainda ha dado; o reator rearma e, por nivel, a conexao volta ja
    posta(s, c, r == LEU_FIM ? CMD_FIM_LEITURA : CMD_FIM_TAREFA);
    net_soltar(c);   // a referencia da tarefa
}

static void submete_leitura(NetServidor* s, NetConexao* c)
{
    c->Estado = EST_TAREFA;
    atualiza_interesse(s, c);   // tira a leitura do poll enquanto a tarefa roda
    net_segurar(c);
    s->TarefasVivas++;
    atomic_add_inline(&s->ContTarefas, 1);
    bool ok = s->Pool ? pool_submit_relative(s->Pool, tarefa_leitura, c) : pool_submit(tarefa_leitura, c);
    if (!ok) tarefa_leitura(c);   // pool recusou: le aqui mesmo (degradado, mas nao perde)
}

static void conclui_leitura(NetServidor* s, NetConexao* c, bool fim);

// Modo sincrono: o proprio reator le e despacha.
static void le_inline(NetServidor* s, NetConexao* c)
{
    atomic_add_inline(&s->ContInline, 1);
    c->Estado = EST_TAREFA;   // durante o despacho nada encerra a conexao
    long long t0 = agora_us();
    // Limite de BYTES e de TEMPO. So bytes nao bastava: pedidos tem dezenas de bytes, e um
    // cliente rapido (manda o proximo assim que recebe a resposta) prendia o reator nesta
    // conexao por dezenas de ms enquanto as outras esperavam -- medido: p99 de ~30 ms no
    // arquivo de 64 KB, atendimento no reator de ate 68 ms.
    int r = le_conexao(s, c, s->BufInline, NET_INLINE_BYTES, t0 + NET_VOLTA_US);
    long long dur = agora_us() - t0;
    if (dur > s->MaxInlineUs) s->MaxInlineUs = dur;
    if (dur > NET_INLINE_US) s->InlineLentos++;
    if (r == LEU_ORCAMENTO)
    {
        // Muito dado de uma vez (upload) ou a conexao segue mandando (cliente rapido): o resto
        // vai para uma tarefa -- o reator nao pode ficar preso numa conexao so.
        if (dur > NET_VOLTA_US) s->MotivoTempo++; else s->MotivoBytes++;
        c->Estado = EST_ARMADO;
        submete_leitura(s, c);
        return;
    }
    conclui_leitura(s, c, r == LEU_FIM);
}

void net_pedir_relogio(NetConexao* c) { posta(c->Srv, c, CMD_RELOGIO); }

bool net_submeter(NetConexao* c, void (*fn)(void*), void* arg)
{
    NetServidor* s = c->Srv;
    return s->Pool ? pool_submit_relative(s->Pool, fn, arg) : pool_submit(fn, arg);
}

long long net_servidor_acordadas(NetServidor* s) { return atomic_get_inline(&s->Acordadas); }

void net_servidor_contadores(NetServidor* s, long long* inline_, long long* tarefas)
{
    if (inline_) *inline_ = atomic_get_inline(&s->ContInline);
    if (tarefas) *tarefas = atomic_get_inline(&s->ContTarefas);
}

// ---- reator ------------------------------------------------------------------------------

static void endereco(struct sockaddr_in* a, char* out, int cap)
{
    char ip[INET_ADDRSTRLEN] = "?";
    InetNtop(AF_INET, &a->sin_addr, ip, sizeof(ip));
    snprintf(out, (size_t)cap, "%s:%d", ip, ntohs(a->sin_port));
}

static void aceita(NetServidor* s)
{
    for (;;)
    {
        struct sockaddr_in a; socklen_t al = sizeof(a);
        SOCKET k = accept(s->Escuta, (struct sockaddr*)&a, &al);
        if (k == INVALID_SOCKET) return;   // would-block (fila de accept vazia) ou erro passageiro

        net_set_nonblocking(k, 1);
        net_set_nodelay(k);

        if (s->NConex >= s->Cfg.MaxConexoes)
        {
            if (s->RecusaLen > 0) send(k, s->Recusa, s->RecusaLen, NET_SEND_FLAGS);
            closesocket(k);
            continue;
        }

        NetConexao* c = (NetConexao*)memop_calloc_raw(1, sizeof(NetConexao));
        if (!c) { closesocket(k); continue; }
        c->Srv = s; c->S = k; c->Id = (int)k;
        thread_mutex_init_inline(&c->Lock);
        atomic_set_inline(&c->Refs, 1);   // a do reator
        endereco(&a, c->Remoto, sizeof(c->Remoto));
        struct sockaddr_in l; socklen_t ll = sizeof(l);
        if (getsockname(k, (struct sockaddr*)&l, &ll) == 0) endereco(&l, c->Local, sizeof(c->Local));
        c->Estado = EST_ARMADO;
        c->Atividade = agora_ms();
        c->UltimoEnvio = c->Atividade;
        if (s->Cfg.OciosoMs > 0) prazo_varre(s, c->Atividade + s->Cfg.OciosoMs);

        c->Prox = s->Lista; if (s->Lista) s->Lista->Ant = c; s->Lista = c;
        s->NConex++;
        atomic_add_inline(&g_total, 1);

        c->Ctx = s->Proto.AoAbrir ? s->Proto.AoAbrir(c, s->Ctx) : 0;
        if (!c->Ctx) { encerra(s, c); continue; }
        atualiza_interesse(s, c);
    }
}

static void processa_comandos(NetServidor* s)
{
    // Troca os dois vetores: quem posta passa a escrever no outro enquanto este e processado
    // sem lock. Nenhum dos dois e liberado; so crescem.
    thread_mutex_lock_inline(&s->CmdLock);
    Cmd* lote = s->Cmds; int n = s->NCmd; int cap = s->CapCmd;
    s->Cmds = s->Proc; s->CapCmd = s->CapProc; s->NCmd = 0;
    s->Proc = lote; s->CapProc = cap;
    thread_mutex_unlock_inline(&s->CmdLock);

    for (int i = 0; i < n; i++)
    {
        NetConexao* c = lote[i].C;
        switch (lote[i].Tipo)
        {
        case CMD_FIM_TAREFA:
        case CMD_FIM_LEITURA:
            s->TarefasVivas--;   // so tarefas postam FIM_*; o modo sincrono conclui direto
            conclui_leitura(s, c, lote[i].Tipo == CMD_FIM_LEITURA);
            break;
        case CMD_RELOGIO:
            s->TickPedido = true;
            break;
        case CMD_RETOMAR:
            if (!c->Encerrada && c->Estado == EST_PAUSADO && !c->FimLeitura && !c->PausaPedida) c->Estado = EST_ARMADO;
            reavalia(s, c);
            break;
        default:
            reavalia(s, c);
            break;
        }
        net_soltar(c);   // a referencia do comando
    }
}

// A leitura (tarefa ou sincrona) terminou. fim = o outro lado fechou ou deu erro: nao ha
// mais o que ler; termina o que estiver em andamento e o que estiver na fila, depois fecha.
static void conclui_leitura(NetServidor* s, NetConexao* c, bool fim)
{
    if (!c->Encerrada)
    {
        if (!fim) c->Estado = c->PausaPedida ? EST_PAUSADO : EST_ARMADO;
        else
        {
            c->Estado = EST_PAUSADO;
            c->FimLeitura = true;
            thread_mutex_lock_inline(&c->Lock); c->FechaDepois = true; thread_mutex_unlock_inline(&c->Lock);
            if (c->Ctx && s->Proto.AoFimLeitura) s->Proto.AoFimLeitura(c);
        }
    }
    reavalia(s, c);
}

// Encerra o que venceu (dreno, ociosidade) e recalcula o proximo prazo. A atividade de uma
// conexao so EMPURRA o prazo dela para frente: o prazo guardado pode estar cedo (o reator
// acorda, nada venceu, recalcula) mas nunca tarde.
static void varre(NetServidor* s)
{
    long long agora = agora_ms();
    long long prox = NET_SEM_PRAZO;
    NetConexao* c = s->Lista;
    while (c)
    {
        NetConexao* proxc = c->Prox;
        bool escoando;
        {   // envio de arquivo parado (cliente que nao le): solta o bloco
            thread_mutex_lock_inline(&c->Lock);
            escoando = c->Escoando;   // tarefa de envio rodando: o bloco e dela
            NoSaida* n = c->Cab;
            if (!escoando && n && n->Arquivo && n->Bloco)
            {
                long long vence = c->UltimoEnvio + NET_ARQ_SOLTA_MS;
                if (agora >= vence) solta_bloco_locked(n);
                else if (vence < prox) prox = vence;
            }
            thread_mutex_unlock_inline(&c->Lock);
        }
        if (c->Drenando)
        {
            if (agora > c->DrenoFim)
            {
                // com tarefa rodando o socket nao pode fechar debaixo do recv/send dela: pede, e o
                // fim da tarefa (CMD_FIM_* ou o reavalia da tarefa de envio) encerra
                if (c->Estado != EST_TAREFA && !escoando) encerra(s, c);
                else { thread_mutex_lock_inline(&c->Lock); c->FechaPedido = true; thread_mutex_unlock_inline(&c->Lock); }
            }
            else if (c->DrenoFim < prox) prox = c->DrenoFim;
        }
        else if (c->Estado == EST_ARMADO && atomic_get_inline(&c->Ocupado) == 0 && s->Cfg.OciosoMs > 0)
        {
            long long vence = c->Atividade + s->Cfg.OciosoMs;
            if (agora > vence)
            {
                thread_mutex_lock_inline(&c->Lock);
                bool vazia = c->Cab == 0 && !c->Escoando;
                thread_mutex_unlock_inline(&c->Lock);
                if (vazia) { encerra(s, c); c = proxc; continue; }
            }
            if (vence < prox) prox = vence;
        }
        else if (s->Cfg.OciosoMs > 0)
        {
            // ocupada, em tarefa ou pausada: volta a ser candidata quando liberar; revisita
            // no prazo de ociosidade a partir de agora
            long long vence = agora + s->Cfg.OciosoMs;
            if (vence < prox) prox = vence;
        }
        c = proxc;
    }
    s->PrazoVarre = prox;
}

static void libera_mortas(NetServidor* s)
{
    while (s->Mortas)
    {
        NetConexao* c = s->Mortas;
        s->Mortas = c->ProxMorta;
        net_soltar(c);   // a referencia do reator
    }
}

static xthread_result_t reator(void* arg)
{
    NetServidor* s = (NetServidor*)arg;
    t_reator = 1;
#ifndef _WIN32
    {
        // sendfile (escoa) pode escrever num socket que o cliente fechou: sem isto, SIGPIPE
        // mataria o processo. Bloqueado so nesta thread.
        sigset_t ss; sigemptyset(&ss); sigaddset(&ss, SIGPIPE);
        pthread_sigmask(SIG_BLOCK, &ss, 0);
    }
#endif
    NetPollEvento ev[256];
    NetConexao* prontas[256];
    long long prox_debug = agora_ms() + NET_DEBUG_MS;
    s->PrazoVarre = NET_SEM_PRAZO;
    s->TickPedido = true;   // pergunta ao protocolo se ele precisa de relogio

    while (!s->Parar)
    {
        // Dorme ate o menor prazo pendente -- ou para sempre, se nao ha nenhum. Parado e sem
        // conexao ociosa a vencer, o reator nao acorda (bateria).
        long long alvo = s->PrazoVarre < s->PrazoTick ? s->PrazoVarre : s->PrazoTick;
        if (s->Debug && prox_debug < alvo) alvo = prox_debug;
        if (s->TickPedido) alvo = 0;
        int espera;
        if (alvo == NET_SEM_PRAZO) espera = -1;
        else
        {
            long long d = alvo - agora_ms();
            espera = d <= 0 ? 0 : (d > 0x7fffffff ? 0x7fffffff : (int)d);
        }
        int n = net_poll_esperar(s->Poll, ev, 256, espera);
        atomic_add_inline(&s->Acordadas, 1);
        processa_comandos(s);
        int np = 0;
        for (int i = 0; i < n; i++)
        {
            if (ev[i].Dado == &g_marca_escuta) { aceita(s); continue; }
            NetConexao* c = (NetConexao*)ev[i].Dado;
            if (c->Encerrada) continue;
            if (ev[i].Eventos & NET_POLL_ESCREVER) reavalia(s, c);   // escoa a fila
            if (c->Encerrada) continue;
            if ((ev[i].Eventos & (NET_POLL_LER | NET_POLL_FIM)) && c->Estado == EST_ARMADO) prontas[np++] = c;
            else if ((ev[i].Eventos & NET_POLL_FIM) && c->Estado != EST_TAREFA) encerra(s, c);
        }
        // O EXCEDENTE vai primeiro para o pool (comeca a trabalhar em paralelo ja); depois o
        // reator atende as suas, ate MaxSincronoVolta e dentro do orcamento de tempo. Tarefas
        // ja rodando nao impedem: sao outras conexoes.
        if (np > 0)
        {
            int nsinc = np < s->Cfg.MaxSincronoVolta ? np : s->Cfg.MaxSincronoVolta;
            for (int i = nsinc; i < np; i++)
            {
                NetConexao* c = prontas[i];
                if (c->Encerrada || c->Estado != EST_ARMADO) continue;
                s->MotivoQtd++;
                submete_leitura(s, c);
            }
            long long ini = agora_us();
            for (int i = 0; i < nsinc; i++)
            {
                NetConexao* c = prontas[i];
                if (c->Encerrada || c->Estado != EST_ARMADO) continue;
                if (agora_us() - ini < NET_VOLTA_US) le_inline(s, c);
                else { s->MotivoTempo++; submete_leitura(s, c); }
            }
        }
        long long agora = agora_ms();
        if (agora >= s->PrazoVarre) varre(s);
        if (s->TickPedido || agora >= s->PrazoTick)
        {
            s->TickPedido = false;
            int r = s->Proto.AoTick ? s->Proto.AoTick(s->Ctx) : -1;
            s->PrazoTick = r < 0 ? NET_SEM_PRAZO : agora + r;
        }
        if (s->Debug && agora >= prox_debug)
        {
            fprintf(stderr, "[reator] acordadas=%d sincronas=%d tarefas=%d | para o pool: excedente=%lld tempo=%lld bytes=%lld | inline >%d us: %lld, max %lld us\n",
                    atomic_get_inline(&s->Acordadas), atomic_get_inline(&s->ContInline), atomic_get_inline(&s->ContTarefas),
                    s->MotivoQtd, s->MotivoTempo, s->MotivoBytes, NET_INLINE_US, s->InlineLentos, s->MaxInlineUs);
            prox_debug = agora + NET_DEBUG_MS;
        }
        libera_mortas(s);
    }

    // parada: fecha tudo
    closesocket(s->Escuta);
    processa_comandos(s);
    while (s->Lista) encerra(s, s->Lista);
    libera_mortas(s);
    return (xthread_result_t)0;
}

NetServidor* net_servidor_criar(const NetConfig* cfg, const NetProtocolo* proto, void* ctx)
{
    NetServidor* s = (NetServidor*)memop_calloc_raw(1, sizeof(NetServidor));
    if (!s) return 0;
    s->Cfg = *cfg;
    if (s->Cfg.MaxConexoes <= 0) s->Cfg.MaxConexoes = 512;
    if (s->Cfg.MaxFilaSaida <= 0) s->Cfg.MaxFilaSaida = 8LL * 1024 * 1024;
    if (s->Cfg.MaxSincronoVolta <= 0) s->Cfg.MaxSincronoVolta = NET_SINCRONO_VOLTA;
    if (cfg->RecusaCheio)
    {
        snprintf(s->Recusa, sizeof(s->Recusa), "%s", cfg->RecusaCheio);
        s->RecusaLen = (int)strlen(s->Recusa);
    }
    s->Cfg.RecusaCheio = s->Recusa;
    s->Proto = *proto;
    s->Ctx = ctx;
    s->Pool = cfg->Pool;
    s->PrazoVarre = NET_SEM_PRAZO;
    s->PrazoTick  = NET_SEM_PRAZO;
    thread_mutex_init_inline(&s->CmdLock);
    s->BufInline = (char*)memop_alloc_raw(NET_LER_BLOCO);
    {
        const char* d = getenv("APPSERVER_DEBUG_REATOR");
        s->Debug = d && d[0] == '1';
    }
    if (!s->BufInline) { memop_free_raw(s); return 0; }

    s->Escuta = socket(AF_INET, SOCK_STREAM, 0);
    if (s->Escuta == INVALID_SOCKET) { printf("Erro ao criar o socket. Codigo: %d\n", WSAGetLastError()); memop_free_raw(s); return 0; }
    net_set_listen_reuse(s->Escuta);
    struct sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_addr.s_addr = INADDR_ANY; a.sin_port = htons((unsigned short)cfg->Porta);
    if (bind(s->Escuta, (struct sockaddr*)&a, sizeof(a)) == SOCKET_ERROR)
    {
        printf("Erro no bind da porta para servidor. Porta: %d. Codigo: %d\n", cfg->Porta, WSAGetLastError());
        closesocket(s->Escuta); memop_free_raw(s); return 0;
    }
    if (listen(s->Escuta, SOMAXCONN) == SOCKET_ERROR)
    {
        printf("Erro ao colocar em escuta. Codigo: %d\n", WSAGetLastError());
        closesocket(s->Escuta); memop_free_raw(s); return 0;
    }
    socklen_t al = sizeof(a);
    getsockname(s->Escuta, (struct sockaddr*)&a, &al);
    s->Porta = ntohs(a.sin_port);
    net_set_nonblocking(s->Escuta, 1);

    s->Poll = net_poll_criar();
    if (!s->Poll) { closesocket(s->Escuta); memop_free_raw(s); return 0; }
    net_poll_definir(s->Poll, s->Escuta, NET_POLL_LER, &g_marca_escuta);

    int st = 0;
    s->Reator = thread_create(reator, s, &st);
    if (!s->Reator) { net_poll_destruir(s->Poll); closesocket(s->Escuta); memop_free_raw(s); return 0; }
    return s;
}

void net_servidor_parar(NetServidor* s)
{
    if (!s || s->Parar) return;
    s->Parar = 1;
    net_poll_acordar(s->Poll);
    thread_join(&s->Reator);
    // A struct fica: tarefas que ainda estejam no pool vao postar comandos nela. So o poll
    // e liberado quando nao ha mais conexao viva.
}
