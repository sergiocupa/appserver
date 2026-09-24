//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Nucleo de sincronizacao (VOD). Puxa do source, decodifica/bypass/passthrough,
//  escala por rendition (libyuv), codifica no codec de saida, corta o fragmento no
//  keyframe e escreve no sink. Alocacoes via memory_pool. NAO TESTADO EM RUNTIME.

#include "sync_gateway.h"
#include "media_codec.h"
#include "gw_decode.h"     // GwVideoDec: decode unificado H264/H265/VP9/AV1
#include "enc_select.h"
#include <string.h>     // backend que atendeu (reportado no TRACK_START)
#include "audio_aac.h"     // aac_dec_* (transcode AAC->Opus)
#include "thread_pool.h"   // pool_* do xplatbase (paraleliza renditions)
#include "xplatbase.h"     // xcpu_count() (nucleos logicos p/ orcamento de threads)
#include "atomics.h"       // xatomic_int (backpressure/progresso)
#include "thread_wait.h"   // xwait_t: espera por sinal (substitui os busy-waits)
#include "mux_webm.h"      // webm_open_chunk / webm_open_segmented
#include <stdio.h>

#ifdef HAVE_LIBYUV
#include "libyuv/scale.h"
#endif

#define GW_MAXR 8

typedef struct GwRun GwRun;
typedef struct { GwRun* g; int r; } GwTask;   // arg de cada task do pool (1 rendition)

struct GwRun
{
    MediaSink* sink; const MediaProfile* prof; const GwFeedback* fb;
    int nrend; MediaCodec out_vcodec;
    int rw[GW_MAXR], rh[GW_MAXR], rbw[GW_MAXR]; const char* rname[GW_MAXR];
    MediaEncoder* venc[GW_MAXR];
    uint8_t* ry[GW_MAXR]; uint8_t* ru[GW_MAXR]; uint8_t* rv[GW_MAXR];
    mtime_us* fpts; int fpts_n, fpts_cap;
    mtime_us seg_start; int seg_ms;

    // --- paralelizacao das renditions (pool do xplatbase) ---
    ThreadPool* pool;                 // NULL = caminho serial (fallback)
    GwTask   tctx[GW_MAXR];           // args fixos das tasks (g, r)
    MediaPacket rpkt[GW_MAXR];        // pacote capturado por rendition no frame atual
    int      rhas[GW_MAXR];           // 1 se rpkt[r] tem pacote deste frame
    // frame atual (setado antes de disparar as tasks; valido ate o wait_idle):
    uint8_t *cy, *cu, *cv; int csy, csu, csv, cw, ch; int64_t cidx;
};

static mtime_us pts_of(GwRun* g, int64_t idx)
{ return (idx >= 0 && idx < g->fpts_n) ? g->fpts[idx] : (g->fpts_n ? g->fpts[g->fpts_n - 1] : 0); }

// Escreve 1 pacote de video no sink; corta o segmento na fronteira (keyframe da rendition 0).
static void emit_vpkt(GwRun* g, int r, const MediaPacket* p)
{
    GwPacket gp; GW_ZERO(&gp, sizeof(gp));
    gp.Stream = r; gp.Data = p->Data; gp.Size = p->Size; gp.KeyFrame = p->KeyFrame;
    gp.Pts = gp.Dts = pts_of(g, p->Pts);
    if (r == 0 && p->KeyFrame)
    {
        // Tolerancia de 25%: exigir o alvo CHEIO fazia o keyframe que chega um instante
        // antes ser recusado, e o segmento ia ate o keyframe seguinte -- com GOP igual ao
        // alvo (o caso do ao vivo), isso DOBRAVA a duracao (2s viravam ~3,6s).
        if (g->seg_start == MTIME_NONE) g->seg_start = gp.Pts;
        else if (gp.Pts - g->seg_start >= (mtime_us)g->seg_ms * 750) { g->sink->Cut(g->sink, gp.Pts); g->seg_start = gp.Pts; }
    }
    g->sink->Write(g->sink, r, &gp);
}

// Task do pool: escala (se preciso) e codifica 1 frame numa rendition, guardando o
// pacote em g->rpkt[r] (o EMIT no sink e feito depois, na main thread, em ordem).
// So toca estado por-rendition (venc[r], ry/ru/rv[r], rpkt[r]) -> seguro em paralelo.
// memop e thread-safe (mimalloc + remote_free), entao alloc/scale aqui e ok.
static void rend_task(void* arg)
{
    GwTask* t = (GwTask*)arg; GwRun* g = t->g; int r = t->r;
    g->rhas[r] = 0;
    int rw = g->rw[r], rh = g->rh[r];
    uint8_t *Y = g->cy, *U = g->cu, *V = g->cv; int SY = g->csy, SU = g->csu, SV = g->csv;
    if (rw != g->cw || rh != g->ch)
    {
        int cw = (rw + 1) / 2, ch = (rh + 1) / 2;
        if (!g->ry[r]) { g->ry[r] = (uint8_t*)GW_ALLOC((size_t)rw * rh); g->ru[r] = (uint8_t*)GW_ALLOC((size_t)cw * ch); g->rv[r] = (uint8_t*)GW_ALLOC((size_t)cw * ch); }
#ifdef HAVE_LIBYUV
        I420Scale(g->cy, g->csy, g->cu, g->csu, g->cv, g->csv, g->cw, g->ch, g->ry[r], rw, g->ru[r], cw, g->rv[r], cw, rw, rh, kFilterBilinear);
#else
        return;   // sem libyuv nao ha rescale
#endif
        Y = g->ry[r]; U = g->ru[r]; V = g->rv[r]; SY = rw; SU = cw; SV = cw;
    }
    MediaFrame mf; GW_ZERO(&mf, sizeof(mf));
    mf.Kind = MEDIA_KIND_VIDEO; mf.Width = rw; mf.Height = rh;
    mf.Y = Y; mf.StrideY = SY; mf.U = U; mf.StrideU = SU; mf.V = V; mf.StrideV = SV; mf.Pts = g->cidx;
    g->venc[r]->SendFrame(g->venc[r], &mf);
    // lag_in_frames=0 (realtime) => no maximo 1 pacote por frame. O Data fica valido
    // ate o proximo SendFrame nesta rendition (= proximo frame, apos o emit desta).
    MediaPacket p;
    if (g->venc[r]->ReceivePacket(g->venc[r], &p) == 1) { g->rpkt[r] = p; g->rhas[r] = 1; }
}

