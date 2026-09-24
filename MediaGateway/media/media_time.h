//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Timebase comum do gateway de sync: microssegundos (int64). Ver media/SYNC_GATEWAY.md.

#pragma once
#include <stdint.h>

typedef int64_t mtime_us;                 // tempo em microssegundos (timebase comum)
#define MTIME_NONE ((mtime_us)INT64_MIN)  // "sem timestamp"

// Conversoes timebase-comum <-> timescale de container.
static inline int64_t mt_to_ms(mtime_us t)            { return t / 1000; }
static inline mtime_us mt_from_ms(int64_t ms)         { return (mtime_us)ms * 1000; }
static inline int64_t mt_to_scale(mtime_us t, int ts) { return (int64_t)((double)t * ts / 1000000.0 + 0.5); }
static inline mtime_us mt_from_scale(int64_t v, int ts){ return (mtime_us)((double)v * 1000000.0 / (ts > 0 ? ts : 1) + 0.5); }
