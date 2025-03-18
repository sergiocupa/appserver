#ifndef CLIENT_ASSEMBLER_H
#define CLIENT_ASSEMBLER_H

#ifdef __cplusplus
extern "C" {
#endif

    #include "../server_type.h"


	const char* message_assembler_append_http_status(HttpStatusCode http_satus);
	const char* message_assembler_append_content_type(ContentTypeOption content_type);
	void message_assembler_prepare(HttpStatusCode http_status, const char* agent, const char* host, MessageField** headers, int header_length, ResourceBuffer* object, ResourceBuffer* http, int cid);


#ifdef __cplusplus
}
#endif

#endif /* CLIENT_ASSEMBLER */