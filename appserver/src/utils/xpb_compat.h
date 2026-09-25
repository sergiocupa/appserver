//  MIT License - Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Compat local: funcoes que a suite 'utility' (removida como submodulo) fornecia e
//  que o xplatbase/yason_compat ainda nao cobrem — vendorizadas aqui:
//    - SHA1 (era shalib)           -> handshake WebSocket
//    - string_base64_encode (era stringlib)
//    - file_read_bin (era filelib) -> file_read_text/write_text vem do yason_compat;
//                                     numeric_* e list/string vem do xplatbase.

#ifndef XPB_COMPAT_H
#define XPB_COMPAT_H

#ifdef __cplusplus
extern "C" {
#endif

    #include "xplatbase.h"
    #include <stdbool.h>
    #include <stddef.h>
    #include <string.h>

    // Compara StringX com uma C-string literal. NECESSARIO porque o xplatbase mudou
    // string_equals para (StringX*, StringX*): passar um char* literal ali o reinterpreta
    // como StringX* e corrompe/crasha. O antigo stringlib usava (StringX*, const char*).
    static inline boolean string_equals_c(StringX* s, const char* c)
    {
        // O return abaixo foi apagado por engano no commit d0aa10e ("removido nao
        // usados mais"): a funcao ficou com corpo VAZIO. Sem return, o valor devolvido
        // e lixo do registrador, e quem chamava passou a decidir por acaso.
        // Derrubou todo arquivo estatico do servidor (o guarda de ".." em
        // binder_get_web_resource rejeitava qualquer rota com segmento), e ainda
        // alcanca o parser de HTTP, que compara metodo e Connection por aqui.
        // O string_equal do xplatbase nunca saiu de la; so a chamada se perdeu.
        return string_equal(s, c, c ? (int)strlen(c) : 0);
    }

    // ---- SHA1 (FIPS PUB 180-1) ----
    typedef struct { uint32_t state[5]; uint64_t count; byte buffer[64]; } SHA1_CTX;
    #define SHA1_BLOCK_SIZE 20

    void  sha1_init(SHA1_CTX* context);
    void  sha1_update(SHA1_CTX* context, const byte* data, size_t len);
    void  sha1_final(byte digest[SHA1_BLOCK_SIZE], SHA1_CTX* context);
    byte* sha1(const byte* data, size_t len, byte* digest);

    // ---- base64 (buffer alocado com memop_alloc_raw; chamador libera com memop_free_raw) ----
    char* string_base64_encode(const byte* data, size_t input_length);

    // ---- leitura binaria de arquivo (buffer memop_alloc_raw; chamador libera com memop_free_raw) ----
    bool  file_read_bin(const char* path_file, byte** out, int* out_length);

    // ---- API stringlib rica reimplementada sobre StringX/ListX (era stringlib) ----
    // Mesmos nomes que o appserver ja chama; agora operam em StringX (nao no String antigo).
    StringX* string_new(void);
    byte*    string_utf8_to_bytes(const char* utf8_str, size_t* out_length);
    void     string_append_char(StringX* dst, const char data);
    void     string_append_s(StringX* dst, StringX* data);
    void     string_sub(const char* content, const int content_length, const int start, const int count, const int initialize, StringX* target);
    int      string_index_first(const char* data, const int data_length, const char* token, const int token_length, const int start, int* position);
    void     string_init_copy(StringX* dst, char* data, int length);
    int      string_equals_s(StringX* s1, StringX* s2);
    ListX*   string_array_release(ListX* ar, bool only_data);
    void     string_release_data(StringX* ar);
    void     string_split_param(const char* content, const int length, const char* token, const int token_length, const bool remove_empty, ListX* array);
    int      string_index_of_char(const char* data, const int data_length, const char token, const int start, const int count);
    void     string_append_sub(StringX* dst, const char* data, int data_length, int data_start, int data_count);
    void     string_array_init(ListX* ar);
    int      string_equals_range(StringX* s1, const int s1_start, const int s1_count, const char* s2);
    int      string_equals_range_s2leng(StringX* s1, const int s1_start, const int s1_count, const char* s2, const int s2_length);
    char*    string_http_url_decode_s(const char* src, size_t count, int* out_length);
    int      string_index_first_string(const char* data, const int data_length, const int data_start, const char** tokens, const int* tokens_length, const int token_count, int* position);
    int      string_index_of(const char* data, const int data_length, const char* token, const int token_length, const int start);
    void     string_resize_forward(StringX* content, int position);

#ifdef __cplusplus
}
#endif

#endif /* XPB_COMPAT_H */
