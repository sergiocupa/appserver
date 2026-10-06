//  MIT License � Modified for Mandatory Attribution
//  
//  Copyright(c) 2025 Sergio Paludo
//
//  github.com/sergiocupa
//  
//  Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files, 
//  to use, copy, modify, merge, publish, distribute, and sublicense the software, including for commercial purposes, provided that:
//  
//     01. The original author�s credit is retained in all copies of the source code;
//     02. The original author�s credit is included in any code generated, derived, or distributed from this software, including templates, libraries, or code - generating scripts.
//  
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED.


#include "websocket_util.h"
#include "xpb_compat.h"
#include "stdlib.h"
#include "stdio.h"


#define WS_FIN  0x80
#define WS_TEXT 0x01
#define WS_BIN  0x02
#define WS_MASK 0x80


// Quadros (codificar/decodificar) ficam em http/http_ws.c: incremental, e o quadro do
// servidor sem mascara. Os daqui mascaravam o que o servidor mandava (o RFC proibe) e
// tratavam cada recv como um quadro inteiro.

void websocket_handshake_prepare(void* args, ResourceBuffer* http)
{
	StringX* web_key = (StringX*)args;

	resource_buffer_append_string(http, "Connection: Upgrade\r\n");
	resource_buffer_append_string(http, "Upgrade: websocket\r\n");

	// GUID FIXO do RFC 6455 (secao 1.3). Nao e para ser gerado: o cliente calcula o mesmo valor
	// e confere o Sec-WebSocket-Accept. Com qualquer outro GUID todo navegador recusa a conexao.
	const char* UID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
	char nk[256];
	snprintf(nk, sizeof(nk), "%s%s", web_key->Content, UID);

	size_t ub_length = 0;   // string_utf8_to_bytes grava size_t
	byte* ub = string_utf8_to_bytes(nk, &ub_length);
	byte digest[SHA1_BLOCK_SIZE];
	sha1(ub, ub_length, digest);
	memop_free_raw(ub);
	char* base64 = string_base64_encode(digest, SHA1_BLOCK_SIZE);

	resource_buffer_append_format(http, "Sec-WebSocket-Accept: %s\r\n", base64);
	memop_free_raw(base64);
}