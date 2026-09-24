//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Carga e descarga de codecs em tempo de execucao.
//
//  O resto do sistema nao muda de jeito nenhum: depois que um plugin e carregado, os
//  backends dele entram na MESMA lista que os embutidos, e media_encoder_open /
//  media_decoder_open os encontram como sempre. Quem chama nao sabe (nem precisa saber) se
//  o codec veio linkado ou de um arquivo.

#ifndef CODEC_LOADER_H
#define CODEC_LOADER_H

#include "codec_plugin.h"

#ifdef __cplusplus
extern "C" {
#endif

// Liga/desliga a carga automatica na primeira consulta ao registro (ligada por padrao).
// Desligar so faz sentido para quem quer controlar quando os modulos entram.
void codec_plugins_autoload(int ligado);

// Carrega o modulo que atende ESSE codec, se houver um na pasta deste modulo. E o que o
// media_encoder_open / media_decoder_open fazem sozinhos: nao ha um comando de carga para
// chamar antes, e nada alem do necessario entra no processo. Retorna quantos entraram.
int codec_plugin_load_for(MediaCodec codec);

// Carrega TODOS os plugins de uma pasta. NULL = a pasta do proprio executavel, que e o
// caso normal. Retorna quantos modulos entraram; ja carregado nao entra de novo.
int codec_plugins_load(const char* pasta);

// Carrega UM modulo pelo caminho. Retorna 1 se entrou, 0 se ja estava, < 0 em erro.
int codec_plugin_load_file(const char* caminho);

// Descarrega um modulo pelo nome do conjunto ("codec_av1"). Retorna:
//    1  descarregado
//    0  nao esta carregado
//   -1  EM USO -- ha encoder ou decoder aberto que veio dele; descarregar mataria o
//       processo na proxima chamada. E o mesmo motivo pelo qual o xplatbase fixa o proprio
//       modulo na memoria (ver prep-xplatbase).
int codec_plugin_unload(const char* modulo);

// Descarrega todos os que nao estao em uso. Retorna quantos sairam.
int codec_plugins_unload_all(void);

// Quantos modulos estao carregados, e o nome/uso de cada um (para diagnostico e teste).
int         codec_plugins_count(void);
const char* codec_plugin_module_name(int index);
int         codec_plugin_module_uses(int index);

#ifdef __cplusplus
}
#endif
#endif // CODEC_LOADER_H
