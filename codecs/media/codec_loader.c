//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Carga e descarga de codecs em tempo de execucao. Ver codec_loader.h.
//
//  A parte delicada nao e carregar: e DESCARREGAR. Fechar o modulo com um encoder aberto
//  derruba o processo na proxima chamada -- foi exatamente assim que o executor de testes
//  morria antes de o xplatbase fixar o proprio modulo (ver prep-xplatbase/PLANO.md).
//  Por isso aqui ha contagem de uso: cada encoder/decoder aberto segura o modulo de onde
//  veio, e so solta quando e fechado. Descarregar um modulo em uso e recusado.

#ifndef _WIN32
  #ifndef _GNU_SOURCE
    #define _GNU_SOURCE   /* dladdr/Dl_info: precisa vir ANTES dos includes */
  #endif
#endif

#include "codec_loader.h"
#include "thread_handler.h"
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
  #include <windows.h>
  typedef HMODULE Modulo;
  #define PLUGIN_PREFIXO "codec_"
  #define PLUGIN_SUFIXO  ".dll"
  static Modulo mod_abre(const char* p)              { return LoadLibraryA(p); }
  static void*  mod_simbolo(Modulo m, const char* s) { return (void*)GetProcAddress(m, s); }
  static void   mod_fecha(Modulo m)                  { FreeLibrary(m); }
#else
  #include <dlfcn.h>
  #include <dirent.h>
  #include <unistd.h>
  #include <limits.h>
  typedef void* Modulo;
  #define PLUGIN_PREFIXO "libcodec_"
  #define PLUGIN_SUFIXO  ".so"
  static Modulo mod_abre(const char* p)              { return dlopen(p, RTLD_NOW | RTLD_LOCAL); }
  static void*  mod_simbolo(Modulo m, const char* s) { return dlsym(m, s); }
  static void   mod_fecha(Modulo m)                  { dlclose(m); }
#endif

#define MAX_MODULOS 16
#define MAX_USOS    256

typedef struct
{
    Modulo                 Handle;
    const CodecPluginSet*  Set;
    int                    Usos;        // encoders/decoders abertos que vieram daqui
    char                   Caminho[512];
}
ModuloCarregado;

static ModuloCarregado g_mods[MAX_MODULOS];
static int             g_nmods;
static xmutex_t        g_mtx;
static int             g_mtx_pronto;

// O nucleo (codec_registry.c) e quem monta a lista final.
void codec_registry_add(const CodecPluginSet* cs);
void codec_registry_remove(const CodecPluginSet* cs);

static void trava(void)
{
    if (!g_mtx_pronto) { thread_mutex_init(&g_mtx); g_mtx_pronto = 1; }
    thread_mutex_lock(&g_mtx);
}
static void destrava(void) { thread_mutex_unlock(&g_mtx); }

// ---- contagem de uso -------------------------------------------------------
// Quem abre um encoder/decoder chama codec_use_begin; o Close original e trocado por um
// desvio que devolve a contagem. Assim o modulo nao sai debaixo de quem esta usando.

typedef struct
{
    void*                  Obj;      // MediaEncoder* ou MediaDecoder*
    const CodecPluginSet*  Set;
    void (*CloseEnc)(MediaEncoder*);
    void (*CloseDec)(MediaDecoder*);
}
Uso;

static Uso g_usos[MAX_USOS];

static ModuloCarregado* mod_do_set(const CodecPluginSet* cs)
{
    for (int i = 0; i < g_nmods; i++)
        if (g_mods[i].Set == cs) return &g_mods[i];
    return 0;   // conjunto embutido no binario: nao ha o que segurar
}

void codec_use_begin(const CodecPluginSet* cs)
{
    if (!cs) return;
    trava();
    { ModuloCarregado* m = mod_do_set(cs); if (m) m->Usos++; }
    destrava();
}

void codec_use_end(const CodecPluginSet* cs)
{
    if (!cs) return;
    trava();
    { ModuloCarregado* m = mod_do_set(cs); if (m && m->Usos > 0) m->Usos--; }
    destrava();
}

