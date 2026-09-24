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


#include "event_server.h"
#include "server_type.h"
#include <stdlib.h>
#include <string.h>



void message_field_param_data(byte* data, int data_leng, int position, int length, MessageFieldParam* param)
{
    int s = string_index_of_char(data, data_leng, '=', position, length);
    if (s >= 0)
    {
        string_sub(data, data_leng, position, (s - position), false, &param->Name);
        string_sub(data, data_leng, (s + 1), (length - (s - position) - 1), false, &param->Value);
    }
    else
    {
        string_sub(data, data_leng, position, length, false, &param->Value);
    }
}


void message_field_param_scalar(byte* data, MessageFieldParam* param)
{
    param->IsScalar   = true;
    param->IsEndGroup = true;
    param->IsEndParam = true;

    string_appends(&param->Name, data, (int)strlen(data), 0, (int)strlen(data));
}


void message_field_param_add(byte* data, int begin, int end, bool first, bool is_within, MessageFieldParam* param)
{
    if (((begin + 1) < end) && (data[begin] == ' '))
    {
        begin++;
    }

    int m = 0;
    int r = !is_within ? string_index_first(data, end + 1, " ,;(", 4, begin, &m)
                       : string_index_first(data, end + 1, ",;)", 3, begin, &m);

    if (r >= 0)
    {
        if (!is_within && r == 3)
        {
            param->IsHardware = true;
            is_within = true;
            message_field_param_add(data, (m + 1), end, false, is_within, param);
            return;
        }

        int en = m - begin;
        if (en > 0)
        {
            if (is_within && r == 2)
            {
                is_within = false;
            }

            message_field_param_data(data, end + 1, begin, (m - begin), param);

            param->Next = (MessageFieldParam*)memop_calloc_raw(1, sizeof(MessageFieldParam));
            param->Next->Previus = param;
            param->Next->IsHardware = is_within;

            message_field_param_add(data, (m + 1), end, false, is_within, param->Next);

            if (r == 0 || r == 1) param->IsEndGroup = true; else param->IsEndParam = true;
        }
        else
        {
            param->IsEndGroup = true;
            param->IsEndParam = true;
            param->IsScalar = first;
            message_field_param_data(data, end + 1, begin, (end - begin), param);
        }
    }
    else
    {
        param->IsEndGroup = true;
        param->IsEndParam = true;
        param->IsScalar = first;
        message_field_param_data(data, end + 1, begin, (end - begin), param);
    }
}


void message_field_param_release(MessageFieldParam* param);

void message_field_param_release(MessageFieldParam* param)
{
    if (param->Name.Max > 0)
    {
        string_release_data(&param->Name);
        param->Name.Max = 0;
        param->Name.Length = 0;
    }
    if (param->Value.Max > 0)
    {
        string_release_data(&param->Value);
        param->Value.Max = 0;
        param->Value.Length = 0;
    }

    if (param->Next)
    {
        message_field_param_release(param->Next);
    }
}


MessageField* message_field_create(bool init_content)
{
    MessageField* ins = (MessageField*)memop_calloc_raw(1,sizeof(MessageField));

    if (init_content)
    {
        string_init(&ins->Name);
    }
    else
    {
        ins->Name.Max = -1;
    }
    return ins;
}


void message_field_list_init(MessageFieldList* list)
{
    list->Count = 0;
    list->MaxCount = 100;
    list->Items = memop_alloc_raw(list->MaxCount * sizeof(void*));
}

void message_field_list_add(MessageFieldList* list, MessageField* field)
{
    if (list)
    {
        if (list->Count >= list->MaxCount)
        {
            list->MaxCount = ((list->Count + sizeof(MessageField)) + list->MaxCount) * 2;
            list->Items = memop_realloc_raw(list->Items, list->MaxCount * sizeof(MessageField*));
        }

        list->Items[list->Count] = field;
        list->Count++;
    }
}

void message_field_list_add_v(MessageFieldList* list, const char* name, const char* value)
{
    MessageField* field = message_field_create(true);

    string_appends(&field->Name, name, (int)strlen(name), 0, (int)strlen(name));
    message_field_param_scalar(value, &field->Param);

    message_field_list_add(list,field);
}

