#ifndef MESSAGE_PARSE_H
#define MESSAGE_PARSE_H

#ifdef __cplusplus
extern "C" {
#endif

    #include "../server_type.h"



    const char* message_command_titule(MessageCommand cmd);
    int message_parser_field(byte* data, int length, MessageFieldList* fields, int* position);
    void message_buildup(MessageParser* parser, AppClientInfo* client, byte* data, int length);
    MessageParser* message_parser_create(MessageMatchReceiverCalback receiver);

    void message_field_param_add(byte* data, int begin, int end, bool first, bool is_within, MessageFieldParam* param);

#ifdef __cplusplus
}
#endif

#endif /* MESSAGE_PARSE */