//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  sink_hls: saida HLS multi-resolucao (fMP4). Cada pista de video vira uma rendition
//  (init-<name>.mp4 + <name>-N.m4s + <name>.m3u8); o audio AAC e' UMA rendition
//  compartilhada (grupo AUDIO). Cut() fecha o segmento corrente em todas as pistas.
//  Video H.264 (avc1) ou H.265 (hvc1). Alocacoes via memory_pool.
//
//  CMAF: os mesmos segmentos fMP4 servem DASH. No Close sai tambem um master.mpd
//  (SegmentTimeline com o tempo e a duracao EXATOS de cada segmento, lidos do que foi
//  gravado): uma preparacao so atende os dois protocolos, com o audio AAC original numa
//  faixa separada e a mesma sincronia do HLS.

#include "media_sink.h"
#include "mux_mp4.h"
#include "h26x_util.h"
#include "mpd_util.h"
#include "aac_silence.h"
#include <stdio.h>
#include <string.h>

#define HLS_MAX_V 8

typedef struct { uint8_t* data; int size; int64_t ms; int64_t dts; int key; } VSample;   // ms = apresentacao, dts = decodificacao
typedef struct { uint8_t* data; int size; int dur_units; int64_t dts_units; } ASample;

typedef struct
{
    char name[32]; int w, h, bw, fps;
    int hevc;                            // 1 = H.265: init hvc1+hvcC e CODECS "hvc1..."
    int avc3;                            // 1 = parametros dentro das amostras: init avc3 (H.264) ou hev1 (H.265), e o CODECS igual
    H26xToLen conv; int init_written, seg_index;
    VSample* seg; int seg_n, seg_cap; int64_t seg_start_ms;
    int corte;                           // > 0: as primeiras 'corte' amostras sao um segmento fechado, a gravar
    double maior_s;                      // maior segmento gravado (para o TARGETDURATION)
    FILE* pl;
}
VTrack;

typedef struct
{
    int have, rate, ch, asc_len; uint8_t asc[64];
    int seg_index;
    ASample* seg; int seg_n, seg_cap; int64_t seg_base_units, seg_dur_units;
    int comecou;                         // ja chegou o primeiro pacote (o silencio inicial e decidido nele)
    int corte;                           // como no video: amostras do segmento fechado, ainda nao gravado
    double maior_s;
    FILE* pl;
}
ATrack;

typedef struct
{
    char  base[1024];
    int   frag_ms;
    int   min_ms;                      // um segmento so e gravado quando o SEGUINTE ja tem isto (ver shls_cut)
    int   td;                          // TARGETDURATION declarado no cabecalho das playlists
    double inicio_s;                   // > 0: a apresentacao comeca aqui (copia de arquivo cortado)
    MpdSeg* vt; int vt_n, vt_cap;        // segmentos de video (iguais em todas as resolucoes), em ms
    MpdSeg* at; int at_n, at_cap;        // segmentos de audio, em amostras (escala = rate)
    int64_t a_bytes;                   // bytes de audio: bitrate real no manifesto
    VTrack v[HLS_MAX_V]; int vcount;
    ATrack a;
    int   track_kind[HLS_MAX_V + 2];   // 0=video,1=audio,-1
    int   track_sub[HLS_MAX_V + 2];    // indice em v[] (video)
    const GwFeedback* fb;
}
SinkHls;

