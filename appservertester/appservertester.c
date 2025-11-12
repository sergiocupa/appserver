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

typedef struct {
    ISVCDecoder* Decoder;
} DecoderSession;


// --- Cria e inicializa decoder ---
DecoderSession* CreateOpenH264Decoder()
{
    DecoderSession* session = calloc(1, sizeof(DecoderSession));
    if (!session) return NULL;

    // 1. Cria o decoder
    if (WelsCreateDecoder(&session->Decoder) != 0 || !session->Decoder) {
        fprintf(stderr, "Erro: WelsCreateDecoder falhou\n");
        free(session);
        return NULL;
    }

    // 2. Configura parâmetros
    SDecodingParam decParam = { 0 };
    decParam.uiTargetDqLayer = (uint8_t)-1;
    decParam.eEcActiveIdc = ERROR_CON_SLICE_COPY;
    decParam.bParseOnly = false;

    // 3. Inicializa (CORRETO: (*decoder)->Function)
    if ((*session->Decoder)->Initialize(session->Decoder, &decParam) != 0) {
        fprintf(stderr, "Erro: Initialize falhou\n");
        WelsDestroyDecoder(session->Decoder);
        free(session);
        return NULL;
    }

    printf("OpenH264 decoder inicializado com sucesso!\n");
    return session;
}



// --- Envia SPS + PPS (sem start code) ---
int SendSpsPps(DecoderSession* session, const char* path)
{
    static const uint8_t sps_pps2[] = {
        0x00, 0x00, 0x00, 0x01,
          0x67, 0x64, 0x00, 0x32,
          0xac, 0x72, 0x84, 0x40, 
          0x50, 0x05, 0xbb, 0x01, 
          0x10, 0x00, 0x00, 0x03, 
          0x00, 0x10, 0x00, 0x00, 
          0x03, 0x03, 0xc0, 0xf1,
          0x83, 0x18, 0x46, 
        0x00, 0x00, 0x00, 0x01,
          0x68, 0xe8, 0x43, 0x89, 0x2c, 0x8b
    };

    uint8_t* pData[3];// = { 0 };
    SBufferInfo info = { 0 };
    memset(&info, 0, sizeof(SBufferInfo));
    info.iBufferStatus = 1;
    int leng = sizeof(sps_pps2);

    

    /*FILE* f = fopen("e:/AmostraVideo/sps_pps_annexb.bin", "rb");
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* buffer = malloc(len);
    fread(buffer, 1, len, f);
    fclose(f);*/

    FrameIndexList* list = concod_get_frames(path);


    // Cria AnnexB
    uint8_t* annexb = malloc(list->Metadata.pps_len + list->Metadata.sps_len + 8);
    annexb[0] = 0; annexb[1] = 0; annexb[2] = 0; annexb[3] = 1;
    int pos = 4;
    memcpy(annexb+ pos, list->Metadata.sps, list->Metadata.sps_len);
    pos += list->Metadata.sps_len;

    annexb[pos] = 0; annexb[pos+1] = 0; annexb[pos+2] = 0; annexb[pos+3] = 1;
    pos += 4;
    memcpy(annexb + pos, list->Metadata.pps, list->Metadata.pps_len);
    pos += list->Metadata.pps_len;


    int ret = (*session->Decoder)->DecodeFrame2(session->Decoder, annexb, pos, pData, &info);

    if (ret != 0)
    {
        fprintf(stderr, "Erro ao enviar SPS/PPS: %d\n", ret);
        return ret;
    }

    printf("SPS/PPS enviados com sucesso!\n");
    return 0;
}



