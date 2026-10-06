//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Conexao HTTP sobre o reator. Ver http_conexao.h.
//
//  Quem tem a vez de despachar requisicoes de uma conexao, em cada momento, e UM so:
//    - a tarefa de leitura, enquanto a conexao nao esta Ocupada e a fila esta vazia;
//    - a rota longa em andamento (Ocupada = true); o que chega nesse meio tempo entra na fila;
//    - a continuacao (tarefa do pool) que, terminada a longa, esvazia a fila em ordem.
//  Assim as respostas saem na ordem das requisicoes, que e o que o HTTP/1.1 exige.

#include "http_conexao.h"
#include "http_parser.h"
#include "http_resposta.h"
#include "http_ws.h"
#include "http_canal.h"
#include "atomics.h"
#include "../utils/pista.h"
#include <stdio.h>
#include <string.h>

static void despacha(AppClientInfo* c, Message* msg);

// ---- fila de requisicoes esperando a vez (sob c->Lock) ------------------------------------

static void fila_poe(AppClientInfo* c, Message* m)
{
    if (c->NFila == c->CapFila)
    {
        int cap = c->CapFila ? c->CapFila * 2 : 8;
        Message** n = (Message**)memop_calloc_raw((uint64)cap, sizeof(Message*));
        for (int i = 0; i < c->NFila; i++) n[i] = c->Fila[(c->IniFila + i) % c->CapFila];
        if (c->Fila) memop_free_raw(c->Fila);
        c->Fila = n; c->CapFila = cap; c->IniFila = 0;
    }
    c->Fila[(c->IniFila + c->NFila) % c->CapFila] = m;
    c->NFila++;
}

static Message* fila_tira(AppClientInfo* c)
{
    if (c->NFila == 0) return 0;
    Message* m = c->Fila[c->IniFila];
    c->IniFila = (c->IniFila + 1) % c->CapFila;
    c->NFila--;
    return m;
}

// ---- rota longa ---------------------------------------------------------------------------

typedef struct { AppClientInfo* C; Message* M; } Longa;

static void continua(void* arg);

static void roda_longa(void* arg)
{
    Longa* l = (Longa*)arg;
    AppClientInfo* c = l->C;
    if (c->IsConnected) c->Server->Despachar(l->M);
    message_release(l->M);
    memop_free_raw(l);
    net_ocupar(c->Net, -1);
    // A fila anda na pista normal: as proximas requisicoes costumam ser curtas.
    if (!net_submeter(c->Net, continua, c)) continua(c);
}

// Terminada uma longa: despacha o que ficou na fila, em ordem; vazia, a leitura volta.
static void continua(void* arg)
{
    AppClientInfo* c = (AppClientInfo*)arg;
    NetConexao* net = c->Net;
    for (;;)
    {
        thread_mutex_lock_inline(&c->Lock);
        Message* m = fila_tira(c);
        if (!m) c->Ocupada = false;
        thread_mutex_unlock_inline(&c->Lock);
        if (!m) { net_retomar_leitura(net); break; }
        if (!c->IsConnected) { message_release(m); continue; }
        // aqui ja e uma tarefa do pool: so a LONGA sai para a pista dela
        if (c->Server->Classificar && c->Server->Classificar(m) == ROTA_LONGA) { despacha(c, m); break; }
        c->Server->Despachar(m);
        message_release(m);
    }
    net_soltar(net);   // a referencia tomada quando a longa comecou
}

// Pista longa: criada na primeira rota longa. Servidor sem rota longa nao paga as threads.
bool http_pista_longa(AppServerInfo* s, void (*fn)(void*), void* arg)
{
    Pista* p = s->PistaLonga;
    if (!p)
    {
        thread_mutex_lock_inline(&s->ClientsLock);
        if (!s->PistaLonga) s->PistaLonga = pista_criar(s->Config.LongWorkers > 0 ? s->Config.LongWorkers : 16);
        p = s->PistaLonga;
        thread_mutex_unlock_inline(&s->ClientsLock);
    }
    return p && pista_submeter(p, fn, arg);
}

