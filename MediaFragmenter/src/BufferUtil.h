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

    void buffer_write16(uint8_t* buf, uint16_t value);
    void buffer_write32(uint8_t* buf, uint32_t value);
    void buffer_write64(uint8_t* buf, uint64_t value);
    void write_fourcc(uint8_t* buf, const char* fourcc);

#ifdef __cplusplus
}
#endif

#endif /* BUFFER_UTIL */