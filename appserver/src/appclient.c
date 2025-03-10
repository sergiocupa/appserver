#include "appclient.h"
#include "message_parser.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <process.h>
#include <string.h>
#include <stdlib.h>


#define BUFFER_SIZE 4096  



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


AppClientInfo* appclient_create(void* ptr, AppServerInfo* server)
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


    client->Handle      = ptr;
    client->Server      = server;
    client->IsConnected = true;

    client->ReceivedThread = _beginthread(appclient_received, 0, (void*)client);

    return client;
}