// Registra o pts do frame e codifica TODAS as renditions em paralelo (pool), depois
// faz o EMIT serial na main (o sink e tocado so pela main -> thread-safe p/ qualquer sink).
static void encode_frame(GwRun* g, uint8_t* y, int sy, uint8_t* u, int su, uint8_t* v, int sv, int w, int h, mtime_us pts)
{
    int idx = g->fpts_n;
    if (g->fpts_n == g->fpts_cap) { g->fpts_cap = g->fpts_cap ? g->fpts_cap * 2 : 1024; g->fpts = (mtime_us*)GW_REALLOC(g->fpts, (size_t)g->fpts_cap * sizeof(mtime_us)); }
    g->fpts[g->fpts_n++] = pts;

    g->cy = y; g->csy = sy; g->cu = u; g->csu = su; g->cv = v; g->csv = sv; g->cw = w; g->ch = h; g->cidx = idx;
    for (int r = 0; r < g->nrend; r++) g->rhas[r] = 0;

    if (g->pool)
    {
        for (int r = 0; r < g->nrend; r++) if (g->venc[r]) pool_submit_relative(g->pool, rend_task, &g->tctx[r]);
        pool_wait_idle_relative(g->pool);
    }
    else
    {
        for (int r = 0; r < g->nrend; r++) if (g->venc[r]) rend_task(&g->tctx[r]);
    }

    // EMIT em ordem de rendition (r==0 controla o corte de segmento).
    for (int r = 0; r < g->nrend; r++) if (g->rhas[r]) emit_vpkt(g, r, &g->rpkt[r]);
}

// ============================================================================
//  Segment-parallel (DASH-WebM, video-only): decode UMA vez na main, bufferiza
//  K segmentos e dispara tasks (rendition x segmento) no pool. Cada task tem seu
//  proprio encoder (Threads=1) e grava um chunk isolado (webm_open_chunk). Com
//  ~nsegs x nrend tasks pequenas, os nucleos ficam cheios. memop e thread-safe.
// ============================================================================
#define GW_SP_KBUF 3        // segmentos em voo (memoria ~ K * frames_seg * bytes_frame)
#define GW_SP_MAXF 256      // teto de frames por segmento (fecha antes se estourar)

typedef struct { uint8_t *y,*u,*v; int sy,su,sv,w,h; int64_t ms; } SpFrame;
typedef struct {
    SpFrame f[GW_SP_MAXF]; int nf; int seg_no;
    xatomic_int pending;    // tasks restantes; 0 => main pode reciclar o slot
    int in_use;
} SpSeg;
typedef struct {
    const char* dir; MediaCodec codec; int fps; int seg_ms; int nrend;
    int rw[GW_MAXR], rh[GW_MAXR], rbw[GW_MAXR]; const char* rname[GW_MAXR];
    xatomic_int done[GW_MAXR];   // segmentos concluidos por rendition (progresso)
    xatomic_int abort;           // 1 = cancelamento cooperativo; tasks saem sem gravar
    xatomic_int failed;          // 1 = alguma task falhou (encoder/mux/alloc)
    xwait_t     wait;            // as tasks acordam a main (fim de task) -> sem busy-wait
    const GwFeedback* fb;        // erros das tasks sobem para a UI
} SpCtx;
typedef struct { SpCtx* c; SpSeg* seg; int r; } SpArg;

static const char* sp_vcodec_id(MediaCodec c) { return c == MEDIA_CODEC_AV1 ? "V_AV1" : "V_VP9"; }   // CodecID do Matroska

// Limite REAL de paralelismo do codec para uma resolucao (nao presumido):
//  - VP9: o paralelismo estrutural sao as COLUNAS DE TILE independentes. Cada coluna
//    precisa de >=256px de largura e o total e potencia de 2 (VP9E_SET_TILE_COLUMNS = log2).
//    Ex.: 1920->4, 1280->4, 854->2, 640->2, 428->1, 256->1. (row-mt fica ligado e distribui
//    essas threads pelas linhas; nao aumenta o nro de unidades independentes.)
//  - AV1 (SVT): paraleliza pipeline/tiles/segmentos e usa praticamente todos os cores.
// Nucleos FISICOS (nao logicos): p/ encode CPU-bound o recurso real sao os cores
// fisicos (HT rende pouco). No Windows conta RelationProcessorCore; fallback = xcpu_count.
static int phys_cores(void)
{
#ifdef _WIN32
    DWORD len = 0; GetLogicalProcessorInformation(NULL, &len);
    if (len > 0)
    {
        SYSTEM_LOGICAL_PROCESSOR_INFORMATION* buf = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION*)GW_ALLOC(len);
        if (buf)
        {
            int count = 0;
            if (GetLogicalProcessorInformation(buf, &len))
            { DWORD n = len / (DWORD)sizeof(buf[0]); for (DWORD i = 0; i < n; i++) if (buf[i].Relationship == RelationProcessorCore) count++; }
            GW_FREE(buf);
            if (count > 0) return count;
        }
    }
#endif
    int n = xcpu_count(); return n > 1 ? n : 1;
}

static int codec_thread_limit(MediaCodec codec, int w, int h, int cores)
{
    (void)h;
    if (cores < 1) cores = 1;
    if (codec == MEDIA_CODEC_AV1)  return cores;         // SVT-AV1: pipeline/tiles/segmentos -> cores
    if (codec == MEDIA_CODEC_H265) return cores;         // x265: frame-threads + WPP -> escala p/ cores
    if (codec == MEDIA_CODEC_H264)                        // openh264: por SLICES, satura ~2-4
    {
        int t = 4; if (t > cores) t = cores; return t;
    }
    int tile_cols = 1;                                   // VP9: colunas de tile (>=256px cada)
    while (w / (tile_cols * 2) >= 256 && tile_cols < 64) tile_cols *= 2;
    if (tile_cols > cores) tile_cols = cores;
    return tile_cols;
}
static const char* sp_mpd_codec(MediaCodec c) { return c == MEDIA_CODEC_AV1 ? "av01.0.08M.08" : "vp9"; }

