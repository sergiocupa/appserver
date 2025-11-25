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


#include "message_parser.h"
#include "websocket_util.h"
#include <process.h>
#include <stdlib.h>
#include <string.h>


typedef struct _ParseMatch
{
	MessageParser* Parser;
	Message*       Msg;
}
ParseMatch;


const char* message_command_titule(MessageCommand cmd)
{
	switch (cmd)
	{
	case CMD_NONE:           return "<None>";
	case CMD_GET:            return "GET";
	case CMD_OPTIONS:        return "OPTIONS";
	case CMD_POST:           return "POST";
	case CMD_ACTION:         return "ACTION";
	case CMD_CALLBACK:       return "CALLBACK";
	case CMD_ACKNOWLEDGMENT: return "ACKNOWLEDGMENT";
	}
}
const MessageCommand message_command_enum(String* cmd)
{
	if (string_equals(cmd, "ACTION"))
	{
		return CMD_ACTION;
	}
	else if(string_equals(cmd, "CALLBACK"))
	{
		return CMD_CALLBACK;
	}
	else if (string_equals(cmd, "ACKNOWLEDGMENT"))
	{
		return CMD_ACKNOWLEDGMENT;
	}
	else if (string_equals(cmd, "GET"))
	{
		return CMD_GET;
	}
	else if (string_equals(cmd, "POST"))
	{
		return CMD_POST;
	}
	else if (string_equals(cmd, "OPTIONS"))
	{
		return CMD_OPTIONS;
	}
	return CMD_NONE;
}




int message_parser_field(byte* data, int length, MessageFieldList* fields, int* position)
{
	int result = 0;
	int p = *position;
	int m = 0;
	int end = 0;

	while (p < length)
	{
		end = string_index_of(data, length, "\r\n", 2, p);
		if (end >= p)
		{
			int r = string_index_first_string(data, length, p, (char* []) { ":", "\r\n" }, (int[]) { 1, 2 }, 2, &m);

			if (r == 0)
			{ 
				MessageField* field = message_field_create(true);
				string_sub(data, length, p, m - p, false, &field->Name);

				message_field_param_add(data, (m + 1), end, true, false, &field->Param);

				message_field_list_add(fields, field);
				result = 1;
				p = end + 2;
			}
			else if(r == 1)
			{
				if (fields->Count > 0)
				{
					result = 1;
				}
				p = m +2;
				break;// fim cabecalho
			}
			else
			{
				/// erro
				result = -1;
			}
		}
		else
		{
			break;
		}
	}

	(*position) = p;
	return result;
}


void message_parser_method_param_populate(String* content, int name_begin, int name_end, int value_begin, int value_end, MessageFieldParam* param)
{
	if (name_begin >= 0)
	{
		param->Name.Data      = string_http_url_decode_s(content->Data + name_begin, name_end - name_begin, &param->Name.Length);
		param->Name.MaxLength = param->Name.Length;
	}
	if (value_begin > name_begin)
	{
		param->Value.Data      = string_http_url_decode_s(content->Data + value_begin, value_end- value_begin, &param->Value.Length);
		param->Value.MaxLength = param->Value.Length;
	}
}


void message_parser_method_param_rec(String* content, int start, MessageFieldParam* param)
{
	int m = 0;
	int r = string_index_first(content->Data, content->Length, "=&[", 3, start, &m);

	if (r == 0)
	{
		int n = string_index_of_char(content->Data, content->Length, '&', m + 1, content->Length);

		if (n > m) 
		{
			message_parser_method_param_populate(content->Data, start, m, m+1, n-1, param);

			param->Next = (MessageFieldParam*)calloc(1, sizeof(MessageFieldParam));
			message_parser_method_param_rec(content, n + 1, param->Next);
		}
		else// last
		{
			message_parser_method_param_populate(content->Data, start, m, m + 1, content->Length-1, param);
		}
	}
	else if (r == 1)
	{
		string_sub(content->Data, content->Length, start, m, false, &param->Value);

		param->Next = (MessageFieldParam*)calloc(1, sizeof(MessageFieldParam));
		message_parser_method_param_rec(content, m + 1, param->Next);
	}
	else if (r == 2)
	{
		if ((m + 4) < content->Length)
		{
			if (content->Data[m+1] == ']' && content->Data[m + 2] == '=')
			{
				int o = string_index_of_char(content->Data, content->Length, '&', m + 3, content->Length);

				if (o > m + 3)
				{
					message_parser_method_param_populate(content->Data, start, m, m + 3, o-1, param);

					param->Next = (MessageFieldParam*)calloc(1, sizeof(MessageFieldParam));
					message_parser_method_param_rec(content, o + 1, param->Next);
				}
				else// last
				{
					message_parser_method_param_populate(content->Data, start, m, m + 3, content->Length-1, param);
				}
			}
		}
	}
}


