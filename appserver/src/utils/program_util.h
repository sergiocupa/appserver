//  MIT License – Modified for Mandatory Attribution
//  
//  Copyright(c) 2025 Sergio Paludo
//  
//  Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files, 
//  to use, copy, modify, merge, publish, distribute, and sublicense the software, including for commercial purposes, provided that:
//  
//  01. The original author’s credit is retained in all copies of the source code;
//  02. The original author’s credit is included in any code generated, derived, or distributed from this software, including templates, libraries, or code - generating scripts.
//  
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED.


#ifndef PROGRAM_UTIL_H
#define PROGRAM_UTIL_H

#ifdef __cplusplus
extern "C" {
#endif

    #include "platform.h"
    #include <stdio.h>

    #ifdef PLATFORM_WIN
        #include <windows.h>

        CRITICAL_SECTION fileCriticalSection;

    #else 
        #include <unistd.h>
        #include <pthread.h>
        #include <sys/stat.h>

        pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;
    #endif 


	static inline bool program_get_exec_path(const char* path)
	{
        #ifdef PLATFORM_WIN
            return GetModuleFileName(NULL, path, MAX_PATH);
        #else 
            ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
            if (len != -1) 
            {
                path[len] = '\0';
                retturn true;
            }
            return false;
        #endif 
	}

    static inline bool program_get_path(char* path)
    {
        #ifdef PLATFORM_WIN
            if (GetModuleFileName(NULL, path, MAX_PATH))
            {
                char* last_backslash = strrchr(path, '\\');
                if (last_backslash) { *last_backslash = '\0'; }
                return true;
            }
            return false;
        #else 
            ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
            if (len != -1)
            {
                path[len] = '\0';

                char* last_slash = strrchr(path, '/');
                if (last_slash) { *last_slash = '\0';  }

                retturn true;
            }
            return false;
        #endif 
    }


#ifdef __cplusplus
}
#endif

#endif /* PROGRAM_UTIL */




