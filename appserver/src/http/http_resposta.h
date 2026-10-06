//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  CAMADA 2 (HTTP): o UNICO lugar que escreve uma resposta HTTP.
//
//  Antes havia quatro: o message_assembler (respostas normais), a recusa do parser escrita a
//  mao no appclient, o 503 do limite de conexoes (outra string a mao) e o cabecalho de SSE
//  dentro do controller de video. Cada um com sua ideia de quais cabecalhos mandar. Agora
//  todos montam a partir de um HttpCabecalho, e so aqui.
//
//  So FORMATA em buffer: nao sabe de socket nem de conexao. Nao imprime nada (o log de
//  requisicao e da camada de aplicacao).

#ifndef APPSERVER_HTTP_RESPOSTA_H
#define APPSERVER_HTTP_RESPOSTA_H

#include "../server_type.h"

#ifdef __cplusplus
extern "C" {
#endif

    const char* http_status_texto(HttpStatusCode st);
    const char* http_tipo_texto(ContentTypeOption t);

    typedef struct
    {
        HttpStatusCode    Status;
        const char*       Servidor;    // "Server:"; 0 = nao manda
        ContentTypeOption Tipo;        // CONTENT_TYPE_NONE = sem Content-Type
        int64             Tamanho;     // Content-Length; < 0 = sem (fluxo aberto, ex.: SSE)
        bool              Fechar;      // "Connection: close": a conexao acaba depois desta resposta
        HeaderAppender    Extra;       // cabecalhos a mais (CORS do preflight, upgrade de WebSocket)
        void*             ExtraArgs;
    }
    HttpCabecalho;

    // Cabecalho com os padroes: sem tipo, Content-Length 0, conexao mantida.
    HttpCabecalho http_cabecalho(HttpStatusCode st, const char* servidor);

    // Linha de status + cabecalhos + linha em branco.
    void http_resposta_cabecalho(ResourceBuffer* out, const HttpCabecalho* c);

    // Resposta completa com corpo em memoria. c->Tamanho e ajustado para n.
    // com_corpo = false (HEAD): mesmos cabecalhos, inclusive o Content-Length, sem os bytes.
    void http_resposta_montar(ResourceBuffer* out, HttpCabecalho c, const byte* corpo, int64 n, bool com_corpo);

    // Recusa sem corpo que encerra a conexao (parser recusou, limite de conexoes). Escreve em
    // buffer fixo -- e usada quando nao ha memoria a desperdicar -- e devolve o tamanho.
    int  http_resposta_recusa(char* out, int cap, HttpStatusCode st);

    // Cabecalho de um fluxo SSE (text/event-stream, sem Content-Length, sem cache).
    void http_resposta_sse(ResourceBuffer* out, const char* servidor);

#ifdef __cplusplus
}
#endif

#endif  // APPSERVER_HTTP_RESPOSTA_H
