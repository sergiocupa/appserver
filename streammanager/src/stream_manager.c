#include "../include/stream_manager.h"
#include <stdlib.h>
#include <stdio.h>
#include <process.h>


bool streamm_is_initialized = false;
List Resources              = { 0 };
List Sessions               = { 0 };

void session_run(void* ptr);


// definir formato de stream, como H.264, H.265, VP8, VP9, AV1, etc...
//   apos implemmentacao basica, disponibilizar definir e consulta de detalhes, como tamanho do pacote, fragmentacao, etc...
void streamm_initialize()
{
	if (streamm_is_initialized) return;

	list_init(&Resources,sizeof(StreamResource));

	// ...

	streamm_is_initialized = true;
}


// fonte pode ser arquivo, camera, ou outro stream
//    normalmente são registrados recursos disponiveis na inicializacao
//    fonte pode ser outro stream, aceita somente frame a frame
//    grupo 
void streamm_create_resource(ResourceType type, char* address, StreamBufferCalback callback)
{
	StreamResource* res = (StreamResource*)malloc(sizeof(StreamResource));
	res->Type = type;

	string_init(&res->Address);
	string_append(&res->Address, address);
	res->Callback = callback;

	list_add(&Resources, res, sizeof(StreamResource));
}


// obter lista de recursos de stream
//    retorna lista de recursos disponiveis, normalmente é uma consulta que um client faz.
List* streamm_get_resources()
{
	return &Resources;
}


// criar sessao de stream
//    cria uma sessao de stream para um usuario
//       inicia buffer de stream com posicao zero, se for um stream de video finito
//           aceita comandos de controle de fluxo, normalmente play, pause, stop
//       se for um stream infinito, inicia buffer e começa a enviar dados assim que o client confirmar recebimento

// para cada sessao de stream, acessa cabecalho da stream e envia ela primeiro, apos isso, envia proximos frames
//    controla fluxo de cada pacote, tambem fluxo de frames, alem do timestamp de cada frame.
// para versao 1.0 ,o controle de fluxo é simples, envia o proximo frame a cada requisicao do client
//    para versao posteriores, implementar buffer de envio, fragmentar frames, para pacotes pequenos, apropriados para UDP.

// existe um controle de idletime, se o client nao requisitar dados por um tempo, a sessao é encerrada
// comando para encerrar sessao de stream manualmente

// Parametros
//    UID               -> client forneca para controlar fonte e saber o destino
//    selected_resource -> recurso selecionado, pode ser arquivo, camera, outro stream
//    setting           -> parametros de configuracao do stream, como bitrate, fps, resolucao, codec, se envia frame completo ou fragmentado
//    Callback          -> envia dados do stream. tem o frame, se for fragmento a posição, o timestamp do frame, se é continuo, etc...
//                         envia cabecallho do H.264 ou H.265; apos isso, envia frames, ou fragmentos de frames, se requerido por setting.         
void streamm_create_session(char* uid, StreamBufferCalback dispatch_callback)
{
	// no callback retorna partes da stream
	// montar execucao com o websocket, enviar os fragmentos sem nenhum tratamento agora, depois fazer o gerenciamento dos fragmentos

	StreamSession* session = (StreamSession*)malloc(sizeof(StreamSession));
	string_init(&session->UID);
	string_append(&session->UID, uid);

	session->DispatchCallback = dispatch_callback;
	session->IsRunning        = true;
	session->RunThread        = _beginthread(session_run, 0, (void*)session);

}



void session_run(void* ptr)
{
	StreamSession* session = (StreamSession*)ptr;

	while (session->IsRunning)
	{
		// preparar e enviar cabecalho inicial

		// esperar retorno??


	}
}



void streamm_append(char* uid)
{

}

// criar outras funcoes auxiliares, como: start, stop, pause, resume, back, forward, release session, etc...