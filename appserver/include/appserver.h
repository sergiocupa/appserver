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



    /* enable_health_monitor: liga a rota embutida de saude, /<prefix>/health.
     *
     *   true  -> amostra CPU, memoria do processo, contadores do pool do xplatbase e, no
     *            Windows, GPU via PDH. A rota expoe contadores internos do processo SEM
     *            passar por autenticacao -- ver a ressalva em health_controller.c antes de
     *            expor o servidor fora de uma rede de confianca.
     *   false -> o monitor nao inicializa, a consulta do PDH nao abre, nenhuma amostra e
     *            colhida e /<prefix>/health nao existe (cai na rota nao encontrada).
     *
     *  O que o parametro NAO muda: pdh.dll continua mapeada no processo nos dois casos,
     *  porque o import e estatico (ver a nota em health_monitor.c). d3d11.dll e dxgi.dll
     *  tambem aparecem sempre, mas vem do encode por hardware em codecs/media/hw_dec_mf.c,
     *  nada a ver com este monitor. Em nenhum dos casos roda codigo de PDH com false.
     */
    XPLATBASE_API AppServerInfo* appserver_create(const char* agent_name, const int port, const char* prefix, const char* web_content_path, FunctionBindList* bind_list, boolean enable_health_monitor);

    void app_add_receiver(FunctionBindList* list, const char* route, MessageMatchReceiverCalback function, bool with_callback);
    void app_add_receiver_extension(FunctionBindList* list, const char* extension, MessageMatchReceiverCalback function, bool with_callback);
    void app_add_web_resource(FunctionBindList* list, const char* route, MessageMatchReceiverCalback function);
    MessageEmitterCalback app_add_emitter(FunctionBindList* list, const char* route);

    void appserver_http_response_send(AppServerInfo* server, Message* request, HttpStatusCode http_status, ResourceBuffer* object, HeaderAppender header_appender, void* appender_args);


#ifdef __cplusplus
}
#endif

#endif /* APPSERVER */