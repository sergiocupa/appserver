//  Registro dos testes no Gerenciador de Testes do Visual Studio.
//
//  Este e o UNICO arquivo C++ do projeto, e de proposito: ele nao tem logica de teste
//  nenhuma. Cada entrada e uma linha que chama a funcao C correspondente e reporta o
//  resultado. A logica esta nos .c, na mesma linguagem das bibliotecas testadas.
//
//  O framework (CppUnitTest) ja vem instalado com o Visual Studio e o adaptador dele e
//  embutido -- nao ha dependencia externa para baixar.

#include "CppUnitTest.h"
#include <string>

extern "C" {
#include "testes.h"
}

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace
{
    // A mensagem de falha vem em char* do lado C; o framework espera wchar_t*.
    std::wstring largura(const char* s)
    {
        std::string texto(s ? s : "");
        return std::wstring(texto.begin(), texto.end());
    }

    void executa(void (*teste)(TestResult*))
    {
        TestResult r;
        teste(&r);
        // Pulado aparece na saida do teste: verde silencioso nao diz o que foi exercitado.
        if (r.ok && r.skipped) Logger::WriteMessage((L"PULADO: " + largura(r.msg)).c_str());
        Assert::IsTrue(r.ok != 0, largura(r.msg).c_str());
    }
}

// Uma linha por teste. O nome aqui e o que aparece no Gerenciador.
#define CASO(nome, funcao) TEST_METHOD(nome) { executa(funcao); }

