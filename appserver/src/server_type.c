#include "server_type.h"


void message_field_param_release(MessageFieldParam* param);

void message_field_param_release(MessageFieldParam* param)
{
    string_release_data(&param->Name);
    string_release_data(&param->Value);

    while (!param->Next)
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


void message_field_release(MessageField* ins)
{
    string_release(&ins->Name);
    message_field_param_release(&ins->Param);
    free(ins);
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

void message_field_list_release(MessageFieldList* list)
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
        free(list);
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
    message_field_list_release(&m->Fields);

    if (m->Route.MaxCount > 0)
    {
        string_array_release(&m->Route, false);
    }

    if (m->Version.MaxLength > 0)
    {
        string_release(&m->Version);
    }

    if (m->Host.MaxLength > 0)
    {
        string_release(&m->Host);
    }

    if (m->Content.MaxLength > 0)
    {
        string_release(&m->Content);
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





FunctionBind* bind_create(const char* route, MessageMatchCallback function)
{
    FunctionBind* ar = (FunctionBind*)malloc(sizeof(FunctionBind));
    string_init_copy(&ar->Route, route, strlen(route));
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

void bind_list_add(FunctionBindList* list, const char* route, MessageMatchCallback function)
{
    if (list)
    {
        if (list->Count >= list->MaxCount)
        {
            list->MaxCount = ((list->Count + sizeof(FunctionBind)) + list->MaxCount) * 2;
            list->Items = (void**)realloc((void**)list->Items, list->MaxCount * sizeof(void*));
        }

        list->Items[list->Count] = bind_create(route, function);
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


