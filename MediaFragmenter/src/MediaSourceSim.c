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
    // ⚠️ CORREÇÃO CRÍTICA: Verificar se os planos são válidos
    if (!planes[0] || !planes[1] || !planes[2])
    {
        fprintf(stderr, "ERRO: Planos YUV inválidos (NULL)\n");
        return;
    }

    // Obter dimensões reais do frame decodificado
    int width = info->UsrData.sSystemBuffer.iWidth;
    int height = info->UsrData.sSystemBuffer.iHeight;

    // Obter strides (podem ser diferentes da largura devido ao alinhamento)
    int stride_y = info->UsrData.sSystemBuffer.iStride[0];
    int stride_u = info->UsrData.sSystemBuffer.iStride[1];
    int stride_v = info->UsrData.sSystemBuffer.iStride[1];  // OpenH264 usa mesmo stride para U e V

    // 🔧 SOLUÇÃO 1: Verificar se as dimensões batem
    if (width != v->Width || height != v->Height)
    {
        fprintf(stderr, "AVISO: Dimensões do frame (%dx%d) diferem da janela (%dx%d)\n",
            width, height, v->Width, v->Height);

        // Recriar textura com dimensões corretas
        SDL_DestroyTexture(v->tex);
        v->tex = SDL_CreateTexture(v->ren, SDL_PIXELFORMAT_IYUV,
            SDL_TEXTUREACCESS_STREAMING, width, height);
        v->Width = width;
        v->Height = height;
    }

    // 🔧 SOLUÇÃO 2: Debug dos valores YUV (apenas primeiros pixels)
#ifdef DEBUG_YUV
    printf("YUV Debug:\n");
    printf("  Y[0]=%d, Y[1]=%d, Y[2]=%d\n", planes[0][0], planes[0][1], planes[0][2]);
    printf("  U[0]=%d, U[1]=%d, U[2]=%d\n", planes[1][0], planes[1][1], planes[1][2]);
    printf("  V[0]=%d, V[1]=%d, V[2]=%d\n", planes[2][0], planes[2][1], planes[2][2]);
    printf("  Strides: Y=%d, U=%d, V=%d\n", stride_y, stride_u, stride_v);
    printf("  Dimensões: %dx%d\n", width, height);
#endif

    // 🔧 SOLUÇÃO 3: Atualizar textura com strides corretos
    int result = SDL_UpdateYUVTexture(
        v->tex, NULL,
        planes[0], stride_y,  // Y plane e seu stride
        planes[1], stride_u,  // U plane e seu stride
        planes[2], stride_v   // V plane e seu stride
    );

    if (result < 0)
    {
        fprintf(stderr, "ERRO SDL_UpdateYUVTexture: %s\n", SDL_GetError());
        return;
    }

    // Limpar renderizador com preto (não verde!)
    SDL_SetRenderDrawColor(v->ren, 0, 0, 0, 255);
    SDL_RenderClear(v->ren);

    // Copiar textura para renderizador
    result = SDL_RenderCopy(v->ren, v->tex, NULL, NULL);
    if (result < 0)
    {
        fprintf(stderr, "ERRO SDL_RenderCopy: %s\n", SDL_GetError());
        return;
    }

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
    if (state != 0)
    {
        fprintf(stderr, "AVISO: DecodeFrame2 retornou estado %d (não é erro necessariamente)\n", state);
    }

    // CORREÇÃO CRÍTICA: Verificar se há frame disponível
    if (info.iBufferStatus == 1)
    {
        // Verificar se os planos são válidos antes de renderizar
        if (planes[0] && planes[1] && planes[2])
        {
            /*printf("✓ Frame decodificado: %dx%d, strides=[%d,%d,%d]\n",
                info.UsrData.sSystemBuffer.iWidth,
                info.UsrData.sSystemBuffer.iHeight,
                info.UsrData.sSystemBuffer.iStride[0],
                info.UsrData.sSystemBuffer.iStride[1],
                info.UsrData.sSystemBuffer.iStride[1]);*/

            video_output_show(out, planes, &info);
        }
        else
        {
            fprintf(stderr, "ERRO: Planos YUV retornaram NULL após decodificação\n");
        }
    }
    else
    {
        //printf("  (sem frame disponível, buffer status = %d)\n", info.iBufferStatus);
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