// Texto do codec de video para os manifestos (RFC 6381), montado do que foi de fato gravado.
// H.264: perfil, compatibilidade e nivel do avcC -- cada resolucao declara o proprio nivel.
// Antes saia "avc1.640028" (High, nivel 4.0) para todas: um 4K era anunciado como nivel 4.0 e
// uma fonte Main/Baseline copiada sem recodificar, como High.
// H.265: montado do hvcC (ISO 14496-15 anexo E) -- perfil, compatibilidade, camada+nivel e
// restricoes. Antes saia "avc1.640028" tambem para H.265, e o player recusava ou abria o
// decoder errado.
static void vcodec_str(const VTrack* t, char* out, size_t n)
{
    const uint8_t* c = t->conv.cfg;
    if (!t->hevc)
    {
        if (c && t->conv.cfg_len >= 4) snprintf(out, n, "%s.%02X%02X%02X", t->avc3 ? "avc3" : "avc1", c[1], c[2], c[3]);
        else                           snprintf(out, n, "avc1.640028");   // sem avcC: nada foi gravado
        return;
    }
    const char* entrada = t->avc3 ? "hev1" : "hvc1";
    if (!c || t->conv.cfg_len < 13) { snprintf(out, n, "%s.1.6.L120.90", entrada); return; }
    static const char* ESPACO[4] = { "", "A", "B", "C" };
    uint32_t compat = ((uint32_t)c[2] << 24) | ((uint32_t)c[3] << 16) | ((uint32_t)c[4] << 8) | c[5], inv = 0;
    for (int i = 0; i < 32; i++) if (compat & (1u << i)) inv |= 1u << (31 - i);   // bits na ordem inversa
    int k = snprintf(out, n, "%s.%s%d.%X.%c%d", entrada, ESPACO[(c[1] >> 6) & 3], c[1] & 0x1F, inv, (c[1] & 0x20) ? 'H' : 'L', c[12]);
    int ult = 11; while (ult >= 6 && c[ult] == 0) ult--;                          // restricoes: sem os zeros do fim
    for (int i = 6; i <= ult && k > 0 && (size_t)k < n; i++) k += snprintf(out + k, n - (size_t)k, ".%X", c[i]);
}

// Grava como UM segmento as 'n' primeiras amostras da pista e mantem as demais.
static void flush_v(SinkHls* s, VTrack* t, int n)
{
    if (n > t->seg_n) n = t->seg_n;
    if (n <= 0) { t->corte = 0; return; }
    int last = t->fps > 0 ? (1000 / t->fps) : 33;
    Mp4Sample* ms = (Mp4Sample*)GW_ALLOC((size_t)n * sizeof(Mp4Sample));
    // Copia sem recodificar de fonte com quadros B: as amostras vem na ordem de decodificacao
    // e a apresentacao e outra. Ai duracoes e tempo-base saem da DECODIFICACAO e cada amostra
    // leva o deslocamento de composicao. (Recodificando, os dois tempos sao iguais.)
    int reord = 0; for (int i = 0; i < n; i++) if (t->seg[i].dts != t->seg[i].ms) { reord = 1; break; }
    int* cts = reord ? (int*)GW_ALLOC((size_t)n * sizeof(int)) : 0;
    double dur_s = 0;
    for (int i = 0; i < n; i++)
    {
        // a duracao e a distancia ate a amostra seguinte -- inclusive a primeira do proximo
        // segmento, quando ela ja chegou; so a ultima do video usa a duracao de um quadro
        int d = (i + 1 < t->seg_n) ? (int)(reord ? t->seg[i+1].dts - t->seg[i].dts : t->seg[i+1].ms - t->seg[i].ms) : last; if (d <= 0) d = last;
        ms[i].data = t->seg[i].data; ms[i].size = t->seg[i].size; ms[i].duration_ms = d; ms[i].keyframe = t->seg[i].key;
        if (cts) cts[i] = (int)(t->seg[i].ms - t->seg[i].dts);
        dur_s += d / 1000.0;
    }
    char p[1200]; snprintf(p, sizeof(p), "%s/%s-%d.m4s", s->base, t->name, t->seg_index);
    mp4_write_segment_cts(p, (uint32_t)(t->seg_index + 1), reord ? t->seg[0].dts : t->seg[0].ms, ms, n, cts);
    GW_FREE(cts);
    if (t->pl) fprintf(t->pl, "#EXTINF:%.3f,\n%s-%d.m4s\n", dur_s, t->name, t->seg_index);
    if (dur_s > t->maior_s) t->maior_s = dur_s;
    if (t == &s->v[0]) mpd_seg_add(&s->vt, &s->vt_n, &s->vt_cap, t->seg[0].ms, (int64_t)(dur_s * 1000.0 + 0.5));
    for (int i = 0; i < n; i++) GW_FREE(t->seg[i].data);
    GW_FREE(ms);
    for (int i = n; i < t->seg_n; i++) t->seg[i - n] = t->seg[i];
    t->seg_n -= n; t->corte = 0; t->seg_index++;
}

