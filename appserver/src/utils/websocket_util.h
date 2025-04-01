#ifndef WEBSOCKET_UTIL_H
#define WEBSOCKET_UTIL_H

#ifdef __cplusplus
extern "C" {
#endif

    #include "../server_type.h"


    byte* websocket_encode_text(const byte* message, size_t* output_length);
    byte* websocket_decode_frame(const byte* buffer, size_t buffer_len, byte* opcode, size_t* output_length);
    byte* websocket_encode_frame(const byte* data, size_t data_len, byte opcode, int use_mask, size_t* output_length);


#ifdef __cplusplus
}
#endif

#endif /* WEBSOCKET_UTIL */