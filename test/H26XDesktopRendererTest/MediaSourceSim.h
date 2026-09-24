//  Simulador de player: abre uma janela SDL e desenha os quadros decodificados.
//
//  Isto NAO e parte do fragmentador. Estava dentro do MediaFragmenter -- que passava a
//  carregar uma dependencia de UI e ainda declarava a API dela no cabecalho publico --
//  para servir a um unico consumidor: este teste. Agora mora junto de quem usa.

#ifndef MEDIA_SOURCE_SIM_H
#define MEDIA_SOURCE_SIM_H

#ifdef __cplusplus
extern "C" {
#endif

#include "MediaFragmenterType.h"

#ifdef _WIN32
#define SDL_MAIN_HANDLED
#include "SDL2/SDL.h"
#include <wtypes.h>

    typedef void (*KeyDownEvent)(SDL_KeyCode key);
    typedef void (*QuitEvent)();
    typedef void (*WaitEvent)();

    typedef struct 
    {
        int           Width;
        int           Height;
        int           WindowWidth;
        int           WindowHeight;
        int           a;
        SDL_Window*   win;
        SDL_Renderer* ren;
        SDL_Texture*  tex;
        void*         EventThread;
        int           Running;
        KeyDownEvent  KeyDown;
        QuitEvent     Quit;
        HANDLE        Wait;
        HANDLE        WaitShow;
    } 
    VideoOutput;
#else
// Fora do Windows o simulador nao existe; um tipo opaco basta para compilar.
typedef struct VideoOutputSim VideoOutput;
#endif

    typedef struct _MediaSourceSession
    {
        VideoOutput*     Output;
        DecoderInstance* Decoder;
    }
    MediaSourceSession;

MediaSourceSession* media_sim_create(int width, int height, int codec);
int                 media_sim_feed(MediaSourceSession* source, MediaBuffer* data);
void                media_sim_release(MediaSourceSession** source);
void                medias_waiting(VideoOutput* v);

#ifdef __cplusplus
}
#endif
#endif /* MEDIA_SOURCE_SIM_H */
