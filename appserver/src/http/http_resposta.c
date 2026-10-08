//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Formatador unico de respostas HTTP. Ver http_resposta.h.

// fopen/snprintf padrao C: o SDL do MSVC os trata como erro (C4996); o codigo e portavel.
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "http_resposta.h"
#include <stdio.h>
#include <string.h>

const char* http_status_texto(HttpStatusCode st)
{
	switch (st)
	{
	case HTTP_STATUS_NONE:                break;
	case HTTP_STATUS_OK:                  return "OK";
	case HTTP_STATUS_ACCEPT:              return "Accepted";
	case HTTP_STATUS_PARTIAL_CONTENT:     return "Partial Content";
	case HTTP_STATUS_RANGE_NOT_SATISFIABLE: return "Range Not Satisfiable";
	case HTTP_STATUS_BAD_REQUEST:         return "Bad Request";
	case HTTP_STATUS_UNAUTHORIZED:        return "Unauthorized";
	case HTTP_STATUS_FORBIDDEN:           return "Forbidden";
	case HTTP_STATUS_NOT_FOUND:           return "Not Found";
	case HTTP_STATUS_METHOD_NOT_ALLOWED:  return "Method Not Allowed";
	case HTTP_STATUS_PRECONDITION_FAILED: return "Precondition Failed";
	case HTTP_STATUS_PAYLOAD_TOO_LARGE:   return "Payload Too Large";
	case HTTP_STATUS_HEADERS_TOO_LARGE:   return "Request Header Fields Too Large";
	case HTTP_STATUS_INTERNAL_ERROR:      return "Internal Server Error";
	case HTTP_STATUS_NOT_IMPLEMENTED:     return "Not Implemented";
	case HTTP_STATUS_SERVICE_UNAVAILABLE: return "Service Unavailable";
	case HTTP_STATUS_SWITCHING_PROTOCOLS: return "Switching Protocols";
	}
	return "Internal Server Error";
}

const char* http_tipo_texto(ContentTypeOption t)
{
	switch (t)
	{
	case CONTENT_TYPE_NONE:        break;
	case TEXT_HTML:                return "text/html";
	case TEXT_CSS:                 return "text/css";
	case TEXT_JAVASCRIPT:          return "text/javascript";
	case TEXT_PLAIN:               return "text/plain";
	case APPLICATION_JAVASCRIPT:   return "application/javascript";
	case APPLICATION_JSON:         return "application/json";
	case APPLICATION_XML:          return "application/xml";
	case APPLICATION_OCTET_STREAM: return "application/octet-stream";
	case APPLICATION_PDF:          return "application/pdf";
	case APPLICATION_ZIP:          return "application/zip";
	case APPLICATION_GZIP:         return "application/gzip";
	case IMAGE_JPEG:               return "image/jpeg";
	case IMAGE_PNG:                return "image/png";
	case IMAGE_GIF:                return "image/gif";
	case IMAGE_SVG:                return "image/svg+xml";
	case AUDIO_MPEG:               return "audio/mpeg";
	case AUDIO_OGG:                return "audio/ogg";
	case VIDEO_MP4:                return "video/mp4";
	case VIDEO_WEBM:               return "video/webm";
	case MULTIPART_FORMDATA:       return "multipart/form-data";
	case APPLICATION_MPEGURL:      return "application/vnd.apple.mpegurl";
	case VIDEO_MP2T:               return "video/mp2t";
	}
	return "application/octet-stream";
}

HttpCabecalho http_cabecalho(HttpStatusCode st, const char* servidor)
{
	HttpCabecalho c;
	memset(&c, 0, sizeof(c));
	c.Status   = st;
	c.Servidor = servidor;
	c.Tipo     = CONTENT_TYPE_NONE;
	c.Tamanho  = 0;
	return c;
}

void http_resposta_cabecalho(ResourceBuffer* out, const HttpCabecalho* c)
{
	resource_buffer_append_format(out, "HTTP/1.1 %d %s\r\n", (int)c->Status, http_status_texto(c->Status));
	if (c->Servidor && c->Servidor[0]) resource_buffer_append_format(out, "Server: %s\r\n", c->Servidor);
	// O front e servido de outras origens em desenvolvimento (e o painel de saude tambem).
	resource_buffer_append_string(out, "Access-Control-Allow-Origin: *\r\n");
	if (c->Extra) c->Extra(c->ExtraArgs, out);
	if (c->Tipo != CONTENT_TYPE_NONE) resource_buffer_append_format(out, "Content-Type: %s\r\n", http_tipo_texto(c->Tipo));
	if (c->AceitaFaixa) resource_buffer_append_string(out, "Accept-Ranges: bytes\r\n");
	if (c->Status == HTTP_STATUS_PARTIAL_CONTENT)
		resource_buffer_append_format(out, "Content-Range: bytes %lld-%lld/%lld\r\n", (long long)c->FaixaIni, (long long)c->FaixaFim, (long long)c->FaixaTotal);
	else if (c->Status == HTTP_STATUS_RANGE_NOT_SATISFIABLE)
		resource_buffer_append_format(out, "Content-Range: bytes */%lld\r\n", (long long)c->FaixaTotal);
	if (c->Tamanho >= 0) resource_buffer_append_format(out, "Content-Length: %lld\r\n", (long long)c->Tamanho);
	if (c->Fechar) resource_buffer_append_string(out, "Connection: close\r\n");
	resource_buffer_append_string(out, "\r\n");
}

void http_resposta_montar(ResourceBuffer* out, HttpCabecalho c, const byte* corpo, int64 n, bool com_corpo)
{
	if (!corpo) n = 0;
	c.Tamanho = n;
	if (n == 0 && c.Tipo != CONTENT_TYPE_NONE) c.Tipo = CONTENT_TYPE_NONE;   // corpo vazio: sem tipo, como antes
	http_resposta_cabecalho(out, &c);
	if (com_corpo && n > 0) resource_buffer_append(out, (byte*)corpo, (int)n);
}

int http_resposta_recusa(char* out, int cap, HttpStatusCode st)
{
	int n = snprintf(out, (size_t)cap,
		"HTTP/1.1 %d %s\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
		(int)st, http_status_texto(st));
	return (n > 0 && n < cap) ? n : 0;
}

void http_resposta_sse(ResourceBuffer* out, const char* servidor)
{
	HttpCabecalho c = http_cabecalho(HTTP_STATUS_OK, servidor);
	c.Tamanho = -1;   // fluxo aberto: termina quando a conexao fecha
	resource_buffer_append_format(out, "HTTP/1.1 200 OK\r\n");
	if (c.Servidor && c.Servidor[0]) resource_buffer_append_format(out, "Server: %s\r\n", c.Servidor);
	resource_buffer_append_string(out,
		"Access-Control-Allow-Origin: *\r\n"
		"Content-Type: text/event-stream\r\n"
		"Cache-Control: no-cache\r\n"
		"Connection: keep-alive\r\n"
		"X-Accel-Buffering: no\r\n"   // proxy (nginx) na frente nao pode segurar os eventos
		"\r\n");
}
