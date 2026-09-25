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


#include "../include/appserver.h"
#include "appclient.h"
#include "event_server.h"
#include "utils/callback_binder.h"
#include "utils/message_assembler.h"
#include "utils/message_parser.h"
#include "utils/activity_binder.h"
#include "utils/health_monitor.h"   // painel de saude embutido
#include "yason.h"
#include "utils/websocket_util.h"

#include "utils/net_compat.h"   // Winsock no Windows, BSD sockets no Linux

#include <stdio.h>


int ServerInitialized = false;
MessageField HTTP_HEADER_ALLOW_HEADERS;
MessageField HTTP_HEADER_ALLOW_METHODS;
AppServerList Servers;



// Definido em health_controller.c. Declarado aqui para nao criar um header so por
// causa de um simbolo interno da biblioteca.
Element* appserver_health_route(Message* request);


void send_response_server_error(Message* request, const char* msg)
{
    // Era sizeof(msg): o TAMANHO DO PONTEIRO, 8 em x64. Todo erro 500 saia com
    // Content-Length 8 e o texto cortado, o que escondia qual das causas tinha sido.
    int msgz = msg ? (int)strlen(msg) : 0;
    ResourceBuffer rb = { .Data = msg, .Length = msgz, .Type = TEXT_PLAIN };
    appserver_http_response_send(request->Client->Server, request, HTTP_STATUS_INTERNAL_ERROR, &rb, 0, 0);
}


void _ReportRequest(Message* request)
{
    char* a  = message_command_titule(request->Cmd);
    char* tp = message_assembler_append_content_type(request->ContentType);

    StringX b;
    string_init(&b);
    if (request->Route.Count > 0)
    {
        int CNT = request->Route.Count - 1;
        int ix = 0;
        while (ix < CNT)
        {
            string_append_s(&b, request->Route.Items[ix]);
            string_append_char(&b, '/');
            ix++;
        }
        string_append_s(&b, request->Route.Items[ix]);
    }

    printf("REQUEST  | Client: %d | Method: %s | Route: '%s' | Type: %s | Content Length: %d\n", (int)(intptr_t)request->Client->Handle, a, b.Content, tp, request->ContentLength);
    string_release_data(&b);
}

void ReportRequest(Message* request)
{
    //#ifdef _DEBUG
    _ReportRequest(request);
    //#endif 
}


int WsaInit()
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        printf("Erro na inicializa��o do Winsock. C�digo: %d\n", WSAGetLastError());
        return 1;
    }
    return 0;
}




void appserver_create_default_headers()
{
    memset(&HTTP_HEADER_ALLOW_HEADERS, 0, sizeof(MessageField));
    memset(&HTTP_HEADER_ALLOW_METHODS, 0, sizeof(MessageField));

    string_init(&HTTP_HEADER_ALLOW_HEADERS.Name);
    string_init(&HTTP_HEADER_ALLOW_HEADERS.Param.Value);
    string_appends(&HTTP_HEADER_ALLOW_HEADERS.Name, "Access-Control-Allow-Headers", (int)strlen("Access-Control-Allow-Headers"), 0, (int)strlen("Access-Control-Allow-Headers"));
    string_appends(&HTTP_HEADER_ALLOW_HEADERS.Param.Value, "Content-Type, Authorization", (int)strlen("Content-Type, Authorization"), 0, (int)strlen("Content-Type, Authorization"));

    string_init(&HTTP_HEADER_ALLOW_METHODS.Name);
    string_init(&HTTP_HEADER_ALLOW_METHODS.Param.Value);
    string_appends(&HTTP_HEADER_ALLOW_METHODS.Name, "Access-Control-Allow-Methods", (int)strlen("Access-Control-Allow-Methods"), 0, (int)strlen("Access-Control-Allow-Methods"));
    string_appends(&HTTP_HEADER_ALLOW_METHODS.Param.Value, "GET, POST, OPTIONS", (int)strlen("GET, POST, OPTIONS"), 0, (int)strlen("GET, POST, OPTIONS"));
}


