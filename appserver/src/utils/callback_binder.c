#include "../event_server.h"
#include "callback_binder.h"
#include <stdio.h>
#include <stdint.h>
#include <windows.h> 




void __stdcall trampoline0(void* data, void* callback) 
{
    if (g_bindings[0] && g_bindings[0]->Sender) {
        g_bindings[0]->Sender(data, callback, g_bindings[0]->Client);
    }
}



void  bind_list_add_receiver(FunctionBindList* list, const char* route, MessageMatchReceiverCalback function, bool with_callback)
{
    bind_list_add(list, route, function, false, with_callback, false);
}

void bind_list_add_web_resource(FunctionBindList* list, const char* route, MessageMatchReceiverCalback function)
{
    bind_list_add(list, route, function, true, false, false);
}

MessageMatchEmitterCalback bind_list_add_emitter(FunctionBindList* list, const char* route)
{
    MessageSenderCalback mk = event_sender_server;

    FunctionBind* func;




    bind_list_add(list, route, mk, false, true, true);
    return mk;
}