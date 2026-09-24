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


#include "appclient.h"
#include "utils/message_parser.h"
#include "utils/websocket_util.h"

#include "utils/net_compat.h"   // Winsock no Windows, BSD sockets no Linux

#include <string.h>
#include <stdlib.h>


#define BUFFER_SIZE 4096  


// Envia TODOS os bytes. send() num socket pode enviar MENOS que 'length' e retornar o
// parcial; sem laco, respostas grandes (ex.: segmentos HLS de varios MB) saiam TRUNCADAS
// -> o player decodava so o inicio e travava. Repete ate escoar tudo; retenta em WSAEINTR.
static bool appclient_send_all(AppClientInfo* cli, const byte* buffer, int length)
{
    int off = 0;
    while (off < length)
    {
        int n = send(NET_HANDLE_TO_SOCKET(cli->Handle), (const char*)buffer + off, length - off, 0);
        if (n == SOCKET_ERROR)
        {
            if (WSAGetLastError() == WSAEINTR) continue;   // interrompido: repete
            return false;                                  // erro real de socket
        }
        if (n <= 0) return false;                          // conexao fechada pelo peer
        off += n;
    }
    return true;
}

void appclient_send(AppClientInfo* cli, byte* content, int length, bool is_websocket)
{
    if (length <= 0) return;

	if (is_websocket)
	{
        size_t wlength = 0;
        byte* webs = websocket_encode_text(content, &wlength);
        if (webs)
        {
            if (!appclient_send_all(cli, webs, (int)wlength))
            {
                perror("Erro ao enviar mensagem");
            }
            memop_free_raw(webs);   // websocket_encode_frame aloca via memop; sem isso, vaza
        }
	}
    else
    {
        if (!appclient_send_all(cli, content, length))
        {
            perror("Erro ao enviar mensagem");
        }
    }
}


void appclient_received(void* ptr);

// Wrapper p/ thread_create (registra a lane TLS do memory_pool desta thread — a de leitura
// tambem aloca via memop em message_buildup; _beginthread cru corromperia a heap).
static xthread_result_t WINAPI appclient_received_thread(void* p) { appclient_received(p); return 0; }

void appclient_received(void* ptr)
{
    AppClientInfo* cli = (AppClientInfo*)ptr;

    while (cli->IsConnected)
    {
        byte* buffer = (byte*)memop_alloc_raw(sizeof(byte) * (BUFFER_SIZE + 1)); 

        int bytes_received = recv(NET_HANDLE_TO_SOCKET(cli->Handle), buffer, BUFFER_SIZE, 0);
        if (bytes_received > 0)
        {
            buffer[bytes_received] = '\0';

            message_buildup(cli->Parser, cli, buffer, bytes_received);

            memop_free_raw(buffer);
        }
        else
        {
            cli->IsConnected = false;

            closesocket(NET_HANDLE_TO_SOCKET(cli->Handle));

            // TO-DO: precisa desconecter o TCP CLient??
        }
    }
}


AppClientInfo* appclient_create(void* ptr, AppServerInfo* server, RequestCallback receiver)
{
    AppClientInfo* client = (AppClientInfo*)memop_calloc_raw(1, sizeof(AppClientInfo));


    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    if (getsockname(NET_HANDLE_TO_SOCKET(ptr), (struct sockaddr*)&addr, &addr_len) == -1)
    {
        perror("Erro no getsockname");
        //close(ptr);
        exit(EXIT_FAILURE);
    }
    char ip_str[INET_ADDRSTRLEN];
    InetNtop(AF_INET, &addr.sin_addr, ip_str, sizeof(ip_str));
    string_init(&client->LocalHost);
    string_append_format(&client->LocalHost, "%s:%d", ip_str, ntohs(addr.sin_port));


    if (getpeername(NET_HANDLE_TO_SOCKET(ptr), (struct sockaddr*)&addr, &addr_len) == -1)
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
    int _rthst = 0;
    client->ReceivedThread = thread_create(appclient_received_thread, (void*)client, &_rthst);

    printf("Accepted client '%s' | Handle: %d\n", client->LocalHost.Content, (int)(intptr_t)ptr);
    return client;
}
