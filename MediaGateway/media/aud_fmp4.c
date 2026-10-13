//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Faixa de audio AAC em fMP4 para o DASH. Ver aud_fmp4.h.

#include "aud_fmp4.h"
#include "mux_mp4.h"
#include "mpd_util.h"
#include "aac_silence.h"
#include "media_time.h"

typedef struct { uint8_t* data; int size; int dur; } AfSample;

struct AudFmp4
{
    char      base[1024];
    char      prefixo[48];
    int       rate, ch;
    int64_t   seg_units;                      // alvo de um segmento, em amostras
    int       seg_index;
    AfSample* seg; int seg_n, seg_cap;
    int64_t   seg_base, seg_dur;
    MpdSeg*   tl; int tl_n, tl_cap;           // cortes gravados (para o manifesto)
    int64_t   bytes;
    const uint8_t* sil; int sil_n;            // quadro de silencio desta faixa (NULL = nao ha)
    int       comecou;                        // ja chegou o primeiro pacote
};

AudFmp4* aud_fmp4_open(const char* base_dir, const char* prefixo, const uint8_t* asc, int asc_len,
                       int rate, int channels, int seg_ms)
{
    if (!asc || asc_len <= 0) return 0;
    AudFmp4* a = (AudFmp4*)GW_CALLOC(1, sizeof(AudFmp4));
    if (!a) return 0;
    snprintf(a->base, sizeof(a->base), "%s", base_dir ? base_dir : ".");
    snprintf(a->prefixo, sizeof(a->prefixo), "%s", (prefixo && *prefixo) ? prefixo : "audio");
    a->rate = rate > 0 ? rate : 48000;
    a->ch   = channels > 0 ? channels : 2;
    a->seg_units = (int64_t)a->rate * (seg_ms > 0 ? seg_ms : 2000) / 1000;

    char ip[1200]; snprintf(ip, sizeof(ip), "%s/init-%s.mp4", a->base, a->prefixo);
    if (mp4_write_init_audio(ip, asc, asc_len, a->rate, a->ch) != 0) { GW_FREE(a); return 0; }
    a->sil = aac_silence_frame(asc, asc_len, &a->sil_n);
    return a;
}

static void af_flush(AudFmp4* a)
{
    if (a->seg_n <= 0) return;
    int n = a->seg_n;
    Mp4Sample* ms = (Mp4Sample*)GW_ALLOC((size_t)n * sizeof(Mp4Sample));
    if (ms)
    {
        for (int i = 0; i < n; i++)
        { ms[i].data = a->seg[i].data; ms[i].size = a->seg[i].size; ms[i].duration_ms = a->seg[i].dur; ms[i].keyframe = 1; a->bytes += a->seg[i].size; }
        char p[1200]; snprintf(p, sizeof(p), "%s/%s-%d.m4s", a->base, a->prefixo, a->seg_index);
        mp4_write_segment(p, (uint32_t)(a->seg_index + 1), a->seg_base, ms, n);
        mpd_seg_add(&a->tl, &a->tl_n, &a->tl_cap, a->seg_base, a->seg_dur);
        GW_FREE(ms);
    }
    for (int i = 0; i < n; i++) GW_FREE(a->seg[i].data);
    a->seg_n = 0; a->seg_dur = 0; a->seg_index++;
}

static void af_put(AudFmp4* a, const uint8_t* data, int size, int dur, int64_t dts)
{
    if (a->seg_n > 0 && a->seg_dur >= a->seg_units) af_flush(a);   // corte em fronteira de quadro AAC
    if (a->seg_n == 0) a->seg_base = dts;
    if (a->seg_n == a->seg_cap) { a->seg_cap = a->seg_cap ? a->seg_cap * 2 : 128; a->seg = (AfSample*)GW_REALLOC(a->seg, (size_t)a->seg_cap * sizeof(AfSample)); }
    uint8_t* cp = (uint8_t*)GW_ALLOC(size);
    if (!cp) return;
    GW_COPY(cp, data, size);
    a->seg[a->seg_n].data = cp; a->seg[a->seg_n].size = size; a->seg[a->seg_n].dur = dur;
    a->seg_n++; a->seg_dur += dur;
}

void aud_fmp4_write(AudFmp4* a, const GwPacket* pkt)
{
    if (!a || !pkt || pkt->Size <= 0) return;
    int     dur = pkt->Dur > 0 ? (int)mt_to_scale(pkt->Dur, a->rate) : 1024;
    int64_t dts = mt_to_scale(pkt->Pts, a->rate);
    if (!a->comecou)
    {
        // Audio que entra atrasado na origem: silencio ate o primeiro pacote (ver aac_silence.h),
        // para a faixa comecar junto com a de video.
        a->comecou = 1;
        int64_t t = 0, n = a->sil ? aac_silence_count(dts, &t) : 0;
        for (int64_t i = 0; i < n; i++, t += AAC_SILENCE_SAMPLES) af_put(a, a->sil, a->sil_n, AAC_SILENCE_SAMPLES, t);
    }
    af_put(a, pkt->Data, pkt->Size, dur, dts);
}

void aud_fmp4_finish(AudFmp4* a) { if (a) af_flush(a); }

double aud_fmp4_duration(const AudFmp4* a)
{
    if (!a || a->tl_n <= 0) return 0.0;
    return (double)(a->tl[a->tl_n - 1].t + a->tl[a->tl_n - 1].d) / a->rate;
}

void aud_fmp4_mpd(const AudFmp4* a, FILE* f)
{
    if (!a || !f || a->tl_n <= 0) return;
    double dur = aud_fmp4_duration(a);
    int bw = dur > 0 ? (int)((double)a->bytes * 8.0 / dur) : 128000;
    fprintf(f,
        "    <AdaptationSet contentType=\"audio\" mimeType=\"audio/mp4\" codecs=\"mp4a.40.2\" segmentAlignment=\"true\" startWithSAP=\"1\">\n"
        "      <SegmentTemplate timescale=\"%d\" startNumber=\"0\" initialization=\"init-%s.mp4\" media=\"%s-$Number$.m4s\">\n",
        a->rate, a->prefixo, a->prefixo);
    mpd_timeline(f, a->tl, a->tl_n, "        ");
    fprintf(f,
        "      </SegmentTemplate>\n"
        "      <Representation id=\"audio\" bandwidth=\"%d\" audioSamplingRate=\"%d\">\n"
        "        <AudioChannelConfiguration schemeIdUri=\"urn:mpeg:dash:23003:3:audio_channel_configuration:2011\" value=\"%d\"/>\n"
        "      </Representation>\n"
        "    </AdaptationSet>\n", bw > 0 ? bw : 128000, a->rate, a->ch);
}

void aud_fmp4_close(AudFmp4* a)
{
    if (!a) return;
    for (int i = 0; i < a->seg_n; i++) GW_FREE(a->seg[i].data);
    GW_FREE(a->seg);
    GW_FREE(a->tl);
    GW_FREE(a);
}
