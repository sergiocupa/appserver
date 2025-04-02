#include "../include/appserver.h"
#include "appclient.h"
#include "event_server.h"
#include "utils/message_assembler.h"
#include "utils/message_parser.h"
#include "utils/activity_binder.h"
#include "yason.h"
#include "utils/websocket_util.h"

#ifndef _WINSOCKAPI_
#define _WINSOCKAPI_
#include <ws2tcpip.h>
#endif

#include <stdio.h>
#include <process.h>

#pragma comment(lib, "ws2_32.lib")

int ServerInitialized = false;
MessageField HTTP_HEADER_ALLOW_HEADERS;
MessageField HTTP_HEADER_ALLOW_METHODS;
AppServerList Servers;


void _ReportRequest(Message* request)
{
    char* a  = message_command_titule(request->Cmd);
    char* tp = message_assembler_append_content_type(request->ContentType);

    String b;
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

    printf("REQUEST  | Client: %d | Method: %s | Route: '%s' | Type: %s | Content Length: %d\n", (int)request->Client->Handle, a, b.Data, tp, request->ContentLength);
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
        printf("Erro na inicialização do Winsock. Código: %d\n", WSAGetLastError());
        return 1;
    }
}




void appserver_create_default_headers()
{
    memset(&HTTP_HEADER_ALLOW_HEADERS, 0, sizeof(MessageField));
    memset(&HTTP_HEADER_ALLOW_METHODS, 0, sizeof(MessageField));

    string_init(&HTTP_HEADER_ALLOW_HEADERS.Name);
    string_init(&HTTP_HEADER_ALLOW_HEADERS.Param.Value);
    string_append(&HTTP_HEADER_ALLOW_HEADERS.Name, "Access-Control-Allow-Headers");
    string_append(&HTTP_HEADER_ALLOW_HEADERS.Param.Value, "Content-Type, Authorization");

    string_init(&HTTP_HEADER_ALLOW_METHODS.Name);
    string_init(&HTTP_HEADER_ALLOW_METHODS.Param.Value);
    string_append(&HTTP_HEADER_ALLOW_METHODS.Name, "Access-Control-Allow-Methods");
    string_append(&HTTP_HEADER_ALLOW_METHODS.Param.Value, "GET, POST, OPTIONS");
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


    //...


    ServerInitialized = false;
}


void appserver_http_response_send(AppServerInfo* server, Message* request, HttpStatusCode http_status, ResourceBuffer* object, HeaderAppender header_appender, void* appender_args)
{
    ResourceBuffer http;
    resource_buffer_init(&http);

    message_assembler_prepare(http_status, server->AgentName.Data, request->Client->LocalHost.Data, header_appender, appender_args, object, &http, (int)request->Client->Handle);
    appclient_send(request->Client, http.Data, http.Length, false);
}

void appserver_http_default_options(AppServerInfo* server, Message* request)
{
    ResourceBuffer http;
    resource_buffer_init(&http);

    MessageField defauts[2] = {HTTP_HEADER_ALLOW_METHODS, HTTP_HEADER_ALLOW_HEADERS};
    message_assembler_prepare(HTTP_STATUS_OK, server->AgentName.Data, request->Client->LocalHost.Data, &defauts, 2, 0, &http, (int)request->Client->Handle);
    appclient_send(request->Client, http.Data, http.Length, false);
}


bool appserver_web_process(AppServerInfo* server, Message* request)
{
    // busca no bind, para ver se pelo menos um em parte da rota, somente para direcionar pasta com conteudo
    ResourceBuffer buffer;
    memset(&buffer, 0, sizeof(ResourceBuffer));
    bool found = binder_get_web_resource(server->BindList, server->Prefix, &request->Route, &server->AbsLocal, &buffer);
    if (found)
    {
        appserver_http_response_send(server, request, HTTP_STATUS_OK, &buffer, 0,0);
    }
    else
    {
        appserver_http_response_send(server, request, HTTP_STATUS_NOT_FOUND, 0, 0,0);
    }
    return false;
}


