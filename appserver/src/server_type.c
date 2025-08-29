//  MIT License – Modified for Mandatory Attribution
//  
//  Copyright(c) 2025 Sergio Paludo
//
//  github.com/sergiocupa
//  
//  Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files, 
//  to use, copy, modify, merge, publish, distribute, and sublicense the software, including for commercial purposes, provided that:
//  
//     01. The original author’s credit is retained in all copies of the source code;
//     02. The original author’s credit is included in any code generated, derived, or distributed from this software, including templates, libraries, or code - generating scripts.
//  
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED.


#include "event_server.h"
#include "server_type.h"
#include <stdlib.h>
#include <string.h>


void message_field_param_release(MessageFieldParam* param);

void message_field_param_release(MessageFieldParam* param)
{
    if (param->Name.MaxLength > 0)
    {
        string_release_data(&param->Name);
        param->Name.MaxLength = 0;
        param->Name.Length = 0;
    }
    if (param->Value.MaxLength > 0)
    {
        string_release_data(&param->Value);
        param->Value.MaxLength = 0;
        param->Value.Length = 0;
    }

    if (param->Next)
    {
        message_field_param_release(param->Next);
    }
}


MessageField* message_field_create(bool init_content)
{
    MessageField* ins = (MessageField*)calloc(1,sizeof(MessageField));

    if (init_content)
    {
        string_init(&ins->Name);
    }
    else
    {
        ins->Name.MaxLength = -1;
    }
    return ins;
}


void message_field_list_init(MessageFieldList* list)
{
    list->Count = 0;
    list->MaxCount = 100;
    list->Items = (void**)malloc(list->MaxCount * sizeof(void*));
}

void message_field_list_add(MessageFieldList* list, MessageField* field)
{
    if (list)
    {
        if (list->Count >= list->MaxCount)
        {
            list->MaxCount = ((list->Count + sizeof(MessageField)) + list->MaxCount) * 2;
            list->Items = (void**)realloc((void**)list->Items, list->MaxCount * sizeof(MessageField*));
        }

        list->Items[list->Count] = field;
        list->Count++;
    }
}

MessageField* message_field_release(MessageField* ins)
{
    if (ins->Name.MaxLength > 0)
    {
        string_release_data(&ins->Name);
        ins->Name.MaxLength = 0;
        ins->Name.Length    = 0;
    }

    message_field_param_release(&ins->Param);
    free(ins);
    return 0;
}

void message_field_list_release(MessageFieldList* list, bool only_data)
{
    if (list)
    {
        int ix = 0;
        while (ix < list->Count)
        {
            message_field_release(list->Items[ix]);
            ix++;
        }
        free(list->Items);

        if (!only_data)
        {
            free(list);
        }
    }
    return 0;
}


Message* message_create()
{
    Message* ar = (Message*)malloc(sizeof(Message));
    memset(ar,0, sizeof(Message));
    string_array_init(&ar->Route);
    message_field_list_init(&ar->Fields);
    return ar;
}


void message_release(Message* m)
{
    message_field_list_release(&m->Fields, true);

    if (m->Route.MaxCount > 0)
    {
        string_array_release(&m->Route, true);
    }

    if (m->Version.MaxLength > 0)
    {
        string_release_data(&m->Version);
    }

    if (m->Host.MaxLength > 0)
    {
        string_release_data(&m->Host);
    }

    if (m->Content.MaxLength > 0)
    {
        string_release_data(&m->Content);
    }

    if (m->Param)
    {
        message_field_param_release(m->Param);
    }

    free(m);
}






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
    ar->Count = 0;
    ar->MaxCount = 100;
    ar->Items = (void**)malloc(ar->MaxCount * sizeof(void*));
    return ar;
}




//FunctionBind* bind_create(const char* route, bool is_web_application, bool with_callback, bool is_event_emitter)
FunctionBind* bind_create(const char* route)
{
    FunctionBind* ar = (FunctionBind*)calloc(1,sizeof(FunctionBind));
    string_array_init(&ar->Route);
    string_split_param(route, strlen(route), "/", 1, true, &ar->Route);
    return ar;
}

FunctionBind* bind_release(FunctionBind* _this)
{
    if (_this)
    {
        string_release_data(&_this->Route);
        free(_this);
    }
    return 0;
}


//void bind_list_add(FunctionBindList* list, const char* route, void* function, bool is_web_application, bool with_callback, bool is_event_emitter)
void bind_list_add(FunctionBindList* list, FunctionBind* bind)
{
    if (list)
    {
        if (list->Count >= list->MaxCount)
        {
            list->MaxCount = ((list->Count + sizeof(FunctionBind)) + list->MaxCount) * 2;
            list->Items = (void**)realloc((void**)list->Items, list->MaxCount * sizeof(void*));
        }

        list->Items[list->Count] = bind;
        list->Count++;
    }
}

FunctionBindList* bind_list_release(FunctionBindList* list)
{
    if (list)
    {
        int ix = 0;
        while (ix < list->Count)
        {
            bind_release(list->Items[ix]);
            ix++;
        }
        free(list->Items);
        free(list);
    }
    return 0;
}

FunctionBindList* bind_list_create()
{
    FunctionBindList* ar = (FunctionBindList*)malloc(sizeof(FunctionBindList));
    ar->Count = 0;
    ar->MaxCount = 100;
    ar->Items = (void**)malloc(ar->MaxCount * sizeof(void*));
    return ar;
}







void serverinfo_list_init(AppServerList* list)
{
    list->Count = 0;
    list->MaxCount = 100;
    list->Items = (void**)malloc(list->MaxCount * sizeof(void*));
}