// Task: encoda 1 rendition de 1 segmento e grava chunk-<name>-<seg+1>.webm.
static void sp_task(void* arg)
{
    SpArg* a = (SpArg*)arg; SpCtx* c = a->c; SpSeg* seg = a->seg; int r = a->r;
    int rw = c->rw[r], rh = c->rh[r];
    MediaEncoderParams ep; GW_ZERO(&ep, sizeof(ep));
    ep.Codec = c->codec; ep.Width = rw; ep.Height = rh; ep.Fps = c->fps; ep.BitrateBps = c->rbw[r]; ep.SpeedPreset = 8; ep.Threads = 1;
    // Segment-parallel fica em SOFTWARE de proposito, mesmo com hardware disponivel:
    //  - abre UM encoder por segmento x rendition, em paralelo; a GeForce limita quantas
    //    sessoes NVENC rodam juntas, entao parte das tasks cairia para software;
    //  - e isso misturaria encoders DENTRO de uma mesma rendition, com cabecalhos de
    //    sequencia diferentes -- o init segment do AV1 carrega um so (av1C).
    // Aqui o degrau que escala e o THREADS: nsegs x nrend tasks enchem todos os nucleos.
    ep.Accel = MEDIA_ACCEL_SOFTWARE;
    if (atomic_get_inline(&c->abort))   // cancelado antes de comecar: nao cria arquivo
    {
        atomic_add_inline(&c->done[r], 1);
        atomic_sub_inline(&seg->pending, 1);
        thread_wait_wake_inline(&c->wait);
        return;
    }
    MediaEncoder* enc = media_encoder_open(&ep);
    WebmMux* cm = webm_open_chunk(c->dir, c->rname[r], seg->seg_no + 1);   // startNumber=1
    if (!enc || !cm)
    {
        // Falha silenciosa aqui produzia um segmento faltando no manifesto (o player
        // morre no meio). Sinaliza para o gateway abortar e reportar.
        atomic_set_inline(&c->failed, 1);
        gw_error(c->fb, enc ? "gateway: falha ao abrir o chunk WebM de saida."
                            : "gateway: encoder de video indisponivel para uma rendition.");
    }
    if (enc && cm)
    {
        uint8_t *sy = 0, *su = 0, *sv = 0;
        for (int i = 0; i < seg->nf; i++)
        {
            if (atomic_get_inline(&c->abort)) break;   // cancelamento cooperativo
            SpFrame* f = &seg->f[i];
            uint8_t *Y = f->y, *U = f->u, *V = f->v; int SY = f->sy, SU = f->su, SV = f->sv;
            if (rw != f->w || rh != f->h)
            {
                int cw = (rw + 1) / 2, ch = (rh + 1) / 2;
                if (!sy) { sy = (uint8_t*)GW_ALLOC((size_t)rw * rh); su = (uint8_t*)GW_ALLOC((size_t)cw * ch); sv = (uint8_t*)GW_ALLOC((size_t)cw * ch); }
#ifdef HAVE_LIBYUV
                I420Scale(f->y, f->sy, f->u, f->su, f->v, f->sv, f->w, f->h, sy, rw, su, cw, sv, cw, rw, rh, kFilterBilinear);
                Y = sy; U = su; V = sv; SY = rw; SU = cw; SV = cw;
#else
                continue;   // sem libyuv nao ha rescale
#endif
            }
            MediaFrame mf; GW_ZERO(&mf, sizeof(mf));
            mf.Kind = MEDIA_KIND_VIDEO; mf.Width = rw; mf.Height = rh;
            mf.Y = Y; mf.StrideY = SY; mf.U = U; mf.StrideU = SU; mf.V = V; mf.StrideV = SV; mf.Pts = i;   // vpx timebase 1/fps
            enc->SendFrame(enc, &mf);
            MediaPacket p; while (enc->ReceivePacket(enc, &p) == 1) webm_write_video(cm, f->ms, p.KeyFrame, p.Data, p.Size);
        }
        enc->SendFrame(enc, 0);   // drena
        { MediaPacket p; int64_t lastms = seg->nf ? seg->f[seg->nf - 1].ms : 0; while (enc->ReceivePacket(enc, &p) == 1) webm_write_video(cm, lastms, p.KeyFrame, p.Data, p.Size); }
        GW_FREE(sy); GW_FREE(su); GW_FREE(sv);
    }
    if (cm) webm_close(cm);        // grava chunk-<name>-<seg+1>.webm
    if (enc) enc->Close(enc);
    atomic_add_inline(&c->done[r], 1);
    atomic_sub_inline(&seg->pending, 1);
    thread_wait_wake_inline(&c->wait);   // libera a main (backpressure / espera final)
}

static void sp_seg_free(SpSeg* s)
{
    for (int i = 0; i < s->nf; i++) { GW_FREE(s->f[i].y); GW_FREE(s->f[i].u); GW_FREE(s->f[i].v); }
    s->nf = 0; s->in_use = 0;
}
// 1 = frame acumulado; 0 = segmento cheio (o chamador deve fechar e abrir outro);
// -1 = falha de alocacao (erro real, o chamador aborta). Antes as tres situacoes eram
// um 'return' mudo: o frame sumia do segmento sem ninguem ficar sabendo.
static int sp_seg_append(SpSeg* s, uint8_t* y, int sy, uint8_t* u, int su, uint8_t* v, int sv, int w, int h, int64_t ms)
{
    if (s->nf >= GW_SP_MAXF) return 0;
    int cw = (w + 1) / 2, ch = (h + 1) / 2;
    SpFrame* f = &s->f[s->nf];
    f->y = (uint8_t*)GW_ALLOC((size_t)w * h); f->u = (uint8_t*)GW_ALLOC((size_t)cw * ch); f->v = (uint8_t*)GW_ALLOC((size_t)cw * ch);
    if (!f->y || !f->u || !f->v) { GW_FREE(f->y); GW_FREE(f->u); GW_FREE(f->v); f->y = f->u = f->v = 0; return -1; }
    for (int rr = 0; rr < h;  rr++) GW_COPY(f->y + (size_t)rr * w,  y + (size_t)rr * sy, w);
    for (int rr = 0; rr < ch; rr++) { GW_COPY(f->u + (size_t)rr * cw, u + (size_t)rr * su, cw); GW_COPY(f->v + (size_t)rr * cw, v + (size_t)rr * sv, cw); }
    f->sy = w; f->su = cw; f->sv = cw; f->w = w; f->h = h; f->ms = ms;
    s->nf++;
    return 1;
}

static void sp_write_mpd(const SpCtx* c, int64_t dur_ms)
{
    char path[1200]; snprintf(path, sizeof(path), "%s/manifest.mpd", c->dir);
    FILE* fmpd = 0; if (fopen_s(&fmpd, path, "wb") != 0 || !fmpd) return;
    fprintf(fmpd,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<MPD xmlns=\"urn:mpeg:dash:schema:mpd:2011\" type=\"static\" "
        "mediaPresentationDuration=\"PT%.3fS\" minBufferTime=\"PT2S\" "
        "profiles=\"urn:mpeg:dash:profile:isoff-live:2011\">\n  <Period>\n"
        "    <AdaptationSet mimeType=\"video/webm\" codecs=\"%s\" segmentAlignment=\"true\" startWithSAP=\"1\">\n"
        "      <SegmentTemplate timescale=\"1000\" duration=\"%d\" startNumber=\"1\" "
        "initialization=\"init-$RepresentationID$.webm\" media=\"chunk-$RepresentationID$-$Number$.webm\"/>\n",
        dur_ms / 1000.0, sp_mpd_codec(c->codec), c->seg_ms);
    for (int r = 0; r < c->nrend; r++)
        fprintf(fmpd, "      <Representation id=\"%s\" bandwidth=\"%d\" width=\"%d\" height=\"%d\"/>\n",
                c->rname[r], c->rbw[r], c->rw[r], c->rh[r]);
    fprintf(fmpd, "    </AdaptationSet>\n  </Period>\n</MPD>\n");
    fclose(fmpd);
}

// Bitrate padrao por resolucao, quando o perfil nao pede um.
static int gw_default_bitrate(int w, int h)
{
    if (w >= 1920 || h >= 1080) return 5000000;
    if (w >= 1280 || h >=  720) return 3000000;
    if (w >=  854 || h >=  480) return 1400000;
    if (w >=  640 || h >=  360) return  800000;
    if (w >=  428 || h >=  240) return  500000;
    return 300000;
}