void render_video(const char* path)
{
    FrameIndexList* list = concod_get_frames(path);
    if (!list) return;


    FILE* file = fopen(path, "rb");
    if (!file) {
        perror("Erro ao abrir arquivo");
        return;
    }

    //MediaSourceSession* session = media_sim_create(list->Metadata.width, list->Metadata.height);// (1920, 1080);  // Ajuste width/height de stsd ou hardcoded
    MediaSourceSession* session = media_sim_create(list->Metadata.width, list->Metadata.height);




    // --- SPS + PPS SEM start code (NAL puro) ---
    static const uint8_t sps_pps_raw[] = {
        // SPS (NAL type 7)
        0x00, 0x00, 0x00, 0x01,
        0x67, 0x64, 0x00, 0x1E, 0xAC, 0xD9, 0x40, 0x78,
        0x05, 0x07, 0x29, 0xC1, 0xF0, 0xC8, 0x00, 0x00,
        0x03, 0x00, 0xC8, 0x00, 0x00, 0x03, 0x01, 0xE0, 0x80,

        // PPS (NAL type 8)
        0x00, 0x00, 0x00, 0x01,
        0x68, 0xEE, 0x3C, 0x80
    };
    static const int sps_pps_len = sizeof(sps_pps_raw);

    // --- Envio para OpenH264 ---
    uint8_t* planes22 = NULL;  // NULL!
    SBufferInfo info22 = { 0 };

    // CRÍTICO: iBufferStatus = 1 → "parameter sets"
    info22.iBufferStatus = 1;

    int ret = (*session->Decoder)->DecodeFrame2(
        session->Decoder,
        sps_pps_raw,      // buffer SEM start code
        sps_pps_len,      // tamanho total
        &planes22,          // NULL
        &info22             // iBufferStatus = 1
    );



    static const uint8_t config_nal[] = {
    0x67, 0x64, 0x00, 0x1E, 0xAC, 0xD9, 0x40, 0x78,
    0x05, 0x07, 0x29, 0xC1, 0xF0, 0xC8, 0x00, 0x00,
    0x03, 0x00, 0xC8, 0x00, 0x00, 0x03, 0x01, 0xE0, 0x80,
    0x68, 0xEE, 0x3C, 0x80
    };
    static const int config_len = sizeof(config_nal);

    uint8_t* planes = NULL;
    SBufferInfo info = { 0 };
    info.iBufferStatus = 1;  // crucial!

    ret = (*session->Decoder)->DecodeFrame2(session->Decoder, config_nal, config_len, &planes, &info);



    // --- SPS (com start code) ---
    static const uint8_t sps[] = {
        0x00, 0x00, 0x00, 0x01,
        0x67, 0x64, 0x00, 0x1E, 0xAC, 0xD9, 0x40, 0x78,
        0x05, 0x07, 0x29, 0xC1, 0xF0, 0xC8, 0x00, 0x00,
        0x03, 0x00, 0xC8, 0x00, 0x00, 0x03, 0x01, 0xE0, 0x80
    };
    static const int sps_len = sizeof(sps);

    // --- PPS (com start code) ---
    static const uint8_t pps[] = {
        0x00, 0x00, 0x00, 0x01,
        0x68, 0xEE, 0x3C, 0x80
    };
    static const int pps_len = sizeof(pps);

    // --- Enviar SPS + PPS separadamente ---
  //  uint8_t* planes = NULL;  // MUITO IMPORTANTE: NULL para parameter sets
  //  SBufferInfo info = { 0 };


    // 1. Envia SPS
    ret = (*session->Decoder)->DecodeFrame2(session->Decoder, sps, sps_len, &planes, &info);
    if (ret != 0) {
        printf("ERRO ao enviar SPS: %d\n", ret);
        return ret;
    }
    printf("SPS enviado com sucesso\n");

    // 2. Envia PPS
    ret = (*session->Decoder)->DecodeFrame2(session->Decoder, pps, pps_len, &planes, &info);
    if (ret != 0) {
        printf("ERRO ao enviar PPS: %d\n", ret);
        return ret;
    }








    int res = concod_send_initial_header_from_meta(session->Decoder, &list->Metadata);




    ////Extrair SPS/PPS de 'avcC' (parse stsd ou do primeiro frame) e alimentar inicial
    //int res = concod_send_initial_header_from_meta(session->Decoder, &list->Metadata);
    if (res != 0)return;

    // Loop por frames
    for (uint32_t i = 0; i < list->Count; i++) 
    {
        FrameIndex* frame = list->Frames[i];
        unsigned char* buffer = malloc(frame->Size);
        fseek(file, frame->Offset, SEEK_SET);
        if (fread(buffer, 1, frame->Size, file) != frame->Size)
        {
            free(buffer);
            continue;
        }

        // Converter para Annex B
        size_t annexb_size;
        unsigned char* annexb = concod_convert_avcc_to_annexb(file, frame, &list->Metadata, &annexb_size);
        if (annexb) 
        {
            MediaBuffer mb = { .Data = annexb, .Length = annexb_size };
            media_sim_feed(session, &mb);
            free(annexb);
        }

        free(buffer);
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


    DecoderSession* session = CreateOpenH264Decoder();

    SendSpsPps(session, "e:/AmostraVideo/big.mp4");



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