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


#ifndef MESSAGE_PARSE_H
#define MESSAGE_PARSE_H

#ifdef __cplusplus
extern "C" {
#endif

    #include "../server_type.h"



    const char* message_command_titule(MessageCommand cmd);
    int message_parser_field(byte* data, int length, MessageFieldList* fields, int* position);
    void message_buildup(MessageParser* parser, AppClientInfo* client, byte* data, int length);
    MessageParser* message_parser_create(MessageMatchReceiverCalback receiver);

    void message_field_param_add(byte* data, int begin, int end, bool first, bool is_within, MessageFieldParam* param);

#ifdef __cplusplus
}
#endif

#endif /* MESSAGE_PARSE */