MessageField* message_field_release(MessageField* ins)
{
    if (ins->Name.Max > 0)
    {
        string_release_data(&ins->Name);
        ins->Name.Max = 0;
        ins->Name.Length    = 0;
    }

    if (ins->Raw.Max > 0)
    {
        string_release_data(&ins->Raw);
        ins->Raw.Max = 0;
        ins->Raw.Length = 0;
    }

    message_field_param_release(&ins->Param);
    memop_free_raw(ins);
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
        memop_free_raw(list->Items);

        if (!only_data)
        {
            memop_free_raw(list);
        }
    }
    return 0;
}


Message* message_create()
{
    Message* ar = (Message*)memop_alloc_raw(sizeof(Message));
    memset(ar,0, sizeof(Message));
    string_array_init(&ar->Route);
    message_field_list_init(&ar->Fields);
    return ar;
}


void message_release(Message* m)
{
    message_field_list_release(&m->Fields, true);

    if (m->Route.Max > 0)
    {
        string_array_release(&m->Route, true);
    }

    if (m->Version.Max > 0)
    {
        string_release_data(&m->Version);
    }

    if (m->Host.Max > 0)
    {
        string_release_data(&m->Host);
    }

    if (m->Content.Max > 0)
    {
        string_release_data(&m->Content);
    }

    // Campos de WebSocket, alocados em message_parser.c (message_set_string).
    StringX** ws_fields[3] = { &m->SecWebsocketKey, &m->SecWebsocketAccept, &m->Upgrade };
    for (int i = 0; i < 3; i++)
        if (*ws_fields[i]) { string_release_data(*ws_fields[i]); memop_free_raw(*ws_fields[i]); *ws_fields[i] = 0; }

    if (m->Param)
    {
        message_field_param_release(m->Param);
    }
    memop_free_raw(m);
}






void appclient_list_add(AppClientList* list, AppClientInfo* cli)
{
    if (list)
    {
        if (list->Count >= list->MaxCount)
        {
            list->MaxCount = ((list->Count + sizeof(AppClientInfo)) + list->MaxCount) * 2;
            list->Items = memop_realloc_raw(list->Items, list->MaxCount * sizeof(void*));
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
            memop_free_raw(list->Items[ix]);
            ix++;
        }
        memop_free_raw(list->Items);
        memop_free_raw(list);
    }
    return 0;
}

AppClientList* appclient_list_create()
{
    AppClientList* ar = (AppClientList*)memop_alloc_raw(sizeof(AppClientList));
    ar->Count = 0;
    ar->MaxCount = 100;
    ar->Items = memop_alloc_raw(ar->MaxCount * sizeof(void*));
    return ar;
}




//FunctionBind* bind_create(const char* route, bool is_web_application, bool with_callback, bool is_event_emitter)
FunctionBind* bind_create(const char* route)
{
    FunctionBind* ar = (FunctionBind*)memop_calloc_raw(1,sizeof(FunctionBind));
    string_array_init(&ar->Route);
    string_split_param(route, strlen(route), "/", 1, true, &ar->Route);
    return ar;
}
FunctionBind* bind_create_to_extension(const char* extension)
{
    FunctionBind* ar = (FunctionBind*)memop_calloc_raw(1, sizeof(FunctionBind));
    int leng = strlen(extension);
    string_init_copy(&ar->Extension, extension, leng);
    return ar;
}

FunctionBind* bind_release(FunctionBind* _this)
{
    if (_this)
    {
        string_array_release(&_this->Route, true);   // Route e lista, nao string
        memop_free_raw(_this);
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
            list->Items = memop_realloc_raw(list->Items, list->MaxCount * sizeof(void*));
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
        memop_free_raw(list->Items);
        memop_free_raw(list);
    }
    return 0;
}

FunctionBindList* bind_list_create()
{
    FunctionBindList* ar = (FunctionBindList*)memop_alloc_raw(sizeof(FunctionBindList));
    ar->Count = 0;
    ar->MaxCount = 100;
    ar->Items = memop_alloc_raw(ar->MaxCount * sizeof(void*));
    return ar;
}







void serverinfo_list_init(AppServerList* list)
{
    list->Count = 0;
    list->MaxCount = 100;
    list->Items = memop_alloc_raw(list->MaxCount * sizeof(void*));
}


void serverinfo_list_release(AppServerList* list)
{
    if (list)
    {
        memop_free_raw(list->Items);
        memop_free_raw(list);
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
            list->Items = memop_realloc_raw(list->Items, list->MaxCount * sizeof(void*));
        }

        list->Items[list->Count] = server;
        list->Count++;
    }
}

