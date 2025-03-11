#ifndef ACTIVITY_BINDER_H
#define ACTIVITY_BINDER_H

#ifdef __cplusplus
extern "C" {
#endif

    #include "../server_type.h"


	FunctionBind* binder_route_exist(FunctionBindList* binders, StringArray* prefix, StringArray* route);


#ifdef __cplusplus
}
#endif

#endif /* ACTIVITY_BINDER */