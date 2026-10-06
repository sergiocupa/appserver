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


// fopen/snprintf padrao C: o SDL do MSVC os trata como erro (C4996); o codigo e portavel.
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "http_parser.h"
#include "../../include/appserver.h"   // app_corpo_* (API publica implementada aqui)
#include "../utils/net_compat.h"   // getpid/_getpid via process.h / unistd.h
#include "atomics.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifdef _WIN32
  #include <windows.h>
  #define http_getpid() ((int)GetCurrentProcessId())
#else
  #define http_getpid() ((int)getpid())
#endif



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
	case CMD_HEAD:           return "HEAD";
	case CMD_PUT:            return "PUT";
	case CMD_DELETE:         return "DELETE";
	case CMD_PATCH:          return "PATCH";
	}
	// Sem isto o valor de retorno era lixo de registrador para qualquer cmd fora da lista --
	// a mesma classe de defeito do string_equals_c, que derrubou os arquivos estaticos.
	// Quem chama faz printf("%s") com o resultado: lixo aqui e ponteiro invalido la.
	return "<desconhecido>";
}
const MessageCommand message_command_enum(StringX* cmd)
{
	if (string_equals_c(cmd, "ACTION"))
	{
		return CMD_ACTION;
	}
	else if(string_equals_c(cmd, "CALLBACK"))
	{
		return CMD_CALLBACK;
	}
	else if (string_equals_c(cmd, "ACKNOWLEDGMENT"))
	{
		return CMD_ACKNOWLEDGMENT;
	}
	else if (string_equals_c(cmd, "GET"))
	{
		return CMD_GET;
	}
	else if (string_equals_c(cmd, "POST"))
	{
		return CMD_POST;
	}
	else if (string_equals_c(cmd, "OPTIONS"))
	{
		return CMD_OPTIONS;
	}
	else if (string_equals_c(cmd, "HEAD"))   return CMD_HEAD;
	else if (string_equals_c(cmd, "PUT"))    return CMD_PUT;
	else if (string_equals_c(cmd, "DELETE")) return CMD_DELETE;
	else if (string_equals_c(cmd, "PATCH"))  return CMD_PATCH;
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
			int r = string_index_first_string(data, length, p, (const char* []) { ":", "\r\n" }, (int[]) { 1, 2 }, 2, &m);

			if (r == 0)
			{ 
				MessageField* field = message_field_create(true);
				string_sub(data, length, p, m - p, false, &field->Name);

				// Valor CRU: os parametros abaixo quebram o valor em nome=valor; cabecalhos como
				// Sec-WebSocket-Key (base64, com "=" de padding) precisam dele intacto.
				{
					int vb = m + 1, ve = end;
					while (vb < ve && (data[vb] == ' ' || data[vb] == '\t')) vb++;
					while (ve > vb && (data[ve - 1] == ' ' || data[ve - 1] == '\t')) ve--;
					string_sub(data, length, vb, ve - vb, false, &field->Raw);
				}

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








// Decodifica [ini, fim) de 'src' (percent-encoding e '+') para dentro de 'dst'.
static void query_decode_into(const char* src, int ini, int fim, StringX* dst)
{
	int n = 0;
	char* d = string_http_url_decode_s(src + ini, (size_t)(fim > ini ? fim - ini : 0), &n);
	dst->Content = d;
	dst->Length  = (uint64)(n > 0 ? n : 0);
	dst->Max     = dst->Length + 1;   // + 1: vazio tambem tem buffer, e precisa ser liberado
	dst->Active  = true;
}

// Separa "rota?a=1&b=2" em rota ("rota") e parametros em message->Param, na ordem.
// Antes a query era cortada e JOGADA FORA: nenhum handler recebia parametro nenhum.
void message_parser_method_param(Message* message)
{
	if (message->Route.Count <= 0) return;

	StringX* ultimo = message->Route.Items[message->Route.Count - 1];
	int q = string_index_of_char(ultimo->Content, (int)ultimo->Length, '?', 0, (int)ultimo->Length);
	if (q < 0) return;

	const char* c = ultimo->Content;
	int total = (int)ultimo->Length;
	MessageFieldParam** fim_lista = &message->Param;

	int i = q + 1;
	while (i < total)
	{
		int amp = string_index_of_char(c, total, '&', i, total);
		int fp  = amp >= 0 ? amp : total;
		if (fp > i)   // ignora "&&" e "&" final
		{
			int eq = string_index_of_char(c, total, '=', i, fp);
			if (eq < 0 || eq > fp) eq = -1;

			MessageFieldParam* p = (MessageFieldParam*)memop_calloc_raw(1, sizeof(MessageFieldParam));
			if (!p) break;
			query_decode_into(c, i, eq >= 0 ? eq : fp, &p->Name);
			if (eq >= 0) query_decode_into(c, eq + 1, fp, &p->Value);
			p->IsEndParam = (amp < 0);
			*fim_lista = p;
			fim_lista = &p->Next;
		}
		if (amp < 0) break;
		i = amp + 1;
	}

	// a rota em si nao carrega a query
	ultimo->Content[q] = '\0';
	ultimo->Length     = (uint64)q;

	// "/api/x/?a=1": o ultimo segmento era so a query e ficou vazio
	if (ultimo->Length == 0 && message->Route.Count > 0)
	{
		string_release_data(ultimo);
		memop_free_raw(ultimo);
		message->Route.Count--;
	}
}


MessageProtocol message_parser_start_line(byte* data, int length, int* position, Message* message)
{
	MessageProtocol protocol = HTTP;
	int p = *position;
	int o = string_index_of(data, length, "\r\n", 2, p);
	if (o >= p)
	{
		ListX* parts = string_split_cstr((data + p), (o - p), (char)0x20);
		if (parts->Count >= 3)
		{
			StringX* s0 = parts->Items[0]; StringX* s1 = parts->Items[1]; StringX* s2 = parts->Items[2];

			message->Cmd = message_command_enum(s0);

			string_split_param(s1->Content, s1->Length, "/", 1, true, &message->Route);

			message_parser_method_param(message);

			string_append_sub(&message->Version, s2->Content, s2->Length, 0, s2->Length);

			*position = o + 2;
			protocol = HTTP;
			message->Protocol = protocol;
		}
		else if (parts->Count == 2)
		{
			// HTTP sem rota
			StringX* s0 = parts->Items[0]; StringX* s1 = parts->Items[1];

			message->Cmd = message_command_enum(s0);

			string_append_sub(&message->Version, s1->Content, s1->Length, 0, s1->Length);
			
			*position = o + 2;
			protocol = HTTP;
			message->Protocol = protocol;
		}
		else if (parts->Count == 1)
		{
			StringX* s0 = parts->Items[0];

			if (string_equals_c(s0, AOTP_HEADER_SIGN))
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


// Os campos de WebSocket do Message sao PONTEIROS. O codigo antigo passava o endereco do
// ponteiro para string_init_copy, que escrevia uma StringX inteira (32 bytes) em cima dele e
// dos campos vizinhos. Aqui a string e alocada de verdade; message_release a libera.
static void message_set_string(StringX** field, char* data, int length)
{
    if (*field) { string_release_data(*field); memop_free_raw(*field); }
    *field = (StringX*)memop_calloc_raw(1, sizeof(StringX));
    if (*field) string_init_copy(*field, data, length);
}

static int64 parse_content_length(const StringX* v)
{
	if (!v || !v->Content || v->Length == 0) return -1;
	int64 n = 0;
	uint64 i = 0;
	while (i < v->Length && (v->Content[i] == ' ' || v->Content[i] == '\t')) i++;
	if (i >= v->Length) return -1;
	for (; i < v->Length; i++)
	{
		char ch = v->Content[i];
		if (ch == ' ' || ch == '\t' || ch == '\r') break;
		if (ch < '0' || ch > '9') return -1;
		if (n > (INT64_MAX - 9) / 10) return -1;   // estouro
		n = n * 10 + (ch - '0');
	}
	return n;
}


void message_get_standard_header(Message* message)
{
	int ix = 0;

	while (ix < message->Fields.Count)
	{
		MessageField* field = message->Fields.Items[ix];

		// Sec-WebSocket-Key/Accept pelo valor CRU e ANTES do teste de Param.Value: a quebra em
		// parametros deixava so o "=" final da chave base64 (e uma chave sem "=" nem entraria).
		if (field->Raw.Length > 0 && field->Name.Length > 4 && string_equals_range_s2leng(&field->Name, 0, 4, "Sec-", 4))
		{
			if (string_equals_range_s2leng(&field->Name, 4, 13, "WebSocket-Key", 13))
				message_set_string(&message->SecWebsocketKey, field->Raw.Content, (int)field->Raw.Length);
			else if (string_equals_range_s2leng(&field->Name, 4, 16, "WebSocket-Accept", 16))
				message_set_string(&message->SecWebsocketAccept, field->Raw.Content, (int)field->Raw.Length);
		}

		if (field->Name.Length > 0 && field->Param.Value.Length > 0)
		{
			if (string_equals_range_s2leng(&field->Name, 0, 8, "Host", 8))
			{
				string_init_copy(&message->Host, field->Param.Value.Content, field->Param.Value.Length);
			}
			else if (string_equals_range_s2leng(&field->Name, 0, 10, "User-Agent", 10))
			{
				string_init_copy(&message->UserAgent, field->Param.Value.Content, field->Param.Value.Length);
			}
			else if (string_equals_range_s2leng(&field->Name, 0, 10, "Connection", 10))
			{
				if (string_equals_c(&field->Param.Value, "keep-alive"))
				{
					message->ConnectionOption = CONNECTION_KEEP_ALIVE;
				}
				else if (string_equals_c(&field->Param.Value, "close"))
				{
					message->ConnectionOption = CONNECTION_CLOSE;
				}
			}
			else if (string_equals_range_s2leng(&field->Name,  0, 8, "Content-", 8))
			{
				if (string_equals_range_s2leng(&field->Name, 8, 6, "Length", 6))
				{
					// 64 bits e so digitos. Sinal, lixo ou estouro viram -1, que o buildup
					// devolve como 400 -- antes o valor invalido era simplesmente ignorado e a
					// requisicao seguia como se nao tivesse corpo.
					message->ContentLength = parse_content_length(&field->Param.Value);
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
			else if (string_equals_range_s2leng(&field->Name, 0, 7, "Upgrade", 7))
			{
				message_set_string(&message->Upgrade, field->Param.Value.Content, (int)field->Param.Value.Length);
	        }
		}
		ix++;
	}
}











// =========================================================================================
//  Parser incremental
// =========================================================================================

struct HttpParser
{
	HttpParserLimites Lim;
	char              TempDir[1024];
	HttpAoMensagem    AoMensagem;
	HttpAoErro        AoErro;
	void*             Ctx;
	StringX*          Buffer;      // cabecalhos (e o que vier atras deles) ainda nao consumidos
	Message*          Partial;     // requisicao com o corpo chegando
	FILE*             Arquivo;     // corpo de Partial indo para disco (acima de BodyToDiskBytes)
	int64             Recebido;    // bytes do corpo de Partial ja recebidos
	bool              Erro;
};

HttpParserLimites http_parser_limites(const AppServerConfig* cfg)
{
	HttpParserLimites l;
	l.MaxHeaderBytes  = cfg ? cfg->MaxHeaderBytes  : 64 * 1024;
	l.MaxBodyBytes    = cfg ? cfg->MaxBodyBytes    : (int64)1024 * 1024 * 1024;
	l.BodyToDiskBytes = cfg ? cfg->BodyToDiskBytes : (int64)8 * 1024 * 1024;
	l.TempDir         = cfg ? cfg->TempDir : 0;
	return l;
}

HttpParser* http_parser_criar(const HttpParserLimites* lim, HttpAoMensagem ao_mensagem, HttpAoErro ao_erro, void* ctx)
{
	HttpParser* p = (HttpParser*)memop_calloc_raw(1, sizeof(HttpParser));
	if (!p) return 0;
	p->Lim = *lim;
	snprintf(p->TempDir, sizeof(p->TempDir), "%s", (lim->TempDir && lim->TempDir[0]) ? lim->TempDir : ".");
	p->Lim.TempDir = p->TempDir;
	p->AoMensagem  = ao_mensagem;
	p->AoErro      = ao_erro;
	p->Ctx         = ctx;
	p->Buffer      = string_new();
	return p;
}

// Solta a requisicao pela metade; o temporario dela (se houver) e apagado por message_release.
static void descarta_parcial(HttpParser* p)
{
	if (p->Arquivo) { fclose(p->Arquivo); p->Arquivo = 0; }
	if (p->Partial) { message_release(p->Partial); p->Partial = 0; }
	p->Recebido = 0;
}

void http_parser_destruir(HttpParser* p)
{
	if (!p) return;
	descarta_parcial(p);
	// A StringX do buffer veio de string_new (struct no pool): liberar o conteudo E a struct.
	if (p->Buffer) { string_release(p->Buffer); memop_free_raw(p->Buffer); }
	memop_free_raw(p);
}

bool http_parser_ocupado(const HttpParser* p)
{
	return p && (p->Partial != 0 || (p->Buffer && p->Buffer->Length > 0));
}

const char* http_parser_arquivo_parcial(const HttpParser* p)
{
	return (p && p->Partial && p->Arquivo) ? app_corpo_arquivo(p->Partial) : 0;
}

static void falha(HttpParser* p, HttpStatusCode st)
{
	if (p->Erro) return;
	p->Erro = true;
	descarta_parcial(p);
	p->Buffer->Length = 0;
	if (p->Buffer->Content) p->Buffer->Content[0] = 0;
	if (p->AoErro) p->AoErro(p->Ctx, st);
}

// Nome unico do temporario: pid + sequencia do processo. Dois servidores no mesmo TempDir
// (ou duas execucoes seguidas) nao colidem.
static xatomic_int g_seq_temp;

static bool abre_temporario(HttpParser* p, Message* msg)
{
	char cam[1200];
	int seq = atomic_add_inline(&g_seq_temp, 1) + 1;
	snprintf(cam, sizeof(cam), "%s/appsrv_%d_%d.corpo", p->TempDir, http_getpid(), seq);
	p->Arquivo = fopen(cam, "wb");
	if (!p->Arquivo) return false;
	// Buffer grande: recv entrega pedacos de poucos KB; gravar cada um direto no disco custa
	// uma chamada ao sistema por pedaco.
	setvbuf(p->Arquivo, 0, _IOFBF, 256 * 1024);
	string_init(&msg->ContentFile);
	string_appends(&msg->ContentFile, cam, (int)strlen(cam), 0, (int)strlen(cam));
	return true;
}

// Corpo em memoria: o tamanho e conhecido (Content-Length), entao o buffer nasce do tamanho
// final e os pedacos sao copiados direto. Crescer aos poucos (dobrando) realocava e recopiava o
// corpo a cada dobra, e cada realocacao grande ia ao sistema (VirtualAlloc/VirtualFree):
// medido, ~40% da CPU de um POST de 1 MB.
static bool corpo_reserva(StringX* s, int64 cl)
{
	if (s->Content && s->Max >= (uint64)cl) return true;
	char* c = (char*)memop_realloc_raw(s->Content, (size_t)cl + 1);
	if (!c) return false;
	if (!s->Content) { s->Length = 0; c[0] = 0; }
	s->Content = c;
	s->Max     = (uint64)cl;
	s->Active  = true;
	return true;
}

static bool corpo_acrescenta(HttpParser* p, const byte* d, int n)
{
	if (n <= 0) return true;
	if (p->Arquivo)
	{
		if (fwrite(d, 1, (size_t)n, p->Arquivo) != (size_t)n) return false;
	}
	else
	{
		StringX* s = &p->Partial->Content;
		if (s->Length + (uint64)n <= s->Max)
		{
			memcpy(s->Content + s->Length, d, (size_t)n);
			s->Length += (uint64)n;
			s->Content[s->Length] = 0;
		}
		else string_append_sub(s, (const char*)d, n, 0, n);   // nao acontece: o parser nao passa do Content-Length
	}
	p->Recebido += n;
	return true;
}

static bool corpo_completo(HttpParser* p)
{
	Message* msg = p->Partial;
	if (p->Arquivo)
	{
		// fclose descarrega o buffer: disco cheio aparece AQUI, nao no fwrite
		int ok = fclose(p->Arquivo) == 0;
		p->Arquivo = 0;
		if (!ok) return false;
	}
	p->Partial = 0;
	p->Recebido = 0;
	msg->IsMatch = true;
	p->AoMensagem(p->Ctx, msg);
	return true;
}

// Consome do inicio de (d, n) o que falta do corpo em andamento. Devolve quantos usou, ou -1.
static int consome_corpo(HttpParser* p, const byte* d, int n)
{
	int64 falta = p->Partial->ContentLength - p->Recebido;
	int usa = (int64)n < falta ? n : (int)falta;
	if (!corpo_acrescenta(p, d, usa)) { falha(p, HTTP_STATUS_INTERNAL_ERROR); return -1; }
	if (p->Recebido >= p->Partial->ContentLength && !corpo_completo(p)) { falha(p, HTTP_STATUS_INTERNAL_ERROR); return -1; }
	return usa;
}

// Junta os bytes recebidos e entrega cada requisicao COMPLETA.
//
// Nada e analisado ate o "\r\n\r\n" estar no buffer, e tudo sai do buffer. O corpo em
// andamento NAO passa pelo buffer: vai direto do pedaco recebido para a mensagem (ou disco).
bool http_parser_alimentar(HttpParser* p, const byte* dados, int n)
{
	if (p->Erro) return false;

	/* 1) corpo em andamento: direto do pedaco recebido, sem copiar para o buffer */
	if (p->Partial && p->Buffer->Length == 0)
	{
		int usa = consome_corpo(p, dados, n);
		if (usa < 0) return false;
		dados += usa; n -= usa;
		if (n == 0) return true;
	}

	/* 2) acumula */
	if (n > 0) string_append_sub(p->Buffer, (const char*)dados, n, 0, n);

	/* 3) entrega toda requisicao que ja esteja inteira no buffer */
	for (;;)
	{
		StringX* buf = p->Buffer;

		if (p->Partial)   /* corpo que comecou a chegar junto com os cabecalhos */
		{
			int usa = consome_corpo(p, (const byte*)buf->Content, (int)buf->Length);
			if (usa < 0) return false;
			string_resize_forward(buf, usa);
			if (p->Partial) break;   /* corpo ainda incompleto: o resto vem no proximo recv */
			continue;
		}

		if (buf->Length == 0) break;

		int fim = string_index_of(buf->Content, (int)buf->Length, "\r\n\r\n", 4, 0);
		if (fim < 0)
		{
			if ((int64)buf->Length > p->Lim.MaxHeaderBytes) { falha(p, HTTP_STATUS_HEADERS_TOO_LARGE); return false; }
			break;   /* cabecalho ainda incompleto: espera o proximo recv */
		}
		int hlen = fim + 4;
		if ((int64)hlen > p->Lim.MaxHeaderBytes) { falha(p, HTTP_STATUS_HEADERS_TOO_LARGE); return false; }

		Message* msg = message_create();
		int pos = 0;
		message_parser_start_line((byte*)buf->Content, hlen, &pos, msg);
		if (pos > 0) message_parser_field((byte*)buf->Content, hlen, &msg->Fields, &pos);
		message_get_standard_header(msg);
		string_resize_forward(buf, hlen);   /* consome o bloco de cabecalhos */

		if (pos == 0)
		{
			message_release(msg);
			falha(p, HTTP_STATUS_BAD_REQUEST);
			return false;
		}

		int64 cl = msg->ContentLength;
		// sem o cabecalho, ContentLength fica 0 (message_create zera); so -1 e invalido
		if (cl < 0 || cl > p->Lim.MaxBodyBytes)
		{
			message_release(msg);
			falha(p, cl < 0 ? HTTP_STATUS_BAD_REQUEST : HTTP_STATUS_PAYLOAD_TOO_LARGE);
			return false;
		}

		if (cl > 0)
		{
			p->Partial  = msg;
			p->Recebido = 0;
			if (cl > p->Lim.BodyToDiskBytes)
			{
				if (!abre_temporario(p, msg)) { falha(p, HTTP_STATUS_INTERNAL_ERROR); return false; }
			}
			else
			{
				if (!corpo_reserva(&msg->Content, cl)) { falha(p, HTTP_STATUS_INTERNAL_ERROR); return false; }
			}
			continue;   /* o laco consome do buffer o que ja chegou do corpo */
		}

		msg->IsMatch = true;
		p->AoMensagem(p->Ctx, msg);
	}
	return !p->Erro;
}


// =========================================================================================
//  Corpo da requisicao para a aplicacao (appserver.h)
// =========================================================================================

const char* app_corpo_arquivo(Message* m)
{
	return (m && m->ContentFile.Content && m->ContentFile.Length > 0) ? m->ContentFile.Content : 0;
}

static bool copia_arquivo(const char* de, const char* para)
{
	FILE* a = fopen(de, "rb");
	if (!a) return false;
	FILE* b = fopen(para, "wb");
	if (!b) { fclose(a); return false; }
	char* buf = (char*)malloc(256 * 1024);
	bool ok = buf != 0;
	size_t n;
	while (ok && (n = fread(buf, 1, 256 * 1024, a)) > 0) ok = fwrite(buf, 1, n, b) == n;
	free(buf);
	fclose(a);
	if (fclose(b) != 0) ok = false;
	if (!ok) remove(para);
	return ok;
}

bool app_corpo_salvar(Message* m, const char* destino)
{
	if (!m || !destino) return false;
	const char* tmp = app_corpo_arquivo(m);
	if (tmp)
	{
		// Mover e barato (mesmo volume: so renomeia). Em outro volume o SO copia.
		bool ok;
#ifdef _WIN32
		ok = MoveFileExA(tmp, destino, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) != 0;
#else
		remove(destino);
		ok = rename(tmp, destino) == 0;
		if (!ok && errno == EXDEV && copia_arquivo(tmp, destino)) { remove(tmp); ok = true; }
#endif
		if (ok) { m->ContentFile.Length = 0; m->ContentFile.Content[0] = 0; }   // nao e mais nosso
		return ok;
	}
	FILE* f = fopen(destino, "wb");
	if (!f) return false;
	size_t n = (m->Content.Content && m->Content.Length > 0) ? (size_t)m->Content.Length : 0;
	bool ok = n == 0 || fwrite(m->Content.Content, 1, n, f) == n;
	if (fclose(f) != 0) ok = false;
	if (!ok) remove(destino);
	return ok;
}
