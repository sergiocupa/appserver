//  MIT License - Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Implementacoes vendorizadas (ver xpb_compat.h). Origem: shalib/stringlib/filelib.

#include "xpb_compat.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define XPB_ROTL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void sha1_transform(uint32_t state[5], const byte buffer[64])
{
    uint32_t W[80];
    uint32_t a, b, c, d, e, f, k, temp;

    for (int t = 0; t < 16; t++)
        W[t] = ((uint32_t)buffer[t*4]) << 24 | ((uint32_t)buffer[t*4+1]) << 16 | ((uint32_t)buffer[t*4+2]) << 8 | ((uint32_t)buffer[t*4+3]);
    for (int t = 16; t < 80; t++)
        W[t] = XPB_ROTL(W[t-3] ^ W[t-8] ^ W[t-14] ^ W[t-16], 1);

    a = state[0]; b = state[1]; c = state[2]; d = state[3]; e = state[4];
    for (int t = 0; t < 80; t++)
    {
        if      (t < 20) { f = (b & c) | ((~b) & d);            k = 0x5A827999; }
        else if (t < 40) { f = b ^ c ^ d;                       k = 0x6ED9EBA1; }
        else if (t < 60) { f = (b & c) | (b & d) | (c & d);     k = 0x8F1BBCDC; }
        else             { f = b ^ c ^ d;                       k = 0xCA62C1D6; }
        temp = XPB_ROTL(a, 5) + f + e + k + W[t];
        e = d; d = c; c = XPB_ROTL(b, 30); b = a; a = temp;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
}

void sha1_init(SHA1_CTX* context)
{
    context->count = 0;
    context->state[0] = 0x67452301; context->state[1] = 0xEFCDAB89;
    context->state[2] = 0x98BADCFE; context->state[3] = 0x10325476; context->state[4] = 0xC3D2E1F0;
}

void sha1_update(SHA1_CTX* context, const byte* data, size_t len)
{
    size_t i, j;
    j = (context->count >> 3) & 63;
    context->count += (uint64_t)len << 3;
    if ((j + len) > 63)
    {
        memcpy(&context->buffer[j], data, 64 - j);
        sha1_transform(context->state, context->buffer);
        for (i = 64 - j; i + 63 < len; i += 64) sha1_transform(context->state, data + i);
        j = 0;
    }
    else i = 0;
    memcpy(&context->buffer[j], data + i, len - i);
}

void sha1_final(byte digest[SHA1_BLOCK_SIZE], SHA1_CTX* context)
{
    unsigned char finalcount[8], pad = 0x80, zero = 0x00;
    size_t i;
    for (i = 0; i < 8; i++) finalcount[7 - i] = (unsigned char)((context->count >> (i * 8)) & 0xFF);
    sha1_update(context, &pad, 1);
    while (((context->count >> 3) & 63) != 56) sha1_update(context, &zero, 1);
    sha1_update(context, finalcount, 8);
    for (i = 0; i < 20; i++) digest[i] = (unsigned char)((context->state[i >> 2] >> ((3 - (i & 3)) * 8)) & 0xFF);
    memset(context, 0, sizeof(*context));
}

byte* sha1(const byte* data, size_t len, byte* digest)
{
    SHA1_CTX ctx; sha1_init(&ctx); sha1_update(&ctx, data, len); sha1_final(digest, &ctx);
    return digest;
}

static const char BASE64_TABLE[]     = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const int  BASE64_MOD_TABLE[] = { 0, 2, 1 };

char* string_base64_encode(const byte* data, size_t input_length)
{
    size_t output_length = 4 * ((input_length + 2) / 3);
    char* encoded = (char*)memop_alloc_raw(output_length + 1);
    if (!encoded) return NULL;
    for (size_t i = 0, j = 0; i < input_length;)
    {
        uint32_t oa = i < input_length ? data[i++] : 0;
        uint32_t ob = i < input_length ? data[i++] : 0;
        uint32_t oc = i < input_length ? data[i++] : 0;
        uint32_t triple = (oa << 16) | (ob << 8) | oc;
        encoded[j++] = BASE64_TABLE[(triple >> 18) & 0x3F];
        encoded[j++] = BASE64_TABLE[(triple >> 12) & 0x3F];
        encoded[j++] = BASE64_TABLE[(triple >> 6)  & 0x3F];
        encoded[j++] = BASE64_TABLE[triple & 0x3F];
    }
    for (int i = 0; i < BASE64_MOD_TABLE[input_length % 3]; i++)
        encoded[output_length - 1 - i] = '=';
    encoded[output_length] = '\0';
    return encoded;
}

bool file_read_bin(const char* path_file, byte** out, int* out_length)
{
    FILE* file;
    if (fopen_s(&file, path_file, "rb") != 0) return false;
    fseek(file, 0, SEEK_END);
    long leng = ftell(file);
    fseek(file, 0, SEEK_SET);
    byte* o = (byte*)memop_alloc_raw(leng > 0 ? leng : 1);
    if (!o) { fclose(file); return false; }
    size_t read = fread(o, 1, leng, file);
    fclose(file);
    if (read != (size_t)leng) { memop_free_raw(o); return false; }
    *out_length = (int)leng; *out = o;
    return true;
}

// ================= API stringlib reimplementada sobre StringX/ListX =================

StringX* string_new(void)
{
    // Mesmo alocador do yason_string_new (memory_pool), para o memop_free_raw por pool casar.
    StringX* s = (StringX*)memop_alloc_raw(sizeof(StringX));
    if (s) string_init(s);
    return s;
}

byte* string_utf8_to_bytes(const char* utf8_str, size_t* out_length)
{
    size_t n = utf8_str ? strlen(utf8_str) : 0;
    byte* b = (byte*)memop_alloc_raw(n > 0 ? n : 1);
    if (b && n) memcpy(b, utf8_str, n);
    if (out_length) *out_length = n;
    return b;
}

// O string_copy/string_appends do xplatbase NAO cresce a partir de Content==NULL (no-op).
// O stringlib antigo alocava no append. Esta ponte inicializa um alvo ainda "cru" antes
// de anexar, para os callers que anexam sem string_init previo (ex.: MessageFieldParam.Value).
static void xpb_ensure_init(StringX* s)
{
    if (s && (s->Content == NULL || s->Max == 0)) string_init(s);
}

void string_append_char(StringX* dst, const char data)
{
    char t[1]; t[0] = data;
    xpb_ensure_init(dst);
    string_appends(dst, t, 1, 0, 1);
}

void string_append_s(StringX* dst, StringX* data)
{
    if (dst && data) string_append(data, dst, 0, (int)data->Length);
}

void string_sub(const char* content, const int content_length, const int start, const int count, const int initialize, StringX* target)
{
    if (!target) return;
    if (initialize) string_init(target);
    else xpb_ensure_init(target);   // alvo pode nunca ter sido string_init (ex.: Param.Value)
    if (content && count > 0) string_appends(target, content, content_length, start, count);
}

int string_index_first(const char* data, const int data_length, const char* token, const int token_length, const int start, int* position)
{
    // 'token' e um CONJUNTO de delimitadores de 1 char. Retorna o indice (j) do
    // delimitador que casou primeiro e grava a posicao; -1 se nenhum. (semantica stringlib)
    for (int i = start; i < data_length; i++)
        for (int j = 0; j < token_length; j++)
            if (data[i] == token[j]) { if (position) *position = i; return j; }
    if (position) *position = -1;
    return -1;
}

void string_init_copy(StringX* dst, char* data, int length)
{
    if (!dst) return;
    string_init(dst);
    if (data && length > 0) string_appends(dst, data, length, 0, length);
}

int string_equals_s(StringX* s1, StringX* s2)
{
    return (s1 && s2) ? (int)string_equals(s1, s2) : 0;
}

ListX* string_array_release(ListX* ar, bool only_data)
{
    if (!ar) return 0;
    // Itens sao StringX* alocados no pool (ver string_split_param): liberar Content E a struct.
    for (uint64 i = 0; i < ar->Count; i++) { StringX* s = (StringX*)ar->Items[i]; if (s) { string_release(s); memop_free_raw(s); } }
    if (only_data) { ar->Count = 0; return ar; }   // mantem o container (pode ser embutido, ex.: Message.Route)
    list_release(&ar);                              // libera o container inteiro (alocado via list_create)
    return 0;
}

void string_release_data(StringX* ar)
{
    if (ar) string_release(ar);
}

void string_split_param(const char* content, const int length, const char* token, const int token_length, const bool remove_empty, ListX* array)
{
    if (!content || !array || token_length <= 0) return;
    int start = 0, i = 0;
    while (i <= length)
    {
        int is_tok = 0;
        if (i < length && i + token_length <= length)
        {
            is_tok = 1;
            for (int j = 0; j < token_length; j++) if (content[i+j] != token[j]) { is_tok = 0; break; }
        }
        if (i == length || is_tok)
        {
            int seglen = i - start;
            if (!(remove_empty && seglen == 0))
            {
                // ListX guarda PONTEIROS (Items[i]=instance). O item precisa viver no heap
                // do pool, nao na pilha desta funcao — senao vira ponteiro pendente e corrompe.
                StringX* seg = string_new();
                if (seg && seglen > 0) string_appends(seg, content, length, start, seglen);
                list_add(array, seg, sizeof(StringX));
            }
            if (i == length) break;
            i += token_length; start = i;
        }
        else i++;
    }
}

int string_index_of_char(const char* data, const int data_length, const char token, const int start, const int count)
{
    int end = count > 0 ? start + count : data_length;
    if (end > data_length) end = data_length;
    for (int i = start; i < end; i++) if (data[i] == token) return i;
    return -1;
}

void string_append_sub(StringX* dst, const char* data, int data_length, int data_start, int data_count)
{
    xpb_ensure_init(dst);
    if (dst && data && data_count > 0) string_appends(dst, data, data_length, data_start, data_count);
}

void string_array_init(ListX* ar)
{
    if (ar) list_init(ar, -1, sizeof(StringX));
}

int string_equals_range(StringX* s1, const int s1_start, const int s1_count, const char* s2)
{
    if (!s1 || !s2) return 0;
    for (int i = 0; i < s1_count; i++)
    {
        if (s2[i] == 0) return 0;
        if (s1_start + i >= (int)s1->Length || s1->Content[s1_start + i] != s2[i]) return 0;
    }
    return s2[s1_count] == 0 ? 1 : 0;
}

int string_equals_range_s2leng(StringX* s1, const int s1_start, const int s1_count, const char* s2, const int s2_length)
{
    if (!s1 || !s2 || s1_count != s2_length) return 0;
    for (int i = 0; i < s1_count; i++)
    {
        if (s1_start + i >= (int)s1->Length || s1->Content[s1_start + i] != s2[i]) return 0;
    }
    return 1;
}

char* string_http_url_decode_s(const char* src, size_t count, int* out_length)
{
    char* out = (char*)memop_alloc_raw(count + 1);
    if (!out) { if (out_length) *out_length = 0; return NULL; }
    size_t o = 0;
    for (size_t i = 0; i < count; i++)
    {
        char ch = src[i];
        if (ch == '%' && i + 2 < count)
        {
            char h[3] = { src[i+1], src[i+2], 0 };
            out[o++] = (char)strtol(h, NULL, 16);
            i += 2;
        }
        else if (ch == '+') out[o++] = ' ';
        else out[o++] = ch;
    }
    out[o] = '\0';
    if (out_length) *out_length = (int)o;
    return out;
}

int string_index_of(const char* data, const int data_length, const char* token, const int token_length, const int start)
{
    for (int i = start; token_length > 0 && i + token_length <= data_length; i++)
    {
        int match = 1;
        for (int j = 0; j < token_length; j++) if (data[i+j] != token[j]) { match = 0; break; }
        if (match) return i;
    }
    return -1;
}

int string_index_first_string(const char* data, const int data_length, const int data_start, const char** tokens, const int* tokens_length, const int token_count, int* position)
{
    for (int i = data_start; i < data_length; i++)
    {
        for (int t = 0; t < token_count; t++)
        {
            int tl = tokens_length[t];
            if (tl <= 0 || i + tl > data_length) continue;
            int match = 1;
            for (int j = 0; j < tl; j++) if (data[i+j] != tokens[t][j]) { match = 0; break; }
            if (match) { if (position) *position = i; return t; }
        }
    }
    if (position) *position = -1;
    return -1;
}

void string_resize_forward(StringX* content, int position)
{
    if (!content || content->Max == 0) return;
    int leng = position < (int)content->Length ? (int)content->Length - position : 0;
    if (leng > 0)
    {
        for (int ix = 0; ix < leng; ix++) content->Content[ix] = content->Content[position + ix];
        content->Length = leng;
        content->Content[leng] = 0;
    }
    else { content->Length = 0; content->Content[0] = 0; }
}
