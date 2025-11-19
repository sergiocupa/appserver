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
#include <string.h>
#include <stdlib.h>
//#include <dashstream.h>


char* get_filename_without_ext(const char* filename)
{
    char* dot = strrchr(filename, '.');
    if (dot == NULL)
    {
        return _strdup(filename);  // sem extensão, retorna cópia
    }

    size_t len = dot - filename;
    char* result = malloc(len + 1);
    if (result)
    {
        strncpy(result, filename, len);
        result[len] = '\0';
    }
    return result;
}

int file_exist(const char* filename)
{
    FILE* file = fopen(filename, "r");
    if (file)
    {
        fclose(file);
        return 1; // existe
    }
    return 0; // não existe
}



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


// processa todas as entrada de requisição de inicializador de player DASH
void* get_mpd(Message* message)
{
    if (message->Route.Count > 0)
    {
        String* name = message->Route.Items[message->Route.Count - 1];
        char* ss = get_filename_without_ext(name->Data);
        char path[1024]; 
        sprintf(path, "E:/AmostraVideo/%s.mp4", ss);

        if (file_exist(path))
        {
            FrameIndexList* list = mp4builder_get_frames(path);
            if (!list) return 0;

            size_t mpd_size;
            char* mpd_content = dash_create_mpd(&list->Metadata, list, 2.0, &mpd_size);

            message->Response = message_response_create(200, APPLICATION_XML);
            resource_buffer_append(&message->Response->Content, mpd_content, mpd_size);

            // add nos caebcalhos HTTP. Se nao tem funcionalidade para add fields, entao criar
            ...
        }
        else
        {
            message->Response = message_response_create_text(500, "File not found");
        }
    }

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
    mi.Data = h26x_create_annexb(&list->Metadata, &mi.Size);
    media_sim_feed(session, &mi);
    free(mi.Data);

    int res = 0;

    // Loop por frames
    for (uint32_t i = 0; i < list->Count; i++) 
    {
        FrameIndex* frame = list->Frames[i];

        MediaBuffer mb;
        res = h26x_create_single_frame(file, frame, &list->Metadata, &mb);

        if (!res)
        {
            int fe = media_sim_feed(session, &mb);
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

    //render_video("e:/AmostraVideo/sample-3.mp4");// small// "e:/sample-5s.mp4"

    


    FunctionBindList* bind = bind_list_create();
    //app_add_receiver(bind, "service/videoplayer", video_player, true);
    app_add_web_resource(bind, "service/videolist", video_list);
    app_add_receiver(bind, "index", app_root, true);
    app_add_receiver_extension(bind, ".mpd", get_mpd, true);
    app_add_receiver(bind, "service/login", app_login, true);


    Notification = app_add_emitter(bind, "service/notification");

    AppServerInfo* server = appserver_create("video-service", 1234, "api", bind);


    //int data = 12344;

    //Notification(&data, Notification_Result);


   

	getchar();
	return 0;
}