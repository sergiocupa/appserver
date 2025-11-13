//  MIT License – Modified for Mandatory Attribution
//  
//  Copyright(c) 2025 Sergio Paludo
//
//  github.com/sergiocupa
//  
//  Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files, 
//  to use, copy, modify, merge, publish, distribute, and sublicense the software, including for commercial purposes, provided that:
//  
//     01. The original author’s credit is retained in all copies of the source code;
//     02. The original author’s credit is included in any code generated, derived, or distributed from this software, including templates, libraries, or code - generating scripts.
//  
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED.


#include "appserver.h"
#include "MediaFragmenter.h"
//#include <dashstream.h>



void* app_login(Message* message)
{
    Element* obj = (Element*)message->Object;

    if (obj)
    {
        Element* user = yason_find_element(obj,"UserName");
        Element* pass = yason_find_element(obj,"Password");

        if (user && pass)
        {
			if (string_equals(&user->Value, "admin") && string_equals(&pass->Value, "admin"))
			{
                message->ResponseContent = string_new();
                string_append(message->ResponseContent, "0123456789...");
			}
            else
            {
                message->ResponseStatus = HTTP_STATUS_UNAUTHORIZED;
            }
        }
    }
    return 0;
}

void* app_root(Message* message)
{

    return 0;
}

void* video_list(Message* message)
{

    return 0;
}

void* video_select_stream(Message* message)
{
    // documentar modo WebAPI, como criar session de Objetos

    // Criar gerenciador de stream
	//   Repassar metodo que envia dados do stream. Tem que ser tipo MessageEmitterCalback
    

    // Cria sessao de stream
	//    Retorna metadados da sessao. Se client retornar ACK, inicia stream
	//    Apos ACK, iniciar envio de dados do stream, se encontrar fonte de video por exemplo, fragmentar e enviar
	//    Na instancia da sessao, controla fluxo, buffer, sequenciamento de pacotes. Se client requerar pacote, entao reenviar

    return 0;
}



MessageEmitterCalback Notification;
void Notification_Result(ResourceBuffer* object)
{
    printf("Notificou...");
}



void render_video(const char* path)
{
    FrameIndexList* list = mp4builder_get_frames(path);
    if (!list) return;


    FILE* file = fopen(path, "rb");
    if (!file) {
        perror("Erro ao abrir arquivo");
        return;
    }

    MediaSourceSession* session = media_sim_create(list->Metadata.Width / 2, list->Metadata.Height / 2);
    //MediaSourceSession* session = media_sim_create(list->Metadata.Width, list->Metadata.Height);


    MediaBuffer mi;
    mi.Data = mp4builder_create_annexb(&list->Metadata, &mi.Size);
    media_sim_feed(session, &mi);
    free(mi.Data);

    // Loop por frames
    for (uint32_t i = 0; i < list->Count; i++) 
    {
        FrameIndex* frame = list->Frames[i];

        MediaBuffer mb;
        mb.Data = mp4builder_single_frame(file, frame, &list->Metadata, &mb.Size);
        if (mb.Size > 0)
        {
            int fe = media_sim_feed(session, &mb);
            if (!fe)
            {
                printf("Render Frame %d...\r\n", i);
            }
            else
            {
                printf("Render - FAIL %d...\r\n", fe);
            }

            free(mb.Data);
        }

        Sleep(33);
    }

    // Fecha sessão
    // media_sim_destroy(session);
    fclose(file);
}




//int main(int argc, char* argv[])
int main()
{
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    // sample-5s.mp4
    // e:/small.mp4
   
    //FrameIndexList* frames = mmp4_index_frames("e:/AmostraVideo/Big_Buck_Bunn_H265.mp4");
    //FrameIndexList* frames = mmp4_index_frames("e:/AmostraVideo/sample_960x540.mkv");

    //concod_display_frame_index(frames);

    render_video("e:/sample-5s.mp4");// small// "e:/sample-5s.mp4"

    //medias_waiting(session->Output);

    //concod_display_frame_index(frames);

    /*VideoInitData vid;
    MediaBuffer* init = mmp4_read_init_segment(&vid);

    MediaSourceSession* session = media_sim_create(vid.width, vid.height);

    media_sim_init_segment(session, init);

    FILE* f = fopen("e:/small.mp4", "rb");
    int ix = 0;
    while (ix < frames->Count)
    {
        FrameIndex* frame = frames->Frames[ix];

        MediaBuffer* mb = mmp4_read_frame(frame, f);

        media_sim_feed(session, mb);

        mbuffer_release(mb);
        ix++;
    }

    fclose(f);
    media_sim_release(session);*/






    //FrameIndexList* frames = index_frames_full("e:/small.mp4");

    //printf("Frame count %d\r\n", frames->Count);

    //int i = 0;
    //while (i < frames->Count)
    //{
    //    FrameIndex* f = frames->Frames[i];

    //    //if (f->NalType == 1 || f->NalType == 5)
    //    //{
    //        printf("Frame %3d | Offset %-8llu | Size %-6llu | NAL %-3d | Type %c | PTS %.3f\n", i, f->Offset, f->Size, f->NalType, f->FrameType, f->PTS);
    //    //}
    //    i++;
    //}



    //FunctionBindList* bind = bind_list_create();
    ////app_add_receiver(bind, "service/videoplayer", video_player, true);
    //app_add_web_resource(bind, "service/videolist", video_list);
    //app_add_receiver(bind, "index", app_root, true);
    //app_add_receiver(bind, "service/login", app_login, true);
    //Notification = app_add_emitter(bind, "service/notification");

    //AppServerInfo* server = appserver_create("video-service", 1234, "api", bind);


    //int data = 12344;

    //Notification(&data, Notification_Result);


   

	getchar();
	return 0;
}