void appserver_init()
{
    if (ServerInitialized) return;

    WsaInit();
    appserver_create_default_headers();
    serverinfo_list_init(&Servers);

    ServerInitialized = true;
}


void appserver_shutdown()
{
    if (!ServerInitialized) return;

    health_monitor_shutdown();   // no-op se nunca foi iniciado; fecha a consulta do PDH


    //...


    ServerInitialized = false;
}


void appserver_http_response_send(AppServerInfo* server, Message* request, HttpStatusCode http_status, ResourceBuffer* object, HeaderAppender header_appender, void* appender_args)
{
    ResourceBuffer http;
    resource_buffer_init(&http);

    message_assembler_prepare(http_status, server->AgentName.Content, request->Client->LocalHost.Content, header_appender, appender_args, object, &http, (int)(intptr_t)request->Client->Handle);
    appclient_send(request->Client, http.Data, http.Length, false);
}

// Cabecalhos CORS da resposta ao OPTIONS (preflight). O codigo antigo passava o ARRAY de
// campos onde message_assembler_prepare espera uma FUNCAO (header_appender): o preflight de
// qualquer cliente de outra origem fazia o servidor saltar para dentro de dados.
static void append_cors_defaults(void* args, ResourceBuffer* http)
{
    (void)args;
    resource_buffer_append_format(http, "%s: %s\r\n", HTTP_HEADER_ALLOW_METHODS.Name.Content, HTTP_HEADER_ALLOW_METHODS.Param.Value.Content);
    resource_buffer_append_format(http, "%s: %s\r\n", HTTP_HEADER_ALLOW_HEADERS.Name.Content, HTTP_HEADER_ALLOW_HEADERS.Param.Value.Content);
}

void appserver_http_default_options(AppServerInfo* server, Message* request)
{
    ResourceBuffer http;
    resource_buffer_init(&http);

    message_assembler_prepare(HTTP_STATUS_OK, server->AgentName.Content, request->Client->LocalHost.Content, append_cors_defaults, 0, 0, &http, (int)(intptr_t)request->Client->Handle);
    appclient_send(request->Client, http.Data, http.Length, false);
}

bool appserver_web_process(AppServerInfo* server, Message* request)
{
    // busca no bind, para ver se pelo menos um em parte da rota, somente para direcionar pasta com conteudo
    ResourceBuffer buffer;
    memset(&buffer, 0, sizeof(ResourceBuffer));
    bool found = binder_get_web_resource(&request->Route, &server->AbsLocal, &buffer);
    if (found)
    {
        appserver_http_response_send(server, request, HTTP_STATUS_OK, &buffer, 0,0);
    }
    return found;
}

bool websocket_handshake_received(AppServerInfo* server, Message* request)
{
    if (request->SecWebsocketKey && request->SecWebsocketKey->Length > 0)
    {
        bool found = binder_route_exist(server->BindList, server->Prefix, &request->Route, &(int){ -1 }) != 0;
        if (found)
        {
            request->Client->IsWebSocketMode = true;
            appserver_http_response_send(server, request, HTTP_STATUS_SWITCHING_PROTOCOLS, 0,  websocket_handshake_prepare, request->SecWebsocketKey);
        }
        else
        {
            appserver_http_response_send(server, request, HTTP_STATUS_NOT_FOUND, 0, 0,0);
        }
        return true;
    }
    return false;
}