// Idem para o audio.
static void flush_a(SinkHls* s, ATrack* a, int n)
{
    if (n > a->seg_n) n = a->seg_n;
    if (n <= 0) { a->corte = 0; return; }
    int64_t dur = 0;
    Mp4Sample* ms = (Mp4Sample*)GW_ALLOC((size_t)n * sizeof(Mp4Sample));
    for (int i = 0; i < n; i++)
    { ms[i].data = a->seg[i].data; ms[i].size = a->seg[i].size; ms[i].duration_ms = a->seg[i].dur_units; ms[i].keyframe = 1; dur += a->seg[i].dur_units; }
    char p[1200]; snprintf(p, sizeof(p), "%s/audio-%d.m4s", s->base, a->seg_index);
    mp4_write_segment(p, (uint32_t)(a->seg_index + 1), a->seg[0].dts_units, ms, n);
    if (a->pl) fprintf(a->pl, "#EXTINF:%.3f,\naudio-%d.m4s\n", (double)dur / (a->rate > 0 ? a->rate : 48000), a->seg_index);
    if ((double)dur / (a->rate > 0 ? a->rate : 48000) > a->maior_s) a->maior_s = (double)dur / (a->rate > 0 ? a->rate : 48000);
    mpd_seg_add(&s->at, &s->at_n, &s->at_cap, a->seg[0].dts_units, dur);
    for (int i = 0; i < n; i++) s->a_bytes += a->seg[i].size;
    for (int i = 0; i < n; i++) GW_FREE(a->seg[i].data);
    GW_FREE(ms);
    for (int i = n; i < a->seg_n; i++) a->seg[i - n] = a->seg[i];
    a->seg_n -= n; a->seg_dur_units -= dur; a->corte = 0; a->seg_index++;
}

// Grava o segmento fechado de todas as pistas (o que estava a espera em 'corte').
static void grava_fechados(SinkHls* s)
{
    for (int i = 0; i < s->vcount; i++) if (s->v[i].corte > 0) flush_v(s, &s->v[i], s->v[i].corte);
    if (s->a.have && s->a.corte > 0) flush_a(s, &s->a, s->a.corte);
}

static void put_a(ATrack* a, const uint8_t* data, int size, int dur, int64_t dts)
{
    if (a->seg_n == a->seg_cap) { a->seg_cap = a->seg_cap ? a->seg_cap * 2 : 128; a->seg = (ASample*)GW_REALLOC(a->seg, (size_t)a->seg_cap * sizeof(ASample)); }
    uint8_t* cp = (uint8_t*)GW_ALLOC(size); GW_COPY(cp, data, size);
    a->seg[a->seg_n].data = cp; a->seg[a->seg_n].size = size; a->seg[a->seg_n].dur_units = dur; a->seg[a->seg_n].dts_units = dts;
    a->seg_n++; a->seg_dur_units += dur;
}

// Audio que entra ATRASADO na origem (edicao vazia): o intervalo ate o primeiro pacote e
// preenchido com quadros AAC de silencio (ver aac_silence.h). Sem isto a faixa de audio comecava
// depois da de video, o player abria onde as duas ja tinham dados e o comeco do video nao
// aparecia. Silencio longo e cortado em segmentos do tamanho alvo, para nenhum passar da
// duracao que a playlist declara.
static void silencio_inicial(SinkHls* s, ATrack* a, int64_t inicio)
{
    int qn = 0; const uint8_t* q = aac_silence_frame(a->asc, a->asc_len, &qn);
    int64_t t = 0, n = q ? aac_silence_count(inicio, &t) : 0;
    int64_t alvo = (int64_t)(a->rate > 0 ? a->rate : 48000) * s->frag_ms / 1000;
    for (int64_t i = 0; i < n; i++, t += AAC_SILENCE_SAMPLES)
    {
        if (a->seg_n > 0 && a->seg_dur_units >= alvo) flush_a(s, a, a->seg_n);
        put_a(a, q, qn, AAC_SILENCE_SAMPLES, t);
    }
}

