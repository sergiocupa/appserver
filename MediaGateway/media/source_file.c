//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  source_file_open: escolhe o demux pelo CONTEUDO do arquivo.
//
//  Decide pela assinatura, nao pela extensao: arquivo renomeado e coisa corriqueira, e
//  errar o demux da uma falha obscura ("sem stream de video") em vez de um erro util.
//  A extensao entra so como desempate quando a assinatura nao diz nada.

#include "media_source.h"
#include "gw_path.h"
#include <string.h>

typedef enum { CTN_UNKNOWN = 0, CTN_MP4, CTN_WEBM } Container_;

static Container_ sniff_container(const char* path)
{
    FILE* f = gw_fopen_rb(path);
    if (!f) return CTN_UNKNOWN;

    uint8_t h[16];
    size_t n = fread(h, 1, sizeof(h), f);
    fclose(f);
    if (n < 12) return CTN_UNKNOWN;

    // Matroska/WebM: o arquivo comeca pelo elemento EBML (1A 45 DF A3).
    if (h[0] == 0x1A && h[1] == 0x45 && h[2] == 0xDF && h[3] == 0xA3) return CTN_WEBM;

    // ISO-BMFF (MP4/MOV/fMP4): 4 bytes de tamanho + 'ftyp' no offset 4.
    if (memcmp(h + 4, "ftyp", 4) == 0) return CTN_MP4;

    return CTN_UNKNOWN;
}

// Comparacao sem caso, sem depender de _stricmp/strcasecmp (nomes diferentes por
// plataforma) -- so ASCII, que e o suficiente para extensoes.
static int ext_icmp(const char* a, const char* b)
{
    for (;; a++, b++)
    {
        int ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca - cb;
        if (!ca) return 0;
    }
}

static Container_ by_extension(const char* path)
{
    const char* dot = strrchr(path, '.');
    if (!dot) return CTN_UNKNOWN;

    #define EXT_IS(lit) (ext_icmp(dot, lit) == 0)
    if (EXT_IS(".webm") || EXT_IS(".mkv")) return CTN_WEBM;
    if (EXT_IS(".mp4")  || EXT_IS(".m4v") || EXT_IS(".mov")) return CTN_MP4;
    #undef EXT_IS
    return CTN_UNKNOWN;
}

MediaSource* source_file_open(const char* path)
{
    if (!path || !*path) return 0;

    Container_ c = sniff_container(path);
    if (c == CTN_UNKNOWN) c = by_extension(path);

    if (c == CTN_WEBM) return source_webm_open(path);
    if (c == CTN_MP4)  return source_mp4_open(path);

    // Sem assinatura nem extensao reconhecida: tenta os dois antes de desistir. Custa
    // duas aberturas de arquivo num caso raro, e evita recusar um arquivo valido.
    MediaSource* s = source_mp4_open(path);
    return s ? s : source_webm_open(path);
}
