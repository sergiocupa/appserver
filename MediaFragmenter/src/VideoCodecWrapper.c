#include "H264Builder.h";
#include "H265Builder.h";
#include "../include/MediaFragmenter.h"



static ISVCDecoder* h264_decoder_create()
{
    ISVCDecoder* decoder = NULL;

    int rv = WelsCreateDecoder(&decoder);
    if (rv != 0 || !decoder)
    {
        free(decoder);
        return NULL;
    }

    SDecodingParam param;
    memset(&param, 0, sizeof(SDecodingParam));
    param.uiTargetDqLayer = 0xFF;      // Todas as camadas
    param.eEcActiveIdc = ERROR_CON_DISABLE;
    param.bParseOnly = false;
    param.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;

    int rsv = (*decoder)->Initialize(decoder, &param);
    if (rsv != 0)
    {
        (*decoder)->Uninitialize(decoder);
        WelsDestroyDecoder(decoder);
        free(decoder);
        return NULL;
    }

    return decoder;
}

static de265_decoder_context* h265_decoder_create()
{
    de265_decoder_context* decoder = de265_new_decoder();

    // Configurar decoder
    // Habilitar verificação de erros e tolerância
    de265_set_parameter_bool(decoder, DE265_DECODER_PARAM_SUPPRESS_FAULTY_PICTURES, 0);
    de265_set_parameter_bool(decoder, DE265_DECODER_PARAM_DISABLE_DEBLOCKING, 0);
    de265_set_parameter_bool(decoder, DE265_DECODER_PARAM_DISABLE_SAO, 0);

    // Usar múltiplas threads se disponível
    int num_threads = 4;  // Ajustar conforme necessário
    de265_start_worker_threads(decoder, num_threads);

    return decoder;
}




static void populate_from_h265_image(const struct de265_image* source, ImagePlane* output)
{
    output->Planes[0] = de265_get_image_plane(source, 0, &output->Strides[0]);
    output->Planes[1] = de265_get_image_plane(source, 1, &output->Strides[1]);
    output->Planes[2] = de265_get_image_plane(source, 2, &output->Strides[2]);
    output->Width     = de265_get_image_width(source, 0);
    output->Height    = de265_get_image_height(source, 0);
    output->Sizes[0]  = output->Strides[0] * output->Height;
    output->Sizes[1]  = output->Strides[1] * output->Height;
    output->Sizes[2]  = output->Strides[2] * output->Height;
    output->Size      = output->Sizes[0] + output->Sizes[1] + output->Sizes[2];
}

static void populate_from_h264_image(SBufferInfo* info, unsigned char* planes[3], ImagePlane* output)
{
    output->Planes[0]  = planes[0];
    output->Planes[1]  = planes[1];
    output->Planes[2]  = planes[2];
    output->Strides[0] = info->UsrData.sSystemBuffer.iStride[0];
    output->Strides[1] = info->UsrData.sSystemBuffer.iStride[1];
    output->Strides[2] = info->UsrData.sSystemBuffer.iStride[2];
    output->Width      = info->UsrData.sSystemBuffer.iWidth;
    output->Height     = info->UsrData.sSystemBuffer.iHeight;
    output->Sizes[0]   = output->Strides[0] * output->Height;
    output->Sizes[1]   = output->Strides[1] * output->Height;
    output->Sizes[2]   = output->Strides[2] * output->Height;
    output->Size       = output->Sizes[0] + output->Sizes[1] + output->Sizes[2];;
}




static int put_decode_h265(de265_decoder_context* deco, uint_fast8_t* data, int len, ImagePlaneList* images)
{
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
                ImagePlane* image = malloc(sizeof(ImagePlane*));
                populate_from_h265_image(img,image);
                imagep_list_add(images, image);

                //h265_video_output_show(out, img);
            }
            else
            {
                fprintf(stderr, "AVISO: Formato chroma não suportado (%d)\n", chroma);
                return -2;
            }

            de265_release_next_picture(deco);
        }
    }
}

