#include "message_parser.h"
#include "numeric.h"
#include "stdlib.h"
#include "string.h"
#include <process.h>


const char* AOTP_HEADER_SIGN = "AOTP[9DAC10BA43E2402694C84B21A4219497]";


typedef struct _ParseMatch
{
	MessageParser* Parser;
	Message*       Msg;
}
ParseMatch;


int message_parser_field(byte* data, int length, MessageFieldList* fields, int* position)
{
	int result = 0;
	int p = *position;

	while (p < length)
	{
		int o = string_index_of(data, length, "\r\n", 2, p);
		if (o >= p)
		{
			int r = string_index_first_string(data, length, (o+2), (char* []) { ":", "\r\n" }, (int[]) { 1, 2 }, 2, &p);

			if (r == 0)
			{ 
				MessageField* field = message_field_create(true);
				string_sub(data, length, p, r - p, false, &field->Name);
				string_split_param((data + r + 1), (o - r), " ", 1, &field->Content);
				message_field_list_add(fields, field);
				result = 1;
			}
			else if(r == 1)
			{
				if (fields->Count > 0)
				{
					result = 1;
				}
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


MessageProtocol message_parser_start_line(byte* data, int length, int* position, Message* message)
{
	MessageProtocol protocol = HTTP;
	int p = *position;
	int o = string_index_of(data, length, "\r\n", 2, p);
	if (o >= p)
	{
		StringArray* parts = string_split((data + p), (o - p), " ", 1);
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

			string_split_param(s1->Data, s1->Length, " ", 1, &message->Route);
			string_append_sub(&message->Version, s2->Data, s2->Length, 0, s2->Length);

			*position = o + 2;
			protocol = HTTP;
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
		}
		else if (parts->Count == 1)
		{
			String* s0 = parts->Items[0];

			if (string_equals(s0, AOTP_HEADER_SIGN))
			{
				protocol = AOTP;
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

		if (field->Content.Count > 0)
		{
			if (string_equals(&field->Name, "Content-Length"))
			{
				int error = 0;
				int num = numeric_parse_int(field->Content.Items[0]->Data, &error);

				if (!error)
				{
					message->ContentLength = num;
				}
			}
		}
		ix++;
	}
}


void on_message_match(void* ptr)
{
	ParseMatch* msg = (ParseMatch*)ptr;
	msg->Parser->MatchCallback(msg->Msg);
	message_release(msg->Msg);
}


void create_on_message_match(MessageParser* parser, Message* msg, AppClientInfo* client)
{
	msg->Client = client;
	ParseMatch pmatch;
	pmatch.Parser = parser;
	pmatch.Msg = msg;
	msg->MatchThread = _beginthread(on_message_match, 0, (void*)&pmatch);
}


void message_buildup(MessageParser* parser, AppClientInfo* client, byte* data, int length)
{
	string_append(parser->Buffer, data);

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
			    string_resize_forward(parser->Buffer, parser->Partial->ContentLength);

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

		bool res = message_parser_start_line(data, length, &parser->Position, msg);
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

					    string_resize_forward(parser->Buffer, parser->Position);
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
				}

				// remover resto do buffer
				// se nao foi possivel construir cabecalhos, entao sair para que proxima chegado de dados, acumular mais no buffer
			}
		}
		pos++;
	}
}


MessageParser* message_parser_create(void(*match_callback) (Message*))
{
	MessageParser* parser = (MessageParser*)malloc(sizeof(MessageParser));
	memset(parser, 0, sizeof(MessageParser));

	parser->Buffer        = string_new();
	parser->MatchCallback = match_callback;

	return parser;
}


MessageParser* message_parser_release(MessageParser* parser)
{
	string_release(parser->Buffer);
	free(parser);
	return 0;
}
