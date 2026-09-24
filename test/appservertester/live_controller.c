//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  live_controller: ver live_controller.h.
//
//  Duas diferencas em relacao a fragmentacao de arquivo:
//   1) a fragmentacao roda numa THREAD propria, nao dentro da requisicao: ao vivo nao tem
//      fim, entao segurar a conexao HTTP ate acabar nunca devolveria a pagina;
//   2) a saida vai para uma JANELA EM MEMORIA (live_store), nao para a pasta da sessao.
//
//  Tempo de vida: o LiveStore e' criado no start e liberado no stop, sempre com o registro
//  travado e DEPOIS do join da thread -- assim nenhuma leitura em voo enxerga memoria
//  liberada, e a captura nunca espera por um socket lento (a leitura copia e solta).

#include "live_controller.h"
#include "frag_session.h"
#include "sync_gateway.h"
#include "media_source.h"
#include "live_store.h"
#include "media_codec.h"
#include "yason_build.h"
#include "thread_handler.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define LIVE_MAX        4      // sessoes ao vivo simultaneas
#define LIVE_WINDOW     6      // segmentos vivos na janela (6 x 2s = 12s de retrocesso)
#define LIVE_SEGMENT_MS 2000

typedef struct
{
    char       id[80];
    LiveStore* store;
    GwControl  ctl;
    Thread*    thread;
    volatile int running;
    int        width, height, fps, bitrate;   // SAIDA (resolucao vem da barra do player)
    int        in_w, in_h; double in_fps;     // ENTRADA: o modo de captura escolhido
    char       in_format[32];                 // codec ("h264") ou formato cru ("nv12") da camera
    char       codec[16];
    char       device[256];
    char       encoder[256];
    char       error[256];
}
LiveSession;

static LiveSession g_live[LIVE_MAX];
static xmutex_t    g_lock;
static int         g_ready;

static void live_init_once(void)
{
    if (g_ready) return;
    thread_mutex_init_inline(&g_lock);
    g_ready = 1;
}

// ---- helpers de rota/resposta ---------------------------------------------

static void route_at(Message* message, int idx, char* out, size_t out_size)
{
    if (out_size > 0) out[0] = '\0';
    if (!message || idx < 0 || idx >= message->Route.Count) return;
    StringX* s = (StringX*)message->Route.Items[idx];
    int len = (int)s->Length;
    int n = len < (int)out_size - 1 ? len : (int)out_size - 1;
    memcpy(out, s->Content, (size_t)n);
    out[n] = '\0';
}

static void respond_element(Message* message, Element* root, int status)
{
    StringX* s = yb_render(root);
    if (s)
    {
        message->Response = message_response_create_content(status, APPLICATION_JSON, s->Content, (int)s->Length);
        yb_free_render(s);
    }
    else message->Response = message_response_create_text(HTTP_STATUS_INTERNAL_ERROR, "render falhou");
    yb_free(root);
}

static void respond_error(Message* message, int status, const char* msg)
{
    Element* root = yb_root_object();
    yb_bool(root, "ok", 0);
    yb_str(root, "error", (char*)msg);
    respond_element(message, root, status);
}

// ---- registro ---------------------------------------------------------------
// Chamar com o registro travado.
static LiveSession* find_locked(const char* id)
{
    for (int i = 0; i < LIVE_MAX; i++)
        if (g_live[i].store && strcmp(g_live[i].id, id) == 0) return &g_live[i];
    return 0;
}

static LiveSession* free_slot_locked(void)
{
    for (int i = 0; i < LIVE_MAX; i++) if (!g_live[i].store) return &g_live[i];
    return 0;
}

// Formato CRU da camera (a UI grava o pixfmt quando o modo nao e' comprimido).
static GwPixFmt pixfmt_from_slug(const char* slug)
{
    if (!slug || !*slug) return GW_PIX_NONE;
    if (_stricmp(slug, "nv12")  == 0) return GW_PIX_NV12;
    if (_stricmp(slug, "i420")  == 0 || _stricmp(slug, "yv12") == 0) return GW_PIX_I420;
    if (_stricmp(slug, "yuyv")  == 0 || _stricmp(slug, "yuy2") == 0) return GW_PIX_YUYV;
    if (_stricmp(slug, "rgb24") == 0) return GW_PIX_RGB24;
    return GW_PIX_NONE;
}