static void despacha(AppClientInfo* c, Message* msg)
{
    AppServerInfo* s = c->Server;
    int modo = s->Classificar ? s->Classificar(msg) : ROTA_CURTA;
    // LENTA so precisa sair do REATOR (no modo sincrono ele e quem esta despachando); numa
    // tarefa do pool ela roda ali mesmo.
    if (modo == ROTA_LENTA && !net_no_reator()) modo = ROTA_CURTA;
    if (modo != ROTA_CURTA)
    {
        thread_mutex_lock_inline(&c->Lock);
        c->Ocupada = true;
        thread_mutex_unlock_inline(&c->Lock);
        net_pausar_leitura(c->Net);
        net_ocupar(c->Net, +1);   // em andamento: nao fecha por ociosidade
        net_segurar(c->Net);      // ate a continuacao terminar

        Longa* l = (Longa*)memop_alloc_raw(sizeof(Longa));
        l->C = c; l->M = msg;
        bool ok;
        if (modo == ROTA_LONGA) ok = http_pista_longa(s, roda_longa, l);
        else ok = net_submeter(c->Net, roda_longa, l);   // LENTA: fora do reator, nos leitores
        if (!ok) roda_longa(l);
        return;
    }
    s->Despachar(msg);
    message_release(msg);
}

// ---- callbacks do parser ------------------------------------------------------------------

static void ao_mensagem(void* ctx, Message* msg)
{
    AppClientInfo* c = (AppClientInfo*)ctx;
    msg->Client = c;
    thread_mutex_lock_inline(&c->Lock);
    bool espera = c->Ocupada || c->NFila > 0;
    if (espera) fila_poe(c, msg);
    thread_mutex_unlock_inline(&c->Lock);
    if (!espera) despacha(c, msg);
}

static void ao_erro(void* ctx, HttpStatusCode st)
{
    appclient_reject((AppClientInfo*)ctx, st);
}

// ---- NetProtocolo -------------------------------------------------------------------------

static void* con_abrir(NetConexao* n, void* ctx_srv)
{
    AppServerInfo* s = (AppServerInfo*)ctx_srv;
    AppClientInfo* c = (AppClientInfo*)memop_calloc_raw(1, sizeof(AppClientInfo));
    if (!c) return 0;
    c->Server = s;
    c->Net = n;
    c->Handle = (void*)(intptr_t)net_id(n);
    c->IsConnected = true;
    const char* l = net_local(n); const char* r = net_remoto(n);
    string_init(&c->LocalHost);  string_appends(&c->LocalHost, l, (int)strlen(l), 0, (int)strlen(l));
    string_init(&c->RemoteHost); string_appends(&c->RemoteHost, r, (int)strlen(r), 0, (int)strlen(r));
    thread_mutex_init_inline(&c->Lock);
    HttpParserLimites lim = http_parser_limites(&s->Config);
    c->Parser = http_parser_criar(&lim, ao_mensagem, ao_erro, c);

    thread_mutex_lock_inline(&s->ClientsLock);
    appclient_list_add(s->Clients, c);
    thread_mutex_unlock_inline(&s->ClientsLock);
    return c;
}

// WebSocket: cada mensagem completa vai ao parser (mensagens AOTP); controle (ping, close)
// e respondido direto na conexao.
static void ws_mensagem(void* ctx, const byte* d, int64 n)
{
    AppClientInfo* c = (AppClientInfo*)ctx;
    if (n > 0) http_parser_alimentar(c->Parser, d, (int)n);
}

static void ws_responder(void* ctx, const byte* q, int n)
{
    net_enviar(((AppClientInfo*)ctx)->Net, q, n);
}

static void con_receber(NetConexao* n, const byte* d, int k)
{
    AppClientInfo* c = (AppClientInfo*)net_contexto(n);
    if (!c->IsConnected || c->EmCanal) return;   // fluxo SSE: o cliente nao manda mais nada util
    if (!c->IsWebSocketMode)
    {
        http_parser_alimentar(c->Parser, d, k);
        return;
    }
    if (!c->Ws) c->Ws = http_ws_criar(c->Server->Config.MaxBodyBytes, ws_mensagem, c);
    if (!c->Ws || !http_ws_receber(c->Ws, d, k, ws_responder)) net_fechar(n, true);
}