static Uso* uso_livre(void)
{
    for (int i = 0; i < MAX_USOS; i++) if (!g_usos[i].Obj) return &g_usos[i];
    return 0;
}

static Uso* uso_de(void* obj)
{
    for (int i = 0; i < MAX_USOS; i++) if (g_usos[i].Obj == obj) return &g_usos[i];
    return 0;
}

static void fecha_enc(MediaEncoder* e)
{
    void (*orig)(MediaEncoder*) = 0;
    const CodecPluginSet* cs = 0;

    trava();
    { Uso* u = uso_de(e); if (u) { orig = u->CloseEnc; cs = u->Set; memset(u, 0, sizeof(*u)); } }
    destrava();

    if (orig) orig(e);
    codec_use_end(cs);
}

static void fecha_dec(MediaDecoder* d)
{
    void (*orig)(MediaDecoder*) = 0;
    const CodecPluginSet* cs = 0;

    trava();
    { Uso* u = uso_de(d); if (u) { orig = u->CloseDec; cs = u->Set; memset(u, 0, sizeof(*u)); } }
    destrava();

    if (orig) orig(d);
    codec_use_end(cs);
}

// Chamadas pelo nucleo logo depois de abrir. Se o backend veio de um modulo carregado,
// segura o modulo e desvia o Close; se veio embutido, nao faz nada.
void codec_use_track_encoder(const CodecPluginSet* cs, MediaEncoder* e)
{
    Uso* u;
    if (!cs || !e || !mod_do_set(cs)) return;

    trava();
    u = uso_livre();
    if (u) { u->Obj = e; u->Set = cs; u->CloseEnc = e->Close; }
    destrava();

    if (u) { e->Close = fecha_enc; codec_use_begin(cs); }
}

void codec_use_track_decoder(const CodecPluginSet* cs, MediaDecoder* d)
{
    Uso* u;
    if (!cs || !d || !mod_do_set(cs)) return;

    trava();
    u = uso_livre();
    if (u) { u->Obj = d; u->Set = cs; u->CloseDec = d->Close; }
    destrava();

    if (u) { d->Close = fecha_dec; codec_use_begin(cs); }
}

// ---- carga -----------------------------------------------------------------

static int ja_carregado(const char* caminho)
{
    for (int i = 0; i < g_nmods; i++)
#ifdef _WIN32
        if (_stricmp(g_mods[i].Caminho, caminho) == 0) return 1;
#else
        if (strcmp(g_mods[i].Caminho, caminho) == 0) return 1;
#endif
    return 0;
}

int codec_plugin_load_file(const char* caminho)
{
    Modulo m;
    CodecPluginEntry entrada;
    const CodecPluginSet* cs;

    if (!caminho || !*caminho) return -1;
    if (g_nmods >= MAX_MODULOS) return -1;
    if (ja_carregado(caminho))  return 0;

    m = mod_abre(caminho);
    if (!m) return -1;

    entrada = (CodecPluginEntry)mod_simbolo(m, CODEC_PLUGIN_ENTRY_NAME);
    if (!entrada) { mod_fecha(m); return -1; }   // DLL que nao e plugin de codec

    cs = entrada();
    // ABI diferente: o descritor mudou de forma e ler este modulo seria ler lixo.
    if (!cs || cs->Abi != CODEC_PLUGIN_ABI || cs->Count <= 0) { mod_fecha(m); return -1; }

    trava();
    {
        ModuloCarregado* mc = &g_mods[g_nmods++];
        mc->Handle = m;
        mc->Set    = cs;
        mc->Usos   = 0;
        snprintf(mc->Caminho, sizeof(mc->Caminho), "%s", caminho);
    }
    destrava();

    codec_registry_add(cs);
    return 1;
}

