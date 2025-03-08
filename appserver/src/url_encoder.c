#include "url_encoder.h"
#include <stdlib.h>
#include <string.h>


static int hex_to_int(char c) 
{
    if (c >= '0' && c <= '9')
        return c - '0';
    else if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    else if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}


char* url_decode(const char* src)
{
    char* decoded = malloc(strlen(src) + 1);

    if (!decoded) return 0;

    char* dst = decoded;

    while (*src) 
    {
        if (*src == '%')
        {
            // Verifica se há pelo menos dois caracteres após '%'
            if (*(src + 1) && *(src + 2)) 
            {
                int high = hex_to_int(*(src + 1));
                int low  = hex_to_int(*(src + 2));

                if (high >= 0 && low >= 0) 
                {
                    *dst++ = (char)(high * 16 + low);
                    src += 3;
                    continue;
                }
            }
        }
        else if (*src == '+')
        {
            // O caractere '+' representa um espaço na URL
            *dst++ = ' ';
            src++;
            continue;
        }

        *dst++ = *src++; // Copia o caractere normal
    }
    *dst = '\0';
    return decoded;
}

char* url_decode_s(const char* src, size_t count, int* out_length) 
{
    char* decoded = malloc(count + 1);

    if (!decoded) return NULL;

    size_t i = 0;
    size_t j = 0; 

    while (i < count && src[i] != '\0') 
    {
        if (src[i] == '%') 
        {
            if (i + 2 < count && src[i + 1] != '\0' && src[i + 2] != '\0')
            {
                int high = hex_to_int(src[i + 1]);
                int low  = hex_to_int(src[i + 2]);

                if (high >= 0 && low >= 0) 
                {
                    decoded[j++] = (char)(high * 16 + low);
                    i += 3;
                    continue;
                }
            }
            // Se não for uma sequência válida, copia o '%' literalmente
            decoded[j++] = src[i++];
        }
        else if (src[i] == '+') 
        {
            // O sinal '+' representa um espaço
            decoded[j++] = ' ';
            i++;
        }
        else 
        {
            decoded[j++] = src[i++];
        }
    }
    decoded[j] = '\0';
    return decoded;
}


char* url_encode(const char* src) 
{
    size_t len = strlen(src);
    char* encoded = malloc(3 * len + 1);
    if (!encoded) return NULL;

    char* dst = encoded;
    while (*src)
    {
        if (isalnum((unsigned char)*src) || *src == '-' || *src == '_' || *src == '.' || *src == '~')
        {
            *dst++ = *src;
        }
        // O espaço é convertido para '+'
        else if (*src == ' ')
        {
            *dst++ = '+';
        }
        else 
        {
            sprintf(dst, "%%%02X", (unsigned char)*src);
            dst += 3;
        }
        src++;
    }
    *dst = '\0';
    return encoded;
}