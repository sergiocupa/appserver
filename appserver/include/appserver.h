//  MIT License � Modified for Mandatory Attribution
//  
//  Copyright(c) 2025 Sergio Paludo
//
//  github.com/sergiocupa
//  
//  Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files, 
//  to use, copy, modify, merge, publish, distribute, and sublicense the software, including for commercial purposes, provided that:
//  
//     01. The original author�s credit is retained in all copies of the source code;
//     02. The original author�s credit is included in any code generated, derived, or distributed from this software, including templates, libraries, or code - generating scripts.
//  
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED.


#ifndef APPSERVER_H
#define APPSERVER_H

#ifdef __cplusplus
extern "C" {
#endif

    #include "../src/server_type.h"



    /* Configuracao padrao: porta 8080, prefixo "api", sem pasta web, health desligado,
     * 512 conexoes, 75 s de ociosidade, cabecalho ate 64 KB, corpo ate 1 GB (em disco acima
     * de 8 MB), fila de saida de 8 MB por conexao, heartbeat SSE de 15 s, pool global.
     *
     * Variaveis de ambiente lidas AQUI (e so aqui), sobrepondo o padrao:
     *   APPSERVER_MAX_BODY_MB=<n>          -> MaxBodyBytes
     *   APPSERVER_HEALTH_ALLOW_REMOTE=1    -> HealthAllowRemote
     *
     * EnableHealthMonitor: liga a rota embutida de saude, /<prefix>/health.
     *   true  -> amostra CPU, memoria do processo, contadores do pool do xplatbase e, no
     *            Windows, GPU via PDH. A rota expoe contadores internos do processo SEM
     *            passar por autenticacao; por isso so atende a propria maquina, salvo
     *            HealthAllowRemote -- ver a ressalva em health_controller.c.
     *   false -> o monitor nao inicializa, a consulta do PDH nao abre, nenhuma amostra e
     *            colhida e /<prefix>/health nao existe (cai na rota nao encontrada).
     *
     *  O que o campo NAO muda: pdh.dll continua mapeada no processo nos dois casos,
     *  porque o import e estatico (ver a nota em health_monitor.c). d3d11.dll e dxgi.dll
     *  tambem aparecem sempre, mas vem do encode por hardware em codecs/media/hw_dec_mf.c,
     *  nada a ver com este monitor. Em nenhum dos casos roda codigo de PDH com false.
     */
    AppServerConfig appserver_config_default(void);

    // Sem XPLATBASE_API: com XPLATBASE_USE_SHARED ele vira __declspec(dllimport), ou seja,
    // declarava esta funcao como IMPORTADA da DLL do xplatbase -- e ela e definida aqui, numa
    // biblioteca estatica. O MSVC avisava (C4273, vinculo de dll inconsistente).
    AppServerInfo* appserver_create(const AppServerConfig* config, FunctionBindList* bind_list);

    void app_add_receiver(FunctionBindList* list, const char* route, MessageMatchReceiverCalback function, bool with_callback);
    /* Rota LONGA: o handler pode demorar (processar video, esperar hardware, transmitir um
     * fluxo). Ele roda na pista longa (Config.LongWorkers threads fixas), fora dos workers que
     * atendem as rotas curtas, e a conexao dele espera: as requisicoes seguintes da mesma
     * conexao so sao atendidas depois, na ordem. As outras conexoes nao sentem nada.
     * Rota comum (app_add_receiver) deve ser curta: roda direto na tarefa que leu a
     * requisicao. */
    void app_add_receiver_longa(FunctionBindList* list, const char* route, MessageMatchReceiverCalback function);

    /* Rota comum que, MEDIDA, demora (acima de Config.RotaLentaUs em 3 execucoes seguidas)
     * passa sozinha a rodar fora do reator (numa tarefa do pool), e volta quando fica rapida.
     * Diagnostico: 1 = aprendida como lenta, 0 = nao, -1 = rota nao existe. */
    int  app_rota_lenta(AppServerInfo* server, const char* route);

    /* Canal SSE: responde a requisicao com um fluxo text/event-stream. Os eventos saem pela
     * fila da conexao (cliente lento nao trava quem publica; cliente que nao le cai ao passar
     * de Config.MaxOutputQueueBytes). Quando o handler retorna, o canal e a conexao sao
     * fechados pelo servidor -- a rota tem de ser longa. */
    AppCanal* app_canal_sse(Message* request);
    bool      app_canal_evento(AppCanal* canal, const char* nome, const char* dados);   // nome pode ser 0
    void      app_canal_fechar(AppCanal* canal);
    bool      app_canal_ativo(AppCanal* canal);   // false = o cliente saiu (ou o envio falhou)

    /* ---- Perfil do pool de tarefas (energia x vazao) --------------------------------------
     *
     * Config.PerfilPool escolhe o perfil do pool de tarefas do xplatbase na criacao do
     * servidor (-1, o padrao, nao mexe). Com POOL_PERFIL_ECONOMIA, um trabalho que precisa
     * de vazao por um tempo pede performance; o ultimo a terminar devolve economia. Os jobs
     * (app_job_iniciar) fazem isso sozinhos quando Config.JobsEmPerformance (padrao).
     * Sem perfil economia configurado, as duas funcoes nao fazem nada.
     */
    void app_perfil_performance_inicio(AppServerInfo* server);
    void app_perfil_performance_fim(AppServerInfo* server);

    /* ---- Topicos: publica / assina, com estado retido --------------------------------------
     *
     * app_assinar: responde a requisicao com um fluxo SSE que recebe primeiro o RETIDO do
     *   topico (ver app_publicar) e depois tudo o que for publicado. O handler retorna NA HORA
     *   (a rota deve ser CURTA): a conexao fica aberta no reator sem ocupar thread nenhuma.
     *   O fluxo fecha quando o topico encerra.
     *
     * app_publicar: slot = 0 -> ACUMULA (todo assinante novo recebe, em ordem: "start",
     *   "done", "error"...); slot != 0 -> SUBSTITUI o ultimo valor daquele slot (progresso:
     *   quem chega so precisa do mais recente, por pista).
     *
     * app_topico_encerrar: fecha os fluxos dos assinantes. O retido fica 5 min para quem
     *   reconectar logo depois do fim.
     *
     * Entrega: quem publica manda aos assinantes ele mesmo ate Config.SseVoltaUs; o que sobra
     *   vai em pistas (no maximo Config.SseMaxPorPista assinantes cada) para o pool, e quem
     *   publica tambem pega pistas. app_publicar so retorna com o evento entregue a todos:
     *   a ordem dos eventos se mantem e nao se acumula fila. Poucos assinantes: nenhuma
     *   tarefa (nao acorda thread). app_topico_pistas: pistas mandadas ao pool (diagnostico).
     */
    bool app_assinar(Message* request, const char* topico);
    void app_publicar(AppServerInfo* server, const char* topico, const char* slot, const char* dados);
    void app_topico_encerrar(AppServerInfo* server, const char* topico);
    int  app_topico_assinantes(AppServerInfo* server, const char* topico);
    int  app_topico_pistas(AppServerInfo* server);

    /* ---- Jobs: trabalho longo, UM por chave ---------------------------------------------------
     *
     * Roda na pista longa e publica no topico de mesmo nome da chave (app_job_publicar). Ao
     * terminar, o topico encerra. Se ja ha um job com a chave, nao inicia outro (devolve false
     * e libera 'arg'): quem pediu de novo so precisa assinar o topico.
     */
    typedef struct AppJob AppJob;
    typedef void (*AppJobFn)(AppJob* job, void* arg);
    bool        app_job_iniciar(AppServerInfo* server, const char* chave, AppJobFn fn, void* arg, void (*liberar)(void*));
    void        app_job_publicar(AppJob* job, const char* slot, const char* dados);
    const char* app_job_chave(AppJob* job);
    bool        app_job_rodando(AppServerInfo* server, const char* chave);

    void app_add_receiver_extension(FunctionBindList* list, const char* extension, MessageMatchReceiverCalback function, bool with_callback);
    void app_add_web_resource(FunctionBindList* list, const char* route, MessageMatchReceiverCalback function);
    MessageEmitterCalback app_add_emitter(FunctionBindList* list, const char* route);

    /* Corpo da requisicao. Ate Config.BodyToDiskBytes (8 MB por padrao) ele fica em memoria,
     * em request->Content; acima disso vai direto para um arquivo temporario em
     * Config.TempDir, conforme chega, e request->Content fica vazio. request->ContentLength
     * vale nos dois casos.
     *
     *   app_corpo_arquivo: caminho do temporario, ou 0 se o corpo esta em memoria. O arquivo
     *                      e apagado quando a requisicao termina.
     *   app_corpo_salvar : grava o corpo em 'destino' qualquer que seja o caso. Do temporario
     *                      ele MOVE (no mesmo volume, so renomeia; nada e copiado) e o arquivo
     *                      passa a ser do chamador. Devolve false se nao conseguiu.
     */
    const char* app_corpo_arquivo(Message* request);
    bool        app_corpo_salvar(Message* request, const char* destino);

    void appserver_http_response_send(AppServerInfo* server, Message* request, HttpStatusCode http_status, ResourceBuffer* object, HeaderAppender header_appender, void* appender_args);


#ifdef __cplusplus
}
#endif

#endif /* APPSERVER */