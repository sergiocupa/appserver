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
#include "yason.h"            // appservertester.json
#include "yason_build.h"      // yb_free
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



// ---- configuracao: appservertester.json ao lado do executavel -------------------------
// {"web": "<pasta do conteudo web>", "port": 1234}, os dois opcionais. Caminho relativo vale a
// partir da PASTA DO EXECUTAVEL, nao do diretorio de trabalho: iniciado por servico, script ou
// atalho de outra pasta, o servidor subia e nao achava arquivo nenhum. Sem o arquivo, ou sem
// "web", o conteudo e a propria pasta do executavel. A saida das sessoes fica em <web>/hls.
static char g_web[MAX_PATH];
static int  g_porta = 1234;

static int caminho_absoluto(const char* p)
{
    return p[0] == '/' || p[0] == '\\' || (p[0] && p[1] == ':');
}

// Caminho do executavel e caminho absoluto: cada sistema tem a sua chamada.
#ifdef _WIN32
  #define CFG_SEP "\\"
  static void exe_caminho(char* out, size_t n) { out[0] = '\0'; GetModuleFileNameA(NULL, out, (DWORD)n); }
  static void caminho_completo(char* out, size_t n, const char* p)
  {
      if (!_fullpath(out, p, n)) snprintf(out, n, "%s", p);
  }
#else
  #include <unistd.h>
  #include <limits.h>
  #define CFG_SEP "/"
  static void exe_caminho(char* out, size_t n)
  {
      ssize_t k = readlink("/proc/self/exe", out, n - 1);
      out[k > 0 ? k : 0] = '\0';
  }
  static void caminho_completo(char* out, size_t n, const char* p)
  {
      char real[PATH_MAX];
      snprintf(out, n, "%s", realpath(p, real) ? real : p);
  }
#endif

static void config_carregar(void)
{
    char exe[MAX_PATH] = { 0 };
    exe_caminho(exe, sizeof(exe));
    char* corte = strrchr(exe, '\\');
    char* corte2 = strrchr(exe, '/');
    if (corte2 > corte) corte = corte2;
    if (corte) *corte = '\0';   // pasta do executavel

    char arq[MAX_PATH];
    snprintf(arq, sizeof(arq), "%s" CFG_SEP "appservertester.json", exe);
    char web[MAX_PATH] = { 0 };
    int achou = 0;

    char* texto = 0; int len = 0;
    if (file_read_text(arq, &texto, &len) && texto)
    {
        achou = 1;
        Element* raiz = yason_parse(texto, len, TREE_TYPE_JSON);
        if (raiz)
        {
            Element* w = yason_find_element(raiz, "web");
            if (w && w->Value.Length > 0) snprintf(web, sizeof(web), "%s", w->Value.Content);
            Element* p = yason_find_element(raiz, "port");
            if (p && p->Value.Length > 0 && atoi(p->Value.Content) > 0) g_porta = atoi(p->Value.Content);
            yb_free(raiz);
        }
        else printf("AVISO: %s nao e um JSON valido; usando os padroes\n", arq);
        memop_free_raw(texto);
    }

    char junto[MAX_PATH * 2];
    if (!web[0])                         snprintf(junto, sizeof(junto), "%s", exe);
    else if (caminho_absoluto(web))      snprintf(junto, sizeof(junto), "%s", web);
    else                                 snprintf(junto, sizeof(junto), "%s" CFG_SEP "%s", exe, web);
    caminho_completo(g_web, sizeof(g_web), junto);
    size_t n = strlen(g_web);
    while (n > 3 && (g_web[n - 1] == '\\' || g_web[n - 1] == '/')) g_web[--n] = '\0';

    printf("Configuracao: %s (%s)\n", arq, achou ? "lida" : "ausente: padroes");
    printf("Conteudo web: %s | porta %d\n", g_web, g_porta);
    fflush(stdout);   // saida redirecionada (servico/journal): sem isto a linha ficava presa no buffer
}

//int main(int argc, char* argv[])
int main()
{
    platform_init();   // inicializa memory_pool + thread hooks do xplatbase (auto-init foi suprimido)
    config_carregar();
    {
        char hls[MAX_PATH + 8];
        snprintf(hls, sizeof(hls), "%s\\hls", g_web);
        frag_session_init(hls);   // raiz das sessoes de fragmentacao (uma subpasta por sessao)
        hls_set_raiz(hls);        // onde o upload e a fragmentacao gravam
    }

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
    // Longas: rodam na pista longa, fora dos workers que atendem as rotas curtas.
    //   video/prepare  : grava o upload e analisa o MP4 antes de responder
    //   media/devices  : enumera hardware (pode levar segundos no Windows)
    // Os fluxos (prepare-stream, prepare-dash, convert) sao CURTOS: iniciam um job e assinam
    // o topico dele; o trabalho de minutos roda no job, nao no handler.
    app_add_receiver_longa(bind, "video/prepare", hls_prepare_video);
    app_add_receiver(bind, "video/prepare-stream", hls_stream_video, true);
    app_add_receiver(bind, "video/prepare-dash", dash_stream_video, true);
    app_add_receiver(bind, "video/convert", convert_file, true);
    app_add_receiver(bind, "session", session_route, true);
    app_add_receiver_longa(bind, "media/devices", device_route);
    app_add_receiver(bind, "live", live_route, true);


    Notification = app_add_emitter(bind, "service/notification");

    AppServerConfig cfg = appserver_config_default();
    cfg.AgentName           = "video-service";
    cfg.Port                = g_porta;
    cfg.Prefix              = "api";
    cfg.WebContentPath      = g_web;
    cfg.EnableHealthMonitor = true;   // com painel de saude: a pagina web tem o health-box
    // Aparelho com bateria: pool em economia; os jobs de video pedem performance enquanto rodam.
    cfg.PerfilPool          = POOL_PERFIL_ECONOMIA;
    AppServerInfo* server = appserver_create(&cfg, bind);


    //int data = 12344;

    //Notification(&data, Notification_Result);

	while (true)
		Sleep(1000);
	return 0;
}                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           