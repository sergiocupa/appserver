//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Implementacao de frag_session. Estado em disco (session.json via yason) + um registro
//  em memoria apenas dos jobs EM EXECUCAO (id -> GwControl), que e o unico dado que nao
//  faz sentido persistir: ao reiniciar, nao ha job em voo.

#include "frag_session.h"
#include "yason_build.h"
#include "gw_path.h"
#include "gw_base.h"          // GW_ALLOC/GW_FREE (memory_pool)
#include "memory_pool.h"      // memop_get_stats: balanco de memoria por sessao
#include "mem_leak_watch.h"   // varredura de alcancabilidade no fim do job
#include "thread_handler.h"   // xmutex_t
#include "atomics.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>   // atoi/atof na leitura da config
#include <time.h>

#define FRAG_MAX_JOBS 16
#define FRAG_ID_MAX   64

static char      g_root[512] = "web/hls";
static int       g_ready = 0;
static xmutex_t  g_lock;

typedef struct
{
    char        id[FRAG_ID_MAX];
    GwControl   ctl;
    int         in_use;
    MemPoolStats mem_begin;   // snapshot no job_begin; o delta e gravado no job_end
}
FragJob;

static FragJob g_jobs[FRAG_MAX_JOBS];
static int     g_leak_scan = 0;

void frag_session_set_leak_scan(int on) { g_leak_scan = on ? 1 : 0; }

const char* frag_state_name(FragState s)
{
    switch (s)
    {
        case FRAG_IDLE:      return "idle";
        case FRAG_READY:     return "ready";
        case FRAG_RUNNING:   return "running";
        case FRAG_DONE:      return "done";
        case FRAG_CANCELLED: return "cancelled";
        default:             return "error";
    }
}

static FragState frag_state_parse(const char* n)
{
    if (!n) return FRAG_IDLE;
    if (strcmp(n, "ready") == 0)     return FRAG_READY;
    if (strcmp(n, "running") == 0)   return FRAG_RUNNING;
    if (strcmp(n, "done") == 0)      return FRAG_DONE;
    if (strcmp(n, "cancelled") == 0) return FRAG_CANCELLED;
    if (strcmp(n, "error") == 0)     return FRAG_ERROR;
    return FRAG_IDLE;
}

// Id so com [A-Za-z0-9_-]: ele vira nome de pasta e componente de URL, entao nada de
// separadores nem "..", que sairiam da raiz de sessoes.
static int id_is_safe(const char* id)
{
    if (!id || !*id) return 0;
    for (const char* p = id; *p; p++)
    {
        char c = *p;
        int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) return 0;
    }
    return strlen(id) < FRAG_ID_MAX;
}

