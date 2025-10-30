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


	int concod_codec_version(const char* path);
	MediaBuffer* concod_read_init_segment(const VideoInitData* vid);
	MediaBuffer* concod_read_frame(const FrameIndex* frame, FILE* src);
	FrameIndexList* concod_index_frames(const char* path);
	void concod_display_frame_index(FrameIndexList* frames);
	uint_fast8_t* concod_convert_avcc_to_annexb(FrameIndex* frame, size_t* annexb_size);


	MediaSourceSession* media_sim_create(int width, int height);
	void media_sim_init_segment(MediaSourceSession* source, MediaBuffer* data);
	void media_sim_feed(MediaSourceSession* source, MediaBuffer* data);
	void media_sim_release(MediaSourceSession** source);


#ifdef __cplusplus
}
#endif

#endif /* MEDIA_FRAGMENTER */