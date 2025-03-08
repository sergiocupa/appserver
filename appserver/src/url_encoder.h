#ifndef URL_ENCODER_H
#define URL_ENCODER_H

#ifdef __cplusplus
extern "C" {
#endif


	char* url_decode_s(const char* src, size_t count, int* out_length);
	char* url_decode(const char* src);
	char* url_encode(const char* src);



#ifdef __cplusplus
}
#endif

#endif /* URL_ENCODER */