void serverinfo_release(AppServerInfo* server)
{
    string_release_data(&server->AbsLocal);

    appclient_list_release(server->Clients);
    memop_free_raw(server);
}

AppServerInfo* serverinfo_create()
{
    AppServerInfo* server;
    server = (AppServerInfo*)memop_calloc_raw(1, sizeof(AppServerInfo));
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
    source->Data      = memop_calloc_raw(1, source->MaxLength);
}
void resource_buffer_append(ResourceBuffer* buffer, byte* data, int length)
{
    if (buffer)
    {
        if ((buffer->Length + length) >= buffer->MaxLength)
        {
            buffer->MaxLength = (int)((double)(buffer->Length + length + buffer->MaxLength) * 1.5);
            buffer->Data      = memop_realloc_raw(buffer->Data, buffer->MaxLength + 1);
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
            buffer->Data      = memop_realloc_raw(buffer->Data, buffer->MaxLength + 1);
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
        fstr = (char*)memop_alloc_raw(len + 1);
        va_start(ap, format);
        if (fstr) vsnprintf(fstr, len + 1, format, ap);
        va_end(ap);

        if (len > 0)
        {
            if ((buffer->Length + len) >= buffer->MaxLength)
            {
                buffer->MaxLength = (int)((double)(buffer->Length + len) * 1.5);
                buffer->Data = memop_realloc_raw(buffer->Data, buffer->MaxLength + 1);
            }

            memcpy(buffer->Data + buffer->Length, fstr, len);
            buffer->Length += len;
            *(buffer->Data + buffer->Length) = 0;
        }
        memop_free_raw(fstr);
    }
}

void resource_buffer_copy(ResourceBuffer* source, ResourceBuffer* dest)
{
    if (source && dest && source->Data && source->Length >= 0)
    {
        dest->Length = source->Length;
        dest->Data   = memop_alloc_raw(dest->Length);

        memcpy(dest->Data, source->Data, dest->Length);
    }
}

void resource_buffer_release(ResourceBuffer* source, bool only_data)
{
    if (source)
    {
        if (only_data)
        {
            memop_free_raw(source->Data);
            return;
        }

        memop_free_raw(source);
    }
}






void event_list_init(MessageEventList* list)
{
    list->Count = 0;
    list->MaxCount = 100;
    list->Items = memop_alloc_raw(list->MaxCount * sizeof(void*));
}

MessageEventList* event_list_create()
{
    MessageEventList* list = (MessageEventList*)memop_alloc_raw(sizeof(MessageEventList));
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
            list->Items = memop_realloc_raw(list->Items, list->MaxCount * sizeof(void*));
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
                memop_free_raw(list->Items[ix]);
                ix++;
            }
            memop_free_raw(list->Items);
            return list;
        }
        memop_free_raw(list);
    }
    return 0;
}




MessageResponseInfo* message_response_create(int status, ContentTypeOption type)
{
    MessageResponseInfo* ar = (MessageResponseInfo*)memop_alloc_raw(sizeof(MessageResponseInfo));
    memset(ar, 0, sizeof(MessageResponseInfo));
    resource_buffer_init(&ar->Content);
    message_field_list_init(&ar->Fields);
    ar->ContentType = type;
    ar->Status = status;
    return ar;
}

MessageResponseInfo* message_response_create_content(int status, ContentTypeOption type, char* content, int size)
{
    MessageResponseInfo* ar = (MessageResponseInfo*)memop_alloc_raw(sizeof(MessageResponseInfo));
    memset(ar, 0, sizeof(MessageResponseInfo));
    resource_buffer_init(&ar->Content);
    message_field_list_init(&ar->Fields);
    ar->ContentType = type;
    ar->Content.Type = type;
    ar->Status = status;
    resource_buffer_append(&ar->Content, content, size);
    return ar;
}

MessageResponseInfo* message_response_create_text(int status, char* content)
{
    MessageResponseInfo* ar = (MessageResponseInfo*)memop_alloc_raw(sizeof(MessageResponseInfo));
    memset(ar, 0, sizeof(MessageResponseInfo));
    resource_buffer_init(&ar->Content);
    message_field_list_init(&ar->Fields);
    ar->ContentType = TEXT_PLAIN;
    ar->Content.Type = TEXT_PLAIN;
    ar->Status = status;

    int size = strlen(content);
    resource_buffer_append(&ar->Content, content, size);
    return ar;
}
