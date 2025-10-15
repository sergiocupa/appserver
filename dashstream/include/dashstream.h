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


#ifndef DASH_STREAM_H
#define DASH_STREAM_H
#ifdef __cplusplus
extern "C" {
#endif
    #include <stdint.h>
    #include <stdlib.h>
    #include <assert.h>
    #include <stdio.h>

    #define HDOT_H264 264
    #define HDOT_H265 265

    typedef struct 
    {
        uint64_t Offset;
        uint64_t Size;
        int NalType;
        char FrameType;   // 'I', 'P', 'B'
        double PTS;       // timestamp relativo
    } 
    FrameIndex;

    typedef struct 
    {
        int Codec; // 264 ou 265
        double Fps;
        uint64_t     Max;
        uint64_t     Count;
        FrameIndex** Frames;
    }
    FrameIndexList;

	typedef struct _DashDataBuffer
	{
		int           MaxLength;
		int           Length;
		uint_fast8_t* Data;
	}
	DashDataBuffer;

    typedef struct 
    {
        FILE* fp;
        int hdot_version;  // 264 = H.264, 265 = H.265
        int width;
        int height;
        float fps;
        uint32_t timescale; // ex: 1000
        uint8_t* vps; // HEVC
        size_t vps_size;
        uint8_t* sps;
        size_t sps_size;
        uint8_t* pps;
        size_t pps_size;
    } VideoInitData;


	void dashstr_buffer_release(DashDataBuffer** buffer);
	DashDataBuffer* dash_create_init_segment();
	DashDataBuffer* dash_create_fragment(uint8_t* frame, size_t size);


    FrameIndexList* index_frames_full(const char* path);



#ifdef __cplusplus
}
#endif

#endif /* DASH_STREAM */