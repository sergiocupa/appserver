#include "message_parser.h"
#include "stdlib.h"
#include "string.h"


int message_parser_field(byte* data, int length, MessageFieldList* fields, int* position)
{
	int result = 0;
	int p = *position;

	while (p < length)
	{
		int o = string_index_of(data, length, "\r\n", 2, p);
		if (o >= p)
		{
			int r = string_index_first_string(data, length, (char* []) { ":", "\r\n" }, (int[]) { 1, 2 }, 2, position);

			if (r == 0)
			{ 
				MessageField* field = message_field_create(false);
				field->Name = string_sub_new(data, length, p, r - p);
				field->Content = string_split((data + r + 1), (o - r), " ", 1);
				message_field_list_add(fields, field);
				result = 1;
			}
			else if(r == 1)
			{
				if (fields->Count > 0) result = 1;
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
	return result;
}


bool message_parser_start_line(byte* data, int length, String* method, String* route, String* version, int* position)
{
	bool result = false;
	int p = *position;
	int o = string_index_of(data, length, "\r\n", 2, p);
	if (o >= p)
	{
		StringArray* parts = string_split((data + p), (o - p), " ", 1);
		if (parts->Count >= 3)
		{
			String* s0 = parts->Items[0]; String* s1 = parts->Items[1]; String* s2 = parts->Items[2];
			string_append_sub(method, s0->Data, s0->Length, 0, s0->Length);
			string_append_sub(route, s1->Data, s1->Length, 0, s1->Length);
			string_append_sub(version, s2->Data, s2->Length, 0, s2->Length);
		
			*position = o + 2;
			result = true;
		}
		else if (parts->Count == 2)
		{
			// HTTP sem rota
			String* s0 = parts->Items[0]; String* s1 = parts->Items[1];
			string_append_sub(method, s0->Data, s0->Length, 0, s0->Length);
			string_append_sub(version, s1->Data, s1->Length, 0, s1->Length);
			
			*position = o + 2;
			result = true;
		}
		else if (parts->Count == 1)
		{
			// AOTP (Asynchronous Object Transport Protocol)
			const char* ASSIGN_HEADER = "AOTP(01953aa1-75ef-7b71-960a-f3f80f6d0f87)";

			// ...
			// *position = o + 2;
			// result = true;
		}
		string_array_release(parts,false);
	}
	return result;
}


void message_buildup(MessageParser* parser, byte* data, int length)
{
	string_append(parser->Buffer, data);

	int pos = 0;
	while (pos < length)
	{
		String m, r, v;

		bool res = message_parser_start_line(data, length, &m, &r, &v, &parser->Position);
		if (res)
		{
			Message* msg = message_create();
			string_populate(&m, &msg->HttpMethod);
			string_populate( &r, &msg->Route);
			string_populate(&v, &msg->Version);

			int rf = message_parser_field(data, length, &msg->Fields, &parser->Position);
			if (rf > 0)
			{
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
