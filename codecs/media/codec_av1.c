//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Modulo AV1 (encoder) via SVT-AV1. Ative com -DHAVE_SVTAV1 (vcpkg install svt-av1).
//  ATENCAO: a API do SVT-AV1 muda entre versoes - conferir os nomes/campos contra a
//  versao instalada (svt_av1_enc_* / EbSvtAv1EncConfiguration / EbSvtIOFormat).
//  NAO TESTADO EM RUNTIME.

#include "media_codec.h"
#include "memory_pool.h"   // memop_* (evita declaracao implicita -> ponteiro truncado em x64)
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_SVTAV1
#include "EbSvtAv1Enc.h"
#include "thread_handler.h"   // xmutex_t: serializa a criacao/destruicao do encoder
#include <stdio.h>

// ---- Uma criacao de cada vez ------------------------------------------------
// O gateway abre uma instancia POR RENDITION, em threads do pool. Duas instancias do
// SVT-AV1 subindo ao mesmo tempo nao sobrevivem: medido aqui (6 renditions da escada),
// em paralelo o processo entra em trabalho infinito (1282 s de CPU, 38 threads) ou bate
// no assert de svt_muxing_queue_get_fifo, dentro de svt_av1_enc_init. Com a criacao
// serializada as mesmas 6 sobem em 12,7 s. O lock cobre so criar e destruir -- o encode
// em si continua paralelo entre as renditions.
#ifdef _WIN32
// CRITICAL_SECTION nao tem inicializador estatico: a primeira thread cria, as outras esperam.
static xmutex_t g_svt_lock;
static volatile long g_svt_lock_state;   // 0=nao criado, 2=criando, 1=pronto

static void svt_lock_ready(void)
{
    long st = InterlockedCompareExchange(&g_svt_lock_state, 2, 0);
    if (st == 0)
    {
        thread_mutex_init_inline(&g_svt_lock);
        InterlockedExchange(&g_svt_lock_state, 1);
        return;
    }
    while (st == 2) { SwitchToThread(); st = g_svt_lock_state; }
}
#else
static xmutex_t g_svt_lock = PTHREAD_MUTEX_INITIALIZER;   // pthread ja inicializa estatico
static void svt_lock_ready(void) { }
#endif

typedef struct
{
    EbComponentType*    handle;
    EbBufferHeaderType* in;      // buffer de entrada reutilizado
    EbSvtIOFormat       pic;     // planos apontam para o MediaFrame durante o send
    uint8_t*            out; int out_cap;
    int64_t             pts;
    int                 width, height;
    int                 eos_sent;
} Av1Enc;

static int av1_send(MediaEncoder* e, const MediaFrame* f)
{
    Av1Enc* c = (Av1Enc*)e->Ctx;

    if (!f)   // EOS / flush
    {
        EbBufferHeaderType eos; memset(&eos, 0, sizeof(eos));
        eos.size = sizeof(eos);
        eos.flags = EB_BUFFERFLAG_EOS;
        eos.p_buffer = 0;
        svt_av1_enc_send_picture(c->handle, &eos);
        c->eos_sent = 1;
        return 0;
    }

    c->pic.luma = f->Y; c->pic.cb = f->U; c->pic.cr = f->V;
    c->pic.y_stride = f->StrideY; c->pic.cb_stride = f->StrideU; c->pic.cr_stride = f->StrideV;
    c->pic.width = c->width; c->pic.height = c->height;

    c->in->p_buffer     = (uint8_t*)&c->pic;

    // n_filled_len TEM que ser o tamanho do frame. O SVT-AV1 valida com
    //     read_size = SIZE_OF_ONE_FRAME_IN_BYTES(w, h, csp, is_16bit)
    //     if (read_size > n_filled_len) -> entrada INVALIDA
    // (Source/Lib/Globals/enc_handle.c). Com 0 aqui, TODO frame era rejeitado: o encoder
    // marcava o stream como invalido, imprimia "Invalid API input buffer size detected" e
    // devolvia praticamente nada -- 30 frames viravam um unico pacote de 39 bytes. Ou seja,
    // a saida AV1 nunca produziu video valido.
    // Formula do proprio SVT para 4:2:0 8-bit: w*h + 2*((w*h) >> 1) = w*h*3/2.
    c->in->n_filled_len = (uint32_t)(c->width * c->height * 3 / 2);
    c->in->flags        = 0;
    c->in->pts          = c->pts++;
    c->in->pic_type     = EB_AV1_INVALID_PICTURE;   // deixa o encoder decidir

    svt_av1_enc_send_picture(c->handle, c->in);
    return 0;
}

