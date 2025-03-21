#include "message_assembler.h"
#include "yason.h"
#include <string.h>


const char* message_assembler_append_http_status(HttpStatusCode http_satus)
{
	switch (http_satus)
	{
	case HTTP_STATUS_OK:                  return "OK";
	case HTTP_STATUS_ACCEPT:              return "Accepted";
	case HTTP_STATUS_BAD_REQUEST:         return "Bad Request";
	case HTTP_STATUS_UNAUTHORIZED:        return "Unauthorized";
	case HTTP_STATUS_FORBIDDEN:           return "Forbidden";
	case HTTP_STATUS_NOT_FOUND:           return "Not Found";
	case HTTP_STATUS_PRECONDITION_FAILED: return "Precondition Failed";
	case HTTP_STATUS_INTERNAL_ERROR:      return "Internal Server Error";
	case HTTP_STATUS_NOT_IMPLEMENTED:     return "Not implemented";
	case HTTP_STATUS_SERVICE_UNAVAILABLE: return "Service Unavailable";
	case HTTP_STATUS_SWITCHING_PROTOCOLS: return "Switching Protocols";
	}
	return HTTP_STATUS_INTERNAL_ERROR;
}

const char* message_assembler_append_content_type(ContentTypeOption content_type)
{
	switch (content_type)
	{
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
	}

	return "<unknown>";
}


void message_assembler_prepare(HttpStatusCode http_status, const char* agent, const char* host, HeaderAppender header_appender, void* appender_args, ResourceBuffer* object, ResourceBuffer* http, int cid)
{
	const char* es = message_assembler_append_http_status(http_status);

	resource_buffer_append_format(http, "HTTP/1.1 %d %s\r\n", (int)http_status, es);
	resource_buffer_append_format(http, "Host: %s\r\n", host);

	/*byte* buffer = 0; int leng = 0;
	string_utf8_to_bytes(agent, (byte**)&buffer, &leng);
	resource_buffer_append_string(http, "User-Agent: ");
	resource_buffer_append(http, buffer, leng);
	resource_buffer_append_string(http, "\r\n");
	free(buffer);*/

	resource_buffer_append_format(http, "User-Agent: %s\r\n", agent);
	resource_buffer_append_string(http, "Access-Control-Allow-Origin: *\r\n");

	if (header_appender)
	{
		header_appender(appender_args,http);
	}

	if (object && object->Length > 0)
	{
		const char* desc_type = message_assembler_append_content_type(object->Type);

		resource_buffer_append_format(http, "Content-Type: %s\r\n", desc_type);

		resource_buffer_append_format(http, "Content-Length: %d\r\n\r\n", object->Length);
		resource_buffer_append(http, object->Data, object->Length);

		printf("RESPONSE | Client: %d | Status: %s | Type: %s | Content Length: %d\n", cid, es, desc_type, object->Length);
	}
	else
	{
		resource_buffer_append_string(http, "Content-Length: 0\r\n\r\n");

		printf("RESPONSE | Client: %d | Status: %s | Type: | Content Length: 0\n", cid, es);
	}
}



