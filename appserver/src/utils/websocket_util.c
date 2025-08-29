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


#include "websocket_util.h"
#include "shalib.h"
#include "stdlib.h"
#include "stdio.h"


#define WS_FIN  0x80
#define WS_TEXT 0x01
#define WS_BIN  0x02
#define WS_MASK 0x80


byte* websocket_encode_frame(const byte* data, size_t data_len, byte opcode, int use_mask, size_t* output_length)
{
    int bsize = (data_len <= 125) ? 1 : ((data_len <= 0xFFFF) ? 3 : 8);
    if (use_mask)
    {
		bsize += 4;
    }
	byte* buffer = malloc(data_len + bsize +1);

    size_t index = 0;
    buffer[index++] = WS_FIN | opcode;

    if (data_len <= 125) 
    {
        buffer[index++] = (use_mask ? WS_MASK : 0) | data_len;
    }
    else if (data_len <= 0xFFFF)
    {
        buffer[index++] = (use_mask ? WS_MASK : 0) | 126;
        buffer[index++] = (data_len >> 8) & 0xFF;
        buffer[index++] = data_len & 0xFF;
    }
    else 
    {
        buffer[index++] = (use_mask ? WS_MASK : 0) | 127;
        for (int i = 7; i >= 0; i--) 
        {
            buffer[index++] = (data_len >> (i * 8)) & 0xFF;
        }
    }

    byte mask[4] = { 0 };
    if (use_mask) 
    {
        for (int i = 0; i < 4; i++)
        {
            mask[i] = rand() % 256;
            buffer[index++] = mask[i];
        }
    }

    for (size_t i = 0; i < data_len; i++)
    {
        buffer[index++] = use_mask ? (data[i] ^ mask[i % 4]) : data[i];
    }

	*output_length = index;
    return buffer;
}



byte* websocket_decode_frame(const byte* buffer, size_t buffer_len, byte* opcode, size_t* output_length)
{
    if (buffer_len < 2) return 0;

    size_t index = 0;
    *opcode = buffer[index++] & 0x0F;

    byte payload_len = buffer[index++] & 0x7F;
    size_t data_len = payload_len;

    if (payload_len == 126) 
    {
        if (buffer_len < 4) return 0;
        data_len = (buffer[index] << 8) | buffer[index + 1];
        index += 2;
    }
    else if (payload_len == 127) 
    {
        if (buffer_len < 10) return 0;
        data_len = 0;
        for (int i = 0; i < 8; i++) {
            data_len = (data_len << 8) | buffer[index++];
        }
    }

    int has_mask = buffer[index - 1] & WS_MASK;
    byte mask[4] = { 0 };
    if (has_mask) 
    {
        if (buffer_len < index + 4) return 0;
        for (int i = 0; i < 4; i++) 
        {
            mask[i] = buffer[index++];
        }
    }

    if (buffer_len < index + data_len) return 0;

    byte* output = malloc(data_len+1);
    for (size_t i = 0; i < data_len; i++) 
    {
        output[i] = has_mask ? (buffer[index + i] ^ mask[i % 4]) : buffer[index + i];
    }
	output[data_len] = '\0';
	*output_length = data_len;
    return output;
}


byte* websocket_encode_text(const byte* message, size_t* output_length)
{
    return websocket_encode_frame(message, strlen(message), WS_TEXT, true, output_length);
}




void websocket_handshake_prepare(void* args, ResourceBuffer* http)
{
	String* web_key = (String*)args;

	resource_buffer_append_string(http, "Connection: Upgrade\r\n");
	resource_buffer_append_string(http, "Upgrade: websocket\r\n");

	const char* UID = "56A5421037EA456699D6A7E3EC581DE4";// TO-DO: Implementar um gerador de GUID
	char nk[256];
	snprintf(nk, sizeof(nk), "%s%s", web_key->Data, UID);

	int ub_length = 0;
	byte* ub = string_utf8_to_bytes(nk, &ub_length);
	byte digest[SHA1_BLOCK_SIZE];
	sha1(ub, ub_length, &digest);
	free(ub);
	char* base64 = string_base64_encode(digest, SHA1_BLOCK_SIZE);

	resource_buffer_append_format(http, "Sec-WebSocket-Accept: &s\r\n", base64);
	free(base64);
}