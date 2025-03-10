#ifndef CLIENT_ASSEMBLER_H
#define CLIENT_ASSEMBLER_H

#ifdef __cplusplus
extern "C" {
#endif

    #include "server_type.h"

	
	void message_assembler_prepare(HttpStatusCode http_status, const char* agent, const char* host, MessageField** headers, int header_length, ContentTypeOption content_type, int length, String* content);


#ifdef __cplusplus
}
#endif

#endif /* CLIENT_ASSEMBLER */