//  Lista dos testes. O registro (registro.cpp) inclui este cabecalho dentro de extern "C"
//  e nao precisa saber mais nada sobre eles.
#ifndef TESTES_H
#define TESTES_H

#include "ctest_core.h"

// ---- instancia unica do xplatbase (ver prep-xplatbase/PLANO.md) -------------
void teste_instancia_unica(TestResult* r);
void teste_instancia_vem_da_compartilhada(TestResult* r);

// ---- pool de memoria --------------------------------------------------------
void teste_memoria_contabiliza_alocacao(TestResult* r);
void teste_memoria_escreve_e_le(TestResult* r);

// ---- pool de tarefas --------------------------------------------------------
void teste_pool_executa_tarefas(TestResult* r);

// ---- pipeline de midia (vieram do webmtest, mkvtest e gwtest) ---------------
void teste_webm_roundtrip_vp9(TestResult* r);
void teste_webm_roundtrip_av1(TestResult* r);
void teste_gateway_dash_vp9(TestResult* r);
void teste_gateway_hls_h264(TestResult* r);
void teste_gateway_memoria_nao_cresce(TestResult* r);

// ---- dependem de hardware (pulam sozinhos onde nao houver) ------------------
void teste_encoder_hardware_h264(TestResult* r);
void teste_decode_do_que_o_encoder_produziu(TestResult* r);

// ---- instancia unica vista de fora (carregando modulos) --------------------
void teste_modulo_compartilhado_mesmo_pool(TestResult* r);
void teste_modulo_compartilhado_mesmo_registro_de_threads(TestResult* r);
void teste_duplicata_e_denunciada(TestResult* r);

void teste_ciclo_carrega_descarrega(TestResult* r);

void teste_modulo_usa_mesmo_pool_de_tarefas(TestResult* r);

// ---- ida e volta por codec -------------------------------------------------
void teste_ida_e_volta_h264(TestResult* r);
void teste_ida_e_volta_h265(TestResult* r);
void teste_ida_e_volta_vp9(TestResult* r);
void teste_ida_e_volta_av1(TestResult* r);
void teste_encode_opus(TestResult* r);

// ---- transicao entre codecs no gateway -------------------------------------
void teste_gateway_alterna_codec(TestResult* r);
void teste_gateway_alterna_resolucao_e_fps(TestResult* r);
void teste_gateway_alterna_entrada(TestResult* r);

// ---- codec carregado em tempo de execucao ----------------------------------
void teste_plugin_e_carregado(TestResult* r);
void teste_plugin_usa_o_pool_do_host(TestResult* r);
void teste_plugin_nao_sai_em_uso(TestResult* r);
void teste_plugin_descarrega_e_volta(TestResult* r);

#endif
