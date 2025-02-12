#ifndef APPSERVER_H
#define APPSERVER_H

#ifdef __cplusplus
extern "C" {
#endif

    #include "platform.h"


    typedef struct _AppServerInfo AppServerInfo;


    struct _AppServerInfo
    {
        void* Handle;
        void* AcceptThread;
        char* Prefix;
        void(*DataReceivedClient)(ClientData*);
        AppClientList* Clients;
    };


    PLATFORM_API AppServerInfo* appserver_create(const int port, const char* prefix);


#ifdef __cplusplus
}
#endif

#endif /* APPSERVER */