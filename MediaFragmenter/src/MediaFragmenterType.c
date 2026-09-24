#include "../../appserver/submodules/xplatbase/Xplatbase/Xplatbase/src/memory_pool.h"
#include "../../appserver/submodules/xplatbase/Xplatbase/Xplatbase/src/string_handler.h"
#include "../include/MediaFragmenter.h"
#include "MediaFragmenterType.h"
#include <stdlib.h>
#include <memory.h>
#include <string.h>
#include <assert.h>


static const uint32_t POW10_32[] = { 1000000000, 100000000, 10000000, 1000000, 100000, 10000, 1000, 100, 10, 1 };

static const uint64_t POW10_64[] = 
{
    10000000000000000000ULL, 1000000000000000000ULL, 100000000000000000ULL, 10000000000000000ULL, 1000000000000000ULL,
    100000000000000ULL, 10000000000000ULL, 1000000000000ULL, 100000000000ULL, 10000000000ULL, 1000000000ULL, 
    100000000ULL, 10000000ULL, 1000000ULL, 100000ULL, 10000ULL, 1000ULL, 100ULL, 10ULL, 1ULL
};


void mbuffer_resize(MediaBuffer* buffer, int length)
{
    if ((buffer->Size + length) >= buffer->Max)
    {
        buffer->Max = (buffer->Max + length) * 2;
        char* content = (char*)memop_realloc_raw(buffer->Data, buffer->Max * sizeof(char));
        if (!content) assert(0);
        buffer->Data = (uint_fast8_t*)content;
    }
}





// imagep_list_add/init/new/release movidos para codecs/media/codec_video.c (camada de codecs).





void frame_list_init(FrameList* nalus, int initial_count)
{
    nalus->Max   = initial_count;
    nalus->Count = 0;
    nalus->Items = (H26XFrame**)memop_alloc_raw(nalus->Max * sizeof(H26XFrame*));
}
FrameList* frame_list_new(int initial_count)
{
    FrameList* nalus = memop_alloc_raw(sizeof(FrameList));
    nalus->Max = initial_count;
    nalus->Count = 0;
    nalus->Items = (H26XFrame**)memop_alloc_raw(nalus->Max * sizeof(H26XFrame*));
    return nalus;
}
void frame_list_add(FrameList* frames, uint8_t* data, uint32_t size, int is_key_frame)
{
    int sz = frames->Count + 1;
    if (sz >= frames->Max)
    {
        frames->Max *= 2;
        frames->Items = (H26XFrame**)memop_realloc_raw((H26XFrame**)frames->Items, frames->Max * sizeof(NAL*));
    }

    frames->Items[frames->Count]              = (H26XFrame*)memop_alloc_raw(sizeof(H26XFrame));
    frames->Items[frames->Count]->AnnexB.Data = data;
    frames->Items[frames->Count]->AnnexB.Size = size;
    frames->Items[frames->Count]->Index       = frames->Count;
    frames->Items[frames->Count]->IsKeyframe  = is_key_frame;
    frames->Count++;
}
void frame_list_release(FrameList** frames)
{
    int ix = 0;
    while (ix < (*frames)->Count)
    {
        memop_free_raw((*frames)->Items[ix]->AnnexB.Data);
        memop_free_raw((*frames)->Items[ix]);
        ix++;
    }
    memop_free_raw((*frames)->Items);

    *frames = 0;
}



void nal_list_init(NALList* nalus, int initial_count)
{
    nalus->Max   = initial_count;
    nalus->Count = 0;
    nalus->Items = (NAL**)memop_alloc_raw(nalus->Max * sizeof(NAL*));
}
NALList* nal_list_new(int initial_count)
{
    NALList* nalus = memop_alloc_raw(sizeof(NALList));
    nalus->Max   = initial_count;
    nalus->Count = 0;
    nalus->Items = (NAL**)memop_alloc_raw(nalus->Max * sizeof(NAL*));
    return nalus;
}
void nal_list_add(NALList* nalus, uint8_t* data, uint32_t size, uint8_t type)
{
    int sz = nalus->Count + 1;
    if (sz >= nalus->Max)
    {
        nalus->Max *= 2;
        nalus->Items = (NAL**)memop_realloc_raw((NAL**)nalus->Items, nalus->Max * sizeof(NAL*));
    }

    nalus->Items[nalus->Count]       = (NAL*)memop_alloc_raw(sizeof(NAL));
    nalus->Items[nalus->Count]->Data = data;
    nalus->Items[nalus->Count]->Size = size;
    nalus->Items[nalus->Count]->Type = type;
    nalus->Count++;
}
void nal_list_release(NALList** nalus)
{
    int ix = 0;
    while (ix < (*nalus)->Count)
    {
        memop_free_raw((*nalus)->Items[ix]->Data);
        memop_free_raw((*nalus)->Items[ix]);
        ix++;
    }
    memop_free_raw((*nalus)->Items);

    *nalus = 0;
}



