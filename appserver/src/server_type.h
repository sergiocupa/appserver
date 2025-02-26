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
        String      Name;
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

    typedef enum _MessageCommand
    {
        CMD_NONE           = 0,
        CMD_GET            = 1,
        CMD_OPTIONS        = 2,
        CMD_POST           = 3,
        CMD_ACTION         = 4,
        CMD_CALLBACK       = 5,
        CMD_ACKNOWLEDGMENT = 6
    }
    MessageCommand;

    typedef enum _MessageProtocol
    {
        NONE = 0,
        HTTP = 1,
        AOTP = 2
    }
    MessageProtocol;

    typedef enum _ContentTypeOption
    { 
        TXT  = 0,
        BIN  = 1,
        JSON = 2
    }
    ContentTypeOption;

    typedef struct _Message
    {
        bool              IsMatch;
        MessageProtocol   Protocol;
        String            Version;
        MessageCommand    Cmd;
        StringArray       Route;
        String            Host;
        String            Content;
        ContentTypeOption ContentType;
        int               ContentLength;
        MessageFieldList  Fields;
        void*             MatchThread;
    }
    Message;


    typedef struct _MessageParser
    {
        int                  Position;
        Message*             Partial;
        String*              Buffer;
        void(*MatchCallback) (Message*);
    }
    MessageParser;


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
        void*                     Handle;
        void*                     AcceptThread;
        char*                     Prefix;
        void(*DataReceivedClient) (ClientData*);
        AppClientList*            Clients;
    };



    MessageField* message_field_create(bool init_content);
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