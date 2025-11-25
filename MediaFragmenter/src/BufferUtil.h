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


#ifndef BUFFER_UTIL_H
#define BUFFER_UTIL_H

#ifdef __cplusplus
extern "C" {
#endif

    #include <stdint.h>


    typedef struct
    {
        size_t Index;
        size_t Max;
        size_t Size;
        const uint8_t* Data;
    }
    BufferReader;


    void buffer_write16(uint8_t* buf, uint16_t value);
    void buffer_write32(uint8_t* buf, uint32_t value);
    void buffer_write64(uint8_t* buf, uint64_t value);
    void write_fourcc(uint8_t* buf, const char* fourcc);


    void buffer_init(BufferReader* br, const uint8_t* data, size_t size);
    int buffer_available(BufferReader* br, size_t n);
    uint8_t buffer_read8(BufferReader* br);
    uint16_t buffer_read16(BufferReader* br);
    uint32_t buffer_read32(BufferReader* br);
    uint64_t buffer_read64(BufferReader* br);
    void buffer_skip(BufferReader* br, size_t n);
    size_t buffer_tell(BufferReader* br);
    void buffer_seek(BufferReader* br, size_t pos);
    const uint8_t* buffer_current(BufferReader* br);
    int buffer_eof(BufferReader* br);


#ifdef __cplusplus
}
#endif

#endif /* BUFFER_UTIL */