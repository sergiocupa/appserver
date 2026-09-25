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

typedef struct { const char* nome; void (*fn)(TestResult*); } Caso;

static const Caso CASOS[] = {
    { "InstanciaUnica.ProcessoTemUmaSoInstancia",          teste_instancia_unica },
    { "InstanciaUnica.InstanciaVemDaCompartilhada",        teste_instancia_vem_da_compartilhada },
    { "PoolDeMemoria.ContabilizaAlocacaoELiberacao",       teste_memoria_contabiliza_alocacao },
    { "PoolDeMemoria.EscreveELeBlocoGrande",               teste_memoria_escreve_e_le },
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
    c->fn(&r);
    printf("%-52s %s\n", c->nome, r.ok ? "ok" : "FALHOU");
    if (!r.ok) printf("    %s\n", r.msg);
    return r.ok ? 0 : 1;
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

    for (i = 0; i < TOTAL; i++) falhas += roda(&CASOS[i]);
    printf("\n%d de %d aprovados\n", TOTAL - falhas, TOTAL);
    return falhas ? 1 : 0;
}
