#include "BufferUtil.h"
#include <string.h>





void buffer_write16(uint8_t* buf, uint16_t value)
{
    buf[0] = (value >> 8) & 0xFF;
    buf[1] = value & 0xFF;
}

void buffer_write32(uint8_t* buf, uint32_t value)
{
    buf[0] = (value >> 24) & 0xFF;
    buf[1] = (value >> 16) & 0xFF;
    buf[2] = (value >> 8) & 0xFF;
    buf[3] = value & 0xFF;
}

void  buffer_write64(uint8_t* buf, uint64_t value)
{
    buf[0] = (value >> 56) & 0xFF;
    buf[1] = (value >> 48) & 0xFF;
    buf[2] = (value >> 40) & 0xFF;
    buf[3] = (value >> 32) & 0xFF;
    buf[4] = (value >> 24) & 0xFF;
    buf[5] = (value >> 16) & 0xFF;
    buf[6] = (value >> 8) & 0xFF;
    buf[7] = value & 0xFF;
}

void write_fourcc(uint8_t* buf, const char* fourcc)
{
    memcpy(buf, fourcc, 4);
}




void buffer_init(BufferReader* br, const uint8_t* data, size_t size)
{
    br->Data  = data;
    br->Size  = size;
    br->Max   = 0;
    br->Index = 0;
}

int buffer_available(BufferReader* br, size_t n)
{
    return (br->Index + n) <= br->Size;
}

uint8_t buffer_read8(BufferReader* br)
{
    if (!buffer_available(br, 1)) return 0;
    return br->Data[br->Index++];
}

uint16_t buffer_read16(BufferReader* br)
{
    if (!buffer_available(br, 2)) return 0;
    uint16_t val = (br->Data[br->Index] << 8) | br->Data[br->Index + 1];
    br->Index += 2;
    return val;
}

uint32_t buffer_read32(BufferReader* br)
{
    if (!buffer_available(br, 4)) return 0;
    uint32_t val = ((uint32_t)br->Data[br->Index] << 24) |
        ((uint32_t)br->Data[br->Index + 1] << 16) |
        ((uint32_t)br->Data[br->Index + 2] << 8) |
        ((uint32_t)br->Data[br->Index + 3]);
    br->Index += 4;
    return val;
}

uint64_t buffer_read64(BufferReader* br)
{
    if (!buffer_available(br, 8)) return 0;
    uint64_t val = ((uint64_t)br->Data[br->Index] << 56) |
        ((uint64_t)br->Data[br->Index + 1] << 48) |
        ((uint64_t)br->Data[br->Index + 2] << 40) |
        ((uint64_t)br->Data[br->Index + 3] << 32) |
        ((uint64_t)br->Data[br->Index + 4] << 24) |
        ((uint64_t)br->Data[br->Index + 5] << 16) |
        ((uint64_t)br->Data[br->Index + 6] << 8) |
        (br->Data[br->Index + 7]);

    br->Index += 8;
    return val;
}

void buffer_skip(BufferReader* br, size_t n)
{
    if (br->Index + n <= br->Size)
    {
        br->Index += n;
    }
    else {
        br->Index = br->Size;
    }
}

size_t buffer_tell(BufferReader* br)
{
    return br->Index;
}

void buffer_seek(BufferReader* br, size_t pos)
{
    if (pos <= br->Size) {
        br->Index = pos;
    }
}

const uint8_t* buffer_current(BufferReader* br)
{
    return br->Data + br->Index;
}

int buffer_eof(BufferReader* br)
{
    return br->Index >= br->Size;
}
