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


#include "MediaFragmenter.h"


int main()
{
	const char* path = "e:/AmostraVideo/sample-3.mp4";

    FrameIndexList* list = mp4builder_get_frames(path);
    if (!list) return;


    FILE* file = fopen(path, "rb");
    if (!file) {
        perror("Erro ao abrir arquivo");
        return;
    }

    MediaSourceSession* session = media_sim_create(list->Metadata.Width / 2, list->Metadata.Height / 2);
    //MediaSourceSession* session = media_sim_create(list->Metadata.Width, list->Metadata.Height);


    MediaBuffer mi;
    mi.Data = h26x_create_annexb(&list->Metadata, &mi.Size);
    media_sim_feed(session, &mi);
    free(mi.Data);

    int res = 0;

    // Loop por frames
    for (uint32_t i = 0; i < list->Count; i++)
    {
        FrameIndex* frame = list->Frames[i];

        MediaBuffer mb;
        res = h26x_create_single_frame(file, frame, &list->Metadata, &mb);

        if (!res)
        {
            int fe = media_sim_feed(session, &mb);
            free(mb.Data);
        }

        Sleep(33);
    }

    // Fecha sessão
    // media_sim_destroy(session);
    fclose(file);
}

