#include "server_type.h"
#include "string.h"
#include "stdlib.h"



MessageField* message_field_create()
{
    MessageField* ins = (MessageField*)malloc(sizeof(MessageField));
    memset(ins, 0, sizeof(MessageField));
    string_init(&ins->Name);
    string_array_init(&ins->Content);
    return ins;
}

void message_field_release(MessageField* ins)
{
    string_release(&ins->Name);
    string_array_release(&ins->Content, true);
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
    string_init(&ar->Route);
    message_field_list_init(&ar->Fields);
    return ar;
}


void message_release(Message* m)
{
    message_field_list_release(&m->Fields);
    string_release(&m->Route);
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