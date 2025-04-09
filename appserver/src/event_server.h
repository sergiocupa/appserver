#ifndef EVENT_SERVER_H
#define EVENT_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

    #include "server_type.h"

    void event_sender(ResourceBuffer* object, MessageMatchReceiverCalback callback);
    void appserver_received_aotp(Message* request);


    void event_sender_server(ResourceBuffer* object, MessageMatchReceiverCalback callback, AppClientInfo* client);


#ifdef __cplusplus
}
#endif

#endif /* EVENT_SERVER */