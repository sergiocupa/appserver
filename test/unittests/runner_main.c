//  Executor dos testes em C, para o Linux (e para rodar na linha de comando no Windows).
//
//  Os MESMOS arquivos .c dos testes rodam nas duas plataformas. O que muda e so quem os
//  chama: no Windows, o registro.cpp entrega os casos ao Gerenciador de Testes do Visual
//  Studio; aqui, este executor os roda direto. Nenhuma logica de teste vive nos dois lugares.
//
//  Uso:
//    unittests            -> roda todos
//    unittests <nome>     -> roda so aquele caso (e assim que o CTest registra um por um)
//    unittests --lista    -> imprime os nomes

#include "testes.h"
#include <stdio.h>
#include <string.h>

// Codigo de saida de caso pulado: o CTest o reconhece por SKIP_RETURN_CODE (CMakeLists).
#define RUNNER_PULADO 77

typedef struct { const char* nome; void (*fn)(TestResult*); } Caso;

static const Caso CASOS[] = {
    { "InstanciaUnica.ProcessoTemUmaSoInstancia",          teste_instancia_unica },
    { "InstanciaUnica.InstanciaVemDaCompartilhada",        teste_instancia_vem_da_compartilhada },
    { "PoolDeMemoria.ContabilizaAlocacaoELiberacao",       teste_memoria_contabiliza_alocacao },
    { "PoolDeMemoria.EscreveELeBlocoGrande",               teste_memoria_escreve_e_le },
    { "ServidorRede.PollAcordar",                          teste_poll_acordar },
    { "ServidorRede.PollLeituraEDesarme",                  teste_poll_leitura_e_desarme },
    { "ServidorRede.PollEscritaEVarios",                   teste_poll_escrita_e_varios },
    { "ParserHttp.PedacosDeUmByte", teste_parser_pedacos_de_um_byte },
    { "ParserHttp.PipeliningEmOrdem", teste_parser_pipelining_em_ordem },
    { "ParserHttp.CorpoGrandeEmMemoria", teste_parser_corpo_grande_em_memoria },
    { "ParserHttp.CorpoGrandeVaiParaDisco", teste_parser_corpo_grande_vai_para_disco },
    { "ParserHttp.CorpoSalvarMoveOArquivo", teste_parser_corpo_salvar_move_o_arquivo },
    { "ParserHttp.LimitesEErros", teste_parser_limites_e_erros },
    { "ParserHttp.DestruirNoMeioApagaTemporario", teste_parser_destruir_no_meio_apaga_temporario },
    { "Reator.EcoEmOrdem", teste_reator_eco_em_ordem },
    { "Reator.ConexoesSemThread", teste_reator_conexoes_sem_thread },
    { "Reator.PausaERetoma", teste_reator_pausa_e_retoma },
    { "Reator.ClienteQueNaoLeCai", teste_reator_cliente_que_nao_le_cai },
    { "Reator.FechaOcioso", teste_reator_fecha_ocioso },
    { "Reator.SincronoComUmCliente", teste_reator_sincrono_com_um_cliente },
    { "Reator.ParaleloSobDisputa", teste_reator_paralelo_sob_disputa },
    { "Reator.DisputaLeveFicaNoReator", teste_reator_disputa_leve_fica_no_reator },
    { "Reator.MuitosClientesUsamOPool", teste_reator_muitos_clientes_usam_o_pool },
    { "Reator.PistaSobDemandaEReuso", teste_pista_sob_demanda_e_reuso },
    { "Reator.ParadoNaoAcorda", teste_reator_parado_nao_acorda },
    { "Reator.EnviaArquivo", teste_reator_envia_arquivo },
    { "Reator.EnviaArquivoClienteParado", teste_reator_envia_arquivo_cliente_parado },
    { "Reator.ArquivosSimultaneos", teste_reator_arquivos_simultaneos },
    { "WebSocketETopicos.QuadrosPartidosEJuntos", teste_ws_quadros_partidos_e_juntos },
    { "WebSocketETopicos.FragmentadaPingEClose", teste_ws_fragmentada_ping_e_close },
    { "WebSocketETopicos.TopicoRetidoEAoVivo", teste_topico_retido_e_ao_vivo },
    { "WebSocketETopicos.TopicoAssinantesSemThread", teste_topico_assinantes_sem_thread },
    { "WebSocketETopicos.RotaLentaAprendida", teste_rota_lenta_aprendida },
    { "WebSocketETopicos.PerfilEconomiaEJobs", teste_perfil_economia_e_jobs },
    { "WebSocketETopicos.SsePoucosAssinantesSemPista", teste_sse_poucos_assinantes_sem_pista },
    { "WebSocketETopicos.SsePistasOrdemETodos", teste_sse_pistas_ordem_e_todos },
    { "WebSocketETopicos.SsePistasAssinantesSaem", teste_sse_pistas_assinantes_saem },
    { "WebSocketETopicos.SsePistasDoisPublicadores", teste_sse_pistas_dois_publicadores },
    { "ArquivosEstaticos.RangeArquivoEstatico", teste_estatico_range },
    { "Arquitetura.FronteirasEntreCamadas", teste_fronteiras_entre_camadas },
    { "PoolDeTarefas.ExecutaTodasAsTarefas",               teste_pool_executa_tarefas },
    { "Pipeline.WebmRoundtripVp9",                         teste_webm_roundtrip_vp9 },
    { "Pipeline.WebmRoundtripAv1",                         teste_webm_roundtrip_av1 },
    { "Pipeline.GatewayDashVp9",                           teste_gateway_dash_vp9 },
    { "Pipeline.GatewayHlsH264",                           teste_gateway_hls_h264 },
    { "Pipeline.GatewayMemoriaNaoCresce",                  teste_gateway_memoria_nao_cresce },
    { "Hardware.EncoderPorHardwareH264",                  teste_encoder_hardware_h264 },
    { "Hardware.DecodificaOQueOEncoderProduziu",          teste_decode_do_que_o_encoder_produziu },
    { "Codecs.IdaEVoltaH264",                                    teste_ida_e_volta_h264 },
    { "Codecs.IdaEVoltaH265",                                    teste_ida_e_volta_h265 },
    { "Codecs.IdaEVoltaVp9",                                     teste_ida_e_volta_vp9 },
    { "Codecs.IdaEVoltaAv1",                                     teste_ida_e_volta_av1 },
    { "Codecs.EncodeOpus",                                       teste_encode_opus },
    { "GatewayMatriz.AlternaCodec",                              teste_gateway_alterna_codec },
    { "GatewayMatriz.AlternaResolucaoEFps",                      teste_gateway_alterna_resolucao_e_fps },
    { "GatewayMatriz.AlternaEntrada",                            teste_gateway_alterna_entrada },
    { "Plugins.Carregado",                                       teste_plugin_e_carregado },
    { "Plugins.UsaOPoolDoHost",                                  teste_plugin_usa_o_pool_do_host },
    { "Plugins.NaoSaiEmUso",                                     teste_plugin_nao_sai_em_uso },
    { "Plugins.DescarregaEVolta",                                teste_plugin_descarrega_e_volta },
    { "Bench.H264Threads",                              teste_bench_h264_threads },
    { "Bench.TodosCodecs",                             teste_bench_todos_codecs },
    { "Bench.Varredura",                               teste_bench_varredura },
    { "InstanciaEntreModulos.ModuloCompartilhadoUsaMesmoPool",  teste_modulo_compartilhado_mesmo_pool },
    { "InstanciaEntreModulos.ModuloCompartilhadoMesmasThreads", teste_modulo_compartilhado_mesmo_registro_de_threads },
    { "InstanciaEntreModulos.ModuloUsaMesmoPoolDeTarefas",      teste_modulo_usa_mesmo_pool_de_tarefas },
    { "InstanciaEntreModulos.DuplicataEDenunciada",             teste_duplicata_e_denunciada },
    { "InstanciaEntreModulos.CicloCarregaDescarrega",           teste_ciclo_carrega_descarrega },
};
#define TOTAL (int)(sizeof(CASOS) / sizeof(CASOS[0]))

