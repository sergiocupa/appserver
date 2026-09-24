//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  live_store: ver live_store.h. Anel de segmentos fMP4 em memoria + playlist HLS ao vivo.

#include "live_store.h"
#include "gw_base.h"
#include "thread_handler.h"   // xmutex_t (CRITICAL_SECTION / pthread_mutex_t)
#include <stdio.h>
#include <string.h>

typedef struct { uint8_t* data; int size; double dur; long long seq; } LiveSeg;

struct LiveStore
{
    xmutex_t  lock;
    LiveSeg*  seg; int window, count, head;   // anel: head = posicao do proximo push
    long long next_seq;                       // sequencia do proximo segmento
    uint8_t*  init; int init_size;
    int       width, height, bitrate;
    int       target;                         // TARGETDURATION minimo (s) ate haver medida real
    int       finished;
};

LiveStore* live_store_create(int window, int target_seconds)
{
    if (window < 2) window = 2;
    LiveStore* s = (LiveStore*)GW_CALLOC(1, sizeof(LiveStore));
    if (!s) return 0;
    s->seg = (LiveSeg*)GW_CALLOC((size_t)window, sizeof(LiveSeg));
    if (!s->seg) { GW_FREE(s); return 0; }
    s->window = window;
    s->target = target_seconds > 0 ? target_seconds : 1;
    thread_mutex_init_inline(&s->lock);
    return s;
}

void live_store_free(LiveStore** store)
{
    if (!store || !*store) return;
    LiveStore* s = *store; *store = 0;
    for (int i = 0; i < s->window; i++) if (s->seg[i].data) GW_FREE(s->seg[i].data);
    if (s->init) GW_FREE(s->init);
    thread_mutex_destroy_inline(&s->lock);
    GW_FREE(s->seg);
    GW_FREE(s);
}

void live_store_set_init(LiveStore* s, const uint8_t* data, int size, int width, int height, int bitrate)
{
    if (!s || !data || size <= 0) return;
    uint8_t* cp = (uint8_t*)GW_ALLOC((size_t)size);
    if (!cp) return;
    GW_COPY(cp, data, size);

    thread_mutex_lock_inline(&s->lock);
    if (s->init) GW_FREE(s->init);
    s->init = cp; s->init_size = size;
    s->width = width; s->height = height; s->bitrate = bitrate;
    thread_mutex_unlock_inline(&s->lock);
}

void live_store_push(LiveStore* s, const uint8_t* data, int size, double duration_s)
{
    if (!s || !data || size <= 0) return;
    uint8_t* cp = (uint8_t*)GW_ALLOC((size_t)size);
    if (!cp) return;
    GW_COPY(cp, data, size);

    thread_mutex_lock_inline(&s->lock);
    LiveSeg* slot = &s->seg[s->head];
    if (slot->data) GW_FREE(slot->data);            // o mais antigo sai da janela
    slot->data = cp; slot->size = size;
    slot->dur = duration_s > 0.0 ? duration_s : 0.0;
    slot->seq = s->next_seq++;
    s->head = (s->head + 1) % s->window;
    if (s->count < s->window) s->count++;
    thread_mutex_unlock_inline(&s->lock);
}

void live_store_finish(LiveStore* s)
{
    if (!s) return;
    thread_mutex_lock_inline(&s->lock);
    s->finished = 1;
    thread_mutex_unlock_inline(&s->lock);
}

// Indice no anel do i-esimo segmento mais antigo ainda vivo. Chamar com o lock preso.
static LiveSeg* seg_at(LiveStore* s, int i)
{
    int oldest = (s->head - s->count + s->window) % s->window;
    return &s->seg[(oldest + i) % s->window];
}

int live_store_playlist(LiveStore* s, char* out, int out_size)
{
    if (!s || !out || out_size <= 0) return 0;
    int n = 0;

    thread_mutex_lock_inline(&s->lock);
    {
        // Playlist VALIDA mesmo sem segmento ainda: nos primeiros segundos a camera e o
        // encoder ainda nao fecharam o primeiro segmento, e responder 404 fazia o hls.js
        // esgotar as tentativas e desistir antes de o video comecar.
        double longest = 0.0;
        for (int i = 0; i < s->count; i++) { double d = seg_at(s, i)->dur; if (d > longest) longest = d; }
        int target = (int)(longest + 0.999); if (target < s->target) target = s->target; if (target < 1) target = 1;

        n = snprintf(out, (size_t)out_size,
                     "#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-TARGETDURATION:%d\n"
                     "#EXT-X-MEDIA-SEQUENCE:%lld\n#EXT-X-MAP:URI=\"init.mp4\"\n",
                     target, s->count > 0 ? seg_at(s, 0)->seq : s->next_seq);
        for (int i = 0; i < s->count && n > 0 && n < out_size; i++)
        {
            LiveSeg* g = seg_at(s, i);
            n += snprintf(out + n, (size_t)(out_size - n), "#EXTINF:%.3f,\nseg-%lld.m4s\n", g->dur, g->seq);
        }
        if (s->finished && n > 0 && n < out_size)
            n += snprintf(out + n, (size_t)(out_size - n), "#EXT-X-ENDLIST\n");
    }
    thread_mutex_unlock_inline(&s->lock);

    if (n < 0 || n > out_size) n = 0;   // truncou: melhor nao publicar playlist quebrada
    return n;
}

int live_store_init_copy(LiveStore* s, uint8_t** out, int* size)
{
    if (!s || !out || !size) return 0;
    *out = 0; *size = 0;

    thread_mutex_lock_inline(&s->lock);
    if (s->init && s->init_size > 0)
    {
        uint8_t* cp = (uint8_t*)GW_ALLOC((size_t)s->init_size);
        if (cp) { GW_COPY(cp, s->init, s->init_size); *out = cp; *size = s->init_size; }
    }
    thread_mutex_unlock_inline(&s->lock);
    return *out ? 1 : 0;
}

int live_store_segment_copy(LiveStore* s, long long seq, uint8_t** out, int* size)
{
    if (!s || !out || !size) return 0;
    *out = 0; *size = 0;

    thread_mutex_lock_inline(&s->lock);
    for (int i = 0; i < s->count; i++)
    {
        LiveSeg* g = seg_at(s, i);
        if (g->seq != seq || !g->data) continue;
        uint8_t* cp = (uint8_t*)GW_ALLOC((size_t)g->size);
        if (cp) { GW_COPY(cp, g->data, g->size); *out = cp; *size = g->size; }
        break;
    }
    thread_mutex_unlock_inline(&s->lock);
    return *out ? 1 : 0;
}

void live_store_info(LiveStore* s, int* width, int* height, int* bitrate,
                     long long* last_seq, int* segments, int* finished)
{
    if (width) *width = 0; if (height) *height = 0; if (bitrate) *bitrate = 0;
    if (last_seq) *last_seq = -1; if (segments) *segments = 0; if (finished) *finished = 0;
    if (!s) return;

    thread_mutex_lock_inline(&s->lock);
    if (width)    *width    = s->width;
    if (height)   *height   = s->height;
    if (bitrate)  *bitrate  = s->bitrate;
    if (last_seq) *last_seq = s->next_seq - 1;
    if (segments) *segments = s->count;
    if (finished) *finished = s->finished;
    thread_mutex_unlock_inline(&s->lock);
}
