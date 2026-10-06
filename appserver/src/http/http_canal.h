//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  CAMADA 2 (HTTP): canal SSE. A API publica (app_canal_*) esta em appserver.h; aqui ficam
//  so as ligacoes internas com a conexao, o relogio e os topicos.

#ifndef APPSERVER_HTTP_CANAL_H
#define APPSERVER_HTTP_CANAL_H

#include "../../include/appserver.h"

#ifdef __cplusplus
extern "C" {
#endif

    // O canal deixa de pertencer a requisicao: o despacho nao o fecha quando o handler
    // retorna (topicos guardam o canal e fecham quando o topico acaba ou o cliente sai).
    void http_canal_destacar(Message* request);

    // A conexao do canal acabou (chamado pelo fechamento da conexao).
    void http_canal_conexao_caiu(AppClientInfo* c);

    // Formata um evento SSE ("data: ...\n\n") uma vez, para mandar a varios canais.
    void http_canal_formata(ResourceBuffer* out, const char* nome, const char* dados);
    // Manda bytes ja formatados (http_canal_formata). false = canal morto.
    bool http_canal_enviar_pronto(AppCanal* k, const byte* d, int n);

    // Relogio: heartbeat dos canais parados. Devolve em quantos ms precisa ser chamado de
    // novo, ou -1 se nao ha canal (o reator nao acorda a toa).
    int  http_canal_tick(AppServerInfo* s);

#ifdef __cplusplus
}
#endif

#endif  // APPSERVER_HTTP_CANAL_H
