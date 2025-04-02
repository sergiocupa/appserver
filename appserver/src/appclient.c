#include "appclient.h"
#include "utils/message_parser.h"
#include "utils/websocket_util.h"

#ifndef _WINSOCKAPI_
#define _WINSOCKAPI_
#include <ws2tcpip.h>
#endif

#include <process.h>
#include <string.h>
#include <stdlib.h>


#define BUFFER_SIZE 4096  


void appclient_send(AppClientInfo* cli, byte* content, int length, bool is_websocket)
{
    if (length <= 0) return;

	if (is_websocket)
	{
        size_t wlength = 0;
        byte* webs = websocket_encode_text(content, &wlength);
        
        if (send(cli->Handle, webs, wlength, 0) < 0)
        {
            perror("Erro ao enviar mensagem");
        }
	}
    else
    {
        if (send(cli->Handle, content, length, 0) < 0)
        {
            perror("Erro ao enviar mensagem");
        }
    }
}


void appclient_received(void* ptr)
{
    AppClientInfo* cli = (AppClientInfo*)ptr;

    while (cli->IsConnected)
    {
        byte* buffer = (byte*)malloc(sizeof(byte) * (BUFFER_SIZE + 1)); 

        int bytes_received = recv(cli->Handle, buffer, BUFFER_SIZE, 0);
        if (bytes_received > 0)
        {
            buffer[bytes_received] = '\0';

            message_buildup(cli->Parser, cli, buffer, bytes_received);

            free(buffer);
        }
        else
        {
            cli->IsConnected = false;

            closesocket(cli->Handle);

            // TO-DO: precisa desconecter o TCP CLient??
        }
    }
}


AppClientInfo* appclient_create(void* ptr, AppServerInfo* server, MessageMatchReceiverCalback receiver)
{
    AppClientInfo* client = (AppClientInfo*)calloc(1, sizeof(AppClientInfo));


    struct sockaddr_in addr;
    int addr_len = sizeof(addr);
    if (getsockname(ptr, (struct sockaddr*)&addr, &addr_len) == -1)
    {
        perror("Erro no getsockname");
        //close(ptr);
        exit(EXIT_FAILURE);
    }
    char ip_str[INET_ADDRSTRLEN];
    InetNtop(AF_INET, &addr.sin_addr, ip_str, sizeof(ip_str));
    string_init(&client->LocalHost);
    string_append_format(&client->LocalHost, "%s:%d", ip_str, ntohs(addr.sin_port));


    if (getpeername(ptr, (struct sockaddr*)&addr, &addr_len) == -1)
    {
        perror("Erro no getsockname");
        //close(ptr);
        exit(EXIT_FAILURE);
    }
    InetNtop(AF_INET, &addr.sin_addr, ip_str, sizeof(ip_str));
    string_init(&client->RemoteHost);
    string_append_format(&client->RemoteHost, "%s:%d", ip_str, ntohs(addr.sin_port));


    client->Handle         = ptr;
    client->Server         = server;
    client->IsConnected    = true;
    client->Parser         = message_parser_create(receiver);
    client->ReceivedThread = _beginthread(appclient_received, 0, (void*)client);

    printf("Accepted client '%s' | Handle: %d\n", client->LocalHost.Data, (int)ptr);
    return client;
}
