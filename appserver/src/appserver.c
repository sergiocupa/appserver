#include "../include/appserver.h"
#include "appclient.h"
#include "message_parser.h"
#include "yason.h"
#include <winsock2.h>
#include <stdio.h>
#include <process.h>

#pragma comment(lib, "ws2_32.lib")



//void appserver_


void appserver_received(Message* request)
{
   /* if (request->ContentLength > 0)
    {
        if (request->ContentType == APPLICATION_JSON)
        {
            request->Object = yason_parse(request->Content.Data, request->Content.Length, TREE_TYPE_JSON);
        }
    }*/





    // rotear metodo

    // chamar metodo


    // Recep��o de todos os clients
    // Cada entrada tem uma thread;
}


bool WsaInitialized = false;
int WsaInit()
{
    WSADATA wsa;
    if (!WsaInitialized && WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        printf("Erro na inicializa��o do Winsock. C�digo: %d\n", WSAGetLastError());
        return 1;
    }
    WsaInitialized = true;
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
            printf("Erro ao aceitar conex�o. C�digo: %d\n", WSAGetLastError());
            //closesocket(server->Handle);
            WSACleanup();
            return 1;
        }

        AppClientInfo* cli = appclient_create(client_socket, server);
        cli->Parser = message_parser_create(appserver_received);

        appclient_list_add(server->Clients, cli);
    }
}


// Bind de funcoes
//     funcao com parametro estrutura do Request
//     funcao com retorno o objeto Response;

void appserver_release(AppServerInfo* server)
{
    server->IsRunning = false;

    // ...
}






AppServerInfo* appserver_create(const int port, const char* prefix, FunctionBindList* bind_list)
{
    AppServerInfo* info;
    info = (AppServerInfo*)malloc(sizeof(AppServerInfo));
    memset(info,0, sizeof(AppServerInfo));
    info->Clients = appclient_list_create();
    info->BindList = bind_list;
    info->IsRunning = true;

    struct sockaddr_in server, client;

    WsaInit();

    SOCKET server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == INVALID_SOCKET)
    {
        printf("Erro ao criar o socket. C�digo: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    server.sin_family      = AF_INET;
    server.sin_addr.s_addr = INADDR_ANY;
    server.sin_port        = htons(port);

    // Associa o socket � porta
    if (bind(server_socket, (struct sockaddr*)&server, sizeof(server)) == SOCKET_ERROR) 
    {
        printf("Erro no bind. C�digo: %d\n", WSAGetLastError());
        closesocket(server_socket);
        WSACleanup();
        return 1;
    }

    // Coloca o socket em modo de escuta
    if (listen(server_socket, SOMAXCONN) == SOCKET_ERROR)
    {
        printf("Erro ao colocar em escuta. C�digo: %d\n", WSAGetLastError());
        closesocket(server_socket);
        WSACleanup();
        return 1;
    }

    info->AcceptThread       = _beginthread(accept_client_proc, 0, (void*)info);
    info->Handle             = server_socket;
    info->Prefix             = prefix;
    return info;
}