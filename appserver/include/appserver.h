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