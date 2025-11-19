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


#ifndef ACTIVITY_BINDER_H
#define ACTIVITY_BINDER_H

#ifdef __cplusplus
extern "C" {
#endif

    #include "../server_type.h"


	FunctionBind* binder_extension_exist(FunctionBindList* binders, StringArray* prefix, String* extension);
	FunctionBind* binder_route_exist(FunctionBindList* binders, StringArray* prefix, StringArray* route, int* route_rest);
	bool binder_get_web_resource(FunctionBindList* binders, StringArray* prefix, StringArray* route, String* abs_path, ResourceBuffer* buffer);
	void binder_get_web_content_path(FunctionBindList* bind_list, StringArray* prefix, String* local);


#ifdef __cplusplus
}
#endif

#endif /* ACTIVITY_BINDER */