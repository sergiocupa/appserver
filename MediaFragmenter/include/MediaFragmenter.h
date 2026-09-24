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


#ifndef MEDIA_FRAGMENTER_H
#define MEDIA_FRAGMENTER_H

#ifdef __cplusplus
extern "C" {
#endif

    #include "../src/Mp4Diag.h"
    #include "../src/MediaFragmenterType.h"


	int load_bmp_manual(const char* path, uint8_t** pixels, int* w, int* h);
	int rgb_to_yuv(uint8_t* rgb_pixels, int w, int h, uint8_t** y, uint8_t** u, uint8_t** v, int* stride_y, int* stride_u, int* stride_v);



	int h26x_decode_frames(DecoderInstance* codec, MediaBuffer* input, ImagePlaneList* images);
	int h26x_decoder_release(DecoderInstance** decode);
	DecoderInstance* h26x_decoder_create(int codec);
	int h26x_create_annexb(VideoMetadata* meta, MediaBuffer* output);
	int h26x_put_single_frame(FILE* f, FrameIndex* frame, VideoMetadata* meta, MediaBuffer* output);
	int h26x_put_fragment(FILE* f, FrameIndexList* frame_list, double timeline_offset, double timeline_fragment_duration, int frame_offset, int frame_length, int include_sps_pps, H264FragmentFormat format, MediaBuffer* output);



	// Manipuladores de MP4
	FrameIndexList* mp4builder_get_frames(const char* path);
	int mp4builder_create_init(VideoMetadata* metadata, MP4InitConfig* config, MediaBuffer* output);
	int mp4builder_create_fragment(FILE* f, FrameIndexList* frame_list, double timeline_offset, double timeline_fragment_duration, int frame_offset, int frame_length, MP4FragmentInfo* frag_info, MediaBuffer* output);



	// Visualizador


	char* dash_create_mpd(VideoMetadata* meta, FrameIndexList* frames, double fragment_duration_sec, size_t* output_length);




#ifdef __cplusplus
}
#endif

#endif /* MEDIA_FRAGMENTER */