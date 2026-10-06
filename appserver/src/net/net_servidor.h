//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  CAMADA 1 (transporte): servidor TCP com UM reator e tarefas no pool.
//
//  Uma unica thread (o reator) espera por TODAS as conexoes com net_poll. Quando chegam
//  bytes numa conexao, o reator a DESARMA e entrega uma tarefa ao pool; a tarefa le o que
//  houver (sem bloquear), passa ao protocolo e devolve a conexao ao reator, que a rearma.
//  Consequencias:
//
//    - conexao parada (keep-alive, SSE esperando evento, cliente lento) nao custa thread;
//    - nunca ha duas tarefas da mesma conexao ao mesmo tempo: o protocolo recebe os bytes
//      em ordem, sem lock proprio para isso;
//    - escrever e de qualquer thread (net_enviar): o que o socket nao aceita na hora vai
//      para a fila da conexao, e o reator escoa quando o socket deixa;
//    - so o reator mexe no conjunto do poll; os outros pedem por uma fila + net_poll_acordar.
//
//  Nao sabe nada de HTTP: quem interpreta os bytes e o NetProtocolo (a camada 2).

#ifndef APPSERVER_NET_SERVIDOR_H
#define APPSERVER_NET_SERVIDOR_H

#include "../server_type.h"

#ifdef __cplusplus
extern "C" {
#endif

    typedef struct NetServidor NetServidor;
    typedef struct NetConexao  NetConexao;

    typedef struct
    {
        // Conexao aceita. Devolve o contexto do protocolo para ela (0 = recusa e fecha).
        // Roda no reator: nao pode bloquear.
        void* (*AoAbrir)(NetConexao* c, void* ctx_servidor);

        // Bytes recebidos. Roda numa tarefa do pool, NUNCA em paralelo com outra chamada
        // da mesma conexao. Pode chamar net_enviar, net_fechar, net_pausar_leitura.
        void  (*AoReceber)(NetConexao* c, const byte* dados, int n);

        // A conexao acabou (o outro lado fechou, erro, ociosidade ou net_fechar). Chamado uma
        // vez, no reator, depois da ultima AoReceber. Quem ainda segura a conexao
        // (net_segurar) continua podendo chamar net_enviar -- que so devolve false.
        void  (*AoFechar)(NetConexao* c);

        // Ninguem mais usa a conexao: libera o contexto. Ultima chamada; qualquer thread.
        void  (*AoLiberar)(void* ctx);

        // Relogio do protocolo (heartbeat de canais...): no reator, nao pode bloquear. Devolve
        // em quantos ms precisa ser chamado de novo, ou -1 se nao precisa (ate net_pedir_relogio).
        // O reator NAO acorda periodicamente: so no prazo que alguem pediu. Pode ser 0.
        int   (*AoTick)(void* ctx_servidor);

        // O outro lado fechou o envio (recv devolveu 0). A conexao ainda escoa o que esta na
        // fila e termina o que esta em andamento antes de fechar; o protocolo que sabe que nao
        // ha mais nada a esperar (um fluxo SSE) pode fechar ja. No reator. Pode ser 0.
        void  (*AoFimLeitura)(NetConexao* c);
    }
    NetProtocolo;

    typedef struct
    {
        int         Porta;            // 0 = o sistema escolhe (testes); ver net_servidor_porta
        int         MaxConexoes;      // acima disso, a conexao nova recebe RecusaCheio e fecha
        int         OciosoMs;         // sem bytes chegando nem nada em andamento: fecha
        int64       MaxFilaSaida;     // bytes na fila de uma conexao; acima, ela e fechada
        ThreadPool* Pool;             // 0 = pool global
        const char* RecusaCheio;      // resposta crua para a conexao acima do limite (pode ser 0)
        int         MaxSincronoVolta; // conexoes que o proprio reator atende por volta (0 = 1); o resto vai ao pool
    }
    NetConfig;

    NetServidor* net_servidor_criar(const NetConfig* cfg, const NetProtocolo* proto, void* ctx_servidor);
    void         net_servidor_parar(NetServidor* s);    // fecha tudo e espera o reator sair
    int          net_servidor_porta(NetServidor* s);
    int          net_servidor_conexoes(NetServidor* s);

    // Diagnostico: quantas leituras foram feitas no modo sincrono (no proprio reator) e
    // quantas viraram tarefa no pool (modo paralelo, por demanda).
    void         net_servidor_contadores(NetServidor* s, long long* sincronas, long long* tarefas);

    // Diagnostico: quantas vezes o envio passou do orcamento do reator e foi para uma tarefa
    // de envio no pool (arquivo ou resposta grande).
    long long    net_servidor_escoamentos(NetServidor* s);

    // ---- conexao ---------------------------------------------------------------------
    void*       net_contexto(NetConexao* c);
    const char* net_remoto(NetConexao* c);     // "ip:porta" do cliente
    const char* net_local(NetConexao* c);      // "ip:porta" local
    int         net_id(NetConexao* c);          // numero do socket, so para log

    // Copia e envia (ou enfileira). Qualquer thread. false = conexao fechada ou a fila
    // passou de MaxFilaSaida (cliente que nao le: a conexao e fechada).
    bool  net_enviar(NetConexao* c, const void* dados, int n);

    // Envia um trecho de arquivo sem carrega-lo: vai para a fila e sai em blocos, conforme o
    // socket aceita. O arquivo e aberto aqui (false se nao abriu). tam < 0 = ate o fim.
    bool  net_enviar_arquivo(NetConexao* c, const char* caminho, int64 inicio, int64 tam);

    // Encerra. depois_de_enviar: escoa a fila, fecha o envio e drena o que o cliente ainda
    // mandar por uns segundos (fechar com dado nao lido vira RST e o cliente perde a resposta).
    void  net_fechar(NetConexao* c, bool depois_de_enviar);

    // Pausa: a tarefa atual devolve a conexao SEM rearmar a leitura. Para o protocolo que
    // entregou uma requisicao longa a outra thread e nao quer ler a proxima ainda.
    // net_retomar_leitura (qualquer thread) rearma.
    void  net_pausar_leitura(NetConexao* c);
    void  net_retomar_leitura(NetConexao* c);

    // Algo em andamento na conexao (requisicao longa, canal SSE aberto): ela nao e fechada
    // por ociosidade enquanto o contador for > 0.
    void  net_ocupar(NetConexao* c, int delta);

    // Referencia para quem guarda a conexao fora da tarefa (thread de rota longa, canal).
    void  net_segurar(NetConexao* c);
    void  net_soltar(NetConexao* c);

    // O protocolo passou a precisar do relogio (ex.: abriu um canal com heartbeat): o reator
    // chama AoTick na proxima volta e agenda pelo prazo devolvido. Qualquer thread.
    void  net_pedir_relogio(NetConexao* c);

    // Diagnostico: quantas vezes o reator acordou (parado e sem prazo, nao deve acordar).
    long long net_servidor_acordadas(NetServidor* s);

    // Roda fn no pool de tarefas do servidor (o mesmo das leituras em paralelo): para o
    // protocolo tirar trabalho do reator. Qualquer thread.
    bool  net_submeter(NetConexao* c, void (*fn)(void*), void* arg);

    // A thread atual e um reator? (o protocolo decide se pode bloquear ali)
    bool  net_no_reator(void);

    // Diagnostico: conexoes vivas em todos os servidores do processo.
    int   net_conexoes_total(void);

#ifdef __cplusplus
}
#endif

#endif  // APPSERVER_NET_SERVIDOR_H
