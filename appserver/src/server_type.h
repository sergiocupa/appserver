#ifndef APPSERVERTYPE_H
#define APPSERVERTYPE_H

#ifdef __cplusplus
extern "C" {
#endif

    // TO-DO: esta com referencia INCLUDE redundante por submodules em muitos niveis. listlib.h encontrada em varias origens.
    //        reestruturar os submudules para nao ter esta origem redundante em varios niveis
    #include "../submodules/utility/listlib/include/listlib.h"
    #include "stringlib.h"
    #include "platform.h"
    #include "numeric.h"
    

    #define LIST_CREATE(TYPE, THIS) THIS = list_create(sizeof(TYPE))
    #define LIST_ADD(TYPE, THIS, ITEM) list_add(THIS, ITEM, sizeof(TYPE))
 

    typedef struct _AppClientInfo     AppClientInfo;
    typedef struct _AppClientList     AppClientList;
    typedef struct _ClientData        ClientData;
    typedef struct _AppServerInfo     AppServerInfo;
    typedef struct _MessageFieldParam MessageFieldParam;


    struct _MessageFieldParam
    {
        bool IsScalar;
        bool IsEndGroup;
        bool IsEndParam;
        String Name;
        String Value;
        MessageFieldParam* Next;
    };


    typedef struct _MessageField
    {
        String            Name;
        MessageFieldParam Param;
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
        PROTOCOL_NONE = 0,
        HTTP          = 1,
        AOTP          = 2
    }
    MessageProtocol;

    typedef enum _MessageConnection
    {
        CONNECTION_NONE       = 0,
        CONNECTION_KEEP_ALIVE = 1,
        CONNECTION_CLOSE      = 2
    }
    MessageConnection;

    typedef enum _ContentTypeOption
    { 
        CONTENT_TYPE_NONE        = 0,
        TEXT_HTML                = 1,
        TEXT_CSS                 = 2,
        TEXT_JAVASCRIPT          = 3,
        TEXT_PLAIN               = 4,
        APPLICATION_JAVASCRIPT   = 5,
        APPLICATION_JSON         = 6,
        APPLICATION_XML          = 7,
        APPLICATION_OCTET_STREAM = 8,
        APPLICATION_PDF          = 9,
        APPLICATION_ZIP          = 10,
        APPLICATION_GZIP         = 11,
        IMAGE_JPEG               = 12,
        IMAGE_PNG                = 13,
        IMAGE_GIF                = 14,
        IMAGE_SVG                = 15,
        AUDIO_MPEG               = 16,
        AUDIO_OGG                = 17,
        VIDEO_MP4                = 18,
        VIDEO_WEBM               = 19,
        MULTIPART_FORMDATA       = 20
    }
    ContentTypeOption;

    typedef struct _Message
    {
        bool               IsMatch;
        MessageProtocol    Protocol;
        String             Version;
        MessageCommand     Cmd;
        StringArray        Route;
        String             Host;
        MessageConnection  ConnectionOption;
        String             UserAgent;
        String             Content;
        ContentTypeOption  ContentType;
        int                ContentLength;
        MessageFieldList   Fields;
        MessageFieldParam* Param;
        void*              MatchThread;
     
        AppClientInfo*     Client;
        void*              Object;
    }
    Message;



    typedef void(*MessageMatchCallback) (int*);

    typedef struct _MessageParser
    {
        int                  Position;
        Message*             Partial;
        String*              Buffer;
        MessageMatchCallback MessageMatch;
    }
    MessageParser;



    struct _AppClientInfo
    {
        bool  IsConnected;
        void* Handle;
        void* ReceivedThread;
        AppServerInfo* Server;
        MessageParser* Parser;
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



    typedef void(*RequestCallback) (Message* request);

    typedef struct _FunctionBind
    {
        String Route;
        MessageMatchCallback Function;
    }
    FunctionBind;

    typedef struct _FunctionBindList
    {
        int Count;
        int MaxCount;
        FunctionBind** Items;
    }
    FunctionBindList;



    struct _AppServerInfo
    {
        bool                      IsRunning;
        void*                     Handle;
        void*                     AcceptThread;
        char*                     Prefix;
        AppClientList*            Clients;
        FunctionBindList*         BindList;
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

    void bind_list_add(FunctionBindList* list, const char* route, MessageMatchCallback function);
    FunctionBindList* bind_list_release(FunctionBindList* list);
    FunctionBindList* bind_list_create();



#ifdef __cplusplus
}
#endif

#endif /* APPSERVERTYPE */