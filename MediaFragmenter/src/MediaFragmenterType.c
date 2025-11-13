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
        char* content = (char*)realloc(buffer->Data, buffer->Max * sizeof(char));
        if (!content) assert(0);
        buffer->Data = (uint_fast8_t*)content;
    }
}


void mnalu_list_init(NALUIndexList* nalus, int initial_count)
{
    nalus->Max   = initial_count;
    nalus->Count = 0;
    nalus->Items = (NALUIndex**)malloc(nalus->Max * sizeof(NALUIndex*));
}

void mnalu_list_add(NALUIndexList* nalus, uint64_t offset, uint32_t size, uint8_t type)
{
    int sz = nalus->Count + 1;
    if (sz >= nalus->Max)
    {
        nalus->Max   *= 2;
        nalus->Items  = (NALUIndex**)realloc((NALUIndex**)nalus->Items, nalus->Max * sizeof(NALUIndex*));
    }

    nalus->Items[nalus->Count]         = (NALUIndex*)malloc(sizeof(NALUIndex));
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
        free(nalus->Items[ix]);
        ix++;
    }
    free(nalus->Items);
}



FrameIndex* mframe_new(uint64_t off_set)
{
    FrameIndex* db = (FrameIndex*)malloc(sizeof(FrameIndex));
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
        free(*frame);
        *frame = 0;
    }
}



FrameIndexList* mframe_list_new(uint64_t initial_count)
{
    FrameIndexList* db = (FrameIndexList*)malloc(sizeof(FrameIndexList));
    db->Max    = initial_count;
    db->Count  = 0;
    db->Frames = (FrameIndex**)malloc(db->Max * sizeof(FrameIndexList*));
    return db;
}

void mframe_list_add(FrameIndexList* list, FrameIndex* frame)
{
    int sz = list->Count + 1;
    if (sz >= list->Max)
    {
        list->Max    = (sz + list->Max) * 2;
        list->Frames = (FrameIndex**)realloc((FrameIndex**)list->Frames, list->Max * sizeof(FrameIndex*));
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
        free((*list)->Frames);
        free(*list);
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


void mbuffer_init(MediaBuffer* buffer)
{
    buffer->Max  = 100;
    buffer->Size = 0;
    buffer->Data = (uint_fast8_t*)malloc(buffer->Max * sizeof(char));
}

void mbuffer_prepare(MediaBuffer* buffer, uint64_t size)
{
    buffer->Max  = size;
    buffer->Size = 0;
    buffer->Data = (uint_fast8_t*)malloc(buffer->Max * sizeof(char));
}

MediaBuffer* mbuffer_new()
{
    MediaBuffer* db = (MediaBuffer*)malloc(sizeof(MediaBuffer));
    db->Max  = 100;
    db->Size = 0;
    db->Data = (uint_fast8_t*)malloc(db->Max * sizeof(char));
    return db;
}

MediaBuffer* mbuffer_create(int size)
{
    MediaBuffer* db = (MediaBuffer*)malloc(sizeof(MediaBuffer));
    db->Max  = size;
    db->Size = 0;
    db->Data = (uint_fast8_t*)malloc(db->Max * sizeof(char));
    return db;
}

void mbuffer_release(MediaBuffer** buffer)
{
    if (*buffer)
    {
        free((*buffer)->Data);
        free(*buffer);
        *buffer = 0;
    }
}

void mbuffer_append(MediaBuffer* buffer, const uint_fast8_t* data, const int length)
{
    mbuffer_resize(buffer, length);

    memcpy(buffer->Data, data, length);
    buffer->Size += length;
    buffer->Data[buffer->Size] = 0;
}

void mbuffer_append_string(MediaBuffer* buffer, const char* data)
{
    int size = strlen(data);

    mbuffer_resize(buffer, size);

    memcpy(buffer->Data, data, size);
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

void mbuffer_append_uint64(MediaBuffer* buffer, uint32_t value)
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

