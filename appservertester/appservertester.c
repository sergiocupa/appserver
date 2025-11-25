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


FrameIndexList* _List;
int Index;


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
// index off comecando do fim da string
int string_index_end_cs(const char* data, int length, int end, const char token)
{
    int i = length;

    while (i > end)
    {
        i--;

        if (data[i] == token)
        {
            return i;
        }
    }
    return -1;
}

int _numeric_parse_int(char* data, int index, int length, int* result)
{
    if (length > 22) length = 22;
    if (length < 0) length = 22;

    int res = 0;
    int is_negative = 0;
    int ix = index;
    int end = index + length;

    if (data[0] == '-')
    {
        is_negative = 1;
        ix++;
    }

    while (ix < end)
    {
        if (data[ix] == 0) break;

        if (data[ix] >= '0' && data[ix] <= '9')
        {
            res = (res * 10) + (data[ix] - '0');
        }
        else
        {
            return -1;
        }
        ix++;
    }

    if (is_negative)
    {
        res = -res;
    }
    *result = res;

    return 0;
}

int GetFragmentIndex(String* va)
{
    if (va->Length > 0)
    {
        int ext = string_index_end_cs(va->Data, va->Length, 0, '.');
        if (ext >= 0)
        {
            int sep = string_index_end_cs(va->Data, ext, 0, '_');
            if (sep >= 0 && sep < ext)
            {
                int val = 0;
                if (!_numeric_parse_int(va->Data, sep + 1, (ext - sep -1), &val))
                {
                    return val;
                }
            }
        }
    }
    return -1;
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
                message->Response = message_response_create_text(HTTP_STATUS_OK, "0123456789...");
			}
            else
            {
                message->Response = message_response_create_text(HTTP_STATUS_UNAUTHORIZED, "");
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

            message->Response = message_response_create(HTTP_STATUS_OK, APPLICATION_XML);
            message->Response->Content.Data = mpd_content;
            message->Response->Content.Length = mpd_size;

            message_field_list_add_v(&message->Response->Fields, "Content-Type", "application/dash+xml");
            message_field_list_add_v(&message->Response->Fields, "Cache-Control", "public, max-age=3600");
        }
        else
        {
            message->Response = message_response_create_text(HTTP_STATUS_INTERNAL_ERROR, "File not found");
        }
    }

    return 0;
}


void* init_mp4(Message* message)
{
    _List = mp4builder_get_frames("E:/AmostraVideo/sample-3.mp4");
    if (!_List) return 0;

    MP4InitConfig config;
    MediaBuffer init_segment;
    int result = mp4builder_create_init(&_List->Metadata, &config, &init_segment);

    if (result != 0 || !init_segment.Data)
    {
        message->Response = message_response_create_text(HTTP_STATUS_INTERNAL_ERROR,"Failed to create initialization segment");
        //mp4builder_free_frames(frame_list);
        //if (video_name && !message->Params.Count) free(video_name);
        return NULL;
    }


    FILE* arq = fopen("E:/AmostraVideo/sample-3_init.mp4", "wb");
    fwrite(init_segment.Data, init_segment.Size, 1, arq);
    fclose(arq);

    message->Response                 = message_response_create(HTTP_STATUS_OK, APPLICATION_OCTET_STREAM);
    message->Response->Content.Data   = (char*)init_segment.Data;
    message->Response->Content.Length = init_segment.Size;

    message_field_list_add_v(&message->Response->Fields, "Content-Type", "video/mp4");
    message_field_list_add_v(&message->Response->Fields, "Cache-Control", "public, max-age=31536000"); // Cache por 1 ano
    message_field_list_add_v(&message->Response->Fields, "Access-Control-Allow-Origin", "*"); // CORS
    return 0;
}


void* fragment_m4s(Message* message)
{
    if (!_List) return 0;

    FILE* f = fopen("E:/AmostraVideo/sample-3.mp4", "rb");
    if (!f)
    {
        message->Response = message_response_create_text(HTTP_STATUS_INTERNAL_ERROR, "Failed to open video file");
        return 0;
    }

    String* lastr   = message->Route.Items[message->Route.Count - 1];
    int     frag_ix = GetFragmentIndex(lastr);

    if (frag_ix >= 0)
    {
        if (frag_ix < _List->Count)
        {
            FrameIndex* frame = _List->Frames[frag_ix];

            int range = 60;// Para 2seg
            double timeline_offset = frag_ix * range;

            MP4FragmentInfo frag_info = {
            .SequenceNumber = frag_ix,
            .TrackID = 1,
            .Timescale = _List->Metadata.Timescale,
            .BaseMediaDecodeTime = (uint64_t)(timeline_offset * _List->Metadata.Timescale)
            };
          
            MediaBuffer fragment = { 0 };
            int res = mp4builder_create_fragment(f, _List, -1, -1, frag_ix, range, &frag_info, &fragment);

            if (res != 0 || !fragment.Data)
            {
                message->Response = message_response_create_text(HTTP_STATUS_INTERNAL_ERROR, "Failed to create fragment");
                return 0;
            }


            FILE* arq = fopen("E:/AmostraVideo/sample-3_frag_01.m4s", "wb");
            fwrite(fragment.Data, fragment.Size, 1, arq);
            fclose(arq);


            message->Response                 = message_response_create(HTTP_STATUS_OK, APPLICATION_OCTET_STREAM);
            message->Response->Content.Data   = (char*)fragment.Data;
            message->Response->Content.Length = fragment.Size;

            message_field_list_add_v(&message->Response->Fields, "Content-Type", "video/mp4");
            message_field_list_add_v(&message->Response->Fields, "Cache-Control", "public, max-age=31536000");
            message_field_list_add_v(&message->Response->Fields, "Access-Control-Allow-Origin", "*");
        }
        else
        {
            message->Response = message_response_create_text(HTTP_STATUS_INTERNAL_ERROR, "The end!");
            return 0;
        }
    }
    else
    {
        message->Response = message_response_create_text(HTTP_STATUS_INTERNAL_ERROR, "Invalid fragment index");
        return 0;
    }
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
    app_add_receiver(bind, "init.mp4", init_mp4, true);
    app_add_receiver_extension(bind, ".mpd", get_mpd, true);
    app_add_receiver_extension(bind, ".m4s", fragment_m4s, true);
    app_add_receiver(bind, "service/login", app_login, true);


    Notification = app_add_emitter(bind, "service/notification");

    AppServerInfo* server = appserver_create("video-service", 1234, "api", bind);


    //int data = 12344;

    //Notification(&data, Notification_Result);


   

	getchar();
	return 0;
}                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           