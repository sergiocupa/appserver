// codec_video.h — tipos e decode de video H26x (openh264/de265), na camada de CODECS.
// Movido do MediaFragmenter para quebrar a dependencia (MediaGateway/MediaFragmenter -> codecs).
#ifndef CODEC_VIDEO_H
#define CODEC_VIDEO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { MEDIA_TYPE_NONE = 0, MEDIA_TYPE_AUDIO = 1, MEDIA_TYPE_VIDEO = 2 } MediaType;

typedef struct
{
    int       ID;
    int       Initialized;
    int       Type;
    MediaType Media;
    void*     Instance;
}
DecoderInstance;

typedef struct
{
    int           Width;
    int           Height;
    uint64_t      Size;
    uint64_t      Sizes[3];
    int           Strides[3];
    uint_fast8_t* Planes[3];
}
ImagePlane;

typedef struct
{
    int          Max;
    int          Count;
    ImagePlane** Items;
}
ImagePlaneList;

typedef struct _MediaBuffer
{
    int           Max;
    int           Size;
    uint_fast8_t* Data;
}
MediaBuffer;

// Lista de imagens (planos decodificados).
void            imagep_list_add(ImagePlaneList* list, ImagePlane* image);
void            imagep_list_init(ImagePlaneList* list, int init_count);
ImagePlaneList* imagep_list_new(int init_count);
void            imagep_list_release(ImagePlaneList** list, int free_items);

// Decode H26x (codec=264/265) via openh264/de265.
DecoderInstance* h26x_decoder_create(int codec);
int              h26x_decode_frames(DecoderInstance* codec, MediaBuffer* input, ImagePlaneList* images);
int              h26x_decoder_release(DecoderInstance** decode);
// Drena os frames retidos para reordenacao (so no fim do fluxo). Retorna quantos sairam.
int              h26x_decoder_flush(DecoderInstance* codec, ImagePlaneList* images);

#ifdef __cplusplus
}
#endif
#endif // CODEC_VIDEO_H