static int av1_recv(MediaEncoder* e, MediaPacket* out)
{
    Av1Enc* c = (Av1Enc*)e->Ctx;
    EbBufferHeaderType* pkt = 0;
    uint8_t flush = c->eos_sent ? 1 : 0;

    EbErrorType r = svt_av1_enc_get_packet(c->handle, &pkt, flush);
    if (r != EB_ErrorNone || !pkt) return 0;   // fila vazia -> precisa de mais input

    int sz = (int)pkt->n_filled_len;

    // O SVT-AV1 fecha o stream com um buffer VAZIO marcado EB_BUFFERFLAG_EOS. Devolver
    // isso como pacote punha uma amostra de tamanho ZERO no muxer (WebM/MP4) e quebrava
    // qualquer consumidor a jusante -- o dav1d, por exemplo, recusa na entrada
    // ("in->sz > 0 ... failed in dav1d_send_data"). Fim de fila e 0, nao um pacote vazio.
    if (sz <= 0)
    {
        svt_av1_enc_release_out_buffer(&pkt);
        return 0;
    }

    if (sz > c->out_cap) { c->out = (uint8_t*)memop_realloc_raw(c->out, sz); c->out_cap = sz; }
    memcpy(c->out, pkt->p_buffer, sz);

    out->Data     = c->out;
    out->Size     = sz;
    out->Pts      = out->Dts = pkt->pts;
    out->KeyFrame = (pkt->pic_type == EB_AV1_KEY_PICTURE) ? 1 : 0;

    svt_av1_enc_release_out_buffer(&pkt);
    return 1;
}

// Desmonta a instancia do SVT (sob o mesmo lock da criacao) e zera o handle.
static void av1_handle_destroy(Av1Enc* c)
{
    if (!c || !c->handle) return;
    svt_lock_ready();
    thread_mutex_lock_inline(&g_svt_lock);
    svt_av1_enc_deinit(c->handle);
    svt_av1_enc_deinit_handle(c->handle);
    thread_mutex_unlock_inline(&g_svt_lock);
    c->handle = 0;
}

static void av1_close(MediaEncoder* e)
{
    if (!e) return;
    Av1Enc* c = (Av1Enc*)e->Ctx;
    if (c)
    {
        av1_handle_destroy(c);
        memop_free_raw(c->in);
        memop_free_raw(c->out);
        memop_free_raw(c);
    }
    memop_free_raw(e);
}

MediaEncoder* av1_encoder_open(const MediaEncoderParams* p)
{
    if (!p || p->Width <= 0 || p->Height <= 0) return 0;

    Av1Enc* c = (Av1Enc*)memop_calloc_raw(1, sizeof(Av1Enc));
    if (!c) return 0;
    c->width = p->Width; c->height = p->Height;

    svt_lock_ready();
    thread_mutex_lock_inline(&g_svt_lock);

    EbSvtAv1EncConfiguration cfg;
    memset(&cfg, 0, sizeof(cfg));
    if (svt_av1_enc_init_handle(&c->handle, 0, &cfg) != EB_ErrorNone)
    { thread_mutex_unlock_inline(&g_svt_lock); memop_free_raw(c); return 0; }

    cfg.source_width           = p->Width;
    cfg.source_height          = p->Height;
    cfg.encoder_bit_depth      = 8;
    cfg.frame_rate_numerator   = p->Fps > 0 ? p->Fps : 30;
    cfg.frame_rate_denominator = 1;
    cfg.target_bit_rate        = p->BitrateBps > 0 ? p->BitrateBps : 2000000;
    cfg.rate_control_mode      = 1;                          // VBR (verificar enum na versao)
    cfg.enc_mode               = p->SpeedPreset > 0 ? p->SpeedPreset : 8; // preset rapido

    // Paralelismo POR INSTANCIA. Sem isto, cada encoder se dimensiona para a maquina
    // inteira -- e o gateway abre um por rendition, o que multiplica threads que so
    // disputam os mesmos nucleos. O campo e' um NIVEL (1..6), nao uma contagem de threads;
    // 0 deixa o SVT decidir (caso de encoder unico, como a conversao para arquivo).
    if (p->Threads > 0)
    {
        uint32_t level = (uint32_t)(p->Threads < 6 ? p->Threads : 6);
        cfg.level_of_parallelism = level;
        cfg.logical_processors   = level;   // nome antigo do mesmo controle (SVT < 3.0)
    }

    if (svt_av1_enc_set_parameter(c->handle, &cfg) != EB_ErrorNone)
    { svt_av1_enc_deinit_handle(c->handle); thread_mutex_unlock_inline(&g_svt_lock); memop_free_raw(c); return 0; }
    if (svt_av1_enc_init(c->handle) != EB_ErrorNone)
    { svt_av1_enc_deinit_handle(c->handle); thread_mutex_unlock_inline(&g_svt_lock); memop_free_raw(c); return 0; }

    thread_mutex_unlock_inline(&g_svt_lock);

    // Falha daqui para baixo tem de desmontar o encoder que ja subiu. Antes, um caminho
    // chamava av1_close(0) -- que nao faz nada -- e deixava a instancia do SVT viva.
    c->in = (EbBufferHeaderType*)memop_calloc_raw(1, sizeof(EbBufferHeaderType));
    if (!c->in) { av1_handle_destroy(c); memop_free_raw(c); return 0; }
    c->in->size = sizeof(EbBufferHeaderType);

    MediaEncoder* e = (MediaEncoder*)memop_calloc_raw(1, sizeof(MediaEncoder));
    if (!e) { memop_free_raw(c->in); av1_handle_destroy(c); memop_free_raw(c); return 0; }
    e->Codec = MEDIA_CODEC_AV1; e->Ctx = c;
    e->SendFrame = av1_send; e->ReceivePacket = av1_recv; e->Close = av1_close;
    return e;
}

