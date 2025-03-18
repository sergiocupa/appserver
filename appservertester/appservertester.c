#include "appserver.h"
#include "../src/utils/message_parser.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <stdint.h>


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

	return 0;
}



int main()
{
	//const char* UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/134.0.0.0 Safari/537.36";
	//int dl = strlen(UA);

	//MessageField* field = message_field_create(true);
	//message_field_param_add(UA, 0, (dl-1), 0,false, &field->Param);

	//field = message_field_release(field);


	FunctionBindList* bind = bind_list_create();
	bind_list_add(bind, "service/videoplayer", video_player, true);
	bind_list_add(bind, "service/videolist", video_list, false);
	bind_list_add(bind, "index", app_root, true);
	bind_list_add(bind, "service/login", app_login, true);

	AppServerInfo* server = appserver_create("video-service", 1234, "app", bind);

	getchar();
	return 0;
}