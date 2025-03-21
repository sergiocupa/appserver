#include "websocket_util.h"
#include "shalib.h"
#include "stdlib.h"
#include "stdio.h"


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