#ifndef APPCLIENT_H
#define APPCLIENT_H

#ifdef __cplusplus
extern "C" {
#endif

    #include "../include/appserver.h"
    #include "platform.h"



    typedef struct _AppClientInfo AppClientInfo;
    typedef struct _AppClientList AppClientList;
    typedef struct _ClientData    ClientData;


    struct _ClientData
    {
        AppClientInfo* Client;
        int            Length;
        byte*          Data;
        void*          DispatcherThread;
    };


    struct _AppClientInfo
    {
        void*          Handle;
        void*          ReceivedThread;
        AppServerInfo* Server;
    };


    struct _AppClientList
    {
        int MaxCount;
        int Count;
        AppClientInfo** Items;
    };


    void appclient_list_add(AppClientList* list, AppClientInfo* cli);
    AppClientList* appclient_list_release(AppClientList* list);
    AppClientList* appclient_list_create();


    AppClientInfo* appclient_create(void* ptr, AppServerInfo* server);


#ifdef __cplusplus
}
#endif

#endif /* APPCLIENT */