static MediaCodec codec_from_slug(const char* slug)
{
    if (!slug || !*slug) return MEDIA_CODEC_NONE;
    if (_stricmp(slug, "h264") == 0 || _stricmp(slug, "avc")  == 0) return MEDIA_CODEC_H264;
    if (_stricmp(slug, "h265") == 0 || _stricmp(slug, "hevc") == 0) return MEDIA_CODEC_H265;
    if (_stricmp(slug, "vp9")  == 0) return MEDIA_CODEC_VP9;
    if (_stricmp(slug, "av1")  == 0) return MEDIA_CODEC_AV1;
    return MEDIA_CODEC_NONE;
}

// ---- thread da transmissao --------------------------------------------------

static void live_feedback(void* user, const GwEvent* ev)
{
    LiveSession* s = (LiveSession*)user;
    if (!s || !ev) return;
    if (ev->Kind == GW_EV_TRACK_START && ev->Message)
        snprintf(s->encoder, sizeof(s->encoder), "%s", ev->Message);
    else if (ev->Kind == GW_EV_ERROR && ev->Message)
        snprintf(s->error, sizeof(s->error), "%s", ev->Message);
}

static xthread_result_t live_thread(void* arg)
{
    LiveSession* s = (LiveSession*)arg;

    // A camera abre no modo escolhido na ENTRADA (resolucao, taxa e formato). A resolucao
    // de SAIDA e' outra coisa: se diferir, quem reduz e' o gateway.
    CameraParams cam; memset(&cam, 0, sizeof(cam));
    cam.Device = s->device[0] ? s->device : 0;
    cam.Width  = s->in_w; cam.Height = s->in_h; cam.Fps = s->in_fps;
    cam.Codec  = codec_from_slug(s->in_format);
    cam.PixFmt = pixfmt_from_slug(s->in_format);
    cam.Mode   = cam.Codec  != MEDIA_CODEC_NONE ? SRC_ENCODED
               : cam.PixFmt != GW_PIX_NONE      ? SRC_RAW
                                                : SRC_AUTO;   // formato que nao reconhecemos

    MediaSource* src = source_camera_open(&cam);
    if (!src)
    {
        snprintf(s->error, sizeof(s->error), "nao foi possivel abrir a camera");
        s->running = 0;
        return (xthread_result_t)0;
    }

    MediaProfile profile; memset(&profile, 0, sizeof(profile));
    profile.Container  = CONT_LIVE_MEM;
    profile.VideoCodec = codec_from_slug(s->codec);     // NONE = herda o da camera
    profile.AudioCodec = MEDIA_CODEC_NONE;              // ao vivo: so video por enquanto
    profile.SegmentMs  = LIVE_SEGMENT_MS;
    profile.Mode       = GW_LIVE;
    profile.Width      = s->width; profile.Height = s->height;
    profile.Fps        = s->fps;   profile.BitrateBps = s->bitrate;
    profile.Live       = s->store;

    GwFeedback fb = { live_feedback, s };
    int rc = gateway_run(src, &profile, 0, &fb, &s->ctl);
    src->Close(src);
    printf("[live] sessao '%s' terminou rc=%d stop=%d erro='%s'\n", s->id, rc, s->ctl.Stop, s->error);

    s->running = 0;
    return (xthread_result_t)0;
}

// ---- acoes ------------------------------------------------------------------

static void live_stop_session(LiveSession* s)
{
    // Ordem importa: sinaliza, espera a thread morrer e so entao libera a janela --
    // a leitura HTTP so enxerga o store com o registro travado.
    s->ctl.Stop = 1;
    if (s->thread) thread_join(&s->thread);
    s->thread = 0;
    s->running = 0;

    thread_mutex_lock_inline(&g_lock);
    LiveStore* store = s->store;
    s->store = 0;
    s->id[0] = '\0';
    thread_mutex_unlock_inline(&g_lock);

    live_store_free(&store);
}