void mnalu_list_init(NALUIndexList* nalus, int initial_count)
{
    nalus->Max   = initial_count;
    nalus->Count = 0;
    nalus->Items = (NALUIndex**)memop_alloc_raw(nalus->Max * sizeof(NALUIndex*));
}

void mnalu_list_add(NALUIndexList* nalus, uint64_t offset, uint32_t size, uint8_t type)
{
    int sz = nalus->Count + 1;
    if (sz >= nalus->Max)
    {
        nalus->Max   *= 2;
        nalus->Items  = (NALUIndex**)memop_realloc_raw((NALUIndex**)nalus->Items, nalus->Max * sizeof(NALUIndex*));
    }

    nalus->Items[nalus->Count]         = (NALUIndex*)memop_alloc_raw(sizeof(NALUIndex));
    nalus->Items[nalus->Count]->Offset = offset;
    nalus->Items[nalus->Count]->Size   = size;
    nalus->Items[nalus->Count]->Type   = type;
    nalus->Count++;
}

void mnalu_list_release(NALUIndexList* nalus)
{
    int ix = 0;
    while (ix < nalus->Count)
    {
        memop_free_raw(nalus->Items[ix]);
        ix++;
    }
    memop_free_raw(nalus->Items);
}



FrameIndex* mframe_new(uint64_t off_set)
{
    FrameIndex* db = (FrameIndex*)memop_alloc_raw(sizeof(FrameIndex));
    db->Offset = off_set;
    db->Size   = 0;
    mnalu_list_init(&db->Nals,64);
    return db;
}
void mframe_release(FrameIndex** frame)
{
    if (*frame)
    {
        mnalu_list_release(&(*frame)->Nals);
        memop_free_raw(*frame);
        *frame = 0;
    }
}



FrameIndexList* mframe_list_new(uint64_t initial_count)
{
    FrameIndexList* db = (FrameIndexList*)memop_alloc_raw(sizeof(FrameIndexList));
    db->Max    = initial_count;
    db->Count  = 0;
    db->Frames = (FrameIndex**)memop_alloc_raw(db->Max * sizeof(FrameIndexList*));
    return db;
}

void mframe_list_add(FrameIndexList* list, FrameIndex* frame)
{
    int sz = list->Count + 1;
    if (sz >= list->Max)
    {
        list->Max    = (sz + list->Max) * 2;
        list->Frames = (FrameIndex**)memop_realloc_raw((FrameIndex**)list->Frames, list->Max * sizeof(FrameIndex*));
    }
    list->Frames[list->Count] = frame;
    list->Count++;
}

void mframe_list_release(FrameIndexList** list)
{
    if (*list)
    {
        int ix = 0;
        while (ix < (*list)->Count)
        {
            mframe_release(&(*list)->Frames[ix]);
            ix++;
        }
        memop_free_raw((*list)->Frames);
        memop_free_raw(*list);
        *list = 0;
    }
}



void mbuffer_append_by_file(MediaBuffer* buffer, FILE* src, uint64_t file_offset, uint64_t size)
{
    fseek(src, size, SEEK_SET);
    mbuffer_resize(buffer, size);
    fread(buffer->Data, 1, size, src);
    buffer->Size += size;
}


int mbuffer_ensure(MediaBuffer* buffer, size_t need)
{
    if (!buffer) return 0;
    if (buffer->Data && (size_t)buffer->Max >= need) return 1;   // ja cabe: reusa

    // +1 para os pontos que gravam um terminador em Data[Size] (mbuffer_append).
    uint_fast8_t* grown = (uint_fast8_t*)memop_realloc_raw(buffer->Data, need + 1);
    if (!grown) return 0;                                        // buffer antigo continua valido
    buffer->Data = grown;
    buffer->Max  = (int)need;
    return 1;
}

void mbuffer_init(MediaBuffer* buffer)
{
    buffer->Max  = 100;
    buffer->Size = 0;
    buffer->Data = (uint_fast8_t*)memop_alloc_raw(buffer->Max * sizeof(char));
}

void mbuffer_prepare(MediaBuffer* buffer, uint64_t size)
{
    buffer->Max  = size;
    buffer->Size = 0;
    buffer->Data = (uint_fast8_t*)memop_alloc_raw(buffer->Max * sizeof(char));
}

