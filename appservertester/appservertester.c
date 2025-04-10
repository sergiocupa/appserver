#include "appserver.h"


void* app_login(Message* message)
{

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

void* video_player(Message* message)
{

    return 0;
}



MessageEmitterCalback Notification;
void Notification_Result(ResourceBuffer* object)
{
    printf("Notificou...");
}



int main()
{

    FunctionBindList* bind = bind_list_create();
    app_add_receiver(bind, "service/videoplayer", video_player, true);
    app_add_web_resource(bind, "service/videolist", video_list, false);
    app_add_receiver(bind, "index", app_root, true);
    app_add_receiver(bind, "service/login", app_login, true);
    Notification = app_add_emitter(bind, "service/notification");

    AppServerInfo* server = appserver_create("video-service", 1234, "api", bind);


    int data = 12344;

    Notification(&data, Notification_Result);



	getchar();
	return 0;
}