void message_parser_method_param(Message* message)
{
	if (message->Route.Count > 0)
	{
		String* data = message->Route.Items[message->Route.Count - 1];
		int start = string_index_of_char(data->Data, data->Length, '?', 0, data->Length);
		if (start > 0)
		{
			// TO-DO: erro em message_parser_method_param_rec
			//message->Param = (MessageFieldParam*)calloc(1,sizeof(MessageFieldParam));
			//message_parser_method_param_rec(data->Data, start+1, message->Param);

			data->Data[start] = '\0';
			data->Length      = start;
		}
	}
}


MessageProtocol message_parser_start_line(byte* data, int length, int* position, Message* message)
{
	MessageProtocol protocol = HTTP;
	int p = *position;
	int o = string_index_of(data, length, "\r\n", 2, p);
	if (o >= p)
	{
		StringArray* parts = string_split((data + p), (o - p), " ", 1, true);
		if (parts->Count >= 3)
		{
			String* s0 = parts->Items[0]; String* s1 = parts->Items[1]; String* s2 = parts->Items[2];

			int pos  = 0;
			int cmdp = string_index_first_string(s0->Data, s0->Length, 0, (char* []) { "GET", "POST", "OPTIONS" }, (int[]) { 3, 4, 7 }, 3, &pos);
			switch (cmdp)
			{
				case 0: message->Cmd = CMD_GET; break;
				case 1: message->Cmd = CMD_POST; break;
				case 2: message->Cmd = CMD_OPTIONS; break;
			}

			string_split_param(s1->Data, s1->Length, "/", 1, true, &message->Route);

			message_parser_method_param(message);

			string_append_sub(&message->Version, s2->Data, s2->Length, 0, s2->Length);

			*position = o + 2;
			protocol = HTTP;
			message->Protocol = protocol;
		}
		else if (parts->Count == 2)
		{
			// HTTP sem rota
			String* s0 = parts->Items[0]; String* s1 = parts->Items[1];

			int pos = 0;
			int cmdp = string_index_first_string(s0->Data, s0->Length, 0, (char* []) { "GET", "POST", "OPTIONS" }, (int[]) { 3, 4, 7 }, 3, & pos);
			switch (cmdp)
			{
			case 0: message->Cmd = CMD_GET; break;
			case 1: message->Cmd = CMD_POST; break;
			case 2: message->Cmd = CMD_OPTIONS; break;
			}

			string_append_sub(&message->Version, s1->Data, s1->Length, 0, s1->Length);
			
			*position = o + 2;
			protocol = HTTP;
			message->Protocol = protocol;
		}
		else if (parts->Count == 1)
		{
			String* s0 = parts->Items[0];

			if (string_equals(s0, AOTP_HEADER_SIGN))
			{
				protocol = AOTP;
				message->Protocol = protocol;
			}
			*position = o + 2;
		}
		string_array_release(parts,false);
	}
	return protocol;
}