void appserver_received(Message* request)
{
    if (request->Protocol == AOTP)
	{
		appserver_received_aotp(request);
		return;
	}

    AppServerInfo* server = request->Client->Server;

    ReportRequest(request);

    if(websocket_handshake_received(server, request)) return;

    if (request->Cmd == CMD_OPTIONS)
    {
        appserver_http_default_options(server, request);
        return;
    }
    if (request->Cmd == CMD_GET && appserver_web_process(server, request))
    {
        return;
    }


    int rest = -1;
    FunctionBind* bind = binder_route_exist(server->BindList, server->Prefix, &request->Route, &rest);
    if (bind)
    {
        if (request->ContentLength > 0 && request->Content.Length > 0)
        {
            int first = 0;
            while (first < request->Content.Length &&
                  (request->Content.Content[first] == ' ' || request->Content.Content[first] == '\t' ||
                   request->Content.Content[first] == '\r' || request->Content.Content[first] == '\n'))
            {
                first++;
            }

            bool looks_json = first < request->Content.Length &&
                (request->Content.Content[first] == '{' || request->Content.Content[first] == '[');

            if (request->ContentType == APPLICATION_JSON || looks_json)
            {
                request->ContentType = APPLICATION_JSON;
                request->Object = yason_parse(request->Content.Content, request->Content.Length, TREE_TYPE_JSON);
            }
        }

        MessageMatchReceiverCalback func = bind->CallbackFunc;
        Element* result = func(request);

        // O handler pode ter transmitido a resposta ele mesmo (ex.: SSE).
        if (request->StreamHandled)
        {
            return;
        }

        if (result)
        {
            ResourceBuffer* buffer = memop_alloc_raw(sizeof(ResourceBuffer));
            StringX* json = yason_render(result, 1);
            size_t json_len = 0;   // string_utf8_to_bytes grava size_t; Length e int
            buffer->Data = string_utf8_to_bytes(json->Content, &json_len);
            buffer->Length = (int)json_len;
            buffer->Type = APPLICATION_JSON;
            appserver_http_response_send(server, request, HTTP_STATUS_OK, buffer, 0, 0);
            return;
        }
        else if (request->Response)
        {
            ResourceBuffer* buffer = memop_alloc_raw(sizeof(ResourceBuffer));

            if (request->Response->ContentType != CONTENT_TYPE_NONE)
            {
                ResourceBuffer rb;
                rb.Data   = request->Response->Content.Data;
                rb.Length = request->Response->Content.Length;
                rb.Type   = request->Response->ContentType;
                // O STATUS e' o que o handler gravou na resposta. Estava chumbado em 200:
                // todo 400/404/500 dos controllers chegava ao cliente como sucesso, com o
                // erro escondido no corpo -- o front chegou a contornar isso adivinhando
                // pelo texto, e um player HLS nao tem como adivinhar.
                HttpStatusCode status = request->Response->Status > 0
                                      ? (HttpStatusCode)request->Response->Status : HTTP_STATUS_OK;
                appserver_http_response_send(server, request, status, &rb, 0, 0);
            }
            else
            {
                send_response_server_error(request, "The content type was not defined.");
            }
        }
        else
        {
            send_response_server_error(request, "Instantiating a response is required if no return object is defined.");
        }
    }
    else
    {
        send_response_server_error(request, "Controller route not found.");
    }
}






void accept_client_proc(void* ptr)
{
    AppServerInfo* server = (AppServerInfo*)ptr;

    while (server->IsRunning)
    {
        struct sockaddr_in client;
        socklen_t client_size = sizeof(client);

        SOCKET client_socket = accept(NET_HANDLE_TO_SOCKET(server->Handle), (struct sockaddr*)&client, &client_size);

        if (client_socket == INVALID_SOCKET)
        {
            printf("Erro ao aceitar conex�o. C�digo: %d\n", WSAGetLastError());
            //closesocket(server->Handle);
            WSACleanup();
            return;   // funcao void: o "return 1" nao tinha para onde ir
        }

        AppClientInfo* cli = appclient_create(NET_SOCKET_TO_HANDLE(client_socket), server, appserver_received);
        appclient_list_add(server->Clients, cli);
    }
}