// A pasta padrao de busca e a DESTE modulo, nao a do executavel. Faz diferenca: quando o
// nucleo esta dentro de uma DLL (o unittests.dll, carregado pelo vstest.console.exe), a
// pasta do executavel e a do Visual Studio, e nao a do binario que acabou de ser gerado.
static void pasta_deste_modulo(char* out, int tam)
{
    out[0] = '\0';
#ifdef _WIN32
    {
        HMODULE h = 0;
        char buf[512];
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)(void*)&pasta_deste_modulo, &h) && h)
        {
            DWORD n = GetModuleFileNameA(h, buf, (DWORD)sizeof(buf));
            if (n > 0 && n < sizeof(buf))
            {
                char* barra = strrchr(buf, '\\');
                if (barra) { *barra = '\0'; snprintf(out, (size_t)tam, "%s", buf); }
            }
        }
    }
#else
    {
        Dl_info info;
        if (dladdr((void*)&pasta_deste_modulo, &info) && info.dli_fname)
        {
            char buf[512];
            char* barra;
            snprintf(buf, sizeof(buf), "%s", info.dli_fname);
            barra = strrchr(buf, '/');
            if (barra) { *barra = '\0'; snprintf(out, (size_t)tam, "%s", buf); }
            else        snprintf(out, (size_t)tam, "%s", ".");
        }
    }
#endif
}

// Nome de arquivo a partir do nome do codec: "H.264" -> "h264", "Opus" -> "opus".
// E so normalizacao do que o media_codec_name ja devolve -- o nucleo continua sem uma
// tabela de codecs propria.
static void fragmento(MediaCodec codec, char* out, int tam)
{
    const char* n = media_codec_name(codec);
    int k = 0;
    out[0] = '\0';
    for (; n && *n && k < tam - 1; n++)
    {
        if (*n >= 'A' && *n <= 'Z') out[k++] = (char)(*n - 'A' + 'a');
        else if ((*n >= 'a' && *n <= 'z') || (*n >= '0' && *n <= '9')) out[k++] = *n;
        /* pontos e separadores caem fora: "H.264" vira "h264" */
    }
    out[k] = '\0';
}

// Carrega o modulo que atende ESSE codec, se houver um na pasta. E a carga sob demanda:
// quem abre um encoder nao precisa carregar nada antes, e nada alem do necessario entra.
// Tentado uma vez por codec -- codec que nao tem plugin nao faz varredura a cada chamada.
static unsigned g_tentados;