// Reamostrador de taxa por PTS. Sem tocar no conteudo: decide QUAIS frames seguem, a
// partir do tempo de apresentacao. out_fps <= 0 desliga (tudo passa).
//
// Detalhe que custa 10% da taxa se ignorado: um pacote H.26x pode render MAIS DE UMA
// imagem, e o gateway atribui a todas o PTS do pacote. Com PTS repetido, a segunda imagem
// cairia sempre no mesmo instante da primeira e seria descartada -- pedir 5 fps entregava
// 4,5. O gate mantem um relogio proprio que avanca pelo menos um intervalo de ENTRADA a
// cada imagem, entao imagens irmas ocupam instantes distintos.
typedef struct
{
    int      out_fps;
    mtime_us next_due;
    mtime_us step;       // intervalo alvo de SAIDA
    mtime_us in_step;    // intervalo tipico de ENTRADA (para desempatar PTS repetido)
    mtime_us clock;      // PTS efetivo da ultima imagem vista
}
GwFpsGate;

static void gw_fps_gate_init(GwFpsGate* g, int out_fps, int in_fps)
{
    GW_ZERO(g, sizeof(*g));
    g->out_fps  = out_fps;
    g->step     = out_fps > 0 ? (mtime_us)(1000000.0 / (double)out_fps + 0.5) : 0;
    g->in_step  = in_fps  > 0 ? (mtime_us)(1000000.0 / (double)in_fps  + 0.5) : 0;
    g->next_due = MTIME_NONE;
    g->clock    = MTIME_NONE;
}

// 1 = esta imagem entra na saida; 0 = descarta.
static int gw_fps_gate_take(GwFpsGate* g, mtime_us pts)
{
    if (g->out_fps <= 0) return 1;

    // PTS que nao avancou (imagens irmas do mesmo pacote): usa o relogio interno.
    if (g->clock != MTIME_NONE && pts <= g->clock) pts = g->clock + (g->in_step > 0 ? g->in_step : 1);
    g->clock = pts;

    if (g->next_due == MTIME_NONE) { g->next_due = pts + g->step; return 1; }
    if (pts + 1 < g->next_due) return 0;          // ainda nao chegou a hora do proximo
    // Avanca a grade sem acumular atraso quando a fonte engasga.
    do { g->next_due += g->step; } while (g->next_due <= pts);
    return 1;
}