static int shls_start(MediaSink* k, const MediaTrackOut* tracks, int count)
{
    SinkHls* s = (SinkHls*)k->Ctx;
    int td = s->frag_ms / 1000 + 1;
    s->td = td;
    for (int i = 0; i < count && i < HLS_MAX_V + 2; i++)
    {
        s->track_kind[i] = -1;
        if (tracks[i].Type == MSTREAM_VIDEO && s->vcount < HLS_MAX_V)
        {
            VTrack* t = &s->v[s->vcount];
            snprintf(t->name, sizeof(t->name), "%s", tracks[i].Name ? tracks[i].Name : "v");
            t->w = tracks[i].Width; t->h = tracks[i].Height; t->bw = tracks[i].Bandwidth; t->fps = (int)(tracks[i].Fps + 0.5);
            t->hevc = (tracks[i].Codec == MEDIA_CODEC_H265);
            t->avc3 = tracks[i].InBandParams ? 1 : 0;
            h26x_tl_init(&t->conv, t->hevc);
            t->conv.keep_ps = t->avc3;
            if (tracks[i].StartUs > 0) s->inicio_s = (double)tracks[i].StartUs / 1000000.0;
            char pp[1200]; snprintf(pp, sizeof(pp), "%s/%s.m3u8", s->base, t->name); fopen_s(&t->pl, pp, "wb");
            if (t->pl) fprintf(t->pl, "#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-PLAYLIST-TYPE:VOD\n"
                                      "#EXT-X-TARGETDURATION:%d\n#EXT-X-MEDIA-SEQUENCE:0\n", td);
            if (t->pl && s->inicio_s > 0) fprintf(t->pl, "#EXT-X-START:TIME-OFFSET=%.3f,PRECISE=YES\n", s->inicio_s);
            if (t->pl) fprintf(t->pl, "#EXT-X-MAP:URI=\"init-%s.mp4\"\n", t->name);
            s->track_kind[i] = 0; s->track_sub[i] = s->vcount; s->vcount++;
        }
        else if (tracks[i].Type == MSTREAM_AUDIO && !s->a.have && tracks[i].Codec == MEDIA_CODEC_AAC)
        {
            s->a.have = 1; s->a.rate = tracks[i].SampleRate; s->a.ch = tracks[i].Channels;
            s->a.asc_len = tracks[i].ExtraLen < 64 ? tracks[i].ExtraLen : 64;
            if (tracks[i].Extra && s->a.asc_len > 0) GW_COPY(s->a.asc, tracks[i].Extra, s->a.asc_len);
            char ip[1200]; snprintf(ip, sizeof(ip), "%s/init-audio.mp4", s->base);
            mp4_write_init_audio(ip, s->a.asc, s->a.asc_len, s->a.rate, s->a.ch);
            char pp[1200]; snprintf(pp, sizeof(pp), "%s/audio.m3u8", s->base); fopen_s(&s->a.pl, pp, "wb");
            if (s->a.pl) fprintf(s->a.pl, "#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-PLAYLIST-TYPE:VOD\n"
                                          "#EXT-X-TARGETDURATION:%d\n#EXT-X-MEDIA-SEQUENCE:0\n", td);
            if (s->a.pl && s->inicio_s > 0) fprintf(s->a.pl, "#EXT-X-START:TIME-OFFSET=%.3f,PRECISE=YES\n", s->inicio_s);
            if (s->a.pl) fprintf(s->a.pl, "#EXT-X-MAP:URI=\"init-audio.mp4\"\n");
            s->track_kind[i] = 1;
        }
    }
    return s->vcount > 0 ? 0 : -1;
}