static void live_start(Message* message, const char* id)
{
    if (!id || !id[0] || !frag_session_exists(id))
    { respond_error(message, HTTP_STATUS_BAD_REQUEST, "sessao inexistente"); return; }

    // Ja rodando: para e recomeca com a configuracao nova (trocar resolucao ao vivo
    // significa reabrir a camera e o encoder; nao ha estado a preservar).
    thread_mutex_lock_inline(&g_lock);
    LiveSession* old = find_locked(id);
    thread_mutex_unlock_inline(&g_lock);
    if (old) live_stop_session(old);

    char kind[16] = { 0 }, device[256] = { 0 }, in_codec[32] = { 0 };
    int  in_w = 0, in_h = 0; double in_fps = 0.0;
    frag_session_get_input(id, kind, sizeof(kind), device, sizeof(device),
                           in_codec, sizeof(in_codec), &in_w, &in_h, &in_fps);
    if (strcmp(kind, "camera") != 0)
    { respond_error(message, HTTP_STATUS_BAD_REQUEST, "a entrada da sessao nao e uma camera"); return; }

    char out_codec[32] = { 0 };
    int  out_w = 0, out_h = 0, bitrate = 0; double out_fps = 0.0;
    frag_session_get_output(id, out_codec, sizeof(out_codec), &out_w, &out_h, &out_fps, &bitrate);

    thread_mutex_lock_inline(&g_lock);
    LiveSession* s = free_slot_locked();
    if (s)
    {
        memset(s, 0, sizeof(*s));
        snprintf(s->id, sizeof(s->id), "%s", id);
        snprintf(s->device, sizeof(s->device), "%s", device);
        snprintf(s->codec, sizeof(s->codec), "%s", out_codec);
        snprintf(s->in_format, sizeof(s->in_format), "%s", in_codec);
        s->in_w = in_w; s->in_h = in_h; s->in_fps = in_fps;
        // Resolucao da SAIDA (a barra do player); em branco, a da propria camera.
        s->width  = out_w > 0 ? out_w : in_w;
        s->height = out_h > 0 ? out_h : in_h;
        s->fps     = (int)((out_fps > 0.0 ? out_fps : in_fps) + 0.5);
        s->bitrate = bitrate;
        s->store   = live_store_create(LIVE_WINDOW, LIVE_SEGMENT_MS / 1000);
        s->running = 1;
    }
    thread_mutex_unlock_inline(&g_lock);

    if (!s)        { respond_error(message, HTTP_STATUS_INTERNAL_ERROR, "limite de transmissoes simultaneas"); return; }
    if (!s->store) { s->running = 0; respond_error(message, HTTP_STATUS_INTERNAL_ERROR, "sem memoria para a janela"); return; }

    int status = 0;
    s->thread = thread_create(live_thread, s, &status);
    if (!s->thread)
    {
        live_stop_session(s);
        respond_error(message, HTTP_STATUS_INTERNAL_ERROR, "nao foi possivel criar a thread da transmissao");
        return;
    }

    Element* root = yb_root_object();
    yb_bool(root, "ok", 1);
    yb_str (root, "id", (char*)id);
    yb_int (root, "width",  s->width);
    yb_int (root, "height", s->height);
    yb_int (root, "fps",    s->fps);
    {
        char url[256]; snprintf(url, sizeof(url), "/api/live/media/%s/live.m3u8", id);
        yb_str(root, "playlist", url);
    }
    respond_element(message, root, HTTP_STATUS_OK);
}

static void live_stop(Message* message, const char* id)
{
    thread_mutex_lock_inline(&g_lock);
    LiveSession* s = find_locked(id);
    thread_mutex_unlock_inline(&g_lock);

    if (s) live_stop_session(s);

    Element* root = yb_root_object();
    yb_bool(root, "ok", 1);
    yb_bool(root, "wasRunning", s ? 1 : 0);
    respond_element(message, root, HTTP_STATUS_OK);
}