void serverinfo_list_release(AppServerList* list)
{
    if (list)
    {
        free(list->Items);
        free(list);
    }
    return 0;
}


void serverinfo_list_add(AppServerList* list, AppServerInfo* server)
{
    if (list)
    {
        if (list->Count >= list->MaxCount)
        {
            list->MaxCount = ((list->Count + sizeof(AppServerInfo)) + list->MaxCount) * 2;
            list->Items = (void**)realloc((void**)list->Items, list->MaxCount * sizeof(void*));
        }

        list->Items[list->Count] = server;
        list->Count++;
    }
}

void serverinfo_release(AppServerInfo* server)
{
    string_release_data(&server->AbsLocal);

    appclient_list_release(server->Clients);
    free(server);
}

AppServerInfo* serverinfo_create()
{
    AppServerInfo* server;
    server = (AppServerInfo*)calloc(1, sizeof(AppServerInfo));
    server->Clients = appclient_list_create();
    server->DefaultWebApiObjectType = APPLICATION_JSON;

    string_init(&server->AbsLocal);

    return server;
}





// TO-TO: revisar calculos de MaxLength. Definir curva para crescimento do tamanho de uso da memoria

void resource_buffer_init(ResourceBuffer* source)
{
    memset(source, 0, sizeof(ResourceBuffer));
    source->MaxLength = 1024;
    source->Data      = calloc(1, source->MaxLength);
}
void resource_buffer_append(ResourceBuffer* buffer, byte* data, int length)
{
    if (buffer)
    {
        if ((buffer->Length + length) >= buffer->MaxLength)
        {
            buffer->MaxLength = (int)((double)(buffer->Length + length + buffer->MaxLength) * 1.5);
            buffer->Data      = (byte**)realloc((byte**)buffer->Data, buffer->MaxLength + 1);
        }

        memcpy(buffer->Data + buffer->Length, data, length);
        buffer->Length += length;
        *(buffer->Data + buffer->Length) = 0;
    }
}
void resource_buffer_append_string(ResourceBuffer* buffer, const char* data)
{
    if (buffer)
    {
        int length = strlen(data);

        if ((buffer->Length + length) >= buffer->MaxLength)
        {
            buffer->MaxLength = (int)((double)(buffer->Length + length + buffer->MaxLength) * 1.5);
            buffer->Data      = (byte**)realloc((byte**)buffer->Data, buffer->MaxLength + 1);
        }

        memcpy(buffer->Data + buffer->Length, data, length);
        buffer->Length += length;
        *(buffer->Data + buffer->Length) = 0;
    }
}

void resource_buffer_append_format(ResourceBuffer* buffer, const char* format, ...)
{
    if (buffer)
    {
        va_list ap;
        char* fstr = NULL;
        va_start(ap, format);
        int len = vsnprintf(NULL, 0, format, ap);
        va_end(ap);
        fstr = (char*)malloc(len + 1);
        va_start(ap, format);
        if (fstr) vsnprintf(fstr, len + 1, format, ap);
        va_end(ap);

        if (len > 0)
        {
            if ((buffer->Length + len) >= buffer->MaxLength)
            {
                buffer->MaxLength = (int)((double)(buffer->Length + len) * 1.5);
                buffer->Data = (byte**)realloc((byte**)buffer->Data, buffer->MaxLength + 1);
            }

            memcpy(buffer->Data + buffer->Length, fstr, len);
            buffer->Length += len;
            *(buffer->Data + buffer->Length) = 0;
        }
        free(fstr);
    }
}

void resource_buffer_copy(ResourceBuffer* source, ResourceBuffer* dest)
{
    if (source && dest && source->Data && source->Length >= 0)
    {
        dest->Length = source->Length;
        dest->Data   = malloc(dest->Length);

        memcpy(dest->Data, source->Data, dest->Length);
    }
}

void resource_buffer_release(ResourceBuffer* source, bool only_data)
{
    if (source)
    {
        if (only_data)
        {
            free(source->Data);
            return;
        }

        free(source);
    }
}






void event_list_init(MessageEventList* list)
{
    list->Count = 0;
    list->MaxCount = 100;
    list->Items = (void**)malloc(list->MaxCount * sizeof(void*));
}

MessageEventList* event_list_create()
{
    MessageEventList* list = (MessageEventList*)malloc(sizeof(MessageEventList));
    event_list_init(list);
    return list;
}

void event_list_add(MessageEventList* list, MessageEvent* item)
{
    if (list)
    {
        if (list->Count >= list->MaxCount)
        {
            list->MaxCount = ((list->Count + sizeof(MessageEvent)) + list->MaxCount) * 2;
            list->Items = (void**)realloc((void**)list->Items, list->MaxCount * sizeof(void*));
        }

        list->Items[list->Count] = item;
        list->Count++;
    }
}

void event_list_remove(MessageEventList* list, MessageEvent* item)
{
    if (list == NULL || list->Count == 0) return;

    int index = -1;
    for (int i = 0; i < list->Count; i++) 
    {
        if (list->Items[i] == item)
        {
            index = i;
            break;
        }
    }

    if (index != -1) // item encontrado
    {
        for (int i = index; i < list->Count - 1; i++) 
        {
            list->Items[i] = list->Items[i +1];
        }
        list->Count--;
    }
}


MessageEventList* event_list_release(MessageEventList* list, bool only_data)
{
    if (list)
    {
        if (only_data)
        {
            int ix = 0;
            while (ix < list->Count)
            {
                free(list->Items[ix]);
                ix++;
            }
            free(list->Items);
            return list;
        }
        free(list);
    }
    return 0;
}