#else   // SVT-AV1 nao compilado

MediaEncoder* av1_encoder_open(const MediaEncoderParams* p) { (void)p; return 0; }

#endif

// ---- Decoder AV1 via dav1d (para preview local / decode de entrada AV1) ----
#ifdef HAVE_DAV1D
#include "dav1d/dav1d.h"
#include <errno.h>

// O dav1d tem contrapressao: quando a fila interna esta cheia, dav1d_send_data devolve
// EAGAIN e NAO consome o buffer -- e preciso drenar imagens e reenviar o MESMO buffer.
// Ignorar isso (o que este codigo fazia) descartava o pacote em silencio e ainda vazava a
// referencia do Dav1dData. Por isso o resto pendente fica guardado no contexto e o
// ReceiveFrame reenvia antes de declarar que nao ha mais imagem.
typedef struct
{
    Dav1dContext* c;
    Dav1dPicture  pic; int have_pic;
    Dav1dData     pending; int has_pending;
} Av1Dec;

static int av1d_send(MediaDecoder* d, const MediaPacket* pkt)
{
    Av1Dec* x = (Av1Dec*)d->Ctx;
    if (!pkt) return 0;   // flush: os frames atrasados saem por ReceiveFrame ate secar

    if (x->has_pending) return -1;   // o chamador nao drenou o pacote anterior

    Dav1dData data; memset(&data, 0, sizeof(data));
    uint8_t* p = dav1d_data_create(&data, pkt->Size);
    if (!p) return -1;
    memcpy(p, pkt->Data, pkt->Size);

    int r = dav1d_send_data(x->c, &data);
    if (r == DAV1D_ERR(EAGAIN) || data.sz > 0)
    { x->pending = data; x->has_pending = 1; return 0; }   // sobrou: reenviado no recv
    if (r < 0) { dav1d_data_unref(&data); return -1; }
    return 0;
}

static int av1d_pull(Av1Dec* x, MediaFrame* out)
{
    if (x->have_pic) { dav1d_picture_unref(&x->pic); x->have_pic = 0; }
    memset(&x->pic, 0, sizeof(x->pic));
    if (dav1d_get_picture(x->c, &x->pic) < 0) return 0;   // EAGAIN: precisa de mais input
    x->have_pic = 1;

    memset(out, 0, sizeof(*out));
    out->Kind = MEDIA_KIND_VIDEO;
    out->Width = x->pic.p.w; out->Height = x->pic.p.h;
    out->Y = (uint8_t*)x->pic.data[0]; out->StrideY = (int)x->pic.stride[0];
    out->U = (uint8_t*)x->pic.data[1]; out->StrideU = (int)x->pic.stride[1];
    out->V = (uint8_t*)x->pic.data[2]; out->StrideV = (int)x->pic.stride[1]; // chroma compartilha stride
    return 1;
}

