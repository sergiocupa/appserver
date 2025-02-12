#include "appclient.h"
#include "../include/appserver.h"
#include <winsock2.h>
#include <process.h>
#include <string.h>
#include <stdlib.h>



#define BUFFER_SIZE 4096  


void appclient_list_add(AppClientList* list, AppClientInfo* cli)
{
    if (list)
    {
        if (list->Count >= list->MaxCount)
        {
            list->MaxCount = ((list->Count + sizeof(AppClientInfo)) + list->MaxCount) * 2;
            list->Items = (void**)realloc((void**)list->Items, list->MaxCount * sizeof(void*));
        }

        list->Items[list->Count] = cli;
        list->Count++;
    }
}

AppClientList* appclient_list_release(AppClientList* list)
{
    if (list)
    {
        int ix = 0;
        while (ix < list->Count)
        {
            free(list->Items[ix]);
            ix++;
        }
        free(list->Items);
        free(list);
    }
    return 0;
}

AppClientList* appclient_list_create()
{
    AppClientList* ar = (AppClientList*)malloc(sizeof(AppClientList));
    ar->Count    = 0;
    ar->MaxCount = 100;
    ar->Items    = (void**)malloc(ar->MaxCount * sizeof(void*));
    return ar;
}


void appclient_dispatch(void* ptr)
{
    ClientData* data = (ClientData*)ptr;

    if (data->Client->Server->DataReceivedClient)
    {
        data->Client->Server->DataReceivedClient(data);
        free(data->Data);
        free(data);
    }
}

void appclient_received(void* ptr)
{
    AppClientInfo* cli = (AppClientInfo*)ptr;

    byte buffer[BUFFER_SIZE+1];

    // Recebe dados do cliente
    int bytes_received = recv(cli->Handle, buffer, BUFFER_SIZE, 0);
    if (bytes_received > 0)
    {
        buffer[bytes_received] = '\0';

        ClientData cd;
        memset(&cd, 0, sizeof(ClientData));
        cd.Client = cli;
        cd.Data   = (byte*)malloc(sizeof(byte) * (bytes_received + 1));
        cd.Length = bytes_received;

        memcpy(cd.Data, buffer, bytes_received + 1);

        cd.DispatcherThread = _beginthread(appclient_dispatch, 0, (void*)&cd);
    }
    else
    {
        // Close
        ...
    }


   
}


AppClientInfo* appclient_create(void* ptr, AppServerInfo* server)
{
    AppClientInfo* client;
    client = (AppClientInfo*)malloc(sizeof(AppClientInfo));
    memset(client, 0, sizeof(AppClientInfo));
    client->Handle = ptr;
    client->Server = server;

    client->ReceivedThread = _beginthread(appclient_received, 0, (void*)&client);

    return client;
}