static int gw_dash_segpar(MediaSource* src, MediaStreamInfo* vin, const MediaProfile* profile,
                          const char* base_dir, const GwFeedback* fb, GwControl* ctl, int vidx)
{
    SpCtx c; GW_ZERO(&c, sizeof(c));
    c.dir = base_dir; c.codec = profile->VideoCodec;
    int sp_fps_in = (int)(vin->Fps + 0.5); if (sp_fps_in <= 0) sp_fps_in = 30;
    // Encoder trabalha na taxa de SAIDA (o gate abaixo descarta o excedente da entrada).
    c.fps = profile->Fps > 0 ? profile->Fps : sp_fps_in;

    GwFpsGate sp_gate;
    gw_fps_gate_init(&sp_gate, (profile->Fps > 0 && profile->Fps < sp_fps_in) ? profile->Fps : 0, sp_fps_in);
    c.seg_ms = profile->SegmentMs > 0 ? profile->SegmentMs : 2000;
    int nrend = profile->RenditionCount > 0 ? profile->RenditionCount : 1; if (nrend > GW_MAXR) nrend = GW_MAXR;
    c.nrend = nrend;
    for (int r = 0; r < nrend; r++)
    {
        if (profile->RenditionCount > 0)
        { c.rw[r] = profile->Renditions[r].Width; c.rh[r] = profile->Renditions[r].Height; c.rbw[r] = profile->Renditions[r].BitrateBps; c.rname[r] = profile->Renditions[r].Name; }
        else
        { c.rw[r] = profile->Width; c.rh[r] = profile->Height; c.rbw[r] = profile->BitrateBps; c.rname[r] = "source"; }
        atomic_initialize_inline(&c.done[r], 0);
    }
    atomic_initialize_inline(&c.abort, 0);
    atomic_initialize_inline(&c.failed, 0);
    thread_wait_prepare_inline(&c.wait);
    c.fb = fb;

    // init-<name>.webm por rendition (reusa o muxer segmentado so p/ gravar o init).
    for (int r = 0; r < nrend; r++)
    { WebmMux* im = webm_open_segmented(base_dir, c.rname[r], sp_vcodec_id(c.codec), c.rw[r], c.rh[r], c.fps, c.seg_ms, 0, 0, 0, 0, 0); if (im) webm_close(im); }

    GwVideoDec* dec = 0;
    if (!vin->Raw) dec = gw_vdec_open(vin->Codec);   // disponibilidade ja checada em gateway_run

    { GwEvent e; GW_ZERO(&e, sizeof(e)); e.Kind = GW_EV_START; e.Total = nrend; gw_emit(fb, &e); }
    for (int r = 0; r < nrend; r++)
    {
        // Segment-parallel usa software por decisao (ver sp_task). O degrau reportado e o
        // primeiro de software disponivel para o codec -- o mesmo que as tasks vao abrir.
        char enc_desc[320]; snprintf(enc_desc, sizeof(enc_desc), "indisponivel");
        EncBackendInfo bi[8]; int nb = media_encoder_list(c.codec, bi, 8); if (nb > 8) nb = 8;
        for (int k = 0; k < nb; k++)
            if (bi[k].Available && bi[k].Tier != ENC_TIER_HARDWARE)
            { snprintf(enc_desc, sizeof(enc_desc), "%s [%s] %s (segment-parallel)", bi[k].Name, enc_tier_name(bi[k].Tier), bi[k].Detail); break; }
        if (dec) { size_t L = strlen(enc_desc); snprintf(enc_desc + L, sizeof(enc_desc) - L, " | decode: %s", gw_vdec_backend(dec)); }
        GwEvent e; GW_ZERO(&e, sizeof(e)); e.Kind = GW_EV_TRACK_START; e.TrackIndex = r; e.Name = c.rname[r]; e.Width = c.rw[r]; e.Height = c.rh[r]; e.Bandwidth = c.rbw[r];
        e.Message = enc_desc; gw_emit(fb, &e);
    }

    ThreadPool* pool = pool_create_relative(xcpu_count());   // ~nsegs*nrend tasks pequenas enchem os cores
    SpSeg segs[GW_SP_KBUF]; GW_ZERO(segs, sizeof(segs));
    SpArg args[GW_SP_KBUF][GW_MAXR];
    for (int k = 0; k < GW_SP_KBUF; k++) atomic_initialize_inline(&segs[k].pending, 0);

    int seg_no = 0; int64_t max_ms = 0, seg_base = MTIME_NONE; SpSeg* cur = 0; int cur_slot = -1;

    // fecha (dispara) o segmento atual. seg_no so avanca AQUI: antes era incrementado
    // no acquire, entao um slot adquirido e nunca despachado (EOF logo apos o acquire)
    // deixava seg_no maior que o total real de tasks -> a espera final (done >= seg_no)
    // nunca era satisfeita e o gateway travava para sempre.
    #define SP_DISPATCH() do { \
        if (cur && cur->nf > 0) { cur->seg_no = seg_no++; atomic_set_inline(&cur->pending, nrend); \
            for (int r = 0; r < nrend; r++) { args[cur_slot][r].c = &c; args[cur_slot][r].seg = cur; args[cur_slot][r].r = r; \
                if (pool) pool_submit_relative(pool, sp_task, &args[cur_slot][r]); else sp_task(&args[cur_slot][r]); } } \
        else if (cur) { cur->in_use = 0; } \
        cur = 0; \
    } while (0)

    // pega um slot livre (in_use==0) OU concluido (pending==0) p/ reciclar; dorme se cheio.
    // A espera e por SINAL (thread_wait): a task acorda a main ao terminar. O busy-wait
    // anterior queimava um nucleo inteiro justamente quando o pool estava saturado.
    #define SP_ACQUIRE(base_ms) do { \
        cur = 0; \
        for (;;) { \
            for (int k = 0; k < GW_SP_KBUF; k++) { \
                if (!segs[k].in_use) { cur = &segs[k]; cur_slot = k; break; } \
                if (atomic_get_inline(&segs[k].pending) == 0) { sp_seg_free(&segs[k]); cur = &segs[k]; cur_slot = k; break; } \
            } \
            if (cur) break; \
            thread_wait_sleep_for_inline(&c.wait, 20000); /* backpressure: teto de 20ms */ \
        } \
        cur->nf = 0; cur->in_use = 1; atomic_set_inline(&cur->pending, 0); \
        seg_base = (base_ms); \
    } while (0)

    GwPacket pkt; int rr; mtime_us last_prog = MTIME_NONE; int aborted = 0;
    mtime_us last_pts = 0;                                  // ultimo PTS lido (base do drain)
    while ((rr = src->Read(src, &pkt)) == 1)
    {
        // Cancelamento pedido pela UI, ou falha reportada por alguma task: propaga o
        // abort para as tasks em voo (elas saem sem gravar) e para o laco de leitura.
        if ((ctl && ctl->Stop) || atomic_get_inline(&c.failed))
        { aborted = 1; atomic_set_inline(&c.abort, 1); break; }
        if (pkt.Stream != vidx) continue;
        int64_t ms = pkt.Pts / 1000;
        last_pts = pkt.Pts;

        // Progresso AO VIVO durante a decode+dispatch (a fase mais longa: o backpressure a
        // atrela ao ritmo do encode). Sem isso o cliente so recebia progresso no loop final
        // (ultimos KBUF segmentos) -> a UI ficava "congelada" por minutos no DASH/VP9. Global
        // por Pts/duracao (o sse_feedback calcula o %), ~1x por segundo de conteudo.
        if (last_prog == MTIME_NONE || pkt.Pts - last_prog >= 1000000)
        { last_prog = pkt.Pts; GwEvent e; GW_ZERO(&e, sizeof(e)); e.Kind = GW_EV_PROGRESS; e.Pts = pkt.Pts; gw_emit(fb, &e); }

        if (vin->Raw)
        {
            if (!gw_fps_gate_take(&sp_gate, pkt.Pts)) continue;
            int w = vin->Width, h = vin->Height, cw = (w + 1) / 2, ch = (h + 1) / 2;
            uint8_t* Y = pkt.Data; uint8_t* U = Y + (size_t)w * h; uint8_t* V = U + (size_t)cw * ch;
            if (!cur || (ms - seg_base) >= c.seg_ms || cur->nf >= GW_SP_MAXF) { SP_DISPATCH(); SP_ACQUIRE(ms); }
            if (sp_seg_append(cur, Y, w, U, cw, V, cw, w, h, ms) < 0)
            { gw_error(fb, "gateway: sem memoria para bufferizar o segmento."); aborted = 1; atomic_set_inline(&c.abort, 1); break; }
            if (ms > max_ms) max_ms = ms;
        }
        else if (dec)
        {
            gw_vdec_send(dec, pkt.Data, pkt.Size);
            GwImage im;
            while (gw_vdec_next(dec, &im) == 1)
            {
                if (!gw_fps_gate_take(&sp_gate, pkt.Pts)) continue;
                if (!cur || (ms - seg_base) >= c.seg_ms || cur->nf >= GW_SP_MAXF) { SP_DISPATCH(); SP_ACQUIRE(ms); }
                if (sp_seg_append(cur, im.Planes[0], im.Strides[0], im.Planes[1], im.Strides[1], im.Planes[2], im.Strides[2], im.Width, im.Height, ms) < 0)
                { gw_error(fb, "gateway: sem memoria para bufferizar o segmento."); aborted = 1; atomic_set_inline(&c.abort, 1); break; }
                if (ms > max_ms) max_ms = ms;
            }
            if (aborted) break;
        }
    }
    if (rr < 0 && !aborted) { gw_error(fb, "gateway: falha ao ler a fonte."); aborted = 1; atomic_set_inline(&c.abort, 1); }

    // DRAIN do decoder. Decoder com reordenacao (o dav1d segura alguns frames na fila
    // interna) so devolve os ultimos quando parar de receber entrada; sem este flush o
    // video saia com os frames finais faltando, silenciosamente.
    if (!aborted && dec)
    {
        gw_vdec_send(dec, 0, 0);
        // Os frames drenados nao trazem PTS proprio: seguem a cadencia da entrada a partir
        // do ultimo pacote lido. Empilhar todos no mesmo timestamp faria o segmento final
        // ter varios frames simultaneos.
        mtime_us step = (mtime_us)(sp_fps_in > 0 ? 1000000 / sp_fps_in : 33333);
        GwImage im;
        while (gw_vdec_next(dec, &im) == 1)
        {
            last_pts += step;
            if (!gw_fps_gate_take(&sp_gate, last_pts)) continue;
            int64_t ms = last_pts / 1000;
            if (!cur || (ms - seg_base) >= c.seg_ms || cur->nf >= GW_SP_MAXF) { SP_DISPATCH(); SP_ACQUIRE(ms); }
            if (sp_seg_append(cur, im.Planes[0], im.Strides[0], im.Planes[1], im.Strides[1], im.Planes[2], im.Strides[2], im.Width, im.Height, ms) < 0)
            { gw_error(fb, "gateway: sem memoria para bufferizar o segmento."); aborted = 1; atomic_set_inline(&c.abort, 1); break; }
            if (ms > max_ms) max_ms = ms;
        }
    }

    if (aborted) { if (cur) { sp_seg_free(cur); cur = 0; } }   // segmento parcial nao vai para o disco
    else SP_DISPATCH();   // ultimo segmento

    // aguarda os encodes, emitindo progresso e "pronto" POR PISTA (barras distintas).
    // done[r] = segmentos concluidos da rendition r; total por pista = seg_no.
    int lastp[GW_MAXR], demit[GW_MAXR];
    for (int r = 0; r < nrend; r++) { lastp[r] = -1; demit[r] = 0; }
    for (;;)
    {
        int alldone = 1;
        for (int r = 0; r < nrend; r++)
        {
            int d = atomic_get_inline(&c.done[r]);
            if (d != lastp[r] && !aborted) { lastp[r] = d; GwEvent e; GW_ZERO(&e, sizeof(e)); e.Kind = GW_EV_PROGRESS; e.Name = c.rname[r]; e.Progress = seg_no > 0 ? (double)d / seg_no : 0; gw_emit(fb, &e); }
            if (seg_no > 0 && d >= seg_no)
            {
                if (!demit[r] && !aborted) { demit[r] = 1; GwEvent e; GW_ZERO(&e, sizeof(e)); e.Kind = GW_EV_TRACK_DONE; e.TrackIndex = r; e.Name = c.rname[r]; e.Width = c.rw[r]; e.Height = c.rh[r]; e.Bandwidth = c.rbw[r]; gw_emit(fb, &e); }
            }
            else alldone = 0;
        }
        // Uma task pode reportar falha depois do laco de leitura: propaga o abort para
        // as demais em voo (elas saem cedo) em vez de esperar o encode inteiro.
        if (!aborted && atomic_get_inline(&c.failed)) { aborted = 1; atomic_set_inline(&c.abort, 1); }
        if (alldone) break;
        thread_wait_sleep_for_inline(&c.wait, 50000);   // acordado pelas tasks; teto de 50ms
    }
    if (pool) { pool_wait_idle_relative(pool); pool_destroy_relative(pool); }
    for (int k = 0; k < GW_SP_KBUF; k++) if (segs[k].in_use) sp_seg_free(&segs[k]);
    if (dec) gw_vdec_close(&dec);

    if (aborted)
    {
        // NAO escreve o manifesto. Um .mpd apontando para segmentos que nao existem
        // faz o player morrer no meio, e a sessao fica "meio pronta" -- exatamente o
        // estado que contaminava a proxima fragmentacao.
        gw_cancelled(fb, "preparacao interrompida");
        gw_emit_mem(fb);
        #undef SP_DISPATCH
        #undef SP_ACQUIRE
        return -10;
    }

    for (int r = 0; r < nrend; r++) if (!demit[r]) { GwEvent e; GW_ZERO(&e, sizeof(e)); e.Kind = GW_EV_TRACK_DONE; e.TrackIndex = r; e.Name = c.rname[r]; e.Width = c.rw[r]; e.Height = c.rh[r]; e.Bandwidth = c.rbw[r]; gw_emit(fb, &e); }

    sp_write_mpd(&c, max_ms);
    { GwEvent e; GW_ZERO(&e, sizeof(e)); e.Kind = GW_EV_DONE; e.Playlist = "manifest.mpd"; gw_emit(fb, &e); }
    gw_emit_mem(fb);

    #undef SP_DISPATCH
    #undef SP_ACQUIRE
    return 0;
}

