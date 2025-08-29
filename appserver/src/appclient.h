//  MIT License – Modified for Mandatory Attribution
//  
//  Copyright(c) 2025 Sergio Paludo
//
//  github.com/sergiocupa
//  
//  Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files, 
//  to use, copy, modify, merge, publish, distribute, and sublicense the software, including for commercial purposes, provided that:
//  
//     01. The original author’s credit is retained in all copies of the source code;
//     02. The original author’s credit is included in any code generated, derived, or distributed from this software, including templates, libraries, or code - generating scripts.
//  
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED.


#ifndef APPCLIENT_H
#define APPCLIENT_H

#ifdef __cplusplus
extern "C" {
#endif

    #include "server_type.h"


    void appclient_send(AppClientInfo* cli, byte* content, int length, bool is_websocket);
    void appclient_list_add(AppClientList* list, AppClientInfo* cli);
    AppClientList* appclient_list_release(AppClientList* list);
    AppClientList* appclient_list_create();
    AppClientInfo* appclient_create(void* ptr, AppServerInfo* server, MessageMatchReceiverCalback receiver);


#ifdef __cplusplus
}
#endif

#endif /* APPCLIENT */