bool WebSocketConnectionReceived(AppServerInfo* server, Message* request)
{
    if (request->SecWebsocketKey && request->SecWebsocketKey->Length > 0)
    {
        bool found = binder_get_web_resource(server->BindList, server->Prefix, &request->Route, &server->AbsLocal, 0);
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

    if(WebSocketConnectionReceived(server, request)) return;

    if (request->Cmd == CMD_OPTIONS)
    {
        appserver_http_default_options(server, request);
        return;
    }

    FunctionBind* bind = binder_route_exist(server->BindList, server->Prefix, &request->Route);
    if (bind)
    {
        if (request->ContentLength > 0)
        {
            if (request->ContentType == APPLICATION_JSON)
            {
                request->Object = yason_parse(request->Content.Data, request->Content.Length, TREE_TYPE_JSON);
            }
        }

        MessageMatchReceiverCalback func = (MessageMatchReceiverCalback)bind->Function;
        void* result = func(request);

        String* json = yason_render((Element*)result, 1);
        ResourceBuffer* buffer = malloc(sizeof(ResourceBuffer));
        buffer->Type = APPLICATION_JSON;
        buffer->Data = string_utf8_to_bytes(json->Data, &buffer->Length);

        appserver_http_response_send(server, request, HTTP_STATUS_OK, buffer, 0,0);
    }
    else
    {
        appserver_web_process(server, request);
    }
}



void accept_client_proc(void* ptr)
{
    AppServerInfo* server = (AppServerInfo*)ptr;

    while (server->IsRunning)
    {
        struct sockaddr_in client;
        int client_size = sizeof(client);

        SOCKET client_socket = accept(server->Handle, (struct sockaddr*)&client, &client_size);

        if (client_socket == INVALID_SOCKET)
        {
            printf("Erro ao aceitar conexão. Código: %d\n", WSAGetLastError());
            //closesocket(server->Handle);
            WSACleanup();
            return 1;
        }

        AppClientInfo* cli = appclient_create(client_socket, server, appserver_received);
        appclient_list_add(server->Clients, cli);
    }
}




AppServerInfo* appserver_create(const char* agent_name, const int port, const char* prefix, FunctionBindList* bind_list)
{
    appserver_init();

    AppServerInfo* info = serverinfo_create();
    info->BindList  = bind_list;
    info->IsRunning = true;
	info->Events    = event_list_create();

    string_append(&info->AgentName, agent_name);

    struct sockaddr_in server, client;

    WsaInit();

    SOCKET server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == INVALID_SOCKET)
    {
        printf("Erro ao criar o socket. Código: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    server.sin_family      = AF_INET;
    server.sin_addr.s_addr = INADDR_ANY;
    server.sin_port        = htons(port);

    // Associa o socket à porta
    if (bind(server_socket, (struct sockaddr*)&server, sizeof(server)) == SOCKET_ERROR) 
    {
        printf("Erro no bind. Código: %d\n", WSAGetLastError());
        closesocket(server_socket);
        WSACleanup();
        return 1;
    }

    // Coloca o socket em modo de escuta
    if (listen(server_socket, SOMAXCONN) == SOCKET_ERROR)
    {
        printf("Erro ao colocar em escuta. Código: %d\n", WSAGetLastError());
        closesocket(server_socket);
        WSACleanup();
        return 1;
    }

    info->AcceptThread       = _beginthread(accept_client_proc, 0, (void*)info);
    info->Handle             = server_socket;

    info->Prefix = string_split(prefix, strlen(prefix), "/", 1, true);

    binder_get_web_content_path(info->BindList, info->Prefix, &info->AbsLocal);

    serverinfo_list_add(&Servers, info);       

    printf("Server instantiated '%s' | Port: %d | Prefix: '%s'\n", agent_name, port, prefix);
    return info;
}