int gateway_run(MediaSource* src, const MediaProfile* profile, const char* base_dir, const GwFeedback* fb, GwControl* ctl)
{
    if (!src || !profile) return -1;

    MediaStreamInfo st[4]; int nst = 0; src->Info(src, st, 4, &nst);
    int vidx = -1, aidx = -1;
    for (int i = 0; i < nst; i++) { if (st[i].Type == MSTREAM_VIDEO && vidx < 0) vidx = i; else if (st[i].Type == MSTREAM_AUDIO && aidx < 0) aidx = i; }
    if (vidx < 0) { gw_error(fb, "gateway: sem stream de video."); return -2; }
    MediaStreamInfo* vin = &st[vidx]; MediaStreamInfo* ain = aidx >= 0 ? &st[aidx] : 0;

    // ---- perfil EFETIVO -----------------------------------------------------
    // O que o chamador deixou em 0/NONE significa "herda da entrada". Resolver isso AQUI,
    // uma vez, evita que cada caminho (segpar, por-frame, sink) reinvente a heranca --
    // era assim que o bitrate acabava chumbado em tres lugares diferentes.
    MediaProfile eff = *profile;
    if (eff.VideoCodec == MEDIA_CODEC_NONE)
        eff.VideoCodec = vin->Raw ? MEDIA_CODEC_H264 : vin->Codec;   // fonte crua nao tem codec a herdar
    if (eff.Width  <= 0) eff.Width  = vin->Width;
    if (eff.Height <= 0) eff.Height = vin->Height;
    if (eff.Fps    <= 0) eff.Fps    = (int)(vin->Fps + 0.5);
    if (eff.BitrateBps <= 0) eff.BitrateBps = gw_default_bitrate(eff.Width, eff.Height);
    profile = &eff;

    // Um so ponto de validacao do decode de entrada, ANTES de abrir sink/pool/arquivos:
    // os dois caminhos abaixo (segpar e por-frame) precisam do mesmo decoder, e falhar
    // no meio do setup deixaria diretorio criado e wait preparado para tras.
    if (!vin->Raw && !gw_vdec_available(vin->Codec))
    { gw_error(fb, "gateway: nao ha decoder compilado para o codec de entrada."); return -4; }

    // DASH-WebM sem audio: usa o caminho SEGMENT-PARALLEL (tasks rendition x segmento no pool).
    // Com audio, HLS ou MP4: cai no caminho por-frame abaixo (que trata audio/cut/sink).
    //
    // EXCECAO, AV1: o segment-parallel abre UM encoder por (segmento x rendition), e cada
    // instancia do SVT-AV1 e' um pipeline inteiro -- dezenas de threads e centenas de MB.
    // Medido na escada de 6 renditions: 300 threads, 3,6 GB de RAM e 13x de oversubscription
    // de CPU. O libvpx (VP9) aguenta esse padrao porque a instancia dele e' barata; o SVT
    // nao. Para AV1 vale o caminho por-frame: UM encoder por rendition, vivo do inicio ao
    // fim, com memoria e threads limitadas.
    if (profile->Container == CONT_DASH_WEBM && aidx < 0 && profile->VideoCodec != MEDIA_CODEC_AV1)
        return gw_dash_segpar(src, vin, profile, base_dir, fb, ctl, vidx);   // decode-once: sem re-decode 6x

    MediaSink* sink = media_sink_open(profile, base_dir, fb);
    if (!sink) return -3;   // a fabrica/sink ja emitiu o erro

    GwRun g; GW_ZERO(&g, sizeof(g));
    g.sink = sink; g.prof = profile; g.fb = fb; g.seg_start = MTIME_NONE;
    g.seg_ms = profile->SegmentMs > 0 ? profile->SegmentMs : 2000;

    // BYPASS (remux puro): so quando nada de fato muda -- mesmo codec, mesma resolucao,
    // mesma taxa e uma unica pista. Qualquer campo diferente obriga a cadeia
    // decode -> escala/reamostra -> encode.
    int fps_in  = (int)(vin->Fps + 0.5);
    int v_passthrough = (!vin->Raw
                      && vin->Codec == profile->VideoCodec
                      && profile->RenditionCount <= 1
                      && profile->Width  == vin->Width
                      && profile->Height == vin->Height
                      && (profile->Fps <= 0 || profile->Fps == fps_in));
    g.out_vcodec = v_passthrough ? vin->Codec : profile->VideoCodec;

    int nrend = profile->RenditionCount > 0 ? profile->RenditionCount : 1;
    if (nrend > GW_MAXR) nrend = GW_MAXR;
    g.nrend = v_passthrough ? 1 : nrend;
    for (int r = 0; r < g.nrend; r++)
    {
        if (profile->RenditionCount > 0 && !v_passthrough)
        { g.rw[r] = profile->Renditions[r].Width; g.rh[r] = profile->Renditions[r].Height; g.rbw[r] = profile->Renditions[r].BitrateBps; g.rname[r] = profile->Renditions[r].Name; }
        else
        { g.rw[r] = profile->Width; g.rh[r] = profile->Height; g.rbw[r] = profile->BitrateBps; g.rname[r] = "source"; }
    }

    GwVideoDec* dec = 0;
    if (!v_passthrough && !vin->Raw)
    {
        dec = gw_vdec_open(vin->Codec);
        // Sem decoder o laco de leitura ficaria descartando todo pacote de video em
        // silencio e a saida sairia vazia, sem erro nenhum. Melhor falhar aqui.
        if (!dec) { gw_error(fb, "gateway: sem decoder para o codec de entrada."); sink->Aborted = 1; sink->Close(sink); return -1; }
    }

    // Reamostragem de taxa: so age quando a saida pede MENOS que a entrada. Ligada aqui,
    // antes do encode, para nao gastar escala/encode em frame que sera descartado.
    GwFpsGate fps_gate;
    gw_fps_gate_init(&fps_gate, (profile->Fps > 0 && profile->Fps < fps_in) ? profile->Fps : 0, fps_in);

    // Pool p/ paralelizar as renditions (1 task = 1 rendition por frame). Workers = nrend.
    // Se nrend<=1 ou a criacao falhar, cai no serial.
    for (int r = 0; r < g.nrend; r++) { g.tctx[r].g = &g; g.tctx[r].r = r; }
    g.pool = (!v_passthrough && g.nrend > 1) ? pool_create_relative(g.nrend) : 0;

    // Orcamento HIBRIDO de threads de codec (~nucleos logicos):
    //  - renditions GRANDES espalham o proprio codec em >=2 CPUs (VP9 row-mt/tiles),
    //    para o 1080p nao virar o gargalo da barreira por frame;
    //  - renditions PEQUENAS ficam com 1 thread e paralelizam ENTRE si via pool.
    // Reduz das maiores se a soma estourar o orcamento (evita oversubscription).
    int th[GW_MAXR]; for (int r = 0; r < GW_MAXR; r++) th[r] = 1;
    if (g.pool)
    {
        // Limite REAL por codec de saida (VP9 tiles / H265+AV1 cores / H264 slices), com
        // orcamento = nucleos fisicos. Como aqui e lockstep (todas as renditions por frame),
        // a soma dos threads roda simultanea -> corta das maiores se exceder o orcamento.
        int budget = phys_cores(); if (budget < 2) budget = 2;
        for (int r = 0; r < g.nrend; r++) th[r] = codec_thread_limit(g.out_vcodec, g.rw[r], g.rh[r], budget);
        int sum = 0; for (int r = 0; r < g.nrend; r++) sum += th[r];
        while (sum > budget)
        {
            int mx = 0; for (int r = 1; r < g.nrend; r++) if (th[r] > th[mx]) mx = r;
            if (th[mx] <= 1) break;
            th[mx]--; sum--;
        }
    }

    if (!v_passthrough)
        for (int r = 0; r < g.nrend; r++)
        {
            MediaEncoderParams ep; GW_ZERO(&ep, sizeof(ep));
            // Fps do ENCODER e o de saida (o gate ja descartou o excedente): com o da
            // entrada, o codec calcularia GOP e rate control para uma taxa que nao existe.
            ep.Codec = g.out_vcodec; ep.Width = g.rw[r]; ep.Height = g.rh[r];
            ep.Fps = profile->Fps > 0 ? profile->Fps : fps_in;
            ep.BitrateBps = g.rbw[r]; ep.SpeedPreset = 8;
            ep.Threads = g.pool ? th[r] : 0;   // com pool: threads por rendition; sem pool: default do codec
            g.venc[r] = media_encoder_open(&ep);
            if (!g.venc[r]) gw_error(fb, "gateway: encoder de saida indisponivel para uma rendition.");
        }

    int a_pass  = (ain && !ain->Raw && ain->Codec == MEDIA_CODEC_AAC && profile->AudioCodec == MEDIA_CODEC_AAC);
    int a_trans = (ain && !ain->Raw && ain->Codec == MEDIA_CODEC_AAC && profile->AudioCodec == MEDIA_CODEC_OPUS);
    void* adec = 0; MediaEncoder* aenc = 0; mtime_us a_ts = 0;
    if (a_trans)
    {
        adec = aac_dec_open(ain->Extra, ain->ExtraLen);
        MediaEncoderParams ap; GW_ZERO(&ap, sizeof(ap)); ap.Codec = MEDIA_CODEC_OPUS; ap.SampleRate = ain->SampleRate; ap.Channels = ain->Channels; ap.BitrateBps = 128000;
        aenc = media_encoder_open(&ap);
        if (!adec || !aenc) { if (adec) aac_dec_close(adec); if (aenc) aenc->Close(aenc); adec = 0; aenc = 0; a_trans = 0; }
    }

    MediaTrackOut tr[GW_MAXR + 1]; int tn = 0;
    for (int r = 0; r < g.nrend; r++)
    { MediaTrackOut* t = &tr[tn++]; GW_ZERO(t, sizeof(*t)); t->Type = MSTREAM_VIDEO; t->Codec = g.out_vcodec; t->Width = g.rw[r]; t->Height = g.rh[r]; t->Fps = vin->Fps; t->Bandwidth = g.rbw[r]; t->Name = g.rname[r]; }
    int atrack = -1;
    if (a_pass || a_trans)
    {
        MediaTrackOut* t = &tr[tn]; GW_ZERO(t, sizeof(*t)); t->Type = MSTREAM_AUDIO;
        t->Codec = a_pass ? MEDIA_CODEC_AAC : MEDIA_CODEC_OPUS;
        t->SampleRate = ain->SampleRate; t->Channels = ain->Channels;
        if (a_pass) { t->Extra = ain->Extra; t->ExtraLen = ain->ExtraLen; }
        atrack = tn; tn++;
    }
    sink->Start(sink, tr, tn);

    { GwEvent e; GW_ZERO(&e, sizeof(e)); e.Kind = GW_EV_START; e.Total = g.nrend; gw_emit(fb, &e); }
    for (int r = 0; r < g.nrend; r++)
    {
        // Qual degrau atendeu esta rendition: e o que a UI mostra para dizer se a GPU foi
        // usada -- e, se nao foi, o que entrou no lugar. No bypass nao ha encoder nenhum.
        char enc_desc[320];
        MediaEncoder* ve = g.venc[r];
        if (v_passthrough) snprintf(enc_desc, sizeof(enc_desc), "bypass (sem reencode)");
        else if (ve)       snprintf(enc_desc, sizeof(enc_desc), "%s [%s] %s", ve->Backend ? ve->Backend : "?", enc_tier_name((EncTier)ve->Tier), ve->BackendDetail);
        else               snprintf(enc_desc, sizeof(enc_desc), "indisponivel");
        if (dec) { size_t L = strlen(enc_desc); snprintf(enc_desc + L, sizeof(enc_desc) - L, " | decode: %s", gw_vdec_backend(dec)); }
        GwEvent e; GW_ZERO(&e, sizeof(e)); e.Kind = GW_EV_TRACK_START; e.TrackIndex = r; e.Name = g.rname[r]; e.Width = g.rw[r]; e.Height = g.rh[r]; e.Bandwidth = g.rbw[r];
        e.Message = enc_desc; gw_emit(fb, &e);
    }

    GwPacket pkt; int rr; mtime_us last_prog = MTIME_NONE; int aborted = 0;
    mtime_us last_vpts = 0;                                 // ultimo PTS de video lido (base do drain)
    while ((rr = src->Read(src, &pkt)) == 1)
    {
        if (ctl && ctl->Stop) { aborted = 1; break; }
        if (pkt.Stream == vidx)
        {
            last_vpts = pkt.Pts;
            if (v_passthrough)
            {
                if (pkt.KeyFrame)
                {
                    if (g.seg_start == MTIME_NONE) g.seg_start = pkt.Pts;
                    else if (pkt.Pts - g.seg_start >= (mtime_us)g.seg_ms * 750) { sink->Cut(sink, pkt.Pts); g.seg_start = pkt.Pts; }   // 25% de tolerancia (ver emit_vpkt)
                }
                sink->Write(sink, 0, &pkt);
            }
            else if (vin->Raw)
            {
                if (gw_fps_gate_take(&fps_gate, pkt.Pts))
                {
                    int w = vin->Width, h = vin->Height, cw = (w + 1) / 2, ch = (h + 1) / 2;
                    uint8_t* Y = pkt.Data; uint8_t* U = Y + (size_t)w * h; uint8_t* V = U + (size_t)cw * ch;
                    encode_frame(&g, Y, w, U, cw, V, cw, w, h, pkt.Pts);
                }
            }
            else if (dec)
            {
                gw_vdec_send(dec, pkt.Data, pkt.Size);
                GwImage im;
                while (gw_vdec_next(dec, &im) == 1)
                {
                    if (!gw_fps_gate_take(&fps_gate, pkt.Pts)) continue;
                    encode_frame(&g, im.Planes[0], im.Strides[0], im.Planes[1], im.Strides[1], im.Planes[2], im.Strides[2], im.Width, im.Height, pkt.Pts);
                }
            }
            if (last_prog == MTIME_NONE || pkt.Pts - last_prog >= 1000000)
            { last_prog = pkt.Pts; GwEvent e; GW_ZERO(&e, sizeof(e)); e.Kind = GW_EV_PROGRESS; e.Pts = pkt.Pts; gw_emit(fb, &e); }
        }
        else if (aidx >= 0 && pkt.Stream == aidx && atrack >= 0)
        {
            if (a_pass) sink->Write(sink, atrack, &pkt);
            else if (a_trans)
            {
                int16_t* pcm; int ns;
                if (aac_dec_decode(adec, pkt.Data, pkt.Size, &pcm, &ns) == 1)
                {
                    MediaFrame af; GW_ZERO(&af, sizeof(af)); af.Kind = MEDIA_KIND_AUDIO; af.SampleRate = ain->SampleRate; af.Channels = ain->Channels; af.Pcm = pcm; af.PcmSamples = ns;
                    aenc->SendFrame(aenc, &af);
                    MediaPacket op;
                    while (aenc->ReceivePacket(aenc, &op) == 1)
                    { GwPacket gp; GW_ZERO(&gp, sizeof(gp)); gp.Stream = atrack; gp.Data = op.Data; gp.Size = op.Size; gp.Pts = gp.Dts = a_ts; gp.KeyFrame = 1; sink->Write(sink, atrack, &gp); a_ts += 20000; }
                }
            }
        }
    }

    if (rr < 0 && !aborted) { gw_error(fb, "gateway: falha ao ler a fonte."); aborted = 1; }

    // DRAIN do decoder antes de drenar os encoders -- ver a nota no caminho segment-parallel.
    if (!aborted && dec)
    {
        gw_vdec_send(dec, 0, 0);
        mtime_us step = (mtime_us)(fps_in > 0 ? 1000000 / fps_in : 33333);   // ver nota no segpar
        GwImage im;
        while (gw_vdec_next(dec, &im) == 1)
        {
            last_vpts += step;
            if (gw_fps_gate_take(&fps_gate, last_vpts))
                encode_frame(&g, im.Planes[0], im.Strides[0], im.Planes[1], im.Strides[1], im.Planes[2], im.Strides[2], im.Width, im.Height, last_vpts);
        }
    }

    // Drenar os encoders so faz sentido se a saida vai ser publicada. Num cancelamento,
    // drenar so gastaria tempo escrevendo amostras num arquivo que sera descartado.
    if (!aborted && !v_passthrough)
        for (int r = 0; r < g.nrend; r++)
            if (g.venc[r]) { g.venc[r]->SendFrame(g.venc[r], 0); MediaPacket p; while (g.venc[r]->ReceivePacket(g.venc[r], &p) == 1) emit_vpkt(&g, r, &p); }

    if (!aborted && a_trans && aenc)   // drena o encoder de audio (Opus)
    { aenc->SendFrame(aenc, 0); MediaPacket op; while (aenc->ReceivePacket(aenc, &op) == 1) { GwPacket gp; GW_ZERO(&gp, sizeof(gp)); gp.Stream = atrack; gp.Data = op.Data; gp.Size = op.Size; gp.Pts = gp.Dts = a_ts; gp.KeyFrame = 1; sink->Write(sink, atrack, &gp); a_ts += 20000; } }

    if (!aborted)
        for (int r = 0; r < g.nrend; r++)
        { GwEvent e; GW_ZERO(&e, sizeof(e)); e.Kind = GW_EV_TRACK_DONE; e.TrackIndex = r; e.Name = g.rname[r]; e.Width = g.rw[r]; e.Height = g.rh[r]; e.Bandwidth = g.rbw[r]; gw_emit(fb, &e); }

    sink->Aborted = aborted;   // aborta a publicacao, mas o Close ainda libera tudo
    sink->Close(sink);         // manifesto/patch + GW_EV_DONE + GW_EV_MEM (se nao abortado)

    if (g.pool) pool_destroy_relative(g.pool);   // aguarda idle + junta os workers
    if (dec) gw_vdec_close(&dec);
    if (aenc) aenc->Close(aenc);
    if (adec) aac_dec_close(adec);
    for (int r = 0; r < g.nrend; r++) { if (g.venc[r]) g.venc[r]->Close(g.venc[r]); GW_FREE(g.ry[r]); GW_FREE(g.ru[r]); GW_FREE(g.rv[r]); }
    GW_FREE(g.fpts);
    if (aborted) gw_cancelled(fb, "preparacao interrompida");
    gw_emit_mem(fb);     // amostra final (deve voltar ao baseline) p/ deteccao de vazamento
    return aborted ? -10 : 0;
}
