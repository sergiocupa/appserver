#ifndef APPSERVERTYPE_H
#define APPSERVERTYPE_H

#ifdef __cplusplus
extern "C" {
#endif

    #include "platform.h"

    typedef struct _AppClientInfo AppClientInfo;
    typedef struct _AppClientList AppClientList;
    typedef struct _ClientData    ClientData;
    typedef struct _AppServerInfo AppServerInfo;


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


#ifdef __cplusplus
}
#endif

#endif /* APPSERVERTYPE */