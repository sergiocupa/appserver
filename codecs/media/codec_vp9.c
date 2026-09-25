//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Modulo VP9 (encoder) via libvpx. Referencia de como cada codec se pluga no wrapper.
//  Compile com -DHAVE_LIBVPX e linke a libvpx para ativar; sem isso, o modulo retorna
//  NULL (indisponivel) e o projeto ainda compila.
//
//  Obter libvpx (Windows/MSVC): vcpkg install libvpx  (ou build proprio).

#include "media_codec.h"
#include "codec_parallel.h"
#include "memory_pool.h"   // memop_* (evita declaracao implicita -> ponteiro truncado em x64)
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_LIBVPX

#include <vpx/vpx_encoder.h>
#include <vpx/vp8cx.h>
#include <vpx/vpx_decoder.h>
#include <vpx/vp8dx.h>

typedef struct
{
    vpx_codec_ctx_t  codec;
    vpx_image_t      img;
    vpx_codec_iter_t iter;
    int              width, height, fps;
    int64_t          pts;
    uint8_t*         out;      // buffer para copiar o pacote atual (fica valido ate o proximo SendFrame)
    int              out_cap;
} Vp9Enc;

static int vp9_send(MediaEncoder* e, const MediaFrame* f)
{
    Vp9Enc* c = (Vp9Enc*)e->Ctx;
    c->iter = NULL;   // reinicia o iterador de saida a cada input

    if (!f)   // flush / fim do stream
    {
        vpx_codec_err_t r = vpx_codec_encode(&c->codec, NULL, c->pts, 1, 0, VPX_DL_REALTIME);
        return r == VPX_CODEC_OK ? 0 : -1;
    }

    // Copia os planos I420 do MediaFrame para a vpx_image (respeitando strides).
    for (int y = 0; y < c->height; y++)
        memcpy(c->img.planes[VPX_PLANE_Y] + (size_t)y * c->img.stride[VPX_PLANE_Y],
               f->Y + (size_t)y * f->StrideY, c->width);

    int cw = (c->width + 1) / 2, ch = (c->height + 1) / 2;
    for (int y = 0; y < ch; y++)
    {
        memcpy(c->img.planes[VPX_PLANE_U] + (size_t)y * c->img.stride[VPX_PLANE_U],
               f->U + (size_t)y * f->StrideU, cw);
        memcpy(c->img.planes[VPX_PLANE_V] + (size_t)y * c->img.stride[VPX_PLANE_V],
               f->V + (size_t)y * f->StrideV, cw);
    }

    int flags = f->KeyFrame ? VPX_EFLAG_FORCE_KF : 0;
    vpx_codec_err_t r = vpx_codec_encode(&c->codec, &c->img, c->pts, 1, flags, VPX_DL_REALTIME);
    c->pts++;
    return r == VPX_CODEC_OK ? 0 : -1;
}

static int vp9_recv(MediaEncoder* e, MediaPacket* out)
{
    Vp9Enc* c = (Vp9Enc*)e->Ctx;
    const vpx_codec_cx_pkt_t* pkt;
    while ((pkt = vpx_codec_get_cx_data(&c->codec, &c->iter)) != 0)
    {
        if (pkt->kind != VPX_CODEC_CX_FRAME_PKT) continue;

        int sz = (int)pkt->data.frame.sz;
        if (sz > c->out_cap) { c->out = (uint8_t*)memop_realloc_raw(c->out, sz); c->out_cap = sz; }
        memcpy(c->out, pkt->data.frame.buf, sz);

        out->Data     = c->out;
        out->Size     = sz;
        out->Pts      = out->Dts = pkt->data.frame.pts;
        out->KeyFrame = (pkt->data.frame.flags & VPX_FRAME_IS_KEY) ? 1 : 0;
        return 1;
    }
    return 0;  // precisa de mais input
}

static void vp9_close(MediaEncoder* e)
{
    if (!e) return;
    Vp9Enc* c = (Vp9Enc*)e->Ctx;
    if (c)
    {
        vpx_codec_destroy(&c->codec);
        vpx_img_free(&c->img);
        memop_free_raw(c->out);
        memop_free_raw(c);
    }
    memop_free_raw(e);
}

