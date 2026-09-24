//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Lista de imagens decodificadas e a copia de planos. NAO e de codec nenhum: quem produz
//  ImagePlane e o backend de decode (openh264, de265, ...), mas a lista e a copia sao as
//  mesmas para todos. Por isso ficam no nucleo, e nao junto de um codec.
//
//  Saiu do codec_video.c, que misturava isto com o decode de H.264 e o de H.265.

#include "memory_pool.h"
#include "string_handler.h"
#include "codec_video.h"
#include "dec_select.h"

// ---- Lista de imagens (self-contained; veio do MediaFragmenterType.c) ------
#define IMAGEP_LIST_INIT 4

void imagep_list_init(ImagePlaneList* list, int init_count)
{
    list->Max   = init_count < 0 ? IMAGEP_LIST_INIT : init_count;
    list->Count = 0;
    list->Items = (ImagePlane**)memop_alloc_raw((uint64)((uint64)list->Max * sizeof(ImagePlane*)));
}

ImagePlaneList* imagep_list_new(int init_count)
{
    ImagePlaneList* list = (ImagePlaneList*)memop_alloc_raw((uint64)sizeof(ImagePlaneList));
    imagep_list_init(list, init_count);
    return list;
}

void imagep_list_add(ImagePlaneList* list, ImagePlane* image)
{
    if (list->Count >= list->Max)
    {
        int nm = list->Max > 0 ? list->Max * 2 : IMAGEP_LIST_INIT;
        list->Items = (ImagePlane**)memop_realloc_raw(list->Items, (uint64)((uint64)nm * sizeof(ImagePlane*)));
        list->Max = nm;
    }
    list->Items[list->Count++] = image;
}

void imagep_list_release(ImagePlaneList** list, int free_items)
{
    if (!list || !*list) return;
    // free_items era IGNORADO: cada frame decodificado aloca uma ImagePlane (ver os
    // backends de decode) e nenhuma era liberada -- um vazamento por frame, em todos
    // os chamadores (todos passam 1). Os PLANOS em si pertencem ao decoder; aqui so sai
    // a struct descritora.
    if (free_items)
        for (int i = 0; i < (*list)->Count; i++) memop_free_raw((*list)->Items[i]);
    memop_free_raw((*list)->Items);
    memop_free_raw(*list);
    *list = 0;
}

// Copia o frame para memoria PROPRIA: struct + pixels num bloco so, entao liberar a
// ImagePlane (imagep_list_release / gw_decode) libera tudo. Os planos que openh264 e
// libde265 devolvem pertencem ao DECODER:
//  - o libde265 roda com 4 threads de trabalho, e este codigo soltava a imagem
//    (de265_release_next_picture) logo depois de guardar o ponteiro -- uma thread podia
//    reescrever o buffer enquanto o gateway ainda o lia;
//  - no flush saem varios frames seguidos, e cada chamada ao decoder pode reaproveitar o
//    buffer da anterior.
// Custa um memcpy por frame, pequeno perto do encode que vem depois.
ImagePlane* imagep_copy_new(uint8_t* const src[3], const int stride[3], int w, int h)
{
    if (w <= 0 || h <= 0 || !src[0] || !src[1] || !src[2]) return 0;
    const int cw = (w + 1) / 2, ch = (h + 1) / 2;
    const size_t ys = (size_t)w * h, cs = (size_t)cw * ch;

    ImagePlane* im = (ImagePlane*)memop_alloc_raw((uint64)(sizeof(ImagePlane) + ys + 2 * cs));
    if (!im) return 0;
    uint8_t* base = (uint8_t*)(im + 1);

    im->Planes[0] = (uint_fast8_t*)base;
    im->Planes[1] = (uint_fast8_t*)(base + ys);
    im->Planes[2] = (uint_fast8_t*)(base + ys + cs);
    im->Strides[0] = w; im->Strides[1] = cw; im->Strides[2] = cw;
    im->Width = w; im->Height = h;
    im->Sizes[0] = ys; im->Sizes[1] = cs; im->Sizes[2] = cs;
    im->Size = ys + 2 * cs;

    for (int y = 0; y < h; y++)  memcpy(base + (size_t)y * w,            src[0] + (size_t)y * stride[0], (size_t)w);
    for (int y = 0; y < ch; y++) memcpy(base + ys + (size_t)y * cw,      src[1] + (size_t)y * stride[1], (size_t)cw);
    for (int y = 0; y < ch; y++) memcpy(base + ys + cs + (size_t)y * cw, src[2] + (size_t)y * stride[2], (size_t)cw);
    return im;
}
