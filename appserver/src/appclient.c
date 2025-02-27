#include "appclient.h"
#include <winsock2.h>
#include <process.h>
#include <string.h>
#include <stdlib.h>



#define BUFFER_SIZE 4096  


void appclient_dispatch(void* ptr)
{
    ClientData* data = (ClientData*)ptr;

    if (data->Client->Server->DataReceivedClient)
    {
        data->Client->Server->DataReceivedClient(data);

        free(data->Data);
        //free(data);
    }
}

void appclient_received(void* ptr)
{
    AppClientInfo* cli = (AppClientInfo*)ptr;

    while (cli->IsConnected)
    {
        byte buffer[BUFFER_SIZE + 1];

        // Recebe dados do cliente
        int bytes_received = recv(cli->Handle, buffer, BUFFER_SIZE, 0);
        if (bytes_received > 0)
        {
            buffer[bytes_received] = '\0';

            ClientData cd;
            memset(&cd, 0, sizeof(ClientData));
            cd.Client = cli;
            cd.Data = (byte*)malloc(sizeof(byte) * (bytes_received + 1));
            cd.Length = bytes_received;

            memcpy(cd.Data, buffer, bytes_received + 1);

            cd.DispatcherThread = _beginthread(appclient_dispatch, 0, (void*)&cd);
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
    client->Handle = ptr;
    client->Server = server;
    client->IsConnected = true;

    client->ReceivedThread = _beginthread(appclient_received, 0, (void*)client);

    return client;
}