void message_get_standard_header(Message* message)
{
	int ix = 0;

	while (ix < message->Fields.Count)
	{
		MessageField* field = message->Fields.Items[ix];

		if (field->Name.Length > 0 && field->Param.Value.Length > 0)
		{
			if (string_equals_range_s2leng(&field->Name, 0, 8, "Host", 8))
			{
				string_init_copy(&message->Host, field->Param.Value.Data, field->Param.Value.Length);
			}
			else if (string_equals_range_s2leng(&field->Name, 0, 10, "User-Agent", 10))
			{
				string_init_copy(&message->UserAgent, field->Param.Value.Data, field->Param.Value.Length);
			}
			else if (string_equals_range_s2leng(&field->Name, 0, 10, "Connection", 10))
			{
				if (string_equals(&field->Param.Value, "keep-alive"))
				{
					message->ConnectionOption = CONNECTION_KEEP_ALIVE;
				}
				else if (string_equals(&field->Param.Value, "close"))
				{
					message->ConnectionOption = CONNECTION_CLOSE;
				}
			}
			else if (string_equals_range_s2leng(&field->Name,  0, 8, "Content-", 8))
			{
				if (string_equals_range_s2leng(&field->Name, 8, 6, "Length", 6))
				{
					int error = 0;
					int num = numeric_parse_int(field->Param.Value.Data, &error);

					if (!error)
					{
						message->ContentLength = num;
					}
				}
				else if (string_equals_range_s2leng(&field->Name, 8, 4, "Type", 4))
				{
					if (string_equals_range_s2leng(&field->Param.Value, 0, 5, "text/", 5))
					{
						if (string_equals_range_s2leng(&field->Param.Value, 5, 4, "html", 4))
						{
							message->ContentType = TEXT_HTML;
						}
						else if (string_equals_range_s2leng(&field->Param.Value, 5, 3, "css",3))
						{
							message->ContentType = TEXT_CSS;
						}
						else if (string_equals_range_s2leng(&field->Param.Value, 5, 10, "javascript",10))
						{
							message->ContentType = TEXT_JAVASCRIPT;
						}
						else if (string_equals_range_s2leng(&field->Param.Value, 5, 5, "plain", 5))
						{
							message->ContentType = TEXT_PLAIN;
						}
					}
					else if (string_equals_range_s2leng(&field->Param.Value, 0, 12, "application/", 12))
					{
						if (string_equals_range_s2leng(&field->Param.Value, 12, 10, "javascript", 10))
						{
							message->ContentType = APPLICATION_JAVASCRIPT;
						}
						else if (string_equals_range_s2leng(&field->Param.Value, 12, 4, "json", 4))
						{
							message->ContentType = APPLICATION_JSON;
						}
						else if (string_equals_range_s2leng(&field->Param.Value, 12, 3, "xml", 3))
						{
							message->ContentType = APPLICATION_XML;
						}
						else if (string_equals_range_s2leng(&field->Param.Value, 12, 3, "pdf", 3))
						{
							message->ContentType = APPLICATION_PDF;
						}
						else if (string_equals_range_s2leng(&field->Param.Value, 12, 4, "gzip", 4))
						{
							message->ContentType = APPLICATION_GZIP;
						}
						else if (string_equals_range_s2leng(&field->Param.Value, 12, 3, "zip", 3))
						{
							message->ContentType = APPLICATION_ZIP;
						}
						else if (string_equals_range_s2leng(&field->Param.Value, 12, 12, "octet-stream", 12))
						{
							message->ContentType = APPLICATION_OCTET_STREAM;
						}
					}
					else if (string_equals_range_s2leng(&field->Param.Value, 0, 6, "image/", 6))
					{
						if (string_equals_range_s2leng(&field->Param.Value, 6, 4, "jpeg", 4))
						{
							message->ContentType = IMAGE_JPEG;
						}
						else if (string_equals_range_s2leng(&field->Param.Value, 6, 3, "png", 3))
						{
							message->ContentType = IMAGE_PNG;
						}
						else if (string_equals_range_s2leng(&field->Param.Value, 6, 3, "gif", 3))
						{
							message->ContentType = IMAGE_GIF;
						}
					}
					else if (string_equals_range_s2leng(&field->Param.Value, 0, 6,"audio/", 6))
					{
						if (string_equals_range_s2leng(&field->Param.Value, 6, 4, "mpeg", 4))
						{
							message->ContentType = AUDIO_MPEG;
						}
						else if (string_equals_range_s2leng(&field->Param.Value, 6, 3, "ogg", 3))
						{
							message->ContentType = AUDIO_OGG;
						}
					}
					else if (string_equals_range_s2leng(&field->Param.Value, 0, 6, "video/", 6))
					{
						if (string_equals_range_s2leng(&field->Param.Value, 6, 3, "mp4", 3))
						{
							message->ContentType = VIDEO_MP4;
						}
						else if (string_equals_range_s2leng(&field->Param.Value, 6, 4, "webm", 4))
						{
							message->ContentType = VIDEO_WEBM;
						}
					}
					else if (string_equals_range_s2leng(&field->Param.Value, 0, 10, "multipart/", 10))
					{
						if (string_equals_range_s2leng(&field->Param.Value, 10, 9, "form-data", 9))
						{
							message->ContentType = MULTIPART_FORMDATA;
						}
					}
				}
			}
			else if (string_equals_range_s2leng(&field->Name, 0, 3, "Cmd", 3))
			{
				message->Cmd = message_command_enum(&field->Param.Value);
		    }
			else if (string_equals_range_s2leng(&field->Name, 0, 4, "Sec-", 4))
			{
				if (string_equals_range_s2leng(&field->Name, 4, 13, "WebSocket-Key", 13))
				{
					string_init_copy(&message->SecWebsocketKey, field->Param.Value.Data, field->Param.Value.Length);
				}
				else if (string_equals_range_s2leng(&field->Name, 4, 16, "WebSocket-Accept", 16))
				{
					string_init_copy(&message->SecWebsocketAccept, field->Param.Value.Data, field->Param.Value.Length);
				}
			}
			else if (string_equals_range_s2leng(&field->Name, 0, 7, "Upgrade", 7))
			{
				string_init_copy(&message->Upgrade, field->Param.Value.Data, field->Param.Value.Length);
	        }
		}
		ix++;
	}
}


