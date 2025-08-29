//  MIT License – Modified for Mandatory Attribution
//  
//  Copyright(c) 2025 Sergio Paludo
//  
//  Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files, 
//  to use, copy, modify, merge, publish, distribute, and sublicense the software, including for commercial purposes, provided that:
//  
//  01. The original author’s credit is retained in all copies of the source code;
//  02. The original author’s credit is included in any code generated, derived, or distributed from this software, including templates, libraries, or code - generating scripts.
//  
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED.


#include "event_server.h"
#include "utils/message_assembler.h"
#include "appclient.h"
#include "utils/activity_binder.h"
#include "yason.h"



void event_sender(ResourceBuffer* object, MessageMatchReceiverCalback callback)
{
    // Enviar ACTION 

    // Criar instance para lista de eventos
    // Ao chegar resposta, chamar CALBACK, registrado no EVENT
    


}

void event_sender_server(ResourceBuffer* object, MessageMatchReceiverCalback callback, AppClientInfo* client)
{
    AppServerInfo* server = client->Server;

    int ix = 0;
	while (ix < server->Clients->Count)
	{
		AppClientInfo* c = server->Clients->Items[ix];

        MessageEvent* item = calloc(1, sizeof(MessageEvent));
        item->Client = c;
		item->Callback = callback;
       // item->UID = "";


        event_list_add(server->Events, item);


        appclient_send(c, object->Data, object->Length, c->IsWebSocketMode);
		ix++;
	}



}



MessageEvent* event_find(Message* request, bool test_origin)
{
    AppServerInfo* server = request->Client->Server;
    String* sevent = test_origin ? request->OriginEventUID : request->EventUID;

    if (sevent)
    {
        int ix = server->Events->Count;
        while (ix > 0)
        {
            ix--;
            MessageEvent* event = server->Events->Items[ix];
            if (string_equals_s(&event->UID, sevent))
            {
                return event;
            }
        }
    }
    return 0;
}



void aotp_prepare_send(MessageCommand command, Message* request, ResourceBuffer** objects, int object_length)
{
    ResourceBuffer aotp;
    resource_buffer_init(&aotp);
    message_assembler_prepare_aotp(command, request->Client->LocalHost.Data, request->EventUID, request->OriginEventUID, objects, object_length, &aotp);
    appclient_send(request->Client, aotp.Data, aotp.Length, true);
}


void appserver_aotp_received_event(Message* request, bool is_callback)
{
    AppServerInfo* server = request->Client->Server;

    FunctionBind* bind = binder_route_exist(server->BindList, server->Prefix, &request->Route);
    if (bind)
    {
        MessageMatchReceiverCalback func = bind->CallbackFunc;
        void* result = func(request);

        if (bind->WithCallback)
        {
            String* json = yason_render((Element*)result, 1);
            ResourceBuffer* buffer = malloc(sizeof(ResourceBuffer));
            buffer->Type = APPLICATION_JSON;
            buffer->Data = string_utf8_to_bytes(json->Data, &buffer->Length);

            aotp_prepare_send(CMD_CALLBACK, request, &buffer, 1);
        }
    }
}


void appserver_received_aotp(Message* request)
{
    AppServerInfo* server = request->Client->Server;

    if (request->Cmd != CMD_ACKNOWLEDGMENT)
    {
        if (!request->OriginEventUID)
        {
            request->OriginEventUID = string_new();
        }

        string_append_s(request->OriginEventUID, request->EventUID);
        aotp_prepare_send(CMD_ACKNOWLEDGMENT, request, 0, 0);
        return;
    }

    if (!request->SessionUID)
    {
        // TO-DO:
        // Testar se tem SESSION, se nao tem, requisitar sessao para o client
        //   Ao receber SESSION, assionar ThunkArgs e atribuir o client para ele
        //      Para encontrar ThunkArgs, buscar pela rota o BINDER para poder associar o ThunkArgs
        return;
    }

    if (request->Cmd == CMD_ACKNOWLEDGMENT)
    {
        MessageEvent* eve = event_find(request, true);
        if (eve)
        {
            if (eve->WaitForCallback && eve->LastCommand == CMD_CALLBACK)
            {
                event_list_remove(server->Events, eve);
            }
            eve->LastCommand = request->OriginCmd;
        }
    }
    else if (request->Cmd == CMD_ACTION)
    {
        appserver_aotp_received_event(request, false);
    }
    else if (request->Cmd == CMD_CALLBACK)
    {
        appserver_aotp_received_event(request, true);
    }
}