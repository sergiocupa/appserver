#ifndef APPSERVERTYPE_H
#define APPSERVERTYPE_H

#ifdef __cplusplus
extern "C" {
#endif

    #include "stringlib.h"
    #include "platform.h"



    typedef struct _AppClientInfo AppClientInfo;
    typedef struct _AppClientList AppClientList;
    typedef struct _ClientData    ClientData;
    typedef struct _AppServerInfo AppServerInfo;



    typedef struct _MessageField
    {
        String Name;
        StringArray Content;
    }
    MessageField;
    
    typedef struct _MessageFieldList
    {
        int MaxCount;
        int Count;
        MessageField** Items;
    }
    MessageFieldList;

    typedef struct _Message
    {
        bool IsResponse;
        String Route;
        MessageFieldList Fields;
    }
    Message;


    struct _AppClientInfo
    {
        bool  IsConnected;
        void* Handle;
        void* ReceivedThread;
        AppServerInfo* Server;
    };


    struct _AppClientList
    {
        int MaxCount;
        int Count;
        AppClientInfo** Items;
    };

    struct _ClientData
    {
        AppClientInfo* Client;
        void* DispatcherThread;

        int   Length;
        byte* Data;
    };

    struct _AppServerInfo
    {
        void* Handle;
        void* AcceptThread;
        char* Prefix;
        void(*DataReceivedClient)(ClientData*);
        AppClientList* Clients;
    };



    MessageField* message_field_create();
    void message_field_release(MessageField* ins);
    void message_field_list_init(MessageFieldList* list);
    void message_field_list_add(MessageFieldList* list, MessageField* field);
    void message_field_list_release(MessageFieldList* list);
    Message* message_create();
    void message_release(Message* m);


    void appclient_list_add(AppClientList* list, AppClientInfo* cli);
    AppClientList* appclient_list_release(AppClientList* list);
    AppClientList* appclient_list_create();



#ifdef __cplusplus
}
#endif

#endif /* APPSERVERTYPE */