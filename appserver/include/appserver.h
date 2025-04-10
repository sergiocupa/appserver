#ifndef APPSERVER_H
#define APPSERVER_H

#ifdef __cplusplus
extern "C" {
#endif

    #include "../src/server_type.h"



    PLATFORM_API AppServerInfo* appserver_create(const char* agent_name, const int port, const char* prefix, FunctionBindList* bind_list);

    void app_add_receiver(FunctionBindList* list, const char* route, MessageMatchReceiverCalback function, bool with_callback);
    void app_add_web_resource(FunctionBindList* list, const char* route, MessageMatchReceiverCalback function);
    MessageEmitterCalback app_add_emitter(FunctionBindList* list, const char* route);


#ifdef __cplusplus
}
#endif

#endif /* APPSERVER */