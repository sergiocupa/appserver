//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Decode de H.265 pelo libde265. Este arquivo e o UNICO lugar da camada de codecs que
//  inclui de265.h -- e o que permite ele virar o projeto codec_h265.
//
//  Saiu do codec_video.c, que tinha isto junto com o decode de H.264.

#include "dec_select.h"
#include "codec_parallel.h"
#include <stdio.h>
#include "de265.h"

static void* h265_dec_create(void)
{
    de265_decoder_context* decoder = de265_new_decoder();
    if (!decoder) return NULL;

    // Habilitar verificação de erros e tolerância
    de265_set_parameter_bool(decoder, DE265_DECODER_PARAM_SUPPRESS_FAULTY_PICTURES, 0);
    de265_set_parameter_bool(decoder, DE265_DECODER_PARAM_DISABLE_DEBLOCKING, 0);
    de265_set_parameter_bool(decoder, DE265_DECODER_PARAM_DISABLE_SAO, 0);

    // Threads de trabalho do libde265. Era 4 fixo, "ajustar conforme necessario" --
    // numero sem origem, que nao acompanhava nem a maquina nem a resolucao.
    //
    // Aqui vale so a regra da maquina do H.265 (1 x nucleos fisicos): o teto de geometria
    // depende da altura do quadro, que so se conhece depois do primeiro NAL. E a regra foi
    // medida no ENCODE (x265); o decode do libde265 nao foi medido, entao fica no mesmo
    // numero em vez de ganhar um palpite proprio.
    de265_start_worker_threads(decoder, codec_threads(0, codec_nucleos_fisicos(), 0));

    return decoder;
}

static ImagePlane* image_from_h265(const struct de265_image* img)
{
    uint8_t* src[3]; int st[3];
    for (int k = 0; k < 3; k++) src[k] = (uint8_t*)de265_get_image_plane(img, k, &st[k]);
    return imagep_copy_new(src, st, de265_get_image_width(img, 0), de265_get_image_height(img, 0));
}

static int h265_dec_decode(void* inst, uint_fast8_t* data, int len, ImagePlaneList* images)
{
    de265_decoder_context* deco = (de265_decoder_context*)inst;
    if (!deco || !data || len <= 0) return -1;

    de265_error err = de265_push_data(deco, data, len, 0, NULL);
    if (err != DE265_OK)
    {
        fprintf(stderr, "Erro ao enviar dados: %s\n", de265_get_error_text(err));
        return -1;
    }

    int more = 1;
    while (more)
    {
        err = de265_decode(deco, &more);

        if (err == DE265_ERROR_WAITING_FOR_INPUT_DATA)  // Precisa de mais dados
        {
            break;
        }
        else if (err != DE265_OK)
        {
            if (de265_isOK(err))
            {
                // É apenas um warning
            }
            else
            {
                fprintf(stderr, "Erro de decodificação: %s\n", de265_get_error_text(err));// Não retorna erro imediatamente, tenta pegar frames disponíveis
            }
        }

        const struct de265_image* img = de265_get_next_picture(deco);
        if (img)
        {
            enum de265_chroma chroma = de265_get_chroma_format(img);
            if (chroma == de265_chroma_420)
            {
                ImagePlane* image = image_from_h265(img);   // copia ANTES de soltar a imagem
                if (image) imagep_list_add(images, image);
            }
            else
            {
                fprintf(stderr, "AVISO: Formato chroma não suportado (%d)\n", chroma);
                de265_release_next_picture(deco);
                return -2;
            }

            de265_release_next_picture(deco);
        }
    }
    return 0;
}

static int h265_dec_flush(void* inst, ImagePlaneList* images)
{
    de265_decoder_context* d = (de265_decoder_context*)inst;
    if (!d) return -1;

    int n = 0;
    de265_flush_data(d);
    for (int guard = 0; guard < 1024; guard++)
    {
        int more = 0;
        de265_error err = de265_decode(d, &more);
        const struct de265_image* img;
        while ((img = de265_get_next_picture(d)) != 0)
        {
            if (de265_get_chroma_format(img) == de265_chroma_420)
            {
                ImagePlane* im = image_from_h265(img);
                if (im) { imagep_list_add(images, im); n++; }
            }
            de265_release_next_picture(d);
        }
        if (!more || err == DE265_ERROR_WAITING_FOR_INPUT_DATA) break;
    }

    return n;
}

static void h265_dec_release(void* inst)
{
    de265_decoder_context* d = (de265_decoder_context*)inst;
    if (d) de265_free_decoder(d);
}

const H26xDecBackend h265_dec_backend =
{
    265, "libde265",
    h265_dec_create, h265_dec_decode, h265_dec_flush, h265_dec_release
};
