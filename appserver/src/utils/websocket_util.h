#ifndef WEBSOCKET_UTIL_H
#define WEBSOCKET_UTIL_H

#ifdef __cplusplus
extern "C" {
#endif

    #include "../server_type.h"


    void websocket_handshake_prepare(void* args, ResourceBuffer* http);


#ifdef __cplusplus
}
#endif

#endif /* WEBSOCKET_UTIL */