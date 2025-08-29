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