static int shls_write(MediaSink* k, int track, const GwPacket* pkt)
{
    SinkHls* s = (SinkHls*)k->Ctx;
    if (!pkt || pkt->Size <= 0 || track < 0 || track >= HLS_MAX_V + 2) return 0;

    if (s->track_kind[track] == 0)
    {
        VTrack* t = &s->v[s->track_sub[track]];
        uint8_t* out; int alen = h26x_tl_feed(&t->conv, pkt->Data, pkt->Size, &out);
        if (!t->init_written && h26x_tl_ready(&t->conv))
        { char ip[1200]; snprintf(ip, sizeof(ip), "%s/init-%s.mp4", s->base, t->name);
          mp4_write_init_kind(ip, t->hevc ? (t->avc3 ? MP4_KIND_HEV1 : MP4_KIND_HEVC) : (t->avc3 ? MP4_KIND_AVC3 : MP4_KIND_H264), t->w, t->h, t->conv.cfg, t->conv.cfg_len); t->init_written = 1; }
        if (!t->init_written || alen <= 0) return 0;
        int64_t ms = mt_to_ms(pkt->Pts);
        if (t->seg_n == 0) t->seg_start_ms = ms;
        if (t->seg_n == t->seg_cap) { t->seg_cap = t->seg_cap ? t->seg_cap * 2 : 64; t->seg = (VSample*)GW_REALLOC(t->seg, (size_t)t->seg_cap * sizeof(VSample)); }
        uint8_t* cp = (uint8_t*)GW_ALLOC(alen); GW_COPY(cp, out, alen);
        t->seg[t->seg_n].data = cp; t->seg[t->seg_n].size = alen; t->seg[t->seg_n].ms = ms; t->seg[t->seg_n].dts = mt_to_ms(pkt->Dts); t->seg[t->seg_n].key = pkt->KeyFrame; t->seg_n++;
        // o segmento seguinte ja tem tamanho que se sustenta: o fechado pode ser gravado
        if (t == &s->v[0] && t->corte > 0 && t->corte < t->seg_n && ms - t->seg[t->corte].ms >= s->min_ms) grava_fechados(s);
    }
    else if (s->track_kind[track] == 1 && s->a.have)
    {
        ATrack* a = &s->a;
        int dur = pkt->Dur > 0 ? (int)mt_to_scale(pkt->Dur, a->rate) : 1024;
        int64_t dts = mt_to_scale(pkt->Pts, a->rate);
        if (!a->comecou) { a->comecou = 1; silencio_inicial(s, a, dts); }
        put_a(a, pkt->Data, pkt->Size, dur, dts);
    }
    return 0;
}

// O cabecalho da playlist e gravado ANTES dos segmentos, com a duracao alvo. O corte real cai
// em quadro-chave, e numa copia sem recodificar os quadros-chave sao os da origem: com eles
// espacados, um segmento passa do declarado (medido: 9,167 s com TARGETDURATION 6), o que o HLS
// proibe -- nenhum EXTINF, arredondado, pode passar do TARGETDURATION. No fim, se algum
// passou, a linha e regravada com o valor real.
static void corrige_target(const char* path, int declarado, double maior_s)
{
    int real = (int)(maior_s + 0.5);
    if (real <= declarado) return;
    FILE* f = 0; if (fopen_s(&f, path, "rb") != 0 || !f) return;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char* txt = (char*)GW_ALLOC((size_t)n + 1);
    if (!txt || fread(txt, 1, (size_t)n, f) != (size_t)n) { fclose(f); GW_FREE(txt); return; }
    fclose(f); txt[n] = 0;
    static const char CHAVE[] = "#EXT-X-TARGETDURATION:";
    char* p = strstr(txt, CHAVE);
    if (p)
    {
        char* fim = p + sizeof(CHAVE) - 1; while (*fim >= '0' && *fim <= '9') fim++;
        if (fopen_s(&f, path, "wb") == 0 && f)
        {
            fwrite(txt, 1, (size_t)(p - txt), f);
            fprintf(f, "%s%d", CHAVE, real);
            fwrite(fim, 1, (size_t)(txt + n - fim), f);
            fclose(f);
        }
    }
    GW_FREE(txt);
}

// O corte so MARCA onde o segmento fecha; a gravacao espera o segmento seguinte ter min_ms.
// Se o video acabar antes disso, o que sobrou e gravado JUNTO com o segmento fechado (ver
// shls_close). Sem essa espera, um quadro-chave no fim do video (o encoder poe um a cada N
// quadros) virava um segmento de UM quadro -- medido: 6 segmentos de 4 s e um ultimo de 41 ms.
static int shls_cut(MediaSink* k, mtime_us at)
{
    (void)at; SinkHls* s = (SinkHls*)k->Ctx;
    grava_fechados(s);   // dois cortes seguidos: o anterior nao espera mais
    for (int i = 0; i < s->vcount; i++) s->v[i].corte = s->v[i].seg_n;
    if (s->a.have) s->a.corte = s->a.seg_n;
    return 0;
}

