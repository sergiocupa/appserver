#include "FileUtil.h"


uint8_t read8(FILE* f)
{
    uint8_t b;
    if (fread(&b, 1, 1, f) != 1) return 0;
    return b;
}

uint16_t read16(FILE* f)
{
    uint8_t b[2];
    if (fread(b, 1, 2, f) < 2) return 0;

    return (b[0] << 8) | b[1];
}

uint32_t read32(FILE* f) 
{
    uint8_t b[4];
    if (fread(b, 1, 4, f) < 4) return 0;
    return (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];
}

uint64_t read64(FILE* f) 
{
    uint8_t b[8];
    if (fread(b, 1, 8, f) < 8) return 0;
    return ((uint64_t)b[0] << 56) | ((uint64_t)b[1] << 48) | ((uint64_t)b[2] << 40) |
        ((uint64_t)b[3] << 32) | ((uint64_t)b[4] << 24) | ((uint64_t)b[5] << 16) |
        ((uint64_t)b[6] << 8) | b[7];
}

uint32_t read_n(uint8_t* buf, int n) 
{
    uint32_t val = 0;
    for (int j = 0; j < n; j++) {
        val = (val << 8) | buf[j];
    }
    return val;
}