static void iso_now(char* out, size_t size)
{
    time_t t = time(0);
    struct tm tm;
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    strftime(out, size, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

int frag_session_init(const char* root)
{
    if (root && *root)
    {
        snprintf(g_root, sizeof(g_root), "%s", root);
        gw_path_normalize(g_root);
    }
    if (!g_ready) { thread_mutex_init_inline(&g_lock); g_ready = 1; }
    return gw_mkdir_p(g_root);
}

int frag_session_dir(const char* id, char* out, size_t size)
{
    if (!id_is_safe(id) || !out || size == 0) return 0;
    gw_path_join(out, size, g_root, id);
    return 1;
}

static int session_file(const char* id, char* out, size_t size)
{
    char dir[768];
    if (!frag_session_dir(id, dir, sizeof(dir))) return 0;
    gw_path_join(out, size, dir, "session.json");
    return 1;
}

int frag_session_exists(const char* id)
{
    char f[900];
    return session_file(id, f, sizeof(f)) && gw_file_exists(f);
}

// ---- leitura / escrita do session.json ------------------------------------

Element* frag_session_read(const char* id)
{
    char path[900];
    if (!session_file(id, path, sizeof(path))) return 0;

    char* text = 0; int len = 0;
    if (!file_read_text(path, &text, &len) || !text) return 0;
    Element* e = yason_parse(text, len, TREE_TYPE_JSON);
    memop_free_raw(text);
    if (e) e->TreeType = TREE_TYPE_JSON;
    return e;
}

static int session_write(const char* id, Element* root)
{
    char path[900];
    if (!session_file(id, path, sizeof(path)) || !root) return 0;
    root->TreeType = TREE_TYPE_JSON;
    root->Type = NODE_TYPE_OBJECT;

    StringX* s = yb_render(root);
    if (!s) return 0;
    int ok = file_write_text(path, s->Content, s->Length) != 0;
    yb_free_render(s);
    return ok;
}

// Remove o campo 'name' (se existir) para que o setter possa reescreve-lo sem duplicar.
static void drop_field(Element* root, const char* name)
{
    if (!root) return;
    for (int i = 0; i < root->Children.Count; i++)
    {
        Element* c = root->Children.Items[i];
        if (!yason_string_equals(&c->Name, name)) continue;
        yb_free(c);
        for (int j = i; j + 1 < root->Children.Count; j++) root->Children.Items[j] = root->Children.Items[j + 1];
        root->Children.Count--;
        return;
    }
}

static void touch_updated(Element* root)
{
    char now[32]; iso_now(now, sizeof(now));
    drop_field(root, "updated");
    yb_str(root, "updated", now);
}

// Le, aplica 'edit' e regrava. Assim nenhum caminho de erro deixa o arquivo pela metade.
typedef void (*FragEditFn)(Element* root, void* user);
static int session_edit(const char* id, FragEditFn edit, void* user)
{
    Element* root = frag_session_read(id);
    if (!root) return 0;
    edit(root, user);
    touch_updated(root);
    int ok = session_write(id, root);
    yb_free(root);
    return ok;
}

// ---- criacao / remocao ----------------------------------------------------

int frag_session_create(const char* name, char* out_id, size_t id_size)
{
    if (!g_ready) frag_session_init(0);

    char now[32]; iso_now(now, sizeof(now));

    // Id legivel e ordenavel: sAAAAMMDD-HHMMSS (-N se colidir no mesmo segundo).
    // Formatado a parte do timestamp ISO porque o id vira nome de PASTA e segmento de
    // URL: so pode conter [A-Za-z0-9_-].
    char stamp[32];
    {
        time_t t = time(0);
        struct tm tm;
#ifdef _WIN32
        gmtime_s(&tm, &t);
#else
        gmtime_r(&t, &tm);
#endif
        strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
    }

    char id[FRAG_ID_MAX];
    snprintf(id, sizeof(id), "s%s", stamp);
    for (int n = 1; frag_session_exists(id) && n < 100; n++)
        snprintf(id, sizeof(id), "s%s-%d", stamp, n);

    char dir[768];
    if (!frag_session_dir(id, dir, sizeof(dir))) return 0;
    if (!gw_mkdir_p(dir)) return 0;

    Element* root = yb_root_object();
    yb_str(root, "id", id);
    yb_str(root, "name", (name && *name) ? name : id);
    yb_str(root, "created", now);
    yb_str(root, "updated", now);
    yb_str(root, "state", frag_state_name(FRAG_IDLE));
    yb_str(root, "message", "");
    yb_array(root, "tracks");
    int ok = session_write(id, root);
    yb_free(root);

    if (!ok) { gw_rmtree(dir); return 0; }
    if (out_id && id_size) snprintf(out_id, id_size, "%s", id);
    return 1;
}

int frag_session_delete(const char* id)
{
    char dir[768];
    if (!frag_session_dir(id, dir, sizeof(dir))) return 0;

    // Apagar a pasta debaixo de uma fragmentacao em andamento deixaria o encoder
    // gravando em arquivos orfaos; cancela e espera a saida cooperativa.
    if (frag_session_cancel(id))
        for (int i = 0; i < 200 && frag_session_running(id); i++) thread_sleep0_inline();

    return gw_rmtree(dir);
}

typedef struct { const char* dir; int failed; } ClearCtx;

static void clear_entry(void* user, const char* name, int is_dir)
{
    (void)is_dir;
    ClearCtx* c = (ClearCtx*)user;
    if (strcmp(name, "session.json") == 0) return;   // o estado da sessao sobrevive
    char child[900];
    gw_path_join(child, sizeof(child), c->dir, name);
    if (!gw_rmtree(child)) c->failed = 1;
}

int frag_session_clear_output(const char* id)
{
    char dir[768];
    if (!frag_session_dir(id, dir, sizeof(dir))) return 0;
    if (!gw_dir_exists(dir)) return gw_mkdir_p(dir);

    ClearCtx c; c.dir = dir; c.failed = 0;
    gw_dir_list(dir, clear_entry, &c);
    if (c.failed) return 0;

    frag_session_clear_tracks(id);
    return 1;
}

// ---- listagem --------------------------------------------------------------

typedef struct { Element* arr; } ListCtx;

static void list_entry(void* user, const char* name, int is_dir)
{
    if (!is_dir) return;
    ListCtx* c = (ListCtx*)user;
    Element* s = frag_session_read(name);
    if (!s) return;   // pasta sem session.json: nao e uma sessao desta ferramenta

    Element* it = yb_array_object(c->arr);
    // Copia os campos escalares do session.json (sem 'tracks', que so interessa no detalhe).
    for (int i = 0; i < s->Children.Count; i++)
    {
        Element* f = s->Children.Items[i];
        if (f->Type == NODE_TYPE_ARRAY || f->Type == NODE_TYPE_OBJECT) continue;
        Element* d = yb__child(it, f->Name.Content, NODE_TYPE_FIELD);
        d->IsString = f->IsString;
        if (f->Value.Length > 0) yason_string_append(&d->Value, f->Value.Content);
    }
    yb_bool(it, "running", frag_session_running(name));
    yb_free(s);
}

Element* frag_session_list(void)
{
    if (!g_ready) frag_session_init(0);
    Element* root = yb_root_object();
    ListCtx c; c.arr = yb_array(root, "sessions");
    gw_dir_list(g_root, list_entry, &c);
    return root;
}

// ---- setters ---------------------------------------------------------------

typedef struct { FragState state; const char* message; } StateArg;
static void edit_state(Element* root, void* user)
{
    StateArg* a = (StateArg*)user;
    drop_field(root, "state");   drop_field(root, "message");
    yb_str(root, "state", frag_state_name(a->state));
    yb_str(root, "message", a->message ? a->message : "");
}
int frag_session_set_state(const char* id, FragState state, const char* message)
{
    StateArg a = { state, message };
    return session_edit(id, edit_state, &a);
}

typedef struct { const char *kind, *device, *codec; int w, h; double fps, dur; } InArg;
static void edit_input(Element* root, void* user)
{
    InArg* a = (InArg*)user;
    drop_field(root, "input");
    Element* in = yb_object(root, "input");
    yb_str(in, "kind",   a->kind   ? a->kind   : "file");
    yb_str(in, "device", a->device ? a->device : "");
    yb_str(in, "codec",  a->codec  ? a->codec  : "");
    yb_int(in, "width",  a->w);
    yb_int(in, "height", a->h);
    yb_num(in, "fps",      a->fps, 3);
    yb_num(in, "duration", a->dur, 3);
}
int frag_session_set_input(const char* id, const char* kind, const char* device, const char* codec,
                           int width, int height, double fps, double duration)
{
    InArg a = { kind, device, codec, width, height, fps, duration };
    return session_edit(id, edit_input, &a);
}

typedef struct { const char *proto, *codec; int w, h; double fps; int bitrate; } OutArg;
static void edit_output(Element* root, void* user)
{
    OutArg* a = (OutArg*)user;
    drop_field(root, "output");
    Element* o = yb_object(root, "output");
    yb_str(o, "protocol", a->proto ? a->proto : "");
    yb_str(o, "codec",    a->codec ? a->codec : "");
    yb_int(o, "width",   a->w);
    yb_int(o, "height",  a->h);
    yb_num(o, "fps",     a->fps, 3);
    yb_int(o, "bitrate", a->bitrate);
}
int frag_session_set_output(const char* id, const char* protocol, const char* codec,
                            int width, int height, double fps, int bitrate)
{
    OutArg a = { protocol, codec, width, height, fps, bitrate };
    return session_edit(id, edit_output, &a);
}

typedef struct { const char* playlist; double elapsed; } ResArg;
static void edit_result(Element* root, void* user)
{
    ResArg* a = (ResArg*)user;
    drop_field(root, "playlist"); drop_field(root, "elapsed");
    yb_str(root, "playlist", a->playlist ? a->playlist : "");
    yb_num(root, "elapsed", a->elapsed, 1);
}
int frag_session_set_result(const char* id, const char* playlist, double elapsed_sec)
{
    ResArg a = { playlist, elapsed_sec };
    return session_edit(id, edit_result, &a);
}

typedef struct { long long live, osres; unsigned long long allocs, frees; } MemArg;
static void edit_memory(Element* root, void* user)
{
    MemArg* a = (MemArg*)user;
    drop_field(root, "memory");
    Element* m = yb_object(root, "memory");
    yb_int(m, "liveDelta",       a->live);    // (alloc-free) no fim menos no inicio
    yb_int(m, "osReservedDelta", a->osres);   // bytes reservados do SO a mais/menos
    yb_int(m, "allocs",          (long long)a->allocs);
    yb_int(m, "frees",           (long long)a->frees);
}
int frag_session_set_memory(const char* id, long long live_delta, long long os_reserved_delta,
                            unsigned long long allocs, unsigned long long frees)
{
    MemArg a = { live_delta, os_reserved_delta, allocs, frees };
    return session_edit(id, edit_memory, &a);
}

typedef struct { const char* name; int w, h, bw; } TrackArg;
static void edit_add_track(Element* root, void* user)
{
    TrackArg* a = (TrackArg*)user;
    Element* arr = 0;
    for (int i = 0; i < root->Children.Count; i++)
        if (yason_string_equals(&root->Children.Items[i]->Name, "tracks")) { arr = root->Children.Items[i]; break; }
    if (!arr) arr = yb_array(root, "tracks");
    Element* t = yb_array_object(arr);
    yb_str(t, "name", a->name ? a->name : "");
    yb_int(t, "width",  a->w);
    yb_int(t, "height", a->h);
    yb_int(t, "bandwidth", a->bw);
}
int frag_session_add_track(const char* id, const char* name, int width, int height, int bandwidth)
{
    TrackArg a = { name, width, height, bandwidth };
    return session_edit(id, edit_add_track, &a);
}

static void edit_clear_tracks(Element* root, void* user)
{
    (void)user;
    drop_field(root, "tracks");
    yb_array(root, "tracks");
}
int frag_session_clear_tracks(const char* id) { return session_edit(id, edit_clear_tracks, 0); }

// ---- leitura da configuracao gravada ---------------------------------------

static void copy_field(Element* obj, const char* name, char* out, size_t size)
{
    if (out && size) out[0] = '\0';
    Element* e = obj ? yason_find_element(obj, name) : 0;
    if (e && e->Value.Content && out && size) snprintf(out, size, "%s", e->Value.Content);
}

static int int_field(Element* obj, const char* name)
{
    Element* e = obj ? yason_find_element(obj, name) : 0;
    return (e && e->Value.Content) ? atoi(e->Value.Content) : 0;
}

static double dbl_field(Element* obj, const char* name)
{
    Element* e = obj ? yason_find_element(obj, name) : 0;
    return (e && e->Value.Content) ? atof(e->Value.Content) : 0.0;
}

int frag_session_get_output(const char* id, char* codec, size_t codec_size,
                            int* width, int* height, double* fps, int* bitrate)
{
    if (codec && codec_size) codec[0] = '\0';
    if (width) *width = 0; if (height) *height = 0;
    if (fps) *fps = 0.0;   if (bitrate) *bitrate = 0;

    Element* root = frag_session_read(id);
    if (!root) return 0;

    Element* o = yason_find_element(root, "output");
    copy_field(o, "codec", codec, codec_size);
    if (width)   *width   = int_field(o, "width");
    if (height)  *height  = int_field(o, "height");
    if (fps)     *fps     = dbl_field(o, "fps");
    if (bitrate) *bitrate = int_field(o, "bitrate");

    yb_free(root);
    return 1;
}

int frag_session_get_input(const char* id, char* kind, size_t kind_size,
                           char* device, size_t device_size, char* codec, size_t codec_size,
                           int* width, int* height, double* fps)
{
    if (kind && kind_size) kind[0] = '\0';
    if (device && device_size) device[0] = '\0';
    if (codec && codec_size) codec[0] = '\0';
    if (width) *width = 0; if (height) *height = 0; if (fps) *fps = 0.0;

    Element* root = frag_session_read(id);
    if (!root) return 0;

    Element* in = yason_find_element(root, "input");
    copy_field(in, "kind",   kind,   kind_size);
    copy_field(in, "device", device, device_size);
    copy_field(in, "codec",  codec,  codec_size);
    if (width)  *width  = int_field(in, "width");
    if (height) *height = int_field(in, "height");
    if (fps)    *fps    = dbl_field(in, "fps");

    yb_free(root);
    return 1;
}

// ---- registro de jobs ------------------------------------------------------

static FragJob* job_find(const char* id)
{
    for (int i = 0; i < FRAG_MAX_JOBS; i++)
        if (g_jobs[i].in_use && strcmp(g_jobs[i].id, id) == 0) return &g_jobs[i];
    return 0;
}

GwControl* frag_session_job_begin(const char* id)
{
    if (!g_ready) frag_session_init(0);
    if (!id_is_safe(id) || !frag_session_exists(id)) return 0;

    thread_mutex_lock_inline(&g_lock);
    if (job_find(id)) { thread_mutex_unlock_inline(&g_lock); return 0; }   // ja rodando

    FragJob* slot = 0;
    for (int i = 0; i < FRAG_MAX_JOBS; i++) if (!g_jobs[i].in_use) { slot = &g_jobs[i]; break; }
    if (slot)
    {
        memset(slot, 0, sizeof(*slot));
        snprintf(slot->id, sizeof(slot->id), "%s", id);
        slot->in_use = 1;
        memop_get_stats(&slot->mem_begin);   // baseline de memoria desta sessao
    }
    thread_mutex_unlock_inline(&g_lock);

    if (!slot) return 0;
    frag_session_set_state(id, FRAG_RUNNING, "");
    return &slot->ctl;
}

void frag_session_job_end(const char* id, FragState final_state, const char* message)
{
    if (!id_is_safe(id)) return;

    MemPoolStats now; memop_get_stats(&now);

    thread_mutex_lock_inline(&g_lock);
    FragJob* j = job_find(id);
    MemPoolStats begin; int had = 0;
    if (j) { begin = j->mem_begin; had = 1; j->in_use = 0; }
    thread_mutex_unlock_inline(&g_lock);

    if (had)
    {
        // 'live' = blocos vivos (alloc - free). O contador e do processo inteiro, entao o
        // delta inclui ruido de outras threads; o que importa e o PADRAO entre sessoes
        // sucessivas: um crescimento parecido a cada rodada e acumulo real.
        long long live_end   = (long long)now.alloc_count   - (long long)now.free_count;
        long long live_begin = (long long)begin.alloc_count - (long long)begin.free_count;
        frag_session_set_memory(id, live_end - live_begin,
                                (long long)now.os_reserved_bytes - (long long)begin.os_reserved_bytes,
                                now.alloc_count - begin.alloc_count,
                                now.free_count  - begin.free_count);
    }

    frag_session_set_state(id, final_state, message);

    // Depois de fechar a sessao e liberar tudo: o que ainda estiver alocado e sem
    // referencia viva sai no mem_leak_watch.log com o backtrace da criacao.
    if (g_leak_scan) mem_leak_watch_scan_now();
}

int frag_session_cancel(const char* id)
{
    if (!g_ready || !id_is_safe(id)) return 0;
    thread_mutex_lock_inline(&g_lock);
    FragJob* j = job_find(id);
    if (j) j->ctl.Stop = 1;
    thread_mutex_unlock_inline(&g_lock);
    return j != 0;
}

int frag_session_running(const char* id)
{
    if (!g_ready || !id_is_safe(id)) return 0;
    thread_mutex_lock_inline(&g_lock);
    int r = job_find(id) != 0;
    thread_mutex_unlock_inline(&g_lock);
    return r;
}