// Fim do video: grava o que falta. O resto depois do ultimo corte so vira segmento proprio se
// tiver min_ms; mais curto que isso, vai junto com o segmento anterior.
static void grava_final(SinkHls* s)
{
    VTrack* v0 = &s->v[0];
    int quadro = v0->fps > 0 ? (1000 / v0->fps) : 33;
    int resto_ms = (s->vcount > 0 && v0->corte > 0 && v0->corte < v0->seg_n)
                 ? (int)(v0->seg[v0->seg_n - 1].ms - v0->seg[v0->corte].ms) + quadro : 0;
    if (resto_ms >= s->min_ms) grava_fechados(s);
    for (int i = 0; i < s->vcount; i++) flush_v(s, &s->v[i], s->v[i].seg_n);
    if (s->a.have) flush_a(s, &s->a, s->a.seg_n);
}

// master.mpd sobre os MESMOS arquivos do HLS (init-<nome>.mp4, <nome>-N.m4s, audio-N.m4s).
static void write_mpd(SinkHls* s)
{
    if (s->vt_n <= 0) return;
    char path[1200]; snprintf(path, sizeof(path), "%s/master.mpd", s->base);
    FILE* f = 0; if (fopen_s(&f, path, "wb") != 0 || !f) return;

    double dur = (double)(s->vt[s->vt_n - 1].t + s->vt[s->vt_n - 1].d) / 1000.0;
    int rate = s->a.rate > 0 ? s->a.rate : 48000;
    double adur = (s->a.have && s->at_n > 0) ? (double)(s->at[s->at_n - 1].t + s->at[s->at_n - 1].d) / rate : 0.0;
    if (adur > dur) dur = adur;

    fprintf(f,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<MPD xmlns=\"urn:mpeg:dash:schema:mpd:2011\" type=\"static\" "
        "mediaPresentationDuration=\"PT%.3fS\" minBufferTime=\"PT2S\" "
        "profiles=\"urn:mpeg:dash:profile:isoff-live:2011\">\n"
        "  <Period>\n"
        "    <AdaptationSet contentType=\"video\" mimeType=\"video/mp4\" segmentAlignment=\"true\" startWithSAP=\"1\">\n"
        "      <SegmentTemplate timescale=\"1000\" startNumber=\"0\" "
        "initialization=\"init-$RepresentationID$.mp4\" media=\"$RepresentationID$-$Number$.m4s\">\n", dur);
    mpd_timeline(f, s->vt, s->vt_n, "        ");
    fprintf(f, "      </SegmentTemplate>\n");
    for (int i = 0; i < s->vcount; i++)
    {
        char vcod[96]; vcodec_str(&s->v[i], vcod, sizeof(vcod));   // o nivel muda com a resolucao
        fprintf(f, "      <Representation id=\"%s\" codecs=\"%s\" bandwidth=\"%d\" width=\"%d\" height=\"%d\"/>\n",
                s->v[i].name, vcod, s->v[i].bw > 0 ? s->v[i].bw : 2000000, s->v[i].w, s->v[i].h);
    }
    fprintf(f, "    </AdaptationSet>\n");

    if (s->a.have && s->at_n > 0)
    {
        int abw = adur > 0 ? (int)((double)s->a_bytes * 8.0 / adur) : 128000;
        fprintf(f,
            "    <AdaptationSet contentType=\"audio\" mimeType=\"audio/mp4\" codecs=\"mp4a.40.2\" segmentAlignment=\"true\" startWithSAP=\"1\">\n"
            "      <SegmentTemplate timescale=\"%d\" startNumber=\"0\" initialization=\"init-audio.mp4\" media=\"audio-$Number$.m4s\">\n", rate);
        mpd_timeline(f, s->at, s->at_n, "        ");
        fprintf(f,
            "      </SegmentTemplate>\n"
            "      <Representation id=\"audio\" bandwidth=\"%d\" audioSamplingRate=\"%d\">\n"
            "        <AudioChannelConfiguration schemeIdUri=\"urn:mpeg:dash:23003:3:audio_channel_configuration:2011\" value=\"%d\"/>\n"
            "      </Representation>\n"
            "    </AdaptationSet>\n", abw > 0 ? abw : 128000, rate, s->a.ch > 0 ? s->a.ch : 2);
    }
    fprintf(f, "  </Period>\n</MPD>\n");
    fclose(f);
}

