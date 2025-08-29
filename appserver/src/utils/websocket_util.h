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


#ifndef WEBSOCKET_UTIL_H
#define WEBSOCKET_UTIL_H

#ifdef __cplusplus
extern "C" {
#endif

    #include "../server_type.h"


    byte* websocket_encode_text(const byte* message, size_t* output_length);
    byte* websocket_decode_frame(const byte* buffer, size_t buffer_len, byte* opcode, size_t* output_length);
    byte* websocket_encode_frame(const byte* data, size_t data_len, byte opcode, int use_mask, size_t* output_length);
    void websocket_handshake_prepare(void* args, ResourceBuffer* http);


#ifdef __cplusplus
}
#endif

#endif /* WEBSOCKET_UTIL */