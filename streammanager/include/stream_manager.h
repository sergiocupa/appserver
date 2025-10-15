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


#ifndef STREAM_MANAGER_H
#define STREAM_MANAGER_H

#ifdef __cplusplus
extern "C" {
#endif

    #include "platform.h"
    #include "stringlib.h"
    #include "listlib.h"

	typedef struct _StreamBuffer
	{
		ulong FrameCount;
		ulong BlockCount;
		int   Length;
		byte* Data;
	}
	StreamBuffer;

	typedef enum _ResourceType
	{
		None   = 0,
		File   = 1,
		Device = 2,
		Stream = 3
	}
	ResourceType;

	typedef struct _StreamResource
	{
		ResourceType        Type;
		String              Address;
		StreamBufferCalback Callback;
	}
	StreamResource;

	typedef struct _StreamSession
	{
		bool                IsRunning;
		String              UID;
		void*               RunThread;
		StreamBufferCalback DispatchCallback;
	}
	StreamSession;


	typedef void (*StreamBufferCalback) (StreamBuffer* frame);


	//typedef struct StreamManager
	//{
	//	int StreamID;
	//	int Width;
	//	int Height;
	//	int FPS;
	//	int Bitrate;
	//	string* Codec;
	//	string* URL;
	//	ResourceBuffer* Buffer;
	//	AppClientInfo* Client; // Client dono do stream
	//	int ReferenceCount; // Quantos clientes estão usando este stream
	//	struct StreamManager* Next;
	//} StreamManager;


	StreamManager* stream_manager_create(int stream_id, int width, int height, int fps, int bitrate, const char* codec, const char* url, AppClientInfo* client);
	void stream_manager_free(StreamManager* manager);
	void stream_manager_add_reference(StreamManager* manager);
	void stream_manager_remove_reference(StreamManager** manager);
	StreamManager* stream_manager_find_by_id(StreamManager* head, int stream_id);
	StreamManager* stream_manager_find_by_client(StreamManager* head, AppClientInfo* client);
	void stream_manager_print(StreamManager* manager);


#ifdef __cplusplus
}
#endif

#endif /* STREAM_MANAGER */