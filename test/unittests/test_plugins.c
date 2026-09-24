//  Codec carregado em tempo de execucao: o ciclo de vida do modulo.
//
//  O Opus saiu do binario e virou plugin (codec_opus_plugin.dll / libcodec_opus.so). Estes
//  testes cobrem o que so aparece quando o codec vem de um arquivo:
//    - ele e achado e entra na mesma lista dos embutidos;
//    - o que ele aloca cai no pool do HOST -- e a instancia unica do xplatbase valendo na
//      pratica, que e a razao de toda a prep-xplatbase;
//    - o modulo NAO se descarrega enquanto ha encoder aberto. Descarregar com uso vivo
//      derruba o processo na chamada seguinte, e foi assim que o executor de testes morria
//      antes de o xplatbase fixar o proprio modulo.
//
//  A carga e SOB DEMANDA: nao ha comando para chamar antes. Quem pede o codec (abrir um
//  encoder, perguntar se esta disponivel) e que traz o modulo. Por isso os testes abaixo
//  perguntam pelo codec primeiro e so entao olham a lista de modulos carregados -- se
//  olhassem a lista antes, veriam a verdade de antes da pergunta.

#include "testes.h"
#include "media_codec.h"
#include "codec_loader.h"
#include "memory_pool.h"
#include "xplatbase.h"
#include <string.h>
#include <stdio.h>

#define MODULO "codec_opus"

// Pede o codec (o que dispara a carga) e depois procura o modulo na lista.
static int indice_do_modulo(void)
{
    (void)media_codec_available(MEDIA_CODEC_OPUS);
    for (int i = 0, n = codec_plugins_count(); i < n; i++)
        if (strcmp(codec_plugin_module_name(i), MODULO) == 0) return i;
    return -1;
}

// Sem pedir nada: o que esta carregado NESTE instante.
static int indice_agora(void)
{
    for (int i = 0, n = codec_plugins_count(); i < n; i++)
        if (strcmp(codec_plugin_module_name(i), MODULO) == 0) return i;
    return -1;
}

// Abre um encoder de Opus valido, ou 0.
static MediaEncoder* abre_opus(void)
{
    MediaEncoderParams p;
    memset(&p, 0, sizeof(p));
    p.Codec = MEDIA_CODEC_OPUS;
    p.SampleRate = 48000; p.Channels = 2; p.BitrateBps = 96000;
    return media_encoder_open(&p);
}

// ---- 1) o modulo e achado sozinho ------------------------------------------

void teste_plugin_e_carregado(TestResult* r)
{
    int i;

    t_start(r);

    // Ninguem carregou nada a mao: perguntar pelo Opus e o que traz o modulo. Se isto
    // falhar, o .dll/.so nao foi gerado ao lado do binario.
    i = indice_do_modulo();
    T_ASSERT(r, i >= 0, "o modulo '%s' nao foi carregado (%d modulo(s) na lista)",
             MODULO, codec_plugins_count());

    T_ASSERT(r, media_codec_available(MEDIA_CODEC_OPUS),
             "o modulo carregou mas o Opus nao aparece como disponivel");
}

// ---- 2) o que o plugin aloca e do pool do host -----------------------------

void teste_plugin_usa_o_pool_do_host(TestResult* r)
{
    MemPoolStats antes, depois;
    MediaEncoder* e;
    XplatInstanceInfo first;
    unsigned long long alocou;

    t_start(r);

    if (indice_do_modulo() < 0) T_SKIP(r, "o modulo '%s' nao esta carregado", MODULO);

    // Nenhum modulo pode ter trazido um segundo xplatbase junto.
    T_ASSERT(r, !xplat_instance_check(&first),
             "ha uma SEGUNDA instancia do xplatbase no processo (a primeira veio de '%s')",
             first.Module);

    memop_get_stats(&antes);
    e = abre_opus();
    T_ASSERT(r, e != 0, "o encoder de Opus do plugin nao abriu");
    memop_get_stats(&depois);
    alocou = depois.alloc_count - antes.alloc_count;

    e->Close(e);

    T_ASSERT(r, alocou > 0,
             "abrir o encoder DENTRO do plugin nao mexeu na contabilidade do host: "
             "sinal de que ele tem outro pool");
}

// ---- 3) descarregar com uso vivo e recusado --------------------------------

void teste_plugin_nao_sai_em_uso(TestResult* r)
{
    MediaEncoder* e;
    int i, rc, usos;

    t_start(r);

    i = indice_do_modulo();
    if (i < 0) T_SKIP(r, "o modulo '%s' nao esta carregado", MODULO);

    e = abre_opus();
    T_ASSERT(r, e != 0, "o encoder de Opus nao abriu");

    i = indice_do_modulo();
    usos = codec_plugin_module_uses(i);

    rc = codec_plugin_unload(MODULO);
    if (rc == 1) { /* ja saiu: nao da para fechar o encoder */ T_ASSERT(r, 0,
        "o modulo foi descarregado com um encoder ABERTO -- a proxima chamada nele seria "
        "em memoria que ja nao existe"); }

    T_ASSERT(r, usos > 0, "o encoder aberto nao foi contado como uso do modulo");
    T_ASSERT(r, rc == -1, "descarregar em uso devolveu %d, esperado -1 (em uso)", rc);

    e->Close(e);

    i = indice_do_modulo();
    T_ASSERT(r, i >= 0 && codec_plugin_module_uses(i) == 0,
             "depois do Close o modulo continua contando uso (%d)",
             i >= 0 ? codec_plugin_module_uses(i) : -1);
}

// ---- 4) sai e volta --------------------------------------------------------

void teste_plugin_descarrega_e_volta(TestResult* r)
{
    int rc, sumiu, voltou;

    t_start(r);

    if (indice_do_modulo() < 0) T_SKIP(r, "o modulo '%s' nao esta carregado", MODULO);

    rc = codec_plugin_unload(MODULO);
    T_ASSERT(r, rc == 1, "nao foi possivel descarregar o modulo ocioso (codigo %d)", rc);

    // Olhando a lista SEM pedir nada: o modulo tem de ter saido de verdade.
    sumiu = (indice_agora() < 0);

    // E pedir o codec de novo tem de traze-lo de volta sozinho -- e o ponto da carga sob
    // demanda: o modulo entra e sai quantas vezes for preciso na vida do processo.
    voltou = indice_do_modulo();

    T_ASSERT(r, sumiu, "depois do unload o modulo continuou na lista de carregados");
    T_ASSERT(r, voltou >= 0, "pedir o codec de novo nao trouxe o modulo de volta");
    T_ASSERT(r, media_codec_available(MEDIA_CODEC_OPUS),
             "o Opus nao voltou a ficar disponivel");

    {
        MediaEncoder* e = abre_opus();
        T_ASSERT(r, e != 0, "depois de recarregar, o encoder de Opus nao abre mais");
        e->Close(e);
    }
}
