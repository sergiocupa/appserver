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