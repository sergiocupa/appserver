#ifndef CALLBACK_BINDER_H
#define CALLBACK_BINDER_H

#ifdef __cplusplus
extern "C" {
#endif

    #include "../server_type.h"


    void binder_list_add_receiver(FunctionBindList* list, const char* route, MessageMatchReceiverCalback function, bool with_callback);
    void binder_list_add_web_resource(FunctionBindList* list, const char* route, MessageMatchReceiverCalback function);
    MessageEmitterCalback binder_list_add_emitter(FunctionBindList* list, const char* route);


#ifdef __cplusplus
}
#endif

#endif /* CALLBACK_ASSEMBLER */