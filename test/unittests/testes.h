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

// ---- servidor: camada de rede (appserver/src/net) --------------------------
void teste_poll_acordar(TestResult* r);
void teste_poll_leitura_e_desarme(TestResult* r);
void teste_poll_escrita_e_varios(TestResult* r);

// ---- servidor: parser HTTP (appserver/src/http) ---------------------------
void teste_parser_pedacos_de_um_byte(TestResult* r);
void teste_parser_pipelining_em_ordem(TestResult* r);
void teste_parser_corpo_grande_em_memoria(TestResult* r);
void teste_parser_corpo_grande_vai_para_disco(TestResult* r);
void teste_parser_corpo_salvar_move_o_arquivo(TestResult* r);
void teste_parser_limites_e_erros(TestResult* r);
void teste_parser_destruir_no_meio_apaga_temporario(TestResult* r);

// ---- servidor: reator (appserver/src/net/net_servidor.c) ------------------
void teste_reator_eco_em_ordem(TestResult* r);
void teste_reator_conexoes_sem_thread(TestResult* r);
void teste_reator_pausa_e_retoma(TestResult* r);
void teste_reator_cliente_que_nao_le_cai(TestResult* r);
void teste_reator_fecha_ocioso(TestResult* r);
void teste_reator_sincrono_com_um_cliente(TestResult* r);
void teste_reator_paralelo_sob_disputa(TestResult* r);
void teste_reator_disputa_leve_fica_no_reator(TestResult* r);
void teste_reator_muitos_clientes_usam_o_pool(TestResult* r);
void teste_pista_sob_demanda_e_reuso(TestResult* r);
void teste_reator_parado_nao_acorda(TestResult* r);
void teste_reator_envia_arquivo(TestResult* r);
void teste_reator_envia_arquivo_cliente_parado(TestResult* r);
void teste_reator_arquivos_simultaneos(TestResult* r);

// ---- servidor: WebSocket incremental e topicos ----------------------------
void teste_ws_quadros_partidos_e_juntos(TestResult* r);
void teste_ws_fragmentada_ping_e_close(TestResult* r);
void teste_topico_retido_e_ao_vivo(TestResult* r);
void teste_topico_assinantes_sem_thread(TestResult* r);
void teste_rota_lenta_aprendida(TestResult* r);
void teste_perfil_economia_e_jobs(TestResult* r);
void teste_sse_poucos_assinantes_sem_pista(TestResult* r);
void teste_sse_pistas_ordem_e_todos(TestResult* r);
void teste_sse_pistas_assinantes_saem(TestResult* r);
void teste_sse_pistas_dois_publicadores(TestResult* r);
void teste_estatico_range(TestResult* r);

// ---- servidor: fronteiras entre camadas (le os fontes) ----------------------
void teste_fronteiras_entre_camadas(TestResult* r);

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

// ---- medicao com video real (pula se o arquivo nao estiver na maquina) -----
void teste_bench_h264_threads(TestResult* r);

void teste_bench_todos_codecs(TestResult* r);

void teste_bench_varredura(TestResult* r);

#endif