int codec_plugin_load_for(MediaCodec codec)
{
    char dir[512], frag[32];
    int n = 0;

    if (codec <= MEDIA_CODEC_NONE || codec >= 32) return 0;
    if (g_tentados & (1u << (unsigned)codec)) return 0;
    g_tentados |= (1u << (unsigned)codec);

    fragmento(codec, frag, (int)sizeof(frag));
    if (!frag[0]) return 0;

    pasta_deste_modulo(dir, (int)sizeof(dir));
    if (!dir[0]) return 0;

#ifdef _WIN32
    {
        WIN32_FIND_DATAA fd;
        char padrao[600];
        HANDLE h;
        snprintf(padrao, sizeof(padrao), "%s\\%s%s*%s", dir, PLUGIN_PREFIXO, frag, PLUGIN_SUFIXO);
        h = FindFirstFileA(padrao, &fd);
        if (h == INVALID_HANDLE_VALUE) return 0;
        do
        {
            char caminho[600];
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            snprintf(caminho, sizeof(caminho), "%s\\%s", dir, fd.cFileName);
            if (codec_plugin_load_file(caminho) == 1) n++;
        }
        while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    {
        DIR* d = opendir(dir);
        struct dirent* e;
        char prefixo[64];
        size_t lp, ls = strlen(PLUGIN_SUFIXO);
        if (!d) return 0;
        snprintf(prefixo, sizeof(prefixo), "%s%s", PLUGIN_PREFIXO, frag);
        lp = strlen(prefixo);
        while ((e = readdir(d)) != 0)
        {
            char caminho[600];
            size_t ln = strlen(e->d_name);
            if (ln <= ls) continue;
            if (strncmp(e->d_name, prefixo, lp) != 0) continue;
            if (strcmp(e->d_name + ln - ls, PLUGIN_SUFIXO) != 0) continue;
            snprintf(caminho, sizeof(caminho), "%s/%s", dir, e->d_name);
            if (codec_plugin_load_file(caminho) == 1) n++;
        }
        closedir(d);
    }
#endif
    return n;
}

int codec_plugins_load(const char* pasta)
{
    char dir[512];
    int n = 0;

    if (pasta && *pasta) snprintf(dir, sizeof(dir), "%s", pasta);
    else                 pasta_deste_modulo(dir, (int)sizeof(dir));
    if (!dir[0]) return 0;

#ifdef _WIN32
    {
        WIN32_FIND_DATAA fd;
        char padrao[600];
        HANDLE h;
        snprintf(padrao, sizeof(padrao), "%s\\%s*%s", dir, PLUGIN_PREFIXO, PLUGIN_SUFIXO);
        h = FindFirstFileA(padrao, &fd);
        if (h == INVALID_HANDLE_VALUE) return 0;
        do
        {
            char caminho[600];
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            snprintf(caminho, sizeof(caminho), "%s\\%s", dir, fd.cFileName);
            if (codec_plugin_load_file(caminho) == 1) n++;
        }
        while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    {
        DIR* d = opendir(dir);
        struct dirent* e;
        size_t lp = strlen(PLUGIN_PREFIXO), ls = strlen(PLUGIN_SUFIXO);
        if (!d) return 0;
        while ((e = readdir(d)) != 0)
        {
            char caminho[600];
            size_t ln = strlen(e->d_name);
            if (ln <= lp + ls) continue;
            if (strncmp(e->d_name, PLUGIN_PREFIXO, lp) != 0) continue;
            if (strcmp(e->d_name + ln - ls, PLUGIN_SUFIXO) != 0) continue;
            snprintf(caminho, sizeof(caminho), "%s/%s", dir, e->d_name);
            if (codec_plugin_load_file(caminho) == 1) n++;
        }
        closedir(d);
    }
#endif
    return n;
}

// ---- descarga --------------------------------------------------------------

static int descarrega(int i)
{
    ModuloCarregado* mc = &g_mods[i];
    Modulo h = mc->Handle;

    if (mc->Usos > 0) return -1;

    codec_registry_remove(mc->Set);

    trava();
    for (int k = i; k < g_nmods - 1; k++) g_mods[k] = g_mods[k + 1];
    g_nmods--;
    memset(&g_mods[g_nmods], 0, sizeof(g_mods[g_nmods]));
    destrava();

    mod_fecha(h);
    g_tentados = 0;     // descarregado: pode ser procurado de novo quando for preciso
    return 1;
}

int codec_plugin_unload(const char* modulo)
{
    if (!modulo) return 0;
    for (int i = 0; i < g_nmods; i++)
        if (g_mods[i].Set && g_mods[i].Set->Module && strcmp(g_mods[i].Set->Module, modulo) == 0)
            return descarrega(i);
    return 0;
}

int codec_plugins_unload_all(void)
{
    int n = 0;
    for (int i = g_nmods - 1; i >= 0; i--)
        if (descarrega(i) == 1) n++;
    return n;
}

// Quem pergunta ao CARREGADOR quer saber o que ESTA carregado agora -- nao dispara carga
// nenhuma. A resolucao acontece em quem ABRE um codec (codec_plugin_ensure), que e o unico
// momento em que se sabe do que o processo precisa.
static void garante_carga(void) { }

int codec_plugins_count(void) { garante_carga(); return g_nmods; }

const char* codec_plugin_module_name(int index)
{
    garante_carga();
    if (index < 0 || index >= g_nmods || !g_mods[index].Set) return "";
    return g_mods[index].Set->Module ? g_mods[index].Set->Module : "";
}

int codec_plugin_module_uses(int index)
{
    garante_carga();
    if (index < 0 || index >= g_nmods) return 0;
    return g_mods[index].Usos;
}