static int put_decode_h264(ISVCDecoder* decoder, uint_fast8_t* data, int len, ImagePlaneList* images)
{
    int frames_decoded = 0;
    unsigned char* planes[3] = { NULL, NULL, NULL };
    SBufferInfo info;
    DECODING_STATE state = (*decoder)->DecodeFrame2(decoder, data, len, planes, &info);

    while (1) 
    {
        if (state != dsErrorFree)
        {
            if (frames_decoded == 0)
            {
                return -1; 
            }
            break;
        }

        if (info.iBufferStatus == 1) // Verifica se tem frame disponível
        {
            ImagePlane* image = malloc(sizeof(ImagePlane*));
            populate_from_h264_image(&info, planes, image);
            imagep_list_add(images, image);

            frames_decoded++;
        }

        memset(&info, 0, sizeof(SBufferInfo));
        planes[0] = planes[1] = planes[2] = NULL;

        state = (*decoder)->DecodeFrame2(decoder, NULL, 0, planes, &info);

        if (info.iBufferStatus != 1 && state == dsErrorFree) 
        {
            break;
        }
    }


}



int h26x_decode_frames(DecoderInstance* codec, MediaBuffer* input, ImagePlaneList* images)
{
	if (!codec || !input || !images) return -1;

	switch (codec->Type)
	{
		case 264:
		{
            ISVCDecoder* decoder = (ISVCDecoder*)codec->Instance;
            put_decode_h264(decoder, input->Data, input->Size, images);
		}
		break;
		case 265:
		{
            de265_decoder_context* decoder = (de265_decoder_context*)codec->Instance;
            put_decode_h265(decoder, input->Data, input->Size, images);
		}
		break;
		default:
			return -2;
	}
}

int h26x_decoder_release(DecoderInstance** decode)
{
    if (*decode)
    {
        switch ((*decode)->Type)
        {
        case 264:
        {
            ISVCDecoder* dech264 = (ISVCDecoder*)(*decode)->Instance;
            (*dech264)->Uninitialize(dech264);
            WelsDestroyDecoder(dech264);
            free(*decode);
            *decode = 0;
            return 0;
        }
        case 265:
        {
            de265_decoder_context* dech265 = (de265_decoder_context*)(*decode)->Instance;
            de265_free_decoder(dech265);
            free(*decode);
            *decode = 0;
            return 0;
        }
        default:
            return -1;
        }
    }
    return -1;
}

DecoderInstance* h26x_decoder_create(int codec)
{
    switch (codec)
    {
    case 264:
    {
        DecoderInstance* result = malloc(sizeof(DecoderInstance));
        result->ID = 0;
        result->Media = MEDIA_TYPE_VIDEO;
        result->Type = codec;
        result->Instance = h264_decoder_create();
        return result;
    }
    case 265:
    {
        DecoderInstance* result = malloc(sizeof(DecoderInstance));
        result->ID = 0;
        result->Media = MEDIA_TYPE_VIDEO;
        result->Type = codec;
        result->Instance = h265_decoder_create();
        return result;
    }
    default:
        return 0;
    }
}




int h26x_create_annexb(VideoMetadata* meta, MediaBuffer* output)
{
    if (!meta || !output) return -1;

    switch (meta->Codec)
    {
    case 264:
    {
        output->Data = h264_create_annexb(meta, output->Size);
    }
    break;
    case 265:
    {
        output->Data = h265_create_annexb(meta, output->Size);
    }
    break;
    default:
        return -2;
    }
}

int h26x_put_single_frame(FILE* f, FrameIndex* frame, VideoMetadata* meta, MediaBuffer* output)
{
    if (!meta || !output) return -1;

    switch (meta->Codec)
    {
    case 264:
    {
        return h264_create_single_frame(f, frame, meta, output);
    }
    case 265:
    {
        return h265_create_single_frame(f, frame, meta, output);
    }
    default:
        return -2;
    }
    return -1;
}

int h26x_put_fragment(FILE* f, FrameIndexList* frame_list, double timeline_offset, double timeline_fragment_duration, int frame_offset, int frame_length, int include_sps_pps, H264FragmentFormat format, MediaBuffer* output)
{
    if (!frame_list || !output) return -1;

    switch (frame_list->Metadata.Codec)
    {
    case 264:
    {
        return h264_create_fragment(f, frame_list, timeline_offset, timeline_fragment_duration, frame_offset, frame_length, include_sps_pps, format, output);
    }
    case 265:
    {
        return h265_create_fragment(f, frame_list, timeline_offset, timeline_fragment_duration, frame_offset, frame_length, include_sps_pps, format, output);
    }
    default:
        return -2;
    }
    return -1;
}
