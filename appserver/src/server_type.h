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
    #include "../submodules/yason/yason/src/yason_element.h"
    
    #define AOTP_HEADER_SIGN "AOTP[9DAC10BA43E2402694C84B21A4219497]"

    #define LIST_CREATE(TYPE, THIS) THIS = list_create(sizeof(TYPE))
    #define LIST_ADD(TYPE, THIS, ITEM) list_add(THIS, ITEM, sizeof(TYPE))
 

    typedef struct _AppClientInfo     AppClientInfo;
    typedef struct _AppClientList     AppClientList;
    typedef struct _ClientData        ClientData;
    typedef struct _AppServerInfo     AppServerInfo;
    typedef struct _MessageFieldParam MessageFieldParam;
    typedef struct _Message           Message;
	typedef struct _MessageEvent      MessageEvent;
    typedef struct _MessageEventList  MessageEventList;
    typedef struct _ThunkArgs         ThunkArgs;


    typedef enum _HttpStatusCode
    {
        HTTP_STATUS_NONE                = 0,
        HTTP_STATUS_OK                  = 200,
        HTTP_STATUS_ACCEPT              = 202,
        HTTP_STATUS_BAD_REQUEST         = 400,
        HTTP_STATUS_UNAUTHORIZED        = 401,
        HTTP_STATUS_FORBIDDEN           = 403,
        HTTP_STATUS_NOT_FOUND           = 404,
        HTTP_STATUS_PRECONDITION_FAILED = 412,
        HTTP_STATUS_INTERNAL_ERROR      = 500,
        HTTP_STATUS_NOT_IMPLEMENTED     = 501,
        HTTP_STATUS_SERVICE_UNAVAILABLE = 503,
        HTTP_STATUS_SWITCHING_PROTOCOLS = 101
    }
    HttpStatusCode;


    struct _MessageFieldParam
    {
        bool IsScalar;
        bool IsEndGroup;
        bool IsEndParam;
        bool IsHardware;
        String Name;
        String Value;
        MessageFieldParam* Next;
        MessageFieldParam* Previus;
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


    typedef struct _ResourceBuffer
    {
        ContentTypeOption Type;
        int               MaxLength;
        int               Length;
        byte*             Data;
    }
    ResourceBuffer;


    struct _Message
    {
        bool               IsMatch;
        MessageProtocol    Protocol;
        String             Version;
        MessageCommand     Cmd;
        MessageCommand     OriginCmd;
        int                ResponseStatus;
        StringArray        Route;
        String             Host;
        MessageConnection  ConnectionOption;
        String             UserAgent;
        String             Content;
        ContentTypeOption  ContentType;
        int                ContentLength;
        MessageFieldList   Fields;
        String*            SessionUID;
        String*            EventUID;
        String*            OriginEventUID;
        MessageFieldParam* Param;
        String*            SecWebsocketKey;
        String*            SecWebsocketAccept;
        String*            Upgrade;
        void*              MatchThread;

        AppClientInfo*     Client;
        void*              Object;

        String*            ResponseContent;
    };




    typedef Element* (*MessageMatchReceiverCalback) (ResourceBuffer* result);
    typedef void  (*MessageEmitterCalback)  (ResourceBuffer* object, MessageMatchReceiverCalback callback);
    typedef void  (*MessageSenderCalback) (ResourceBuffer* object, MessageMatchReceiverCalback callback, AppClientInfo* client);

    struct _MessageEventList
    {
		int MaxCount;
		int Count;
		MessageEvent** Items;
    };

    struct _MessageEvent
    {
        String UID;
        String OriginUID;
        bool WaitForCallback;
        MessageCommand LastCommand;
        MessageCommand CurrentStep;
		MessageMatchReceiverCalback Callback;
        AppClientInfo* Client;
    };

    struct _ThunkArgs
    {
        MessageSenderCalback Sender;
        AppClientInfo*       Client;
    };




    typedef struct _MessageParser
    {
        int                         Position;
        Message* Partial;
        String* Buffer;
        MessageMatchReceiverCalback MessageMatch;
    }
    MessageParser;




    struct _AppClientInfo
    {
        bool           IsConnected;
		bool           IsWebSocketMode;
        String         LocalHost;
        String         RemoteHost;
        void*          Handle;
        void*          ReceivedThread;
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



    typedef void(*HeaderAppender) (void* args, ResourceBuffer* http);



    typedef void(*RequestCallback) (Message* request);

    typedef struct _FunctionBind
    {
        bool        IsWebApplication;
        bool        WithCallback;
		bool        IsEventEmitter;
        StringArray Route;
        String      AbsPathWebContent;
        ThunkArgs   Thung;
		MessageMatchReceiverCalback CallbackFunc;
    }
    FunctionBind;

    typedef struct _FunctionBindList
    {
        int Count;
        int MaxCount;
        FunctionBind** Items;
    }
    FunctionBindList;


    typedef struct _AppServerList
    {
        int Count;
        int MaxCount;
        AppServerInfo** Items;
    }
    AppServerList;


    struct _AppServerInfo
    {
        bool                      IsRunning;
        int                       Port;
        String                    AgentName;
        ContentTypeOption         DefaultWebApiObjectType;
        String                    AbsLocal;
        StringArray*              Prefix;
        void*                     Handle;
        void*                     AcceptThread;
        AppClientList*            Clients;
        FunctionBindList*         BindList;
        MessageEventList*         Events;
    };



    MessageField* message_field_create(bool init_content);
    MessageField* message_field_release(MessageField* ins);
    void message_field_list_init(MessageFieldList* list);
    void message_field_list_add(MessageFieldList* list, MessageField* field);
    void message_field_list_release(MessageFieldList* list, bool only_data);
    Message* message_create();
    void message_release(Message* m);


    void appclient_list_add(AppClientList* list, AppClientInfo* cli);
    AppClientList* appclient_list_release(AppClientList* list);
    AppClientList* appclient_list_create();



    FunctionBind* bind_create(const char* route);
    void bind_list_add_web_resource(FunctionBindList* list, const char* route, MessageMatchReceiverCalback function);
    void bind_list_add_receiver(FunctionBindList* list, const char* route, MessageMatchReceiverCalback function, bool with_callback);
    FunctionBindList* bind_list_release(FunctionBindList* list);
    FunctionBindList* bind_list_create();
    void bind_list_add(FunctionBindList* list, FunctionBind* bind);
    FunctionBind* bind_create(const char* route);



    void serverinfo_list_init(AppServerList* list);
    void serverinfo_list_release(AppServerList* list);
    void serverinfo_list_add(AppServerList* list, AppServerInfo* server);
    void serverinfo_release(AppServerInfo* server);
    AppServerInfo* serverinfo_create();


    void resource_buffer_copy(ResourceBuffer* source, ResourceBuffer* dest);
    void resource_buffer_release(ResourceBuffer* source, bool only_data);
    void resource_buffer_append(ResourceBuffer* buffer, byte* data, int length);
    void resource_buffer_init(ResourceBuffer* source);
    void resource_buffer_append_format(ResourceBuffer* buffer, const char* format, ...);
    void resource_buffer_append_string(ResourceBuffer* buffer, const char* data);


    void event_list_init(MessageEventList* list);
    MessageEventList* event_list_create();
    void event_list_add(MessageEventList* list, MessageEvent* item);
    MessageEventList* event_list_release(MessageEventList* list, bool only_data);
    void event_list_remove(MessageEventList* list, MessageEvent* item);


#ifdef __cplusplus
}
#endif

#endif /* APPSERVERTYPE */