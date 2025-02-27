#include "appserver.h"
#include <stdio.h>


int main()
{

	AppServerInfo* server = appserver_create(1234, "api");

	getchar();
	return 0;
}