static void con_fechar(NetConexao* n)
{
    AppClientInfo* c = (AppClientInfo*)net_contexto(n);
    c->IsConnected = false;
    http_canal_conexao_caiu(c);
    AppServerInfo* s = c->Server;
    thread_mutex_lock_inline(&s->ClientsLock);
    appclient_list_remove(s->Clients, c);
    thread_mutex_unlock_inline(&s->ClientsLock);
}

static void con_liberar(void* ctx)
{
    AppClientInfo* c = (AppClientInfo*)ctx;
    Message* m;
    while ((m = fila_tira(c)) != 0) message_release(m);
    if (c->Fila) memop_free_raw(c->Fila);
    if (c->Parser) http_parser_destruir(c->Parser);
    if (c->Ws) http_ws_destruir(c->Ws);
    if (c->LocalHost.Content)  string_release_data(&c->LocalHost);
    if (c->RemoteHost.Content) string_release_data(&c->RemoteHost);
    thread_mutex_destroy_inline(&c->Lock);
    memop_free_raw(c);
}

static int con_tick(void* ctx_srv) { return http_canal_tick((AppServerInfo*)ctx_srv); }

// O cliente fechou o envio. Numa conexao que virou fluxo SSE isso e o fim: ninguem mais le
// o fluxo. Fecha ja, em vez de descobrir so no proximo evento (ou heartbeat) que falhar.
static void con_fim_leitura(NetConexao* n)
{
    AppClientInfo* c = (AppClientInfo*)net_contexto(n);
    if (!c->EmCanal) return;   // requisicao comum: a resposta ainda sai (half-close e valido)
    http_canal_conexao_caiu(c);
    net_fechar(n, false);
}

static const NetProtocolo g_proto = { con_abrir, con_receber, con_fechar, con_liberar, con_tick, con_fim_leitura };

const NetProtocolo* http_conexao_protocolo(void) { return &g_proto; }

// ---- envio --------------------------------------------------------------------------------

void appclient_send(AppClientInfo* cli, byte* content, int length, bool is_websocket)
{
    if (!cli || length <= 0) return;
    if (!is_websocket) { net_enviar(cli->Net, content, length); return; }
    // quadro de texto do SERVIDOR: sem mascara (o antigo mascarava; o RFC proibe)
    ResourceBuffer q; resource_buffer_init(&q);
    http_ws_quadro(&q, HTTP_WS_TEXTO, content, length);
    net_enviar(cli->Net, q.Data, q.Length);
    resource_buffer_release(&q, true);
}

bool appclient_send_file(AppClientInfo* cli, const char* caminho, int64 inicio, int64 tamanho)
{
    if (!cli) return false;
    if (net_enviar_arquivo(cli->Net, caminho, inicio, tamanho)) return true;
    net_fechar(cli->Net, false);
    return false;
}

void appclient_reject(AppClientInfo* cli, HttpStatusCode status)
{
    char r[256];
    int n = http_resposta_recusa(r, sizeof(r), status);
    if (n > 0) net_enviar(cli->Net, r, n);
    if (cli->Server && cli->Server->Config.LogRequests)
        printf("RESPONSE | Client: %d | Status: %d (conexao encerrada)\n", (int)(intptr_t)cli->Handle, (int)status);
    // Escoa, fecha o envio e drena o que o cliente ainda manda: fechar com dado nao lido vira
    // RST e o cliente perde a propria resposta de erro (medido com o 431).
    net_fechar(cli->Net, true);
}

int appclient_count(void) { return net_conexoes_total(); }

void appclient_para_cada(AppServerInfo* s, void (*fn)(AppClientInfo*, void*), void* arg)
{
    thread_mutex_lock_inline(&s->ClientsLock);
    for (int i = 0; i < s->Clients->Count; i++) fn(s->Clients->Items[i], arg);
    thread_mutex_unlock_inline(&s->ClientsLock);
}
