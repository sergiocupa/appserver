#ifndef MESSAGE_PARSE_H
#define MESSAGE_PARSE_H

#ifdef __cplusplus
extern "C" {
#endif

    #include "../server_type.h"



    const char* message_command_titule(MessageCommand cmd);
    int message_parser_field(byte* data, int length, MessageFieldList* fields, int* position);
    void message_buildup(MessageParser* parser, AppClientInfo* client, byte* data, int length);
    MessageParser* message_parser_create(void(*match_callback) (Message*));


#ifdef __cplusplus
}
#endif

#endif /* MESSAGE_PARSE */