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
#include "hls_controller.h"
#include "session_controller.h"
#include "device_controller.h"
#include "live_controller.h"
#include "frag_session.h"
#include "mem_leak_watch.h"   // monitor de alcancabilidade da xplatbase
#include <stdlib.h>



Element* app_login(Message* message)
{
    Element* obj = (Element*)message->Object;

    if (obj)
    {
        Element* user = yason_find_element(obj,"UserName");
        Element* pass = yason_find_element(obj,"Password");

        if (user && pass)
        {
			if (string_equals_c(&user->Value, "admin") && string_equals_c(&pass->Value, "admin"))
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

Element* app_root(Message* message)
{

    return 0;
}

Element* video_list(Message* message)
{

    return 0;
}

Element* video_select_stream(Message* message)
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



//int main(int argc, char* argv[])
int main()
{
    platform_init();   // inicializa memory_pool + thread hooks do xplatbase (auto-init foi suprimido)
    frag_session_init("web/hls");   // raiz das sessoes de fragmentacao (uma subpasta por sessao)

    // Monitor de vazamento da xplatbase. ATENCAO: o platform_init() acima JA chama
    // mem_leak_watch_start(NULL) -- chamar de novo aqui seria no-op ("ja rodando").
    // O que falta e disparar a varredura, porque os limiares padrao (70%/90% da RAM
    // fisica) fazem o scan automatico praticamente nunca acontecer. Com
    //   set MEDIA_LEAK_WATCH=1
    // o scan roda ao FIM DE CADA sessao, que e a fronteira que interessa.
    //
    // Como ler o mem_leak_watch.log (limitacoes documentadas em mem_leak_watch.h):
    //   - so varre spans de size-class (<=16KB); os buffers de FRAME sao blocos LARGE
    //     e NAO entram na varredura -- para eles o sinal e o campo "memory" do
    //     session.json (balanco de memop_get_stats por sessao);
    //   - nao usa .data/.bss como raiz, entao qualquer ponteiro cuja unica referencia
    //     viva seja uma variavel estatica aparece como falso positivo.
    {
        char* on = 0; size_t on_len = 0;
        if (_dupenv_s(&on, &on_len, "MEDIA_LEAK_WATCH") == 0 && on && on[0] == '1')
        {
            frag_session_set_leak_scan(1);
            printf("[mem] varredura de alcancabilidade ao fim de cada sessao (mem_leak_watch.log)\n");
        }
        if (on) free(on);
    }

    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);


    FunctionBindList* bind = bind_list_create();
    //app_add_receiver(bind, "service/videoplayer", video_player, true);
    app_add_web_resource(bind, "service/videolist", video_list);
    app_add_receiver(bind, "index", app_root, true);
    app_add_receiver(bind, "service/login", app_login, true);
    app_add_receiver(bind, "video/prepare", hls_prepare_video, true);
    app_add_receiver(bind, "video/prepare-stream", hls_stream_video, true);
    app_add_receiver(bind, "video/prepare-dash", dash_stream_video, true);
    app_add_receiver(bind, "video/convert", convert_file, true);
    app_add_receiver(bind, "session", session_route, true);
    app_add_receiver(bind, "media/devices", device_route, true);
    app_add_receiver(bind, "live", live_route, true);


    Notification = app_add_emitter(bind, "service/notification");

    AppServerInfo* server = appserver_create("video-service", 1234, "api", "web", bind);


    //int data = 12344;

    //Notification(&data, Notification_Result);

	while (true)
		Sleep(1000);
	return 0;
}                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           