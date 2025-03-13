#include "server_type.h"
#include <stdlib.h>
#include <string.h>


void message_field_param_release(MessageFieldParam* param);

void message_field_param_release(MessageFieldParam* param)
{
    string_release_data(&param->Name);
    string_release_data(&param->Value);

    while (param->Next)
    {
        message_field_param_release(param->Next);
    }
}


MessageField* message_field_create(bool init_content)
{
    MessageField* ins = (MessageField*)malloc(sizeof(MessageField));
    memset(ins, 0, sizeof(MessageField));

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
    string_release_data(&ins->Name);
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





FunctionBind* bind_create(const char* route, MessageMatchCallback function, bool is_web_application)
{
    FunctionBind* ar = (FunctionBind*)calloc(1,sizeof(FunctionBind));
    ar->IsWebApplication = is_web_application;

    string_array_init(&ar->Route);
    string_split_param(route, strlen(route), "/", 1, true, &ar->Route);

    ar->Function = function;
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

void bind_list_add(FunctionBindList* list, const char* route, MessageMatchCallback function, bool is_web_application)
{
    if (list)
    {
        if (list->Count >= list->MaxCount)
        {
            list->MaxCount = ((list->Count + sizeof(FunctionBind)) + list->MaxCount) * 2;
            list->Items = (void**)realloc((void**)list->Items, list->MaxCount * sizeof(void*));
        }

        list->Items[list->Count] = bind_create(route, function, is_web_application);
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



void resource_buffer_copy(ResourceBuffer* source, ResourceBuffer* dest)
{
    if (source && dest && source->Data && source->Length >= 0)
    {
        dest->Length = source->Length;
        dest->Data = malloc(dest->Length);

        memcpy(dest->Data, source->Data, dest->Length);
    }
}


