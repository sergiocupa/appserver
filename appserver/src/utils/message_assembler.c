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


//  Mensagens AOTP (protocolo de eventos sobre WebSocket). As respostas HTTP sao montadas
//  em http/http_resposta.c.

#include "message_assembler.h"
#include "yason.h"
#include "../http/http_parser.h"
#include "../http/http_resposta.h"   // http_tipo_texto
#include <string.h>


// * AOTP[9DAC10BA43E2402694C84B21A4219497]
// * Cmd: ACTION
// * Host: 123.123.123.123:4544
// * Event-ID: 4545202485224288FA9A99A9A999A9B9
// * Event-Origin-ID: 83874957477775743883739847AADA99
//   Object-Type: Poligono, VideoProgress, VideoStream
//   Content-Type: aplicacao/json, aplicacao/json, video/mp4
//   Progress-Type: integer, time, #
//   Progress: 1/10000, 2/32444, #
//   Total-Length: 430000, #
// * Content-Length: 1234, 23332, 33444
//   Content-Hash-Type: SHA1
//   Content-Hash: 1234567890, 3343444433, 3343444433

void message_assembler_prepare_aotp(MessageCommand cmd, const char* host, StringX* event_uid, StringX* event_origin_uid, ResourceBuffer** objects, int object_length, ResourceBuffer* aotp)
{
	resource_buffer_append_format(aotp, "%s\r\n", AOTP_HEADER_SIGN);
	resource_buffer_append_format(aotp, "Cmd: %s\r\n", message_command_titule(cmd));
	resource_buffer_append_format(aotp, "Host: %s\r\n", host);
	resource_buffer_append_format(aotp, "Event-ID: %s\r\n", event_uid->Content);

	if (event_origin_uid && event_origin_uid->Length > 0)
	{
		resource_buffer_append_format(aotp, "Origin-Event-ID: %s\r\n", event_origin_uid->Content);
	}

	if (object_length > 0 && objects)
	{
		ResourceBuffer type, leng;
		resource_buffer_init(&type);
		resource_buffer_init(&leng);

		int CNT = object_length - 1;
		int ix = 0;
		while (ix < CNT)
		{
			ResourceBuffer* obj = objects[ix];
			if (obj->Length > 0)
			{
				resource_buffer_append_format(&type, "%s,", http_tipo_texto(obj->Type));
				resource_buffer_append_format(&leng, "%d,", obj->Length);
			}
			ix++;
		}

		ResourceBuffer* objl = objects[ix];
		if (objl->Length > 0)
		{
			resource_buffer_append_format(&type, "%s", http_tipo_texto(objl->Type));
			resource_buffer_append_format(&leng, "%d", objl->Length);
		}

		if (leng.Length > 0)
		{
			resource_buffer_append_format(aotp, "Content-Type: %s\r\n", type.Data);
			resource_buffer_append_format(aotp, "Content-Length: %d\r\n\r\n", leng.Data);

			ix = 0;
			while (ix < CNT)
			{
				ResourceBuffer* obj = objects[ix];
				if (obj->Length > 0)
				{
					resource_buffer_append(aotp, obj->Data, obj->Length);
				}
				ix++;
			}
		}
		
		resource_buffer_release(&type, true);
		resource_buffer_release(&leng, true);
	}
}