MediaBuffer* mbuffer_new()
{
    MediaBuffer* db = (MediaBuffer*)memop_alloc_raw(sizeof(MediaBuffer));
    db->Max  = 100;
    db->Size = 0;
    db->Data = (uint_fast8_t*)memop_alloc_raw(db->Max * sizeof(char));
    return db;
}

MediaBuffer* mbuffer_create(int size)
{
    MediaBuffer* db = (MediaBuffer*)memop_alloc_raw(sizeof(MediaBuffer));
    db->Max  = size;
    db->Size = 0;
    db->Data = (uint_fast8_t*)memop_alloc_raw(db->Max * sizeof(char));
    return db;
}

void mbuffer_release(MediaBuffer** buffer)
{
    if (*buffer)
    {
        memop_free_raw((*buffer)->Data);
        memop_free_raw(*buffer);
        *buffer = 0;
    }
}

void mbuffer_append(MediaBuffer* buffer, const uint_fast8_t* data, const int length)
{
    mbuffer_resize(buffer, length);

    memop_copy_raw(buffer->Data, data, length);
    buffer->Size += length;
    buffer->Data[buffer->Size] = 0;
}

void mbuffer_append_string(MediaBuffer* buffer, const char* data)
{
    int size = string_length_raw(data);

    mbuffer_resize(buffer, size);

    memop_copy_raw(buffer->Data, data, size);
    buffer->Size += size;
    buffer->Data[buffer->Size] = 0;
}

void mbuffer_append_uint8(MediaBuffer* buffer, uint32_t value)
{
    mbuffer_resize(buffer, 1);

    buffer->Data[buffer->Size]   = value;
    buffer->Data[buffer->Size+1] = 0;
}

void mbuffer_append_uint16(MediaBuffer* buffer, uint32_t value)
{
    mbuffer_resize(buffer, 2);

    buffer->Data[buffer->Size]   = value >> 8;
    buffer->Data[buffer->Size+1] = value & 0xFF;
    buffer->Data[buffer->Size+2] = 0;
}


void mbuffer_append_uint32_string(MediaBuffer* buffer, uint32_t value)
{
    mbuffer_resize(buffer, 11);

    char* p = buffer->Data;
    int started = 0;

    for (int i = 0; i < 10; ++i) 
    {
        uint32_t divisor = POW10_32[i];
        uint32_t digit = 0;
        while (value >= divisor) 
        {
            value -= divisor;
            digit++;
        }

        if (digit != 0 || started || i == 9) 
        {
            *p++ = (char)('0' + digit);
            buffer->Size++;
            started = 1;
        }
    }

    *p = '\0';
}


void mbuffer_append_uint64_string(MediaBuffer* buffer, uint64_t value)
{
    mbuffer_resize(buffer, 20);

    char* p = buffer->Data;
    int started = 0;

    for (int i = 0; i < 20; ++i) 
    {
        uint64_t divisor = POW10_64[i];
        uint8_t digit = 0;

        while (value >= divisor) 
        {
            value -= divisor;
            digit++;
        }

        if (digit != 0 || started || i == 19) 
        {
            *p++ = (char)('0' + digit);
            buffer->Size++;
            started = 1;
        }
    }

    *p = '\0';
}


void mbuffer_append_uint32(MediaBuffer* buffer, uint32_t value)
{
    mbuffer_resize(buffer, 4);

    buffer->Data[buffer->Size]     = value >> 24;
    buffer->Data[buffer->Size + 1] = value >> 16;
    buffer->Data[buffer->Size + 2] = value >> 8;
    buffer->Data[buffer->Size + 3] = value & 0xFF;
    buffer->Data[buffer->Size + 4] = 0;
}

void mbuffer_append_uint64(MediaBuffer* buffer, uint64_t value)   // era uint32_t: >> 32..56 indefinido
{
    mbuffer_resize(buffer, 8);

    buffer->Data[buffer->Size]     = value >> 56;
    buffer->Data[buffer->Size + 1] = value >> 48;
    buffer->Data[buffer->Size + 2] = value >> 40;
    buffer->Data[buffer->Size + 3] = value >> 32;
    buffer->Data[buffer->Size + 4] = value >> 24;
    buffer->Data[buffer->Size + 5] = value >> 16;
    buffer->Data[buffer->Size + 6] = value >> 8;
    buffer->Data[buffer->Size + 7] = value & 0xFF;
    buffer->Data[buffer->Size + 8] = 0;
}

void mbuffer_box_from_buf(MediaBuffer* db, const char* type, MediaBuffer* content)
{
    uint32_t size = (uint32_t)(8 + content->Size);
    mbuffer_append_uint32(db, size);
    mbuffer_append_string(db, type);

    if (content->Size)
    {
        mbuffer_append(db, content->Data, content->Size);
    }
}

