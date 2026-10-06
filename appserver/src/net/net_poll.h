//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  CAMADA 0 (plataforma): espera por prontidao de MUITOS sockets numa thread so.
//
//  E a unica peca do servidor que conhece epoll/WSAPoll. Acima dela (net_servidor, o
//  reator) nada sabe de sistema operacional:
//
//      Linux    epoll + eventfd para acordar
//      Windows  WSAPoll + socket UDP de loopback para acordar
//      outros   poll() + pipe para acordar
//
//  Regras de uso:
//    - net_poll_definir / net_poll_remover / net_poll_esperar: SO a thread do reator.
//      Quem esta em outra thread (uma tarefa do pool) pede a mudanca ao reator por uma
//      fila e chama net_poll_acordar -- essa sim pode ser chamada de qualquer thread.
//    - Interesse 0 = DESARMADO: o socket continua registrado, mas nada e reportado, nem
//      fechamento nem erro. E o que garante "uma tarefa por conexao por vez": o reator
//      desarma antes de entregar a conexao ao pool e so rearma quando a tarefa devolve.
//    - Por nivel (level-triggered): enquanto houver dado nao lido, o evento se repete.

#ifndef APPSERVER_NET_POLL_H
#define APPSERVER_NET_POLL_H

#include "../utils/net_compat.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

    // interesse (entrada)
    #define NET_POLL_LER       1u
    #define NET_POLL_ESCREVER  2u

    // eventos (saida): os dois acima e mais
    #define NET_POLL_FIM       4u   // o outro lado fechou, ou erro no socket

    typedef struct NetPoll NetPoll;

    typedef struct
    {
        void*    Dado;      // o que foi passado em net_poll_definir
        unsigned Eventos;   // NET_POLL_LER | NET_POLL_ESCREVER | NET_POLL_FIM
    }
    NetPollEvento;

    NetPoll* net_poll_criar(void);
    void     net_poll_destruir(NetPoll* p);

    // Registra o socket ou troca o interesse dele (acrescenta se ainda nao estava).
    bool     net_poll_definir(NetPoll* p, SOCKET s, unsigned interesse, void* dado);
    void     net_poll_remover(NetPoll* p, SOCKET s);

    // Espera ate timeout_ms (-1 = sem limite). Devolve quantos eventos escreveu em ev
    // (0 = acordado por net_poll_acordar ou tempo esgotado; nunca negativo).
    int      net_poll_esperar(NetPoll* p, NetPollEvento* ev, int max, int timeout_ms);

    // Faz um net_poll_esperar em andamento (ou o proximo) voltar ja. Qualquer thread.
    // Varias chamadas seguidas viram um acordar so.
    void     net_poll_acordar(NetPoll* p);

#ifdef __cplusplus
}
#endif

#endif  // APPSERVER_NET_POLL_H
