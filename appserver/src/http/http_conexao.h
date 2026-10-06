//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  CAMADA 2 (HTTP): uma conexao HTTP sobre o transporte (net_servidor).
//
//  E o NetProtocolo do servidor: recebe os bytes da tarefa de leitura, passa pelo parser e
//  despacha cada requisicao completa, EM ORDEM:
//
//    - rota curta: roda ali mesmo, na tarefa de leitura (worker do pool). A resposta sai
//      antes de a proxima requisicao da mesma conexao ser lida -- pipelining em ordem.
//    - rota longa: vai para a pista longa (utils/pista.c, AppServerInfo->PistaLonga), a
//      leitura da conexao pausa, e as requisicoes que ja tinham chegado esperam na fila da
//      conexao. Quando a longa termina, a fila anda e a leitura volta.
//
//  Quem diz se a rota e longa e quem executa a requisicao e a camada 3 (AppServerInfo:
//  EhLonga, Despachar). Esta camada so garante a ordem e a vez.

#ifndef APPSERVER_HTTP_CONEXAO_H
#define APPSERVER_HTTP_CONEXAO_H

#include "../server_type.h"
#include "../net/net_servidor.h"

#ifdef __cplusplus
extern "C" {
#endif

    const NetProtocolo* http_conexao_protocolo(void);

    // Pista longa do servidor (rotas longas e jobs): threads que dormem quando paradas,
    // criadas sob demanda ate Config.LongWorkers. false = nao conseguiu submeter.
    bool http_pista_longa(AppServerInfo* s, void (*fn)(void*), void* arg);

    // Envia bytes crus na conexao (qualquer thread). Em modo WebSocket, empacota num quadro.
    void appclient_send(AppClientInfo* cli, byte* content, int length, bool is_websocket);

    // Envia um arquivo (ou trecho) sem carrega-lo: sai em blocos pela fila da conexao,
    // conforme o socket aceita. false = nao abriu (a conexao e fechada: o cabecalho com o
    // Content-Length ja pode ter saido).
    bool appclient_send_file(AppClientInfo* cli, const char* caminho, int64 inicio, int64 tamanho);

    // Responde 'status' com "Connection: close" e encerra a conexao depois de enviar.
    void appclient_reject(AppClientInfo* cli, HttpStatusCode status);

    // Conexoes vivas no processo (diagnostico).
    int  appclient_count(void);

    // Chama fn para cada cliente conectado, sob o lock da lista do servidor. fn nao pode
    // bloquear (so enfileirar envio).
    void appclient_para_cada(AppServerInfo* s, void (*fn)(AppClientInfo* cli, void* arg), void* arg);

#ifdef __cplusplus
}
#endif

#endif  // APPSERVER_HTTP_CONEXAO_H
