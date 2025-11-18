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