#include "appserver.h"
#include <stdio.h>
#include <string.h>



void* video_player(Message* message)
{

	return 0;
}

void* video_list(Message* message)
{

	return 0;
}


int main()
{
	FunctionBindList* bind = bind_list_create();
	bind_list_add(bind, "service/videoplayer", video_player);
	bind_list_add(bind, "service/videolist", video_list);

	AppServerInfo* server = appserver_create("video-service", 1234, "api", bind);

	getchar();
	return 0;
}