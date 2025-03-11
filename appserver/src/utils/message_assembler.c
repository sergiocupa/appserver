#include "message_assembler.h"
#include "yason.h"


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
}


String* message_write_content(void* object, ContentTypeOption content_type)
{
	if (object)
	{
		if (content_type == APPLICATION_JSON)
		{
			return yason_render((Element*)object, 1);
		}
	}
	return 0;
}


void message_assembler_prepare(HttpStatusCode http_status, const char* agent, const char* host, MessageField** headers, int header_length, ContentTypeOption content_type, void* object, String* http)
{
	string_append_format(http, "HTTP/1.1 %d %s\r\n", (int)http_status, message_assembler_append_http_status(http_status));
	string_append_format(http, "Host: %s\r\n", host);
	string_append_format(http, "User-Agent: %s\r\n", agent);
	string_append(http, "Access-Control-Allow-Origin: *\r\n");

	int ix = 0;
	while (ix < header_length)
	{
		MessageField* header = headers[ix];
		string_append_format(http, "%s: %s\r\n", header->Name.Data, header->Param.Value.Data);
		ix++;
	}

	string_append_format(http, "Content-Type: %s\r\n", message_assembler_append_content_type(content_type));

	int leng = 0;
	String* obj_content = message_write_content(object, content_type);
	if (obj_content && obj_content->Length > 0)
	{
		leng = obj_content->Length;
		string_append_format(http, "Content-Length: %d\r\n\r\n", leng);
		string_append_s(http, obj_content);
		string_release(obj_content);
	}
	else
	{
		string_append_format(http, "Content-Length: %d\r\n\r\n", leng);
	}
}