namespace Suite
{
TEST_CLASS(InstanciaUnica)
{
public:
    CASO(ProcessoTemUmaSoInstancia,        teste_instancia_unica)
    CASO(InstanciaVemDaBibliotecaCompartilhada, teste_instancia_vem_da_compartilhada)
};

TEST_CLASS(PoolDeMemoria)
{
public:
    CASO(ContabilizaAlocacaoELiberacao, teste_memoria_contabiliza_alocacao)
    CASO(EscreveELeBlocoGrande,         teste_memoria_escreve_e_le)
};

TEST_CLASS(ServidorRede)
{
public:
    CASO(PollAcordar,           teste_poll_acordar)
    CASO(PollLeituraEDesarme,   teste_poll_leitura_e_desarme)
    CASO(PollEscritaEVarios,    teste_poll_escrita_e_varios)
};

TEST_CLASS(ParserHttp)
{
public:
    CASO(PedacosDeUmByte, teste_parser_pedacos_de_um_byte)
    CASO(PipeliningEmOrdem, teste_parser_pipelining_em_ordem)
    CASO(CorpoGrandeEmMemoria, teste_parser_corpo_grande_em_memoria)
    CASO(CorpoGrandeVaiParaDisco, teste_parser_corpo_grande_vai_para_disco)
    CASO(CorpoSalvarMoveOArquivo, teste_parser_corpo_salvar_move_o_arquivo)
    CASO(LimitesEErros, teste_parser_limites_e_erros)
    CASO(DestruirNoMeioApagaTemporario, teste_parser_destruir_no_meio_apaga_temporario)
};

TEST_CLASS(Reator)
{
public:
    CASO(EcoEmOrdem, teste_reator_eco_em_ordem)
    CASO(ConexoesSemThread, teste_reator_conexoes_sem_thread)
    CASO(PausaERetoma, teste_reator_pausa_e_retoma)
    CASO(ClienteQueNaoLeCai, teste_reator_cliente_que_nao_le_cai)
    CASO(FechaOcioso, teste_reator_fecha_ocioso)
    CASO(SincronoComUmCliente, teste_reator_sincrono_com_um_cliente)
    CASO(ParaleloSobDisputa, teste_reator_paralelo_sob_disputa)
    CASO(DisputaLeveFicaNoReator, teste_reator_disputa_leve_fica_no_reator)
    CASO(MuitosClientesUsamOPool, teste_reator_muitos_clientes_usam_o_pool)
    CASO(PistaSobDemandaEReuso, teste_pista_sob_demanda_e_reuso)
    CASO(ParadoNaoAcorda, teste_reator_parado_nao_acorda)
    CASO(EnviaArquivo, teste_reator_envia_arquivo)
    CASO(EnviaArquivoClienteParado, teste_reator_envia_arquivo_cliente_parado)
    CASO(ArquivosSimultaneos, teste_reator_arquivos_simultaneos)
};

TEST_CLASS(WebSocketETopicos)
{
public:
    CASO(QuadrosPartidosEJuntos, teste_ws_quadros_partidos_e_juntos)
    CASO(FragmentadaPingEClose, teste_ws_fragmentada_ping_e_close)
    CASO(TopicoRetidoEAoVivo, teste_topico_retido_e_ao_vivo)
    CASO(TopicoAssinantesSemThread, teste_topico_assinantes_sem_thread)
    CASO(RotaLentaAprendida, teste_rota_lenta_aprendida)
    CASO(PerfilEconomiaEJobs, teste_perfil_economia_e_jobs)
    CASO(SsePoucosAssinantesSemPista, teste_sse_poucos_assinantes_sem_pista)
    CASO(SsePistasOrdemETodos, teste_sse_pistas_ordem_e_todos)
    CASO(SsePistasAssinantesSaem, teste_sse_pistas_assinantes_saem)
    CASO(SsePistasDoisPublicadores, teste_sse_pistas_dois_publicadores)
};

TEST_CLASS(ArquivosEstaticos)
{
public:
    CASO(RangeArquivoEstatico, teste_estatico_range)
    CASO(CondicionalArquivoEstatico, teste_estatico_condicional)
};

TEST_CLASS(Arquitetura)
{
public:
    CASO(FronteirasEntreCamadas, teste_fronteiras_entre_camadas)
};

TEST_CLASS(PoolDeTarefas)
{
public:
    CASO(ExecutaTodasAsTarefas, teste_pool_executa_tarefas)
};

TEST_CLASS(Pipeline)
{
public:
    CASO(WebmRoundtripVp9,        teste_webm_roundtrip_vp9)
    CASO(WebmRoundtripAv1,        teste_webm_roundtrip_av1)
    CASO(GatewayDashVp9,          teste_gateway_dash_vp9)
    CASO(GatewayHlsH264,          teste_gateway_hls_h264)
    CASO(GatewayMemoriaNaoCresce, teste_gateway_memoria_nao_cresce)
};

TEST_CLASS(Hardware)
{
public:
    CASO(EncoderPorHardwareH264,        teste_encoder_hardware_h264)
    CASO(DecodificaOQueOEncoderProduziu, teste_decode_do_que_o_encoder_produziu)
};

TEST_CLASS(InstanciaEntreModulos)
{
public:
    CASO(ModuloCompartilhadoUsaMesmoPool,     teste_modulo_compartilhado_mesmo_pool)
    CASO(ModuloCompartilhadoMesmasThreads,    teste_modulo_compartilhado_mesmo_registro_de_threads)
    CASO(ModuloUsaMesmoPoolDeTarefas,         teste_modulo_usa_mesmo_pool_de_tarefas)
    CASO(DuplicataEDenunciada,                teste_duplicata_e_denunciada)
    CASO(CicloCarregaDescarrega,              teste_ciclo_carrega_descarrega)
};

TEST_CLASS(Codecs)
{
public:
    CASO(IdaEVoltaH264,           teste_ida_e_volta_h264)
    CASO(IdaEVoltaH265,           teste_ida_e_volta_h265)
    CASO(IdaEVoltaVp9,            teste_ida_e_volta_vp9)
    CASO(IdaEVoltaAv1,            teste_ida_e_volta_av1)
    CASO(EncodeOpus,              teste_encode_opus)
};

TEST_CLASS(GatewayMatriz)
{
public:
    CASO(AlternaCodec,            teste_gateway_alterna_codec)
    CASO(AlternaResolucaoEFps,    teste_gateway_alterna_resolucao_e_fps)
    CASO(AlternaEntrada,          teste_gateway_alterna_entrada)
};


TEST_CLASS(Plugins)
{
public:
    CASO(Carregado,               teste_plugin_e_carregado)
    CASO(UsaOPoolDoHost,          teste_plugin_usa_o_pool_do_host)
    CASO(NaoSaiEmUso,             teste_plugin_nao_sai_em_uso)
    CASO(DescarregaEVolta,        teste_plugin_descarrega_e_volta)
};

TEST_CLASS(Bench)
{
public:
    CASO(H264Threads,             teste_bench_h264_threads)
    CASO(TodosCodecs,             teste_bench_todos_codecs)
    CASO(Varredura,               teste_bench_varredura)
};
}   // namespace Suite
