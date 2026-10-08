//  MIT License � Modified for Mandatory Attribution
//  
//  Copyright(c) 2025 Sergio Paludo
//
//  github.com/sergiocupa
//  
//  Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files, 
//  to use, copy, modify, merge, publish, distribute, and sublicense the software, including for commercial purposes, provided that:
//  
//     01. The original author�s credit is retained in all copies of the source code;
//     02. The original author�s credit is included in any code generated, derived, or distributed from this software, including templates, libraries, or code - generating scripts.
//  
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED.


#ifndef APPSERVERTYPE_H
#define APPSERVERTYPE_H

#ifdef __cplusplus
extern "C" {
#endif

    // Migrado para xplatbase (ListX/StringX/memory_pool/numeric/threads) — camada utilitaria unica.
    #include "atomics.h"   // xatomic_int: contadores de vida do cliente
#include "xplatbase.h"
    #include "utils/xpb_compat.h"   // funcoes stringlib/sha/base64/file reimplementadas sobre StringX
    #include "../submodules/yason/yason/src/yason_element.h"
    
    #define AOTP_HEADER_SIGN "AOTP[9DAC10BA43E2402694C84B21A4219497]"

    #define LIST_CREATE(TYPE, THIS) THIS = list_create(8, sizeof(TYPE))
    #define LIST_ADD(TYPE, THIS, ITEM) list_add(THIS, ITEM, sizeof(TYPE))

    // Split de um trecho de C-string (nao necessariamente terminado em NUL) por 1 char,
    // usando o string_split do xplatbase (que COPIA cada segmento). Retorna ListX de StringX*.
    static inline ListX* string_split_cstr(const char* s, int len, char token)
    {
        StringX _t; string_init(&_t);
        if (len > 0) string_appends(&_t, s, len, 0, len);
        ListX* r = string_split(&_t, token);
        string_release(&_t);
        return r;
    }
 

    typedef struct _AppClientInfo     AppClientInfo;
    typedef struct _AppClientList     AppClientList;
    typedef struct _ClientData        ClientData;
    typedef struct _AppServerInfo     AppServerInfo;
    typedef struct _MessageFieldParam MessageFieldParam;
    typedef struct _Message           Message;
	typedef struct _MessageEvent      MessageEvent;
    typedef struct _MessageEventList  MessageEventList;
    typedef struct _ThunkArgs         ThunkArgs;


    typedef enum _HttpStatusCode
    {
        HTTP_STATUS_NONE                = 0,
        HTTP_STATUS_OK                  = 200,
        HTTP_STATUS_ACCEPT              = 202,
        HTTP_STATUS_PARTIAL_CONTENT     = 206,
        HTTP_STATUS_RANGE_NOT_SATISFIABLE = 416,
        HTTP_STATUS_BAD_REQUEST         = 400,
        HTTP_STATUS_UNAUTHORIZED        = 401,
        HTTP_STATUS_FORBIDDEN           = 403,
        HTTP_STATUS_NOT_FOUND           = 404,
        HTTP_STATUS_METHOD_NOT_ALLOWED  = 405,
        HTTP_STATUS_PRECONDITION_FAILED = 412,
        HTTP_STATUS_PAYLOAD_TOO_LARGE   = 413,
        HTTP_STATUS_HEADERS_TOO_LARGE   = 431,
        HTTP_STATUS_INTERNAL_ERROR      = 500,
        HTTP_STATUS_NOT_IMPLEMENTED     = 501,
        HTTP_STATUS_SERVICE_UNAVAILABLE = 503,
        HTTP_STATUS_SWITCHING_PROTOCOLS = 101
    }
    HttpStatusCode;


    struct _MessageFieldParam
    {
        bool IsScalar;
        bool IsEndGroup;
        bool IsEndParam;
        bool IsHardware;
        StringX Name;
        StringX Value;
        MessageFieldParam* Next;
        MessageFieldParam* Previus;
    };


    typedef struct _MessageField
    {
        StringX            Name;
        StringX            Raw;     // valor CRU (apos ":", sem espacos nas pontas). Param quebra o
                                    // valor em nome=valor, o que destroi base64 (padding "=").
        MessageFieldParam Param;
    }
    MessageField;
    
    typedef struct _MessageFieldList
    {
        int MaxCount;
        int Count;
        MessageField** Items;
    }
    MessageFieldList;

    typedef enum _MessageCommand
    {
        CMD_NONE           = 0,
        CMD_GET            = 1,
        CMD_OPTIONS        = 2,
        CMD_POST           = 3,
        CMD_ACTION         = 4,
        CMD_CALLBACK       = 5,
        CMD_ACKNOWLEDGMENT = 6,
        // Metodos HTTP que antes caiam em CMD_NONE e viravam 500. HEAD e o que player,
        // CDN e ferramenta de diagnostico usam para sondar um recurso sem baixa-lo.
        CMD_HEAD           = 7,
        CMD_PUT            = 8,
        CMD_DELETE         = 9,
        CMD_PATCH          = 10
    }
    MessageCommand;

    typedef enum _MessageProtocol
    {
        PROTOCOL_NONE = 0,
        HTTP          = 1,
        AOTP          = 2
    }
    MessageProtocol;

    typedef enum _MessageConnection
    {
        CONNECTION_NONE       = 0,
        CONNECTION_KEEP_ALIVE = 1,
        CONNECTION_CLOSE      = 2
    }
    MessageConnection;

    typedef enum _ContentTypeOption
    { 
        CONTENT_TYPE_NONE        = 0,
        TEXT_HTML                = 1,
        TEXT_CSS                 = 2,
        TEXT_JAVASCRIPT          = 3,
        TEXT_PLAIN               = 4,
        APPLICATION_JAVASCRIPT   = 5,
        APPLICATION_JSON         = 6,
        APPLICATION_XML          = 7,
        APPLICATION_OCTET_STREAM = 8,
        APPLICATION_PDF          = 9,
        APPLICATION_ZIP          = 10,
        APPLICATION_GZIP         = 11,
        IMAGE_JPEG               = 12,
        IMAGE_PNG                = 13,
        IMAGE_GIF                = 14,
        IMAGE_SVG                = 15,
        AUDIO_MPEG               = 16,
        AUDIO_OGG                = 17,
        VIDEO_MP4                = 18,
        VIDEO_WEBM               = 19,
        MULTIPART_FORMDATA       = 20,
        APPLICATION_MPEGURL      = 21,
        VIDEO_MP2T               = 22
    }
    ContentTypeOption;


    typedef struct _ResourceBuffer
    {
        ContentTypeOption Type;
        int               MaxLength;
        int               Length;
        byte*             Data;
    }
    ResourceBuffer;


    typedef struct _MessageResponseInfo
    {
        int               Status;
        ContentTypeOption ContentType;
        ResourceBuffer    Content;
        MessageFieldList  Fields;
    }
    MessageResponseInfo;


    struct _Message
    {
        bool                 IsMatch;
        MessageProtocol      Protocol;
        StringX               Version;
        MessageCommand       Cmd;
        MessageCommand       OriginCmd;
                             
        ListX          Route;
        StringX               Host;
        MessageConnection    ConnectionOption;
        StringX               UserAgent;
        StringX               Content;       // corpo em memoria (ate Config.BodyToDiskBytes)
        StringX               ContentFile;   // corpo em arquivo temporario (acima); ver app_corpo_*
        ContentTypeOption    ContentType;
        // int64: com int, um upload acima de 2 GiB estourava para negativo e o parser se
        // perdia no calculo do que faltava receber.
        int64                ContentLength;
        MessageFieldList     Fields;
        StringX*              SessionUID;
        StringX*              EventUID;
        StringX*              OriginEventUID;
        MessageFieldParam*   Param;
        StringX*              SecWebsocketKey;
        StringX*              SecWebsocketAccept;
        StringX*              Upgrade;
        void*                MatchThread;
                             
        AppClientInfo*       Client;
        void*                Object;
        struct AppCanal*     Canal;          // canal SSE aberto pelo handler (app_canal_sse)

        MessageResponseInfo* Response;

        // Quando true, o handler ja escreveu a resposta diretamente no socket
        // (ex.: streaming SSE) e o framework nao deve enviar resposta automatica.
        bool                 StreamHandled;
    };




    typedef Element* (*MessageMatchReceiverCalback) (Message* request);
    typedef void (*RequestCallback) (Message* request);
    typedef void (*MessageResultCallback) (ResourceBuffer* result);
    typedef void (*MessageEmitterCalback) (ResourceBuffer* object, MessageResultCallback callback);
    typedef void (*MessageSenderCalback) (ResourceBuffer* object, MessageResultCallback callback, AppClientInfo* client);

    struct _MessageEventList
    {
		int MaxCount;
		int Count;
		MessageEvent** Items;
    };

    struct _MessageEvent
    {
        StringX UID;
        StringX OriginUID;
        bool WaitForCallback;
        MessageCommand LastCommand;
        MessageCommand CurrentStep;
		MessageResultCallback Callback;
        AppClientInfo* Client;
    };

    struct _ThunkArgs
    {
        MessageSenderCalback Sender;
        AppClientInfo*       Client;
    };




    struct HttpParser;   // http/http_parser.h




    struct _AppClientInfo
    {
        bool               IsConnected;
        bool               IsWebSocketMode;
        StringX            LocalHost;
        StringX            RemoteHost;
        void*              Handle;       // numero do socket: so para log
        AppServerInfo*     Server;
        struct HttpParser* Parser;
        struct NetConexao* Net;          // transporte (camada 1)
        struct HttpWs*     Ws;           // quadros WebSocket (modo WebSocket)
        struct AppCanal*   Canal;        // canal SSE aberto nesta conexao
        bool               EmCanal;      // a conexao virou fluxo SSE: entrada e ignorada

        // Requisicoes que chegaram enquanto uma rota longa tinha a vez (http_conexao.c).
        xmutex_t           Lock;
        Message**          Fila;
        int                NFila, CapFila, IniFila;
        bool               Ocupada;
    };


    struct _AppClientList
    {
        int MaxCount;
        int Count;
        AppClientInfo** Items;
    };

    struct _ClientData
    {
        AppClientInfo* Client;
        void* DispatcherThread;

        int   Length;
        byte* Data;
    };



    typedef void(*HeaderAppender) (void* args, ResourceBuffer* http);




    typedef struct _FunctionBind
    {
        bool        IsWebApplication;
        bool        WithCallback;
		bool        IsEventEmitter;
        ListX Route;
        StringX      Extension;
        StringX      AbsPathWebContent;
        bool        Longa;           // roda na pista longa (app_add_receiver_longa)
        xatomic_int Lentas;          // execucoes recentes acima de Config.RotaLentaUs (0..8)
        volatile bool Lenta;         // aprendida: rota curta que demora -> fora do reator
        ThunkArgs   Thung;
		MessageMatchReceiverCalback CallbackFunc;
    }
    FunctionBind;

    typedef struct _FunctionBindList
    {
        int Count;
        int MaxCount;
        FunctionBind** Items;
    }
    FunctionBindList;


    typedef struct _AppServerList
    {
        int Count;
        int MaxCount;
        AppServerInfo** Items;
    }
    AppServerList;


    /* Configuracao do servidor. Parta SEMPRE de appserver_config_default() e mude so o
     * que precisar: assim um campo novo nunca chega zerado a quem ja usava a struct.
     * As strings sao copiadas pelo appserver_create; o chamador pode liberar as suas. */
    typedef struct AppCanal AppCanal;   // http/http_canal.c

    // Onde uma requisicao roda (camada 3 decide, camada 2 executa):
    //   CURTA: na mesma thread que leu (o reator, no modo sincrono)
    //   LENTA: rota curta que demora (aprendido pelo tempo medido): numa tarefa do pool
    //   LONGA: na pista longa (app_add_receiver_longa); a conexao espera a vez
    enum { ROTA_CURTA = 0, ROTA_LENTA = 1, ROTA_LONGA = 2 };

    typedef struct _AppServerConfig
    {
        const char* AgentName;            // nome no cabecalho Server/User-Agent
        int         Port;
        const char* Prefix;               // prefixo das rotas de api ("api" -> /api/...)
        const char* WebContentPath;       // pasta dos arquivos estaticos; 0 ou "" = sem
        bool        EnableHealthMonitor;  // rota embutida /<prefix>/health (ver appserver.h)
        bool        HealthAllowRemote;    // health atende fora de 127.x (env APPSERVER_HEALTH_ALLOW_REMOTE=1)
        int         MaxClients;           // conexoes simultaneas; acima disso responde 503
        int         IdleTimeoutMs;        // conexao sem requisicao em andamento fecha depois disto
        int         MaxHeaderBytes;       // bloco de cabecalhos maior -> 431
        int64       MaxBodyBytes;         // corpo maior -> 413 (env APPSERVER_MAX_BODY_MB)
        int64       BodyToDiskBytes;      // corpo acima disto vai para arquivo em TempDir
        const char* TempDir;              // 0 = pasta temporaria do sistema
        int64       MaxOutputQueueBytes;  // fila de saida por conexao; cliente lento acima disso cai
        int         SseHeartbeatMs;       // comentario periodico nos canais SSE (0 = sem)
        ThreadPool* Pool;                 // 0 = pool global do xplatbase
        bool        LogRequests;          // uma linha no stdout por requisicao e por resposta
        int         RotaLentaUs;          // rota curta acima disto (3 vezes seguidas) sai do reator
        int         MaxSincronoPorVolta;  // conexoes que o reator atende ele mesmo por volta (1); o resto vai ao pool
        int         PerfilPool;           // perfil do pool de tarefas (xplatbase): -1 = nao mexe (padrao),
                                          // POOL_PERFIL_PERFORMANCE ou POOL_PERFIL_ECONOMIA (aparelho com bateria)
        bool        JobsEmPerformance;    // com perfil economia: o pool fica em performance enquanto houver job rodando
        int         LongWorkers;          // maximo de threads da pista longa (criadas sob demanda; paradas, dormem)
        int         SseVoltaUs;           // SSE: quem publica entrega sozinho ate este tempo (1000); o resto vai
                                          // em pistas para o pool. 0 = sempre em serie
        int         SseMaxPorPista;       // SSE: maximo de assinantes por pista (64; 0 = so o tempo decide)
    }
    AppServerConfig;


    struct _AppServerInfo
    {
        AppServerConfig           Config;   // copia; strings apontam para os campos abaixo
        char                      TempDir[1024];
        bool                      IsRunning;
        int                       Port;
        StringX                    AgentName;
        ContentTypeOption         DefaultWebApiObjectType;
        StringX                    AbsLocal;
        ListX*              Prefix;
        struct NetServidor*       Net;           // transporte: reator + conexoes
        struct Pista*             PistaLonga;    // rotas longas e jobs (utils/pista.c), criada no 1o uso
        RequestCallback           Despachar;     // executa uma requisicao (camada 3)
        int                     (*Classificar)(Message* m);   // ROTA_CURTA / ROTA_LENTA / ROTA_LONGA
        xmutex_t                  ClientsLock;
        AppClientList*            Clients;
        xmutex_t                  CanaisLock;    // canais SSE abertos (heartbeat)
        struct AppCanal*          Canais;
        xmutex_t                  TopicosLock;   // topicos e jobs (app_topico.c)
        void*                     Topicos;
        xatomic_int               SsePistas;     // diagnostico: pistas de entrega SSE mandadas ao pool
        xmutex_t                  PerfilLock;    // pedidos de performance (app_perfil_performance_*)
        int                       PerfilPedidos;
        FunctionBindList*         BindList;
        MessageEventList*         Events;
    };



    MessageField* message_field_create(bool init_content);
    MessageField* message_field_release(MessageField* ins);
    void message_field_list_init(MessageFieldList* list);
    void message_field_list_add(MessageFieldList* list, MessageField* field);
    void message_field_list_release(MessageFieldList* list, bool only_data);
    Message* message_create();
    void message_release(Message* m);


    void appclient_list_add(AppClientList* list, AppClientInfo* cli);
    AppClientList* appclient_list_release(AppClientList* list);
    AppClientList* appclient_list_create();



    FunctionBind* bind_create(const char* route);
    FunctionBind* bind_create_to_extension(const char* extension);
    FunctionBindList* bind_list_release(FunctionBindList* list);
    FunctionBindList* bind_list_create();
    void bind_list_add(FunctionBindList* list, FunctionBind* bind);
    FunctionBind* bind_create(const char* route);



    void serverinfo_list_init(AppServerList* list);
    void serverinfo_list_release(AppServerList* list);
    void serverinfo_list_add(AppServerList* list, AppServerInfo* server);
    void serverinfo_release(AppServerInfo* server);
    AppServerInfo* serverinfo_create();


    void resource_buffer_copy(ResourceBuffer* source, ResourceBuffer* dest);
    void resource_buffer_release(ResourceBuffer* source, bool only_data);
    void resource_buffer_append(ResourceBuffer* buffer, byte* data, int length);
    void resource_buffer_init(ResourceBuffer* source);
    void resource_buffer_append_format(ResourceBuffer* buffer, const char* format, ...);
    void resource_buffer_append_string(ResourceBuffer* buffer, const char* data);

    void message_field_list_add_v(MessageFieldList* list, const char* name, const char* value);


    void event_list_init(MessageEventList* list);
    MessageEventList* event_list_create();
    void event_list_add(MessageEventList* list, MessageEvent* item);
    MessageEventList* event_list_release(MessageEventList* list, bool only_data);
    void event_list_remove(MessageEventList* list, MessageEvent* item);

    MessageResponseInfo* message_response_create(int status, ContentTypeOption type);
    void                 message_response_release(MessageResponseInfo* r);

    // Parametro da query string (?nome=valor&...), ja decodificado. Devolve NULL se ausente.
    // O ponteiro vale enquanto a mensagem viver.
    const char*          message_query_get(Message* m, const char* name);

    void                 appclient_list_remove(AppClientList* list, AppClientInfo* cli);
    MessageResponseInfo* message_response_create_content(int status, ContentTypeOption type, char* content, int size);
    MessageResponseInfo* message_response_create_text(int status, char* content);


#ifdef __cplusplus
}
#endif

#endif /* APPSERVERTYPE */