//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  CAMADA 2 (HTTP): canal de eventos (SSE) sobre uma conexao.
//
//  Substitui o SSE escrito a mao nos controllers (send() cru no socket, cabecalho montado
//  na mao). O canal usa o formatador unico e a fila de saida da conexao: escrever num
//  cliente lento nao bloqueia quem publica, e um cliente que nao le cai ao passar do limite
//  da fila, em vez de segurar memoria sem fim.
//
//  Dois usos:
//    - canal do handler (app_canal_sse numa rota longa): vive enquanto o handler roda; ao
//      retornar, o despacho fecha o canal e a conexao;
//    - canal de assinatura (topicos, app_assinar): destacado da requisicao, sobrevive ao
//      handler, que ja retornou. Nao ocupa thread nenhuma: so a conexao aberta no reator.
//
//  Heartbeat: a cada Config.SseHeartbeatMs sem evento, um comentario SSE (": \n\n"). Mantem
//  proxies sem cortar a conexao ociosa e descobre cliente que sumiu (o envio falha).

#include "http_canal.h"
#include "http_resposta.h"
#include "http_conexao.h"
#include "../net/net_servidor.h"
#include <stdio.h>
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

struct AppCanal
{
    NetConexao*        Net;
    AppClientInfo*     Cli;
    AppServerInfo*     Srv;
    volatile int       Morto;          // a conexao caiu ou um envio falhou
    volatile long long UltimoEnvio;
    struct AppCanal   *Prox, *Ant;     // lista do servidor (CanaisLock)
};

static void lista_tira(AppServerInfo* s, AppCanal* k)
{
    if (k->Ant) k->Ant->Prox = k->Prox; else s->Canais = k->Prox;
    if (k->Prox) k->Prox->Ant = k->Ant;
    k->Prox = k->Ant = 0;
}

AppCanal* app_canal_sse(Message* request)
{
    if (!request || !request->Client || !request->Client->Net) return 0;
    AppClientInfo* c = request->Client;
    AppServerInfo* s = c->Server;

    ResourceBuffer b; resource_buffer_init(&b);
    http_resposta_sse(&b, s ? s->Config.AgentName : 0);
    bool ok = net_enviar(c->Net, b.Data, b.Length);
    resource_buffer_release(&b, true);
    request->StreamHandled = true;   // a resposta e o proprio fluxo: o despacho nao manda outra
    if (!ok) return 0;

    AppCanal* k = (AppCanal*)memop_calloc_raw(1, sizeof(AppCanal));
    if (!k) return 0;
    k->Net = c->Net; k->Cli = c; k->Srv = s;
    k->UltimoEnvio = agora_ms();
    net_segurar(k->Net);
    net_ocupar(k->Net, +1);   // canal aberto: a conexao nao e fechada por ociosidade

    thread_mutex_lock_inline(&s->CanaisLock);
    k->Prox = s->Canais; if (s->Canais) s->Canais->Ant = k; s->Canais = k;
    c->Canal = k;
    c->EmCanal = true;        // o que o cliente ainda mandar nesta conexao e ignorado
    thread_mutex_unlock_inline(&s->CanaisLock);

    request->Canal = k;
    net_pedir_relogio(k->Net);   // agora ha canal: o reator agenda o heartbeat
    return k;
}

void http_canal_destacar(Message* request)
{
    if (request) request->Canal = 0;   // o despacho nao fecha: quem guardou o canal fecha
}

bool app_canal_ativo(AppCanal* k) { return k && !k->Morto; }

static bool envia(AppCanal* k, const void* d, int n)
{
    if (k->Morto) return false;
    if (!net_enviar(k->Net, d, n)) { k->Morto = 1; return false; }
    k->UltimoEnvio = agora_ms();
    return true;
}

// "event: <nome>\n" (opcional) + uma linha "data: " por linha de 'dados' + linha em branco.
void http_canal_formata(ResourceBuffer* b, const char* nome, const char* dados)
{
    if (nome && nome[0]) resource_buffer_append_format(b, "event: %s\n", nome);
    const char* p = dados ? dados : "";
    for (;;)
    {
        const char* nl = strchr(p, '\n');
        int n = nl ? (int)(nl - p) : (int)strlen(p);
        resource_buffer_append_string(b, "data: ");
        if (n > 0) resource_buffer_append(b, (byte*)p, n);
        resource_buffer_append_string(b, "\n");
        if (!nl) break;
        p = nl + 1;
    }
    resource_buffer_append_string(b, "\n");
}

bool http_canal_enviar_pronto(AppCanal* k, const byte* d, int n)
{
    return k && envia(k, d, n);
}

bool app_canal_evento(AppCanal* k, const char* nome, const char* dados)
{
    if (!k || k->Morto) return false;
    ResourceBuffer b; resource_buffer_init(&b);
    http_canal_formata(&b, nome, dados);
    bool ok = envia(k, b.Data, b.Length);
    resource_buffer_release(&b, true);
    return ok;
}

void app_canal_fechar(AppCanal* k)
{
    if (!k) return;
    AppServerInfo* s = k->Srv;
    thread_mutex_lock_inline(&s->CanaisLock);
    lista_tira(s, k);
    if (k->Cli && k->Cli->Canal == k) k->Cli->Canal = 0;
    thread_mutex_unlock_inline(&s->CanaisLock);

    net_fechar(k->Net, true);   // SSE nao tem Content-Length: o fim do fluxo e o fim da conexao
    net_ocupar(k->Net, -1);
    net_soltar(k->Net);
    memop_free_raw(k);
}

void http_canal_conexao_caiu(AppClientInfo* c)
{
    AppServerInfo* s = c->Server;
    thread_mutex_lock_inline(&s->CanaisLock);
    if (c->Canal) c->Canal->Morto = 1;
    thread_mutex_unlock_inline(&s->CanaisLock);
}

int http_canal_tick(AppServerInfo* s)
{
    int hb = s->Config.SseHeartbeatMs;
    if (hb <= 0) return -1;
    long long agora = agora_ms();
    long long prox = -1;   // ms ate o proximo heartbeat devido
    static const char comentario[] = ": \n\n";
    thread_mutex_lock_inline(&s->CanaisLock);
    for (AppCanal* k = s->Canais; k; k = k->Prox)
    {
        if (k->Morto) continue;
        if (agora - k->UltimoEnvio >= hb) envia(k, comentario, (int)sizeof(comentario) - 1);
        long long falta = k->UltimoEnvio + hb - agora;
        if (falta < 0) falta = 0;
        if (prox < 0 || falta < prox) prox = falta;
    }
    thread_mutex_unlock_inline(&s->CanaisLock);
    return prox < 0 ? -1 : (int)(prox > 0x7fffffff ? 0x7fffffff : prox);
}
