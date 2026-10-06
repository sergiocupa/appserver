//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  CAMADA 2 (HTTP): parser INCREMENTAL de requisicoes.
//
//  Recebe bytes em pedacos de qualquer tamanho (como chegam do socket) e entrega cada
//  requisicao COMPLETA, na ordem, por callback -- sincrono, dentro de http_parser_alimentar.
//  Nao sabe de socket, de thread, nem de como a mensagem vai ser despachada: isso e de quem
//  chama (hoje a conexao por thread; na Fase 4, a tarefa do reator).
//
//  Corpo:
//    - ate BodyToDiskBytes: em memoria, em Message->Content;
//    - acima: gravado direto num arquivo temporario em TempDir, conforme chega. A memoria
//      fica constante qualquer que seja o tamanho do upload. Message->ContentFile guarda o
//      caminho; message_release apaga o arquivo, a menos que o handler o tenha levado
//      (app_corpo_salvar). Message->ContentLength vale nos dois casos.
//
//  Erro (cabecalho grande demais, requisicao malformada, corpo acima do limite, falha ao
//  gravar o temporario): chama ao_erro UMA vez com o status HTTP adequado e passa a recusar
//  tudo (alimentar devolve false). Quem chama responde e fecha a conexao.

#ifndef APPSERVER_HTTP_PARSER_H
#define APPSERVER_HTTP_PARSER_H

#include "../server_type.h"

#ifdef __cplusplus
extern "C" {
#endif

    typedef struct HttpParser HttpParser;

    typedef struct
    {
        int64       MaxHeaderBytes;
        int64       MaxBodyBytes;
        int64       BodyToDiskBytes;
        const char* TempDir;          // sem barra final
    }
    HttpParserLimites;

    typedef void (*HttpAoMensagem)(void* ctx, Message* msg);       // msg passa a ser de quem recebe
    typedef void (*HttpAoErro)(void* ctx, HttpStatusCode status);

    // Limites a partir da configuracao do servidor.
    HttpParserLimites http_parser_limites(const AppServerConfig* cfg);

    HttpParser* http_parser_criar(const HttpParserLimites* lim, HttpAoMensagem ao_mensagem, HttpAoErro ao_erro, void* ctx);
    void        http_parser_destruir(HttpParser* p);   // descarta requisicao pela metade (e o temporario dela)

    // Alimenta bytes recebidos. false = parser em erro (ja avisou por ao_erro).
    bool        http_parser_alimentar(HttpParser* p, const byte* dados, int n);

    // Ha uma requisicao pela metade (cabecalho ou corpo ainda chegando)?
    bool        http_parser_ocupado(const HttpParser* p);

    // Caminho do temporario da requisicao EM ANDAMENTO (corpo grande chegando), ou 0.
    const char* http_parser_arquivo_parcial(const HttpParser* p);

    // ---- auxiliares de parse, usados tambem pelo formatador e pelo despacho ----
    const char* message_command_titule(MessageCommand cmd);
    int         message_parser_field(byte* data, int length, MessageFieldList* fields, int* position);
    void        message_field_param_add(byte* data, int begin, int end, bool first, bool is_within, MessageFieldParam* param);

#ifdef __cplusplus
}
#endif

#endif  // APPSERVER_HTTP_PARSER_H