MediaEncoder* vp9_encoder_open(const MediaEncoderParams* p)
{
    if (!p || p->Width <= 0 || p->Height <= 0) return 0;

    Vp9Enc* c = (Vp9Enc*)memop_calloc_raw(1, sizeof(Vp9Enc));
    if (!c) return 0;
    c->width  = p->Width;
    c->height = p->Height;
    c->fps    = p->Fps > 0 ? p->Fps : 30;

    vpx_codec_enc_cfg_t cfg;
    if (vpx_codec_enc_config_default(vpx_codec_vp9_cx(), &cfg, 0) != VPX_CODEC_OK) { memop_free_raw(c); return 0; }

    cfg.g_w                = p->Width;
    cfg.g_h                = p->Height;
    cfg.g_timebase.num     = 1;
    cfg.g_timebase.den     = c->fps;
    cfg.rc_target_bitrate  = p->BitrateBps > 0 ? (unsigned)(p->BitrateBps / 1000) : 2000; // kbps
    cfg.rc_end_usage       = VPX_VBR;
    // Explicito, pela mesma razao do bEnableFrameSkip do OpenH264: preparar arquivo nao
    // pode perder frame para segurar bitrate. O default do libvpx ja e 0, mas deixar
    // implicito foi exatamente o que custou ~8% dos frames no caminho H.264.
    cfg.rc_dropframe_thresh = 0;
    cfg.g_pass             = VPX_RC_ONE_PASS;
    cfg.g_lag_in_frames    = 0;   // realtime: sem lookahead
    // Regra do VP9: 2 x nucleos fisicos (= 1 x logicos). Medido -- com ROW_MT a curva
    // so satura em 16 numa maquina de 8 fisicos / 16 logicos, e acima disso oscila
    // dentro do ruido. O trabalho por linha e fino e regular, que e o caso em que o
    // hyper-threading ainda rende.
    //
    // Geometria: o ROW_MT distribui LINHAS DE SUPERBLOCO (64 px) dentro de cada tile.
    // As colunas de tile saem da largura, logo abaixo, e sao outro eixo.
    {
        int budget = codec_nucleos_fisicos() * 2;
        int faixas = p->Height / 64;
        cfg.g_threads = (unsigned)codec_threads(p->Threads, budget, faixas);
    }

    if (vpx_codec_enc_init(&c->codec, vpx_codec_vp9_cx(), &cfg, 0) != VPX_CODEC_OK) { memop_free_raw(c); return 0; }

    int speed = p->SpeedPreset > 0 ? p->SpeedPreset : 8;  // 0..9 (maior = mais rapido)
    vpx_codec_control(&c->codec, VP8E_SET_CPUUSED, speed);
    vpx_codec_control(&c->codec, VP9E_SET_ROW_MT, 1);
    // Tile columns = log2(threads), limitado pela largura (>=256px por coluna).
    // Com ROW_MT, isso faz o VP9 realmente espalhar o encode entre os threads.
    {
        // O numero de colunas de tile sai da mesma conta de threads, e o teto pela
        // LARGURA (>= 256 px por coluna, potencia de 2) e o cols_by_w logo abaixo.
        int T = (int)cfg.g_threads;
        int tc = 0; while ((1 << (tc + 1)) <= T && tc < 6) tc++;              // log2(T)
        int cols_by_w = p->Width / 256; int max_tc = 0;
        while ((1 << (max_tc + 1)) <= cols_by_w && max_tc < 6) max_tc++;      // teto pela largura
        if (tc > max_tc) tc = max_tc;
        vpx_codec_control(&c->codec, VP9E_SET_TILE_COLUMNS, tc);
    }

    if (!vpx_img_alloc(&c->img, VPX_IMG_FMT_I420, p->Width, p->Height, 1))
    {
        vpx_codec_destroy(&c->codec); memop_free_raw(c); return 0;
    }

    MediaEncoder* e = (MediaEncoder*)memop_calloc_raw(1, sizeof(MediaEncoder));
    if (!e) { vpx_codec_destroy(&c->codec); vpx_img_free(&c->img); memop_free_raw(c); return 0; }

    e->Codec         = MEDIA_CODEC_VP9;
    e->Ctx           = c;
    e->SendFrame     = vp9_send;
    e->ReceivePacket = vp9_recv;
    e->Close         = vp9_close;
    return e;
}

// ---- decoder ---------------------------------------------------------------
// Os fontes do decoder VP9 (vp9/decoder/*, vp9_dx_iface.c) ja entram no codecs.vcxproj;
// faltava so o adaptador. A saida e I420 em buffer PROPRIO: a vpx_image pertence ao
// decoder e vale so ate o proximo vpx_codec_decode, enquanto o contrato do MediaDecoder
// e "valido ate a proxima chamada" -- copiar aqui evita depender desse detalhe.

typedef struct
{
    vpx_codec_ctx_t  codec;
    vpx_codec_iter_t iter;
    uint8_t*         plane;    // I420 contiguo do frame corrente
    int              cap;
    int              width, height;
} Vp9Dec;

static int vp9d_send(MediaDecoder* d, const MediaPacket* pkt)
{
    Vp9Dec* c = (Vp9Dec*)d->Ctx;
    c->iter = NULL;   // reinicia a iteracao de saida a cada entrada

    if (!pkt) return 0;   // flush: o VP9 nao tem reordenacao pendente

    vpx_codec_err_t r = vpx_codec_decode(&c->codec, pkt->Data, (unsigned int)pkt->Size, NULL, 0);
    return r == VPX_CODEC_OK ? 0 : -1;
}

