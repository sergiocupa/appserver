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


#ifndef FILE_UTIL_H
#define FILE_UTIL_H

#ifdef __cplusplus
extern "C" {
#endif

    #include <stdio.h>
    #include <inttypes.h>

    uint8_t  read8(FILE* f);
    uint16_t read16(FILE* f);
    uint32_t read32(FILE* f);
    uint64_t read64(FILE* f);
    uint32_t read_n(uint8_t* buf, int n);
    

#ifdef __cplusplus
}
#endif

#endif /* FILE_UTIL */