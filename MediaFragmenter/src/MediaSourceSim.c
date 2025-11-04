#include "../include/MediaFragmenter.h"
#include "MediaSourceSim.h"
#include "MediaFragmenterType.h"



void* event_loop_thread(void* arg)
{
    VideoOutput* v = (VideoOutput*)arg;
    v->win = SDL_CreateWindow("DASH Player Simulator", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, v->Width, v->Height, 0);
    v->ren = SDL_CreateRenderer(v->win, -1, 0);
    v->tex = SDL_CreateTexture(v->ren, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, v->Width, v->Height);
    //v->tex = SDL_CreateTexture(v->ren, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STATIC, v->Width, v->Height);

    SetEvent(v->WaitShow);

    while (v->Running)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event)) 
        {
            if (event.type == SDL_QUIT) 
            {
                v->Running = 0;
                if (v->Quit) v->Quit();
                
                if(v->Wait) SetEvent(v->Wait);
            }
            else if (event.type == SDL_KEYDOWN) 
            {
                if (v->KeyDown) v->KeyDown(event.key.keysym.sym);
            }
        }
        SDL_Delay(10);  // Evita 100% CPU
    }
}

static VideoOutput* video_output_create(int w, int h)
{
    SDL_Init(SDL_INIT_VIDEO);
    VideoOutput* v = calloc(1, sizeof(VideoOutput));
    v->Width  = w;
    v->Height = h;

    v->WaitShow = CreateEvent(NULL, TRUE, FALSE, NULL);

    v->Running     = true;
    v->EventThread = _beginthread(event_loop_thread, 0, (void*)v);

    WaitForSingleObject(v->WaitShow, INFINITE);// espera mostrar tela
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

static ISVCDecoder h264_decoder_create()
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

    int rsv = (*decoder)->Initialize(decoder,&param);
    if (rsv != 0) 
    {
        (*decoder)->Uninitialize(decoder);
        WelsDestroyDecoder(decoder);
        free(decoder);
        return NULL;
    }

    return decoder;
}

static void h264_decoder_feed(ISVCDecoder* decoder, unsigned char* data, int len, VideoOutput* out)
{
    unsigned char* planes[3];
    SBufferInfo info;
    memset(&info, 0, sizeof(info));

    DECODING_STATE state = (*decoder)->DecodeFrame2(decoder, data, len, planes, &info);
    if (state == 0 && info.iBufferStatus == 1) 
    {
        video_output_show(out, planes, &info);
    }
}

static void h264_decoder_destroy(ISVCDecoder* decoder)
{
    (*decoder)->Uninitialize(decoder);
    WelsDestroyDecoder(decoder);
    free(decoder);
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

void medias_waiting(VideoOutput* v)
{
    v->Wait = CreateEvent(NULL, TRUE, FALSE, NULL);
    // Em outra thread: SetEvent(hEvent);
    WaitForSingleObject(v->Wait, INFINITE);  // Na main
    CloseHandle(v->Wait);


}