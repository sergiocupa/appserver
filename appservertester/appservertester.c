#include "appserver.h"
#include "../src/utils/message_parser.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <stdint.h>
#include "../appserver/submodules/yason/yason/include/yason.h"


void* video_player(Message* message)
{

	return 0;
}

void* video_list(Message* message)
{

	return 0;
}

void* app_root(Message* message)
{

	return 0;
}

void* app_login(Message* message)
{
	Element* obj = (Element*)message->Object;

	return 0;
}


MessageMatchEmitterCalback Notificar;



int main()
{

	const char* utf8_str = "你";

	int leee = strlen(utf8_str);

	FunctionBindList* bind = bind_list_create();
	bind_list_add_web_resource(bind, "service/videoplayer", video_player);
	bind_list_add_receiver(bind, "service/videolist", video_list, false);
	bind_list_add_web_resource(bind, "index", app_root);
	bind_list_add_receiver(bind, "service/login", app_login, false);

	Notificar = bind_list_add_emitter(bind, "service/notificar");


	AppServerInfo* server = appserver_create("video-service", 1234, "app", bind);

	getchar();
	return 0;
}