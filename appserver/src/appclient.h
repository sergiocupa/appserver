#ifndef APPCLIENT_H
#define APPCLIENT_H

#ifdef __cplusplus
extern "C" {
#endif

    #include "appservertype.h"



    void appclient_list_add(AppClientList* list, AppClientInfo* cli);
    AppClientList* appclient_list_release(AppClientList* list);
    AppClientList* appclient_list_create();
    AppClientInfo* appclient_create(void* ptr, AppServerInfo* server);


#ifdef __cplusplus
}
#endif

#endif /* APPCLIENT */