AppServerInfo* appserver_create(const char* agent_name, const int port, const char* prefix, const char* web_content_path, FunctionBindList* bind_list, boolean enable_health_monitor)
{
    appserver_init();

    AppServerInfo* info = serverinfo_create();
    info->BindList  = bind_list;

    // Rota de saude EMBUTIDA: /<prefixo>/health, so se o chamador pedir. Entra na lista
    // dele para que a aplicacao a tenha sem registrar nada. Desligada, nem o monitor
    // inicializa -- e isso importa, porque o init do PDH carrega pdh.dll, d3d11.dll e
    // dxgi.dll no processo. Ver health_controller.c, inclusive a ressalva de que a rota
    // ainda nao passa por autenticacao.
    //
    // ANTES do laco logo abaixo, de proposito: e ele quem da um Thung.Client a cada bind.
    // Registrada depois, a rota entrava na lista com esse campo nulo, e o servidor travava
    // ao atender por ela -- foi o que aconteceu, e custou caro achar.
    if (enable_health_monitor)
    {
        health_monitor_init();
        if (info->BindList) app_add_receiver(info->BindList, "health", appserver_health_route, true);
    }
    info->IsRunning = true;
	info->Events    = event_list_create();

    // TO-DO: somente para teste. depois que gerenciador de sesseao pronto, nao precisa este loop de atribui��o de client
    // Passar referencia do servidor
    if (info->BindList)
    {
        int ix = 0;
		while (ix < info->BindList->Count)
		{
			FunctionBind* bind = info->BindList->Items[ix];

            // TO-DO: somente para teste, para nao gerar erro, depois tem que remover.
            bind->Thung.Client = (AppClientInfo*)memop_calloc_raw(1, sizeof(AppClientInfo));
            bind->Thung.Client->Server = info;

           // bind->Function
			
           // bind. assembler_method_bind_(void* sender, void* client)

			ix++;
		}
    }

    string_appends(&info->AgentName, agent_name, (int)strlen(agent_name), 0, (int)strlen(agent_name));

    struct sockaddr_in server, client;

    WsaInit();

    SOCKET server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == INVALID_SOCKET)
    {
        printf("Erro ao criar o socket. C�digo: %d\n", WSAGetLastError());
        WSACleanup();
        return NULL;
    }

    server.sin_family      = AF_INET;
    server.sin_addr.s_addr = INADDR_ANY;
    server.sin_port        = htons(port);

    // Associa o socket � porta
    if (bind(server_socket, (struct sockaddr*)&server, sizeof(server)) == SOCKET_ERROR) 
    {
        printf("Erro no bind da porta para servidor. Porta: %d. C�digo: %d\n", port, WSAGetLastError());
        closesocket(server_socket);
        WSACleanup();
        return NULL;
    }

    // Coloca o socket em modo de escuta
    if (listen(server_socket, SOMAXCONN) == SOCKET_ERROR)
    {
        printf("Erro ao colocar em escuta. C�digo: %d\n", WSAGetLastError());
        closesocket(server_socket);
        WSACleanup();
        return NULL;
    }

    // Handle ANTES da thread: ela usa server->Handle no accept(). Na ordem antiga a thread
    // podia chegar ao accept com o Handle ainda vazio -- no Windows o accept falhava e a
    // thread de accept morria em silencio; no Linux o descritor 0 e o stdin.
    info->Handle             = NET_SOCKET_TO_HANDLE(server_socket);
    info->AcceptThread       = (void*)_beginthread(accept_client_proc, 0, (void*)info);

    info->Prefix = string_split_cstr(prefix, (int)strlen(prefix), (char)0x2F);

    if (web_content_path && web_content_path[0] != '\0')
    {
        string_appends(&info->AbsLocal, web_content_path, (int)strlen(web_content_path), 0, (int)strlen(web_content_path));
    }

    serverinfo_list_add(&Servers, info);       

    printf("Server instantiated '%s' | Port: %d | Prefix: '%s'\n", agent_name, port, prefix);
    return info;
}



void app_add_receiver(FunctionBindList* list, const char* route, MessageMatchReceiverCalback function, bool with_callback)
{
    binder_list_add_receiver(list, route, function, with_callback);
}
void app_add_receiver_extension(FunctionBindList* list, const char* extension, MessageMatchReceiverCalback function, bool with_callback)
{
    binder_list_add_receiver_extension(list, extension, function, with_callback);
}
void app_add_web_resource(FunctionBindList* list, const char* route, MessageMatchReceiverCalback function)
{
    binder_list_add_web_resource(list, route, function);
}
MessageEmitterCalback app_add_emitter(FunctionBindList* list, const char* route)
{
    return binder_list_add_emitter(list, route);
}