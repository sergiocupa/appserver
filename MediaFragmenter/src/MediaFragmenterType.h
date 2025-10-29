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


#ifndef MEDIA_FRAGMENTER_TYPE_H
#define MEDIA_FRAGMENTER_TYPE_H

#ifdef __cplusplus
extern "C" {
#endif
    #include "codec_api.h"
    #define SDL_MAIN_HANDLED
    #include "SDL2/SDL.h"
    #include <stdint.h>
    #include <stdio.h>

    #define HDOT_H264 264
    #define HDOT_H265 265

    #define NAL_TYPE(nal) ((nal) & 0x1F)             // Para H.264
    #define NAL_TYPE_HEVC(nal) (((nal) >> 1) & 0x3F) // Para H.265


    typedef struct
    {
        int Codec;
        int ProfileIdc;
        int LevelIdc;
        long Offset;
    }
    CodecInfo;


    typedef struct 
    {
        ISVCDecoder dec;
    } 
    H264Decoder;


    typedef struct 
    {
        int a;
        SDL_Window* win;
        SDL_Renderer* ren;
        SDL_Texture* tex;
    } 
    VideoOutput;


    typedef struct
    {
        uint64_t Offset;
        uint64_t Size;
        int      NalType;
        char     FrameType;   // 'I', 'P', 'B'
        double   PTS;       // timestamp relativo
    }
    FrameIndex;

    typedef struct
    {
        int          Codec; // 264 ou 265
        double       Fps;
        uint64_t     Max;
        uint64_t     Count;
        FrameIndex** Frames;
    }
    FrameIndexList;

    typedef struct _MediaBuffer
    {
        int           Max;
        int           Length;
        uint_fast8_t* Data;
    }
    MediaBuffer;

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
    }
    VideoInitData;


    typedef struct _MediaSourceSession
    {
        VideoOutput* Output;
        H264Decoder* Decoder;
    }
    MediaSourceSession;


    void mbuffer_append_by_file(MediaBuffer* buffer, FILE* src, uint64_t file_offset, uint64_t size);
    FrameIndexList* mframe_list_new();
    FrameIndex* mframe_new();
    void mframe_list_add(FrameIndexList* list, uint64_t offset, uint64_t size, int nal_type, char ftype, double pts);
    void mframe_list_release(FrameIndexList** list);
    void mbuffer_resize(MediaBuffer* buffer, int length);
    void mbuffer_init(MediaBuffer* buffer);
    void mbuffer_prepare(MediaBuffer* buffer, uint64_t size);
    MediaBuffer* mbuffer_new();
    MediaBuffer* mbuffer_create(int size);
    void mbuffer_release(MediaBuffer** buffer);
    void mbuffer_append(MediaBuffer* buffer, const uint_fast8_t* data, const int length);
    void mbuffer_append_string(MediaBuffer* buffer, const char* data);
    void mbuffer_append_uint8(MediaBuffer* buffer, uint32_t value);
    void mbuffer_append_uint16(MediaBuffer* buffer, uint32_t value);
    void mbuffer_append_uint32(MediaBuffer* buffer, uint32_t value);
    void mbuffer_append_uint64(MediaBuffer* buffer, uint32_t value);
    void mbuffer_box_from_buf(MediaBuffer* db, const char* type, MediaBuffer* content);
    void mbuffer_append_uint32_string(MediaBuffer* buffer, uint32_t value);
    void mbuffer_append_uint64_string(MediaBuffer* buffer, uint64_t value);


#ifdef __cplusplus
}
#endif

#endif /* MEDIA_FRAGMENTER_TYPE */