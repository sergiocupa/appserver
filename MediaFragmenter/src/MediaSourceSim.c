#include "../include/MediaFragmenter.h"
#include "MediaSourceSim.h"
#include "MediaFragmenterType.h"



void* event_loop_thread(void* arg)
{
    //SDL_SetHint(SDL_HINT_RENDER_DRIVER, "direct3d"); Gera erro

    // 0 -> Nearest
    // 1 -> Linear
    // 2 -> Anisotropic 
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");


    VideoOutput* v = (VideoOutput*)arg;
    v->win = SDL_CreateWindow("DASH Player Simulator", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, v->Width, v->Height, SDL_WINDOW_RESIZABLE);
    
    // Opção SDL_RENDERER_ACCELERATED congela a tela ao redimencionar.
    //v->ren = SDL_CreateRenderer(v->win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    v->ren = SDL_CreateRenderer(v->win, -1, SDL_RENDERER_SOFTWARE);

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

            if (event.window.event == SDL_WINDOWEVENT_RESIZED) 
            {
                v->WindowWidth  = event.window.data1;
                v->WindowHeight = event.window.data2;
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
    v->WindowWidth = w;
    v->WindowHeight = h;

    v->WaitShow = CreateEvent(NULL, TRUE, FALSE, NULL);

    v->Running     = true;
    v->EventThread = _beginthread(event_loop_thread, 0, (void*)v);

    WaitForSingleObject(v->WaitShow, INFINITE);// espera mostrar tela
    return v;
}



static void video_output_show(VideoOutput* v, unsigned char** planes, SBufferInfo* info)
{
    if (!planes[0] || !planes[1] || !planes[2]) return;

    int video_w = info->UsrData.sSystemBuffer.iWidth;
    int video_h = info->UsrData.sSystemBuffer.iHeight;
    int stride_y = info->UsrData.sSystemBuffer.iStride[0];
    int stride_u = info->UsrData.sSystemBuffer.iStride[1];
    int stride_v = info->UsrData.sSystemBuffer.iStride[1];

    // === Apenas na primeira vez ou quando a resolução do VÍDEO muda ===
    if (video_w != v->Width || video_h != v->Height || !v->tex)
    {
        if (v->tex) SDL_DestroyTexture(v->tex);

        v->tex = SDL_CreateTexture(v->ren, SDL_PIXELFORMAT_IYUV,
            SDL_TEXTUREACCESS_STREAMING,
            video_w, video_h);   // <<<<< SEMPRE tamanho do vídeo

        if (!v->tex) {
            fprintf(stderr, "Erro textura: %s\n", SDL_GetError());
            return;
        }

        v->Width = video_w;   // Width/Height agora = tamanho real do vídeo
        v->Height = video_h;
    }

    // Atualiza textura (sempre com strides do vídeo original)
    SDL_UpdateYUVTexture(v->tex, NULL,
        planes[0], stride_y,
        planes[1], stride_u,
        planes[2], stride_v);

    // === Render com letterbox (agora o dst muda a cada frame) ===
    SDL_SetRenderDrawColor(v->ren, 0, 0, 0, 255);
    SDL_RenderClear(v->ren);

    int win_w, win_h;
    SDL_GetWindowSize(v->win, &win_w, &win_h);


    // Para manter o aspect. Se nao precisa pode remover aqui e o IF abaixo, e o ultimo parametro de SDL_RenderCopy como NULL
    float video_aspect = (float)video_w / (float)video_h;
    float win_aspect = (float)win_w / (float)win_h;

    SDL_Rect dst;
    if (win_aspect > video_aspect) {
        dst.h = win_h;
        dst.w = (int)(win_h * video_aspect + 0.5f);
        dst.x = (win_w - dst.w) / 2;
        dst.y = 0;
    }
    else {
        dst.w = win_w;
        dst.h = (int)(win_w / video_aspect + 0.5f);
        dst.x = 0;
        dst.y = (win_h - dst.h) / 2;
    }

    SDL_RenderCopy(v->ren, v->tex, NULL, &dst);
    SDL_RenderPresent(v->ren);
}



static void video_output_destroy(VideoOutput* v)
{
    SDL_DestroyTexture(v->tex);
    SDL_DestroyRenderer(v->ren);
    SDL_DestroyWindow(v->win);
    SDL_Quit();
    free(v);
}



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


static int h264_decoder_feed(ISVCDecoder* decoder, unsigned char* data, int len, VideoOutput* out)
{
    unsigned char* planes[3] = { NULL, NULL, NULL };
    SBufferInfo info;
    memset(&info, 0, sizeof(info));

    DECODING_STATE state = (*decoder)->DecodeFrame2(decoder, data, len, planes, &info);
    //if (state != 0)
    //{
    //    //fprintf(stderr, "AVISO: DecodeFrame2 retornou estado %d (não é erro necessariamente)\n", state);
    //}

    if (state == dsErrorFree || state == dsFramePending)
    {
        if (info.iBufferStatus == 1 && planes[0] && planes[1] && planes[2])
        {
            video_output_show(out, planes, &info);
            return;
        }

        memset(&info, 0, sizeof(info));
        memset(planes, 0, sizeof(planes));

        state = (*decoder)->DecodeFrame2(decoder, NULL, 0, planes, &info);

        if (info.iBufferStatus == 1 && planes[0] && planes[1] && planes[2]) // Verificar se flush liberou o frame
        {
            video_output_show(out, planes, &info);
        }
        else
        {
            printf("Nenhum frame disponível (normal para SPS/PPS ou frames B)\n");
        }
    }
    else
    {
        // Tentar flush mesmo com erro (pode ter frames válidos no buffer)
        memset(&info, 0, sizeof(info));
        memset(planes, 0, sizeof(planes));

        (*decoder)->DecodeFrame2(decoder, NULL, 0, planes, &info);

        if (info.iBufferStatus == 1 && planes[0] && planes[1] && planes[2])
        {
            video_output_show(out, planes, &info);
        }
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
    source->Decoder = h264_decoder_create();
    source->Output  = video_output_create(width, height);
    return source;
}

int media_sim_init_segment(MediaSourceSession* source, MediaBuffer* data)
{
    return h264_decoder_feed(source->Decoder, data->Data, data->Size, source->Output);
}

int media_sim_feed(MediaSourceSession* source, MediaBuffer* data)
{
    return h264_decoder_feed(source->Decoder, data->Data, data->Size, source->Output);
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