static int vp9d_recv(MediaDecoder* d, MediaFrame* out)
{
    Vp9Dec* c = (Vp9Dec*)d->Ctx;

    vpx_image_t* img = vpx_codec_get_frame(&c->codec, &c->iter);
    if (!img) return 0;
    if (img->fmt != VPX_IMG_FMT_I420) return -1;   // 4:2:0 8-bit e o que a cadeia consome

    const int w = (int)img->d_w, h = (int)img->d_h;
    const int cw = (w + 1) / 2, ch = (h + 1) / 2;
    const int need = w * h + 2 * (cw * ch);

    if (!c->plane || c->cap < need)
    {
        memop_free_raw(c->plane);
        c->plane = (uint8_t*)memop_alloc_raw(need);
        c->cap = c->plane ? need : 0;
        if (!c->plane) return -1;
    }

    uint8_t* dy = c->plane;
    uint8_t* du = dy + (size_t)w * h;
    uint8_t* dv = du + (size_t)cw * ch;

    for (int y = 0; y < h; y++)
        memcpy(dy + (size_t)y * w, img->planes[VPX_PLANE_Y] + (size_t)y * img->stride[VPX_PLANE_Y], w);
    for (int y = 0; y < ch; y++)
    {
        memcpy(du + (size_t)y * cw, img->planes[VPX_PLANE_U] + (size_t)y * img->stride[VPX_PLANE_U], cw);
        memcpy(dv + (size_t)y * cw, img->planes[VPX_PLANE_V] + (size_t)y * img->stride[VPX_PLANE_V], cw);
    }

    c->width = w; c->height = h;

    memset(out, 0, sizeof(*out));
    out->Kind = MEDIA_KIND_VIDEO;
    out->Width = w; out->Height = h;
    out->Y = dy; out->StrideY = w;
    out->U = du; out->StrideU = cw;
    out->V = dv; out->StrideV = cw;
    return 1;
}

static void vp9d_close(MediaDecoder* d)
{
    if (!d) return;
    Vp9Dec* c = (Vp9Dec*)d->Ctx;
    if (c) { vpx_codec_destroy(&c->codec); memop_free_raw(c->plane); memop_free_raw(c); }
    memop_free_raw(d);
}

MediaDecoder* vp9_decoder_open(void)
{
    Vp9Dec* c = (Vp9Dec*)memop_calloc_raw(1, sizeof(Vp9Dec));
    if (!c) return 0;

    vpx_codec_dec_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    // O decode de VP9 escala por tiles. Antes era 4 fixo -- numero sem origem, que
    // nao acompanhava nem a maquina nem a resolucao. A altura ainda nao e conhecida
    // aqui (o primeiro quadro nao chegou), entao vale so a regra da maquina.
    cfg.threads = (unsigned)codec_threads(0, codec_nucleos_fisicos() * 2, 0);

    if (vpx_codec_dec_init(&c->codec, vpx_codec_vp9_dx(), &cfg, 0) != VPX_CODEC_OK)
    { memop_free_raw(c); return 0; }

    MediaDecoder* d = (MediaDecoder*)memop_calloc_raw(1, sizeof(MediaDecoder));
    if (!d) { vpx_codec_destroy(&c->codec); memop_free_raw(c); return 0; }

    d->Codec = MEDIA_CODEC_VP9;
    d->Ctx = c;
    d->SendPacket = vp9d_send;
    d->ReceiveFrame = vp9d_recv;
    d->Close = vp9d_close;
    return d;
}

#else   // ------- libvpx nao compilada: modulo inativo (mas o projeto compila) -------

MediaEncoder* vp9_encoder_open(const MediaEncoderParams* p) { (void)p; return 0; }
MediaDecoder* vp9_decoder_open(void) { return 0; }

#endif


// ============================================================================
//  O descritor deste projeto -- o UNICO simbolo que o nucleo enxerga daqui.
//  Ver codec_plugin.h.
// ============================================================================
#include "codec_plugin.h"

#ifdef HAVE_LIBVPX
  #define VPX_COMPILADO 1
#else
  #define VPX_COMPILADO 0
#endif

static MediaEncoder* vp9_plugin_enc(const MediaEncoderParams* p, char* detail, int size)
{
    (void)detail; (void)size;
    return vp9_encoder_open(p);
}

// libvpx faz os dois lados: SSE2..AVX2 com deteccao de CPU em runtime (vpx_config.h).
static const CodecPlugin VP9[] =
{
    {
        CODEC_PLUGIN_ABI, MEDIA_CODEC_VP9, CODEC_ROLE_ENCODE | CODEC_ROLE_DECODE,
        "libvpx", "libvpx, SIMD com deteccao em runtime",
        CODEC_OS_ALL, CODEC_ARCH_ALL, VPX_COMPILADO, 0,
        ISA_SSE2, ISA_AVX2, 1,
        vp9_plugin_enc,
        vp9_decoder_open, 0, 0,
        0
    },
};

const CodecPluginSet codec_set_vp9 =
{ CODEC_PLUGIN_ABI, "codec_vp9", (int)(sizeof(VP9) / sizeof(VP9[0])), VP9 };

// Compilado como plugin: o conjunto vira o export do modulo. Compilado estatico,
// esta linha some e o nucleo pega o conjunto pelo extern de sempre.
#ifdef CODEC_PLUGIN_BUILD
CODEC_PLUGIN_DECLARE(codec_set_vp9)
#endif
