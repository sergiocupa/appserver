#include "../../appserver/submodules/xplatbase/Xplatbase/Xplatbase/src/memory_pool.h"
#include "../../appserver/submodules/xplatbase/Xplatbase/Xplatbase/src/string_handler.h"
#include "../include/MediaFragmenter.h"
#include "MediaSourceSim.h"



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
    VideoOutput* v = memop_calloc_raw(1, sizeof(VideoOutput));
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



static int video_output_show(VideoOutput* v, ImagePlane* image)
{
    if (!image) return -1;

    int video_w  = image->Width;
    int video_h  = image->Height;
    int stride_y = image->Strides[0];
    int stride_u = image->Strides[1];
    int stride_v = image->Strides[1];

    // === Apenas na primeira vez ou quando a resolução do VÍDEO muda ===
    if (video_w != v->Width || video_h != v->Height || !v->tex)
    {
        if (v->tex) SDL_DestroyTexture(v->tex);

        v->tex = SDL_CreateTexture(v->ren, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, video_w, video_h);   // <<<<< SEMPRE tamanho do vídeo

        if (!v->tex)
        {
            fprintf(stderr, "Erro textura: %s\n", SDL_GetError());
            return -2;
        }

        v->Width = video_w;   // Width/Height agora = tamanho real do vídeo
        v->Height = video_h;
    }

    // Atualiza textura (sempre com strides do vídeo original)
    SDL_UpdateYUVTexture(v->tex, NULL, image->Planes[0], stride_y, image->Planes[1], stride_u, image->Planes[2], stride_v);

    // === Render com letterbox (agora o dst muda a cada frame) ===
    SDL_SetRenderDrawColor(v->ren, 0, 0, 0, 255);
    SDL_RenderClear(v->ren);

    int win_w, win_h;
    SDL_GetWindowSize(v->win, &win_w, &win_h);


    // Para manter o aspect. Se nao precisa pode remover aqui e o IF abaixo, e o ultimo parametro de SDL_RenderCopy como NULL
    float video_aspect = (float)video_w / (float)video_h;
    float win_aspect = (float)win_w / (float)win_h;

    SDL_Rect dst;
    if (win_aspect > video_aspect) 
    {
        dst.h = win_h;
        dst.w = (int)(win_h * video_aspect + 0.5f);
        dst.x = (win_w - dst.w) / 2;
        dst.y = 0;
    }
    else 
    {
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
    memop_free_raw(v);
}




int media_sim_feed(MediaSourceSession* source, MediaBuffer* input)
{
    if (!source) return -1;

    ImagePlaneList* images = imagep_list_new(2);
    int res = h26x_decode_frames(source->Decoder, input, images);
    if (!res)
    {
        // erro
    }

    int ix = 0;
    while (ix < images->Count)
    {
        ImagePlane* image = images->Items[ix];
        video_output_show(source->Output, image);
        ix++;
    }
    imagep_list_release(&images,1);
    return 0;
}



MediaSourceSession* media_sim_create(int width, int height, int codec)
{
    MediaSourceSession* source = memop_alloc_raw(sizeof(MediaSourceSession));
    source->Decoder = h26x_decoder_create(codec);
    source->Output  = video_output_create(width, height);
    return source;
}


void media_sim_release(MediaSourceSession** source)
{
    if (*source)
    {
        h26x_decoder_release((*source)->Decoder);
        video_output_destroy((*source)->Output);
        memop_free_raw(*source);
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