static int roda(const Caso* c)
{
    TestResult r;
    memset(&r, 0, sizeof(r));
    c->fn(&r);
    // Pulado NAO e "ok": nada foi verificado, e o motivo tem de aparecer.
    printf("%-52s %s\n", c->nome, !r.ok ? "FALHOU" : r.skipped ? "pulado" : "ok");
    if (!r.ok || r.skipped) printf("    %s\n", r.msg);
    return !r.ok ? 1 : r.skipped ? RUNNER_PULADO : 0;
}

int main(int argc, char** argv)
{
    int i, falhas = 0;

    if (argc > 1 && strcmp(argv[1], "--lista") == 0)
    {
        for (i = 0; i < TOTAL; i++) printf("%s\n", CASOS[i].nome);
        return 0;
    }

    if (argc > 1)
    {
        for (i = 0; i < TOTAL; i++)
            if (strcmp(CASOS[i].nome, argv[1]) == 0) return roda(&CASOS[i]);
        printf("caso desconhecido: %s\n", argv[1]);
        return 2;
    }

    int pulados = 0;
    for (i = 0; i < TOTAL; i++)
    {
        int rc = roda(&CASOS[i]);
        if (rc == RUNNER_PULADO) pulados++; else falhas += rc;
    }
    printf("\n%d de %d aprovados, %d pulados, %d falhas\n", TOTAL - falhas - pulados, TOTAL, pulados, falhas);
    return falhas ? 1 : 0;
}
