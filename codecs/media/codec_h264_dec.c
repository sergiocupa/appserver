//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Decode de H.264 pelo openh264 (Cisco). Este arquivo e o UNICO lugar da camada de codecs
//  que inclui os headers do openh264 no caminho de decode -- e o que permite ele virar o
//  projeto codec_h264.
//
//  Saiu do codec_video.c, que tinha isto junto com o decode de H.265 e com os utilitarios
//  de lista de imagens.

#include "dec_select.h"
#include "memory_pool.h"
#include "wels/codec_app_def.h"
#include "wels/codec_api.h"

static void* h264_dec_create(void)
{
    ISVCDecoder* decoder = NULL;

    int rv = WelsCreateDecoder(&decoder);
    if (rv != 0 || !decoder)
    {
        // Nao ha memop_free_raw aqui: quem aloca o ISVCDecoder e o openh264, e soltar um
        // ponteiro dele pelo pool era corrupcao esperando acontecer. (Estava assim no
        // codec_video.c, nos dois caminhos de falha.)
        if (decoder) WelsDestroyDecoder(decoder);
        return NULL;
    }

    SDecodingParam param;
    memop_zero_raw(&param, sizeof(SDecodingParam));
    param.uiTargetDqLayer = 0xFF;      // Todas as camadas
    param.eEcActiveIdc = ERROR_CON_DISABLE;
    param.bParseOnly = false;
    param.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;

    int rsv = (*decoder)->Initialize(decoder, &param);
    if (rsv != 0)
    {
        (*decoder)->Uninitialize(decoder);
        WelsDestroyDecoder(decoder);
        return NULL;
    }

    return decoder;
}

static ImagePlane* image_from_h264(const SBufferInfo* info, unsigned char* planes[3])
{
    uint8_t* src[3] = { planes[0], planes[1], planes[2] };
    const int st[3] = { info->UsrData.sSystemBuffer.iStride[0],
                        info->UsrData.sSystemBuffer.iStride[1],
                        info->UsrData.sSystemBuffer.iStride[1] };   // U e V compartilham o stride
    return imagep_copy_new(src, st, info->UsrData.sSystemBuffer.iWidth, info->UsrData.sSystemBuffer.iHeight);
}

static int h264_dec_decode(void* inst, uint_fast8_t* data, int len, ImagePlaneList* images)
{
    ISVCDecoder* decoder = (ISVCDecoder*)inst;
    if (!decoder || !data || len <= 0) return -1;

    int frames_decoded = 0;
    unsigned char* planes[3] = { NULL, NULL, NULL };
    SBufferInfo info;
    memop_zero_raw(&info, sizeof(SBufferInfo));

    // Feed inicial com dados reais
    DECODING_STATE state = (*decoder)->DecodeFrame2(decoder, data, len, planes, &info);

    if (state != dsErrorFree && state != dsFramePending)
    {
        return -1;
    }

    while (1)
    {
        // Se tem frame disponível, salva
        if (info.iBufferStatus == 1 && planes[0] && planes[1] && planes[2])
        {
            ImagePlane* image = image_from_h264(&info, planes);
            if (image)
            {
                imagep_list_add(images, image);
                frames_decoded++;
            }
        }

        // Flush para buscar próximo frame
        memop_zero_raw(&info, sizeof(SBufferInfo));
        planes[0] = planes[1] = planes[2] = NULL;
        state = (*decoder)->DecodeFrame2(decoder, NULL, 0, planes, &info);

        // Condição de saída: não há mais frames E não houve erro
        if (info.iBufferStatus == 0)
        {
            break;  // Buffer vazio, nada mais para extrair
        }

        // Se erro, também sai
        if (state != dsErrorFree)
        {
            break;
        }
    }

    return frames_decoded;
}

static int h264_dec_flush(void* inst, ImagePlaneList* images)
{
    ISVCDecoder* d = (ISVCDecoder*)inst;
    if (!d) return -1;

    int n = 0;
    int eos = 1;
    (*d)->SetOption(d, DECODER_OPTION_END_OF_STREAM, &eos);

    // FlushFrame entrega um frame por chamada enquanto houver na fila de reordenacao.
    for (int guard = 0; guard < 64; guard++)
    {
        int remaining = 0;
        (*d)->GetOption(d, DECODER_OPTION_NUM_OF_FRAMES_REMAINING_IN_BUFFER, &remaining);
        if (remaining <= 0) break;

        unsigned char* planes[3] = { 0, 0, 0 };
        SBufferInfo info; memop_zero_raw(&info, sizeof(info));
        (*d)->FlushFrame(d, planes, &info);
        if (info.iBufferStatus != 1 || !planes[0] || !planes[1] || !planes[2]) break;
        ImagePlane* im = image_from_h264(&info, planes);
        if (im) { imagep_list_add(images, im); n++; }
    }

    // Drain antigo (DecodeFrame2 sem dados, com END_OF_STREAM ligado): cobre o que a
    // contagem acima nao pega em versoes que nao a mantem.
    for (int guard = 0; guard < 64; guard++)
    {
        unsigned char* planes[3] = { 0, 0, 0 };
        SBufferInfo info; memop_zero_raw(&info, sizeof(info));
        (*d)->DecodeFrame2(d, NULL, 0, planes, &info);
        if (info.iBufferStatus != 1 || !planes[0] || !planes[1] || !planes[2]) break;
        ImagePlane* im = image_from_h264(&info, planes);
        if (im) { imagep_list_add(images, im); n++; }
    }

    return n;
}

static void h264_dec_release(void* inst)
{
    ISVCDecoder* d = (ISVCDecoder*)inst;
    if (!d) return;
    (*d)->Uninitialize(d);
    WelsDestroyDecoder(d);
}

const H26xDecBackend h264_dec_backend =
{
    264, "openh264",
    h264_dec_create, h264_dec_decode, h264_dec_flush, h264_dec_release
};
