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

    #include "../src/MediaFragmenterType.h"


	int load_bmp_manual(const char* path, uint8_t** pixels, int* w, int* h);
	int rgb_to_yuv(uint8_t* rgb_pixels, int w, int h, uint8_t** y, uint8_t** u, uint8_t** v, int* stride_y, int* stride_u, int* stride_v);

	// Manipuladores de H264 ou H265
	uint8_t* h26x_create_annexb(VideoMetadata* meta, int* length);
	int h26x_create_single_frame(FILE* f, FrameIndex* frame, VideoMetadata* metadata, MediaBuffer* output);
	int h264_create_fragment(FILE* f, FrameIndexList* frame_list, double timeline_offset, double timeline_fragment_duration, int frame_offset, int frame_length, int include_sps_pps, H264FragmentFormat format, MediaBuffer* output);

	// Manipuladores de MP4
	FrameIndexList* mp4builder_get_frames(const char* path);


	// Visualizador
	MediaSourceSession* media_sim_create(int width, int height);
	int media_sim_init_segment(MediaSourceSession* source, MediaBuffer* data);
	int media_sim_feed(MediaSourceSession* source, MediaBuffer* data);
	void media_sim_release(MediaSourceSession** source);
	void medias_waiting(VideoOutput* v);




#ifdef __cplusplus
}
#endif

#endif /* MEDIA_FRAGMENTER */