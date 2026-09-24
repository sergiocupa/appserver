//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  O registro dos codecs: a lista unica que o nucleo percorre.
//
//  Ela tem duas origens, e o resto do sistema nao distingue uma da outra:
//    EMBUTIDOS  - so o caminho de hardware;
//    PLUGINS    - todos os codecs, trazidos de uma DLL/.so em tempo de execucao.
//
//  ORDEM: hardware antes de software, que e a ordem de tentativa da cascata (enc_select.c)
//  e tambem a ordem em que as listas de diagnostico aparecem. Entre os de software, os
//  plugins vem DEPOIS dos embutidos -- assim um plugin nao rouba silenciosamente o lugar de
//  um codec que ja estava no binario.

#include "codec_plugin.h"
#include <stddef.h>

extern const CodecPluginSet codec_set_hw;

// Codec e SEMPRE plugin: H.264, H.265, VP9, AV1 e Opus vivem em modulos proprios, carregados
// sob demanda (ver codec_loader.c). Aqui dentro fica so o caminho de HARDWARE, que nao e
// plugin porque o gw_decode fala com a GPU direto, por outra via.
static const CodecPluginSet* const EMBUTIDOS[] =
{
    &codec_set_hw,     // mf-hw (encode), dxva / vaapi (decode)
};
#define N_EMBUTIDOS ((int)(sizeof(EMBUTIDOS) / sizeof(EMBUTIDOS[0])))

#define MAX_EXTRA    16
#define MAX_PLUGINS  128

static const CodecPluginSet* g_extra[MAX_EXTRA];
static int g_nextra;

static const CodecPlugin*     g_flat[MAX_PLUGINS];
static const CodecPluginSet*  g_dono[MAX_PLUGINS];
static int g_count = -1;

static void junta(const CodecPluginSet* cs, int hardware)
{
    if (!cs || cs->Abi != CODEC_PLUGIN_ABI) return;   // conjunto de outra versao: ignorado
    for (int i = 0; i < cs->Count && g_count < MAX_PLUGINS; i++)
    {
        const CodecPlugin* p = &cs->Items[i];
        if (p->Abi != CODEC_PLUGIN_ABI) continue;
        if (!!p->Hardware != !!hardware) continue;
        g_dono[g_count] = cs;
        g_flat[g_count] = p;
        g_count++;
    }
}

// Resolucao SOB DEMANDA. Nao ha um comando de carga para chamar antes: quem pede um codec
// carrega o modulo daquele codec, se houver, e so ele. Antes isto era uma varredura da
// pasta inteira na primeira consulta -- funcionava, mas trazia para o processo modulos que
// ninguem tinha pedido.
//
// codec_plugins_autoload(0) desliga, para quem quiser carregar na mao.
int codec_plugin_load_for(MediaCodec codec);
int codec_plugins_load(const char* pasta);

static int g_autoload = 1;
static int g_carregou_tudo = 0;

void codec_plugins_autoload(int ligado) { g_autoload = ligado; }

// Garante que o modulo desse codec, se existir, ja entrou. MEDIA_CODEC_NONE = "quero ver
// tudo o que existe" (as listas de diagnostico), e ai nao ha como fugir da varredura.
void codec_plugin_ensure(MediaCodec codec)
{
    if (!g_autoload) return;

    if (codec == MEDIA_CODEC_NONE)
    {
        if (g_carregou_tudo) return;
        g_carregou_tudo = 1;
        codec_plugins_load(0);
        return;
    }
    codec_plugin_load_for(codec);
}

static void monta(void)
{
    if (g_count >= 0) return;
    g_count = 0;
    for (int s = 0; s < N_EMBUTIDOS; s++) junta(EMBUTIDOS[s], 1);   // hardware primeiro
    for (int s = 0; s < g_nextra;   s++) junta(g_extra[s],   1);
    for (int s = 0; s < N_EMBUTIDOS; s++) junta(EMBUTIDOS[s], 0);   // depois software
    for (int s = 0; s < g_nextra;   s++) junta(g_extra[s],   0);
}

// ---- o que o carregador usa ------------------------------------------------

void codec_registry_add(const CodecPluginSet* cs)
{
    if (!cs || g_nextra >= MAX_EXTRA) return;
    for (int i = 0; i < g_nextra; i++) if (g_extra[i] == cs) return;
    g_extra[g_nextra++] = cs;
    g_count = -1;                  // forca remontar na proxima consulta
}

void codec_registry_remove(const CodecPluginSet* cs)
{
    for (int i = 0; i < g_nextra; i++)
    {
        if (g_extra[i] != cs) continue;
        for (int k = i; k < g_nextra - 1; k++) g_extra[k] = g_extra[k + 1];
        g_nextra--;
        g_extra[g_nextra] = 0;
        g_count = -1;
        return;
    }
}

// ---- consulta --------------------------------------------------------------

int codec_plugin_count(void)
{
    monta();
    return g_count;
}

const CodecPlugin* codec_plugin_at(int index)
{
    monta();
    return (index >= 0 && index < g_count) ? g_flat[index] : 0;
}

// De qual conjunto este backend veio. E o que permite segurar o modulo enquanto ha
// encoder aberto (codec_loader.c).
const CodecPluginSet* codec_plugin_set_of(const CodecPlugin* p)
{
    monta();
    for (int i = 0; i < g_count; i++) if (g_flat[i] == p) return g_dono[i];
    return 0;
}

const CodecPlugin* codec_plugin_find(MediaCodec codec, int role)
{
    codec_plugin_ensure(codec);
    monta();
    for (int i = 0; i < g_count; i++)
    {
        const CodecPlugin* p = g_flat[i];
        if (p->Codec == codec && (p->Role & role) && p->Compiled) return p;
    }
    return 0;
}
