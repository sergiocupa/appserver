#ifndef CLIENT_ASSEMBLER_H
#define CLIENT_ASSEMBLER_H

#ifdef __cplusplus
extern "C" {
#endif

    #include "../server_type.h"


	const char* message_assembler_append_http_status(HttpStatusCode http_satus);
	const char* message_assembler_append_content_type(ContentTypeOption content_type);
	void message_assembler_prepare(HttpStatusCode http_status, const char* agent, const char* host, HeaderAppender header_appender, void* appender_args, ResourceBuffer* object, ResourceBuffer* http, int cid);
	void message_assembler_prepare_aotp(MessageCommand cmd, const char* host, String* action_uid, String* origin_uid, ResourceBuffer** objects, int object_length, ResourceBuffer* aotp);


#ifdef __cplusplus
}
#endif

#endif /* CLIENT_ASSEMBLER */