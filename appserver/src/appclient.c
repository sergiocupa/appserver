#include "appclient.h"
#include "message_parser.h"
#include <winsock2.h>
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
    AppClientInfo* client = (AppClientInfo*)malloc(sizeof(AppClientInfo));
    memset(client, 0, sizeof(AppClientInfo));

    client->Handle      = ptr;
    client->Server      = server;
    client->IsConnected = true;

    client->ReceivedThread = _beginthread(appclient_received, 0, (void*)client);

    return client;
}
