#ifndef APPSERVER_H
#define APPSERVER_H

#ifdef __cplusplus
extern "C" {
#endif

    #include "../src/server_type.h"



    PLATFORM_API AppServerInfo* appserver_create(const int port, const char* prefix, FunctionBindList* bind_list);


#ifdef __cplusplus
}
#endif

#endif /* APPSERVER */