static void live_status(Message* message, const char* id)
{
    Element* root = yb_root_object();

    thread_mutex_lock_inline(&g_lock);
    LiveSession* s = find_locked(id);
    if (s)
    {
        int w = 0, h = 0, bw = 0, segs = 0, fin = 0; long long last = -1;
        live_store_info(s->store, &w, &h, &bw, &last, &segs, &fin);
        yb_bool(root, "live",     s->running ? 1 : 0);
        yb_int (root, "width",    w > 0 ? w : s->width);
        yb_int (root, "height",   h > 0 ? h : s->height);
        yb_int (root, "fps",      s->fps);
        yb_int (root, "segments", segs);
        yb_int (root, "lastSeq",  (long long)last);
        yb_str (root, "encoder",  s->encoder);
        yb_str (root, "error",    s->error);
        char url[256]; snprintf(url, sizeof(url), "/api/live/media/%s/live.m3u8", id);
        yb_str(root, "playlist", url);
    }
    else yb_bool(root, "live", 0);
    thread_mutex_unlock_inline(&g_lock);

    respond_element(message, root, HTTP_STATUS_OK);
}

// Leitura da janela. A COPIA sai com o registro travado (garante que o store existe) e o
// envio acontece depois, fora do lock.
static void live_media(Message* message, const char* id, const char* file)
{
    char       play[4096];
    uint8_t*   data = 0;
    int        size = 0, play_len = 0;

    thread_mutex_lock_inline(&g_lock);
    LiveSession* s = find_locked(id);
    if (s)
    {
        if (strcmp(file, "live.m3u8") == 0)   play_len = live_store_playlist(s->store, play, sizeof(play));
        else if (strcmp(file, "init.mp4") == 0) live_store_init_copy(s->store, &data, &size);
        else if (strncmp(file, "seg-", 4) == 0) live_store_segment_copy(s->store, atoll(file + 4), &data, &size);
    }
    thread_mutex_unlock_inline(&g_lock);

    if (play_len > 0)
    {
        message->Response = message_response_create_content(HTTP_STATUS_OK, APPLICATION_MPEGURL, play, play_len);
        return;
    }
    if (data && size > 0)
    {
        message->Response = message_response_create_content(HTTP_STATUS_OK, VIDEO_MP4, (char*)data, size);
        memop_free_raw(data);   // a resposta copiou o conteudo
        return;
    }
    // Sem transmissao, ou segmento que ja saiu da janela: 404 e' o que o player espera.
    message->Response = message_response_create_text(HTTP_STATUS_NOT_FOUND, "nao disponivel");
}

Element* live_route(Message* message)
{
    if (!message) return 0;
    live_init_once();

    // Ancora em "live" para nao depender do prefixo configurado no appserver_create.
    int base = -1;
    for (int i = 0; i < message->Route.Count; i++)
    {
        StringX* s = (StringX*)message->Route.Items[i];
        if (s->Length == 4 && memcmp(s->Content, "live", 4) == 0) { base = i; break; }
    }
    if (base < 0) { respond_error(message, HTTP_STATUS_BAD_REQUEST, "rota invalida"); return 0; }

    char action[32], id[80], file[64];
    route_at(message, base + 1, action, sizeof(action));
    route_at(message, base + 2, id,     sizeof(id));
    route_at(message, base + 3, file,   sizeof(file));

    if (strcmp(action, "media") == 0)  { live_media(message, id, file); return 0; }
    if (strcmp(action, "start") == 0)  { live_start(message, id);  return 0; }
    if (strcmp(action, "stop") == 0)   { live_stop(message, id);   return 0; }
    if (strcmp(action, "status") == 0) { live_status(message, id); return 0; }

    respond_error(message, HTTP_STATUS_BAD_REQUEST, "acao invalida (start|stop|status|media)");
    return 0;
}

void live_shutdown_all(void)
{
    if (!g_ready) return;
    for (int i = 0; i < LIVE_MAX; i++)
    {
        thread_mutex_lock_inline(&g_lock);
        LiveSession* s = g_live[i].store ? &g_live[i] : 0;
        thread_mutex_unlock_inline(&g_lock);
        if (s) live_stop_session(s);
    }
}