void on_message_match(void* ptr)
{
	ParseMatch* msg = (ParseMatch*)ptr;
	msg->Parser->MessageMatch(msg->Msg);
	message_release(msg->Msg);
	free(msg);
}


void create_on_message_match(MessageParser* parser, Message* msg, AppClientInfo* client)
{
	msg->Client = client;
	ParseMatch* pmatch = malloc(sizeof(ParseMatch));
	pmatch->Parser = parser;
	pmatch->Msg = msg;
	msg->MatchThread = _beginthread(on_message_match, 0, (void*)pmatch);
}


void message_buildup(MessageParser* parser, AppClientInfo* client, byte* data, int length)
{
	if (client->IsWebSocketMode)
	{
		byte* op = 0; size_t decoded_leng = 0;
		byte* decoded = websocket_decode_frame(data, length, &op, &decoded_leng);

		if (decoded_leng > 0)
		{
			string_append(parser->Buffer, decoded);
		}
	}
	else
	{
		string_append(parser->Buffer, data);
	}

	int pos = 0;
	while (pos < length && parser->Buffer->Length > 0)
	{
		if (parser->Partial != 0)
		{
			if (parser->Buffer->Length == parser->Partial->ContentLength)
			{
				parser->Partial->IsMatch = true;

				string_sub(parser->Buffer->Data, parser->Buffer->Length, 0, parser->Partial->ContentLength, true, &parser->Partial->Content);

				parser->Position = 0;
			    string_resize_forward(parser->Buffer, parser->Position + parser->Partial->ContentLength);

				create_on_message_match(parser, parser->Partial, client);
				parser->Partial = 0;
				break;
			}
			else if (parser->Buffer->Length > parser->Partial->ContentLength)
			{
				parser->Partial->IsMatch = true;

				string_sub(parser->Buffer->Data, parser->Buffer->Length, 0, parser->Partial->ContentLength, true, &parser->Partial->Content);

				parser->Position = 0;
			    string_resize_forward(parser->Buffer, parser->Partial->ContentLength);

				create_on_message_match(parser, parser->Partial, client);
				parser->Partial = 0;
			}
			else break;
		}

		Message* msg = message_create();

		MessageProtocol res = message_parser_start_line(data, length, &parser->Position, msg);
		if (res)
		{
			int rf = message_parser_field(data, length, &msg->Fields, &parser->Position);
			if (rf > 0)
			{
				message_get_standard_header(msg);

				if (msg->ContentLength > 0)
				{
					int st = parser->Buffer->Length - parser->Position;

					if (st >= msg->ContentLength)// conteudo completo
					{
						msg->IsMatch = true;

						string_sub(parser->Buffer->Data, parser->Buffer->Length, parser->Position, msg->ContentLength, true, &msg->Content);

					    string_resize_forward(parser->Buffer, parser->Position + msg->ContentLength);
						parser->Position = 0;

						create_on_message_match(parser, msg, client);
					}
					else// partial
					{
						parser->Partial = msg;

					    string_resize_forward(parser->Buffer, parser->Position);
						parser->Position = 0;
						break;
					}
				}
				else
				{
					msg->IsMatch    = true;
					parser->Partial = 0;
				    string_resize_forward(parser->Buffer, parser->Position);
					parser->Position = 0;

					create_on_message_match(parser, msg, client);
				}

				// remover resto do buffer
				// se nao foi possivel construir cabecalhos, entao sair para que proxima chegado de dados, acumular mais no buffer
			}
		}
		pos++;
	}
}


MessageParser* message_parser_create(MessageMatchReceiverCalback receiver)
{
	MessageParser* parser = (MessageParser*)malloc(sizeof(MessageParser));
	memset(parser, 0, sizeof(MessageParser));

	parser->Buffer        = string_new();
	parser->MessageMatch  = receiver;

	return parser;
}


MessageParser* message_parser_release(MessageParser* parser)
{
	string_release(parser->Buffer);
	free(parser);
	return 0;
}
