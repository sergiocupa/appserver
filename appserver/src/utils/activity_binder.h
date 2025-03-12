#ifndef ACTIVITY_BINDER_H
#define ACTIVITY_BINDER_H

#ifdef __cplusplus
extern "C" {
#endif

    #include "../server_type.h"


	FunctionBind* binder_route_exist(FunctionBindList* binders, StringArray* prefix, StringArray* route);
	bool binder_get_web_content(FunctionBindList* binders, StringArray* prefix, StringArray* route, String* abs_path, ResourceBuffer* buffer);
	void binder_get_web_content_path(FunctionBindList* bind_list, StringArray* prefix, String* local);


#ifdef __cplusplus
}
#endif

#endif /* ACTIVITY_BINDER */