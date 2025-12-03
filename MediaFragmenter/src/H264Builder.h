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


#ifndef H264_BUILDER_H
#define H264_BUILDER_H

#ifdef __cplusplus
extern "C" {
#endif

    #include "../src/Mp4Diag.h"
    #include "../src/MediaFragmenterType.h"


    uint8_t* h264_create_annexb(VideoMetadata* meta, int* length);
    int h264_create_single_frame(FILE* f, FrameIndex* frame, VideoMetadata* metadata, MediaBuffer* output);
    int h264_create_fragment(FILE* f, FrameIndexList* frame_list, double timeline_offset, double timeline_fragment_duration, int frame_offset, int frame_length, int include_sps_pps, H264FragmentFormat format, MediaBuffer* output);



#ifdef __cplusplus
}
#endif

#endif /* H264_BUILDER */