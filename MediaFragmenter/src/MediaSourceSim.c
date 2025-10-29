#include "../include/MediaFragmenter.h"
#include "MediaSourceSim.h"
#include "MediaFragmenterType.h"
#include "SDL2/SDL.h"
#include "codec_api.h"


static VideoOutput* video_output_create(int w, int h)
{
    SDL_Init(SDL_INIT_VIDEO);
    VideoOutput* v = calloc(1, sizeof(VideoOutput));
    v->win = SDL_CreateWindow("DASH Player Simulator", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, 0);
    v->ren = SDL_CreateRenderer(v->win, -1, 0);
    v->tex = SDL_CreateTexture(v->ren, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, w, h);
    return v;
}

static void video_output_show(VideoOutput* v, unsigned char** planes, SBufferInfo* info)
{
    SDL_UpdateYUVTexture
    (v->tex, NULL,
        planes[0], info->UsrData.sSystemBuffer.iStride[0],
        planes[1], info->UsrData.sSystemBuffer.iStride[1],
        planes[2], info->UsrData.sSystemBuffer.iStride[2]
    );
    SDL_RenderClear(v->ren);
    SDL_RenderCopy(v->ren, v->tex, NULL, NULL);
    SDL_RenderPresent(v->ren);
    SDL_Delay(33);
}

static void video_output_destroy(VideoOutput* v)
{
    SDL_DestroyTexture(v->tex);
    SDL_DestroyRenderer(v->ren);
    SDL_DestroyWindow(v->win);
    SDL_Quit();
    free(v);
}

static H264Decoder* h264_decoder_create()
{
    H264Decoder* d = (H264Decoder*)calloc(1, sizeof(H264Decoder));
    if (!d) return NULL;

    int rv = WelsCreateDecoder(&d->dec);
    if (rv != 0 || !d->dec) {
        free(d);
        return NULL;
    }

    SDecodingParam param;
    memset(&param, 0, sizeof(SDecodingParam));
    param.uiTargetDqLayer = 0xFF;      // Todas as camadas
    param.eEcActiveIdc = ERROR_CON_DISABLE;
    param.bParseOnly = false;

    rv = d->dec->Initialize(d->dec, &param);
    if (rv != 0) {
        d->dec->Uninitialize(d->dec);
        WelsDestroyDecoder(d->dec);
        free(d);
        return NULL;
    }

    return d;
}

static void h264_decoder_feed(H264Decoder* d, unsigned char* data, int len, VideoOutput* out)
{
    unsigned char* planes[3];
    SBufferInfo info;
    memset(&info, 0, sizeof(info));

    DECODING_STATE state = d->dec->DecodeFrame2(d->dec, data, len, planes, &info);
    if (state == 0 && info.iBufferStatus == 1) 
    {
        video_output_show(out, planes, &info);
    }
}

static void h264_decoder_destroy(H264Decoder* d) 
{
    d->dec->Uninitialize(d->dec);
    WelsDestroyDecoder(d->dec);
    free(d);
}


MediaSourceSession* media_sim_create(int width, int height)
{
    MediaSourceSession* source = malloc(sizeof(MediaSourceSession));
    source->Decoder = h264_decoder_create(width, height);
    source->Output  = video_output_create(width, height);
    return source;
}

void media_sim_init_segment(MediaSourceSession* source, MediaBuffer* data)
{
    h264_decoder_feed(source->Decoder, data->Data, data->Length, source->Output);
}

void media_sim_feed(MediaSourceSession* source, MediaBuffer* data)
{
    h264_decoder_feed(source->Decoder, data->Data, data->Length, source->Output);
}

void media_sim_release(MediaSourceSession** source)
{
    if (*source)
    {
        h264_decoder_destroy((*source)->Decoder);
        video_output_destroy((*source)->Output);
        free(*source);
        *source = 0;
    }
}