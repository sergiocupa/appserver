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

// Copia para o item da lista um objeto do session.json, so com os campos escalares.
static void list_objeto(Element* it, Element* s, const char* nome)
{
    Element* o = 0;
    for (int i = 0; i < s->Children.Count; i++)
        if (yason_string_equals(&s->Children.Items[i]->Name, nome)) { o = s->Children.Items[i]; break; }
    if (!o || o->Type != NODE_TYPE_OBJECT) return;
    Element* m = yb_object(it, nome);
    for (int i = 0; i < o->Children.Count; i++)
    {
        Element* f = o->Children.Items[i];
        if (f->Type == NODE_TYPE_ARRAY || f->Type == NODE_TYPE_OBJECT) continue;
        Element* d = yb__child(m, f->Name.Content, NODE_TYPE_FIELD);
        d->IsString = f->IsString;
        if (f->Value.Length > 0) yason_string_append(&d->Value, f->Value.Content);
    }
}

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
    list_objeto(it, s, "media");     // metadados do video (biblioteca)
    list_objeto(it, s, "formats");   // manifestos prontos para tocar
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

typedef struct { const char* protocol; const char* playlist; double elapsed; } ResArg;
static void edit_result(Element* root, void* user)
{
    ResArg* a = (ResArg*)user;
    drop_field(root, "protocol"); drop_field(root, "playlist"); drop_field(root, "elapsed");
    yb_str(root, "protocol", a->protocol ? a->protocol : "");
    yb_str(root, "playlist", a->playlist ? a->playlist : "");
    yb_num(root, "elapsed", a->elapsed, 1);
}
int frag_session_set_result(const char* id, const char* protocol, const char* playlist, double elapsed_sec)
{
    ResArg a = { protocol, playlist, elapsed_sec };
    return session_edit(id, edit_result, &a);
}

typedef struct { const char* chave; const char* url; } FmtArg;
static void edit_format(Element* root, void* user)
{
    FmtArg* a = (FmtArg*)user;
    Element* f = 0;
    for (int i = 0; i < root->Children.Count; i++)
        if (yason_string_equals(&root->Children.Items[i]->Name, "formats")) { f = root->Children.Items[i]; break; }
    if (f && f->Type != NODE_TYPE_OBJECT) { drop_field(root, "formats"); f = 0; }
    if (!f) f = yb_object(root, "formats");
    drop_field(f, a->chave);
    if (a->url && *a->url) yb_str(f, a->chave, a->url);
}
int frag_session_set_format(const char* id, const char* key, const char* url)
{
    if (!key || !*key) return 0;
    FmtArg a = { key, url };
    return session_edit(id, edit_format, &a);
}

// ---- biblioteca ------------------------------------------------------------

#define FNV_BASE  1469598103934665603ULL
#define FNV_PRIMO 1099511628211ULL
static uint64_t fnv_add(uint64_t h, const void* p, size_t n)
{
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= FNV_PRIMO; }
    return h;
}
// Campo com separador: "ab"+"c" e "a"+"bc" nao podem dar o mesmo hash.
static uint64_t fnv_campo(uint64_t h, const char* s) { h = fnv_add(h, s, strlen(s)); return fnv_add(h, "", 1); }

int frag_video_id(const char* source_path, char* out, size_t size, int* por_metadados, Mp4Meta* meta)
{
    Mp4Meta local; Mp4Meta* m = meta ? meta : &local;
    if (por_metadados) *por_metadados = 0;
    if (!mp4_metadata(source_path, m)) memset(m, 0, sizeof(*m));
    uint64_t h = FNV_BASE;
    if (mp4_metadata_descritiva(m))
    {
        int w = 0, ht = 0; double dur = 0.0;
        mp4_video_info(source_path, &w, &ht, 0, 0, &dur);
        char extra[64]; snprintf(extra, sizeof(extra), "%lld|%dx%d", (long long)(dur * 1000.0 + 0.5), w, ht);
        h = fnv_campo(h, "metadados");
        h = fnv_campo(h, m->Titulo); h = fnv_campo(h, m->Autor); h = fnv_campo(h, m->Album);
        h = fnv_campo(h, m->Data);   h = fnv_campo(h, m->Descricao); h = fnv_campo(h, extra);
        if (por_metadados) *por_metadados = 1;
    }
    else
    {
        FILE* f = 0;
        if (fopen_s(&f, source_path, "rb") != 0) f = 0;
        if (!f) return 0;
        uint8_t* buf = (uint8_t*)GW_ALLOC(1 << 20);
        if (!buf) { fclose(f); return 0; }
        h = fnv_campo(h, "conteudo");
        size_t n;
        while ((n = fread(buf, 1, 1 << 20, f)) > 0) h = fnv_add(h, buf, n);
        GW_FREE(buf);
        fclose(f);
    }
    snprintf(out, size, "%016llx", (unsigned long long)h);
    return 1;
}

// Estado de RESULTADO fora: a saida foi limpa (ou nunca existiu nesta pasta), entao o que
// dizia "pronto, manifesto tal" e mentira. Sem isto, juntar a sessao a pasta de um video ja
// preparado levava o "done" e o manifesto do video ANTERIOR (medido: a biblioteca apontava
// para a pasta de outro video).
static void sem_resultado(Element* root, FragState estado)
{
    static const char* campos[] = { "protocol", "playlist", "elapsed", "tracks", "memory", "state", "message", "formats" };
    for (size_t i = 0; i < sizeof(campos) / sizeof(campos[0]); i++) drop_field(root, campos[i]);
    yb_array(root, "tracks");
    yb_str(root, "state", frag_state_name(estado));
    yb_str(root, "message", "");
}

int frag_session_has_media(const char* id)
{
    Element* root = frag_session_read(id);
    if (!root) return 0;
    Element* m = yason_find_element(root, "media");
    int tem = m != 0;
    yb_free(root);
    return tem;
}

int frag_session_fork(const char* id, char* out_id, size_t id_size)
{
    char novo[FRAG_ID_MAX];
    if (!frag_session_create(0, novo, sizeof(novo))) return 0;
    Element* root = frag_session_read(id);
    if (root)
    {
        // A configuracao (entrada/saida escolhidas na tela) vem junto; o video e o resultado nao.
        char agora[32]; iso_now(agora, sizeof(agora));
        drop_field(root, "id"); drop_field(root, "name"); drop_field(root, "created"); drop_field(root, "media");
        yb_str(root, "id", novo); yb_str(root, "name", novo); yb_str(root, "created", agora);
        sem_resultado(root, FRAG_IDLE);
        touch_updated(root);
        session_write(novo, root);
        yb_free(root);
    }
    if (out_id && id_size) snprintf(out_id, id_size, "%s", novo);
    return 1;
}

static void edit_id(Element* root, void* user)
{
    drop_field(root, "id");
    yb_str(root, "id", (const char*)user);
}

int frag_session_adopt(const char* id, const char* novo_id)
{
    if (!id_is_safe(id) || !id_is_safe(novo_id)) return 0;
    if (strcmp(id, novo_id) == 0) return 1;
    if (frag_session_running(novo_id) || frag_session_running(id)) return -1;

    char de[768], para[768];
    frag_session_dir(id, de, sizeof(de));
    frag_session_dir(novo_id, para, sizeof(para));

    if (!gw_dir_exists(para))
    {
        if (rename(de, para) != 0) return 0;
        return session_edit(novo_id, edit_id, (void*)novo_id);
    }

    // O mesmo video ja tem pasta: a saida antiga sai, a fonte nova e a configuracao desta
    // sessao (entrada/saida escolhidas na tela) entram, e a sessao temporaria some.
    if (!frag_session_clear_output(novo_id)) return 0;
    char fonte_de[900], fonte_para[900];
    gw_path_join(fonte_de, sizeof(fonte_de), de, "source.mp4");
    gw_path_join(fonte_para, sizeof(fonte_para), para, "source.mp4");
    if (gw_file_exists(fonte_de) && rename(fonte_de, fonte_para) != 0) return 0;

    Element* root = frag_session_read(id);
    if (root)
    {
        Element* antigo = frag_session_read(novo_id);
        Element* criado = antigo ? yason_find_element(antigo, "created") : 0;
        if (criado && criado->Value.Content) { drop_field(root, "created"); yb_str(root, "created", criado->Value.Content); }
        edit_id(root, (void*)novo_id);
        sem_resultado(root, FRAG_READY);
        touch_updated(root);
        session_write(novo_id, root);
        if (antigo) yb_free(antigo);
        yb_free(root);
    }
    gw_rmtree(de);
    return 1;
}

typedef struct { const Mp4Meta* meta; const char* file_name; int por_metadados; double duration; int width, height; } MediaArg;
static void edit_media(Element* root, void* user)
{
    MediaArg* a = (MediaArg*)user;
    drop_field(root, "media");
    Element* m = yb_object(root, "media");
    const Mp4Meta* x = a->meta;
    yb_str(m, "title",       x ? x->Titulo : "");
    yb_str(m, "author",      x ? x->Autor : "");
    yb_str(m, "album",       x ? x->Album : "");
    yb_str(m, "date",        x ? x->Data : "");
    yb_str(m, "description", x ? x->Descricao : "");
    yb_str(m, "genre",       x ? x->Genero : "");
    yb_str(m, "fileName",    a->file_name ? a->file_name : "");
    yb_str(m, "hashFrom",    a->por_metadados ? "metadata" : "content");
    yb_num(m, "duration",    a->duration, 3);
    yb_int(m, "width",       a->width);
    yb_int(m, "height",      a->height);
}
int frag_session_set_media(const char* id, const Mp4Meta* meta, const char* file_name, int por_metadados,
                           double duration, int width, int height)
{
    MediaArg a = { meta, file_name, por_metadados, duration, width, height };
    return session_edit(id, edit_media, &a);
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
    // So o nome e validado. Exigir a sessao (session.json) deixava a pasta padrao "current",
    // que nao tem sessao, SEM job registrado: rodava sem poder ser cancelada e sem exclusao
    // mutua -- e o EventSource do navegador reconecta sozinho quando a conexao cai, o que
    // disparava uma SEGUNDA fragmentacao gravando por cima da primeira na mesma pasta.
    // Sem sessao, o job continua exclusivo e cancelavel; so o estado (set_state) nao tem onde
    // ser gravado, e session_edit ja ignora pasta sem session.json.
    if (!id_is_safe(id)) return 0;

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
