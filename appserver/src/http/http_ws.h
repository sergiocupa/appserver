//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  CAMADA 2 (HTTP): WebSocket (RFC 6455) sobre uma conexao ja em modo WebSocket.
//
//  Antes cada recv era tratado como UM quadro inteiro: quadro partido em dois recv, ou dois
//  quadros no mesmo recv, se perdiam. Aqui os bytes acumulam e cada quadro e montado quando
//  chega inteiro; mensagem fragmentada (FIN=0 + continuacoes) e remontada; ping vira pong;
//  close responde close e fecha.
//
//  Quadro do SERVIDOR sai sem mascara: o RFC proibe mascarar do servidor para o cliente, e
//  o navegador derruba a conexao que recebe quadro mascarado (o codigo antigo mascarava).

#ifndef APPSERVER_HTTP_WS_H
#define APPSERVER_HTTP_WS_H

#include "../server_type.h"

#ifdef __cplusplus
extern "C" {
#endif

    #define HTTP_WS_TEXTO   0x1
    #define HTTP_WS_BINARIO 0x2
    #define HTTP_WS_FECHA   0x8
    #define HTTP_WS_PING    0x9
    #define HTTP_WS_PONG    0xA

    typedef struct HttpWs HttpWs;

    // entrega: cada MENSAGEM completa (texto ou binario), ja sem mascara.
    typedef void (*HttpWsAoMensagem)(void* ctx, const byte* dados, int64 n);

    HttpWs* http_ws_criar(int64 max_mensagem, HttpWsAoMensagem ao_mensagem, void* ctx);
    void    http_ws_destruir(HttpWs* w);

    // Bytes crus recebidos. Controle (ping/close) e respondido por 'responder' (bytes prontos
    // para o socket). Devolve false se a conexao deve fechar (close recebido, quadro invalido
    // ou mensagem acima do maximo).
    typedef void (*HttpWsResponder)(void* ctx, const byte* quadro, int n);
    bool    http_ws_receber(HttpWs* w, const byte* dados, int n, HttpWsResponder responder);

    // Monta um quadro do servidor (sem mascara) em 'out'. Devolve o tamanho.
    int64   http_ws_quadro(ResourceBuffer* out, int opcode, const byte* dados, int64 n);

#ifdef __cplusplus
}
#endif

#endif  // APPSERVER_HTTP_WS_H