static void shls_close(MediaSink* k)
{
    if (!k) return;
    SinkHls* s = (SinkHls*)k->Ctx;
    if (s)
    {
        if (!k->Aborted) grava_final(s);   // ultimo segmento

        for (int i = 0; i < s->vcount; i++) if (s->v[i].pl) { if (!k->Aborted) fprintf(s->v[i].pl, "#EXT-X-ENDLIST\n"); fclose(s->v[i].pl); }
        if (s->a.pl) { if (!k->Aborted) fprintf(s->a.pl, "#EXT-X-ENDLIST\n"); fclose(s->a.pl); }
        if (!k->Aborted)
        {
            char pp[1200];
            for (int i = 0; i < s->vcount; i++) { snprintf(pp, sizeof(pp), "%s/%s.m3u8", s->base, s->v[i].name); corrige_target(pp, s->td, s->v[i].maior_s); }
            if (s->a.have) { snprintf(pp, sizeof(pp), "%s/audio.m3u8", s->base); corrige_target(pp, s->td, s->a.maior_s); }
        }

        char mp[1200]; snprintf(mp, sizeof(mp), "%s/master.m3u8", s->base);
        FILE* m = 0; if (!k->Aborted) fopen_s(&m, mp, "wb");
        if (m)
        {
            fprintf(m, "#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-INDEPENDENT-SEGMENTS\n");
            // Copia de arquivo cortado: o trecho que a origem esconde esta nos segmentos (sem
            // recodificar nao da para tira-lo); o player comeca depois dele.
            if (s->inicio_s > 0) fprintf(m, "#EXT-X-START:TIME-OFFSET=%.3f,PRECISE=YES\n", s->inicio_s);
            if (s->a.have) fprintf(m, "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",NAME=\"audio\",DEFAULT=YES,AUTOSELECT=YES,URI=\"audio.m3u8\"\n");
            for (int i = 0; i < s->vcount; i++)
            {
                char vcod[96]; vcodec_str(&s->v[i], vcod, sizeof(vcod));
                fprintf(m, "#EXT-X-STREAM-INF:BANDWIDTH=%d,RESOLUTION=%dx%d,CODECS=\"%s%s\"%s\n%s.m3u8\n",
                        s->v[i].bw > 0 ? s->v[i].bw : 2000000, s->v[i].w, s->v[i].h, vcod, s->a.have ? ",mp4a.40.2" : "",
                        s->a.have ? ",AUDIO=\"aud\"" : "", s->v[i].name);
            }
            fclose(m);
            write_mpd(s);   // CMAF: o mesmo conteudo em DASH
            GwEvent e; GW_ZERO(&e, sizeof(e)); e.Kind = GW_EV_DONE; e.Playlist = "master.m3u8"; gw_emit(s->fb, &e);
            gw_emit_mem(s->fb);
        }

        for (int i = 0; i < s->vcount; i++) { for (int j = 0; j < s->v[i].seg_n; j++) GW_FREE(s->v[i].seg[j].data); GW_FREE(s->v[i].seg); h26x_tl_free(&s->v[i].conv); }
        for (int j = 0; j < s->a.seg_n; j++) GW_FREE(s->a.seg[j].data); GW_FREE(s->a.seg);
        GW_FREE(s->vt); GW_FREE(s->at);
        GW_FREE(s);
    }
    GW_FREE(k);
}

MediaSink* sink_hls_open(const MediaProfile* profile, const char* base_dir, const GwFeedback* fb)
{
    SinkHls* s = (SinkHls*)GW_CALLOC(1, sizeof(SinkHls));
    if (!s) return 0;
    s->fb = fb; s->frag_ms = (profile && profile->SegmentMs > 0) ? profile->SegmentMs : 2000;
    s->min_ms = s->frag_ms / 4; if (s->min_ms > 1000) s->min_ms = 1000; if (s->min_ms < 200) s->min_ms = 200;
    snprintf(s->base, sizeof(s->base), "%s", base_dir ? base_dir : ".");

    MediaSink* k = (MediaSink*)GW_CALLOC(1, sizeof(MediaSink));
    if (!k) { GW_FREE(s); return 0; }
    k->Ctx = s; k->Start = shls_start; k->Write = shls_write; k->Cut = shls_cut; k->Close = shls_close;
    return k;
}