static int av1d_recv(MediaDecoder* d, MediaFrame* out)
{
    Av1Dec* x = (Av1Dec*)d->Ctx;

    if (av1d_pull(x, out)) return 1;

    // Nada saiu: se ainda ha bitstream retido por EAGAIN, agora que a fila esvaziou ele
    // cabe. Reenvia e tenta de novo -- sem isso o pacote ficaria preso para sempre.
    while (x->has_pending)
    {
        int r = dav1d_send_data(x->c, &x->pending);
        if (x->pending.sz == 0) { x->has_pending = 0; }
        else if (r == DAV1D_ERR(EAGAIN)) { /* segue retido */ }
        else if (r < 0) { dav1d_data_unref(&x->pending); x->has_pending = 0; return -1; }

        if (av1d_pull(x, out)) return 1;
        if (x->has_pending) break;   // continua cheio: sem imagem por ora
    }
    return 0;
}

static void av1d_close(MediaDecoder* d)
{
    if (!d) return;
    Av1Dec* x = (Av1Dec*)d->Ctx;
    if (x)
    {
        if (x->have_pic)     dav1d_picture_unref(&x->pic);
        if (x->has_pending)  dav1d_data_unref(&x->pending);
        if (x->c)            dav1d_close(&x->c);
        memop_free_raw(x);
    }
    memop_free_raw(d);
}

MediaDecoder* av1_decoder_open(void)
{
    Av1Dec* x = (Av1Dec*)memop_calloc_raw(1, sizeof(Av1Dec));
    if (!x) return 0;
    Dav1dSettings s; dav1d_default_settings(&s);
    if (dav1d_open(&x->c, &s) != 0) { memop_free_raw(x); return 0; }
    MediaDecoder* d = (MediaDecoder*)memop_calloc_raw(1, sizeof(MediaDecoder));
    if (!d) { dav1d_close(&x->c); memop_free_raw(x); return 0; }
    d->Codec = MEDIA_CODEC_AV1; d->Ctx = x;
    d->SendPacket = av1d_send; d->ReceiveFrame = av1d_recv; d->Close = av1d_close;
    return d;
}
#else
MediaDecoder* av1_decoder_open(void) { return 0; }   // dav1d nao compilado
#endif


// ============================================================================
//  O descritor deste projeto -- o UNICO simbolo que o nucleo enxerga daqui.
//  Ver codec_plugin.h.
// ============================================================================
#include "codec_plugin.h"

#ifdef HAVE_SVTAV1
  #define SVT_COMPILADO 1
#else
  #define SVT_COMPILADO 0
#endif
#ifdef HAVE_DAV1D
  #define DAV1D_COMPILADO 1
#else
  #define DAV1D_COMPILADO 0
#endif

static MediaEncoder* av1_plugin_enc(const MediaEncoderParams* p, char* detail, int size)
{
    (void)detail; (void)size;
    return av1_encoder_open(p);
}

// Como no H.265, as duas metades vem de libs diferentes: a Intel removeu o decoder do
// SVT-AV1, entao quem decodifica e o dav1d.
static const CodecPlugin AV1[] =
{
    {
        CODEC_PLUGIN_ABI, MEDIA_CODEC_AV1, CODEC_ROLE_ENCODE,
        "svt-av1", "SVT-AV1, kernels AVX2/AVX-512",
        CODEC_OS_ALL, CODEC_ARCH_ALL, SVT_COMPILADO, 0,
        ISA_AVX2, ISA_AVX512, 1,
        av1_plugin_enc,
        0, 0, 0,
        0
    },
    {
        // dav1d: assembly LIGADO em x86-64 (os 46 .asm do NASM); ARM64 continua C puro. Os
        // kernels "sse" exigem SSSE3. Ha kernels AVX-512, mas o dav1d so os usa com o
        // conjunto AVX-512 ICL, que o cpu_features nao distingue do AVX512F -- entao o teto
        // declarado e AVX2, para nao anunciar o que talvez nao rode.
        CODEC_PLUGIN_ABI, MEDIA_CODEC_AV1, CODEC_ROLE_DECODE,
        "dav1d", "dav1d",
        CODEC_OS_ALL, CODEC_ARCH_ALL, DAV1D_COMPILADO, 0,
        ISA_SSSE3, ISA_AVX2, 0,
        0,
        av1_decoder_open, 0, 0,
        0
    },
};

const CodecPluginSet codec_set_av1 =
{ CODEC_PLUGIN_ABI, "codec_av1", (int)(sizeof(AV1) / sizeof(AV1[0])), AV1 };

// Compilado como plugin: o conjunto vira o export do modulo. Compilado estatico,
// esta linha some e o nucleo pega o conjunto pelo extern de sempre.
#ifdef CODEC_PLUGIN_BUILD
CODEC_PLUGIN_DECLARE(codec_set_av1)
#endif
