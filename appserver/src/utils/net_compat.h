//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Sockets portaveis entre Windows (Winsock) e Linux (BSD sockets). CAMADA 0: so net/ e os
//  testes de rede usam isto diretamente; as outras camadas falam com net_servidor.h.
//
//  O servidor foi escrito sobre Winsock. Em vez de espalhar #ifdef em cada chamada, os
//  nomes do Winsock que ele usa ganham equivalente POSIX aqui -- sao mapeamentos de um para
//  um (SOCKET e um int, closesocket e close, WSAGetLastError e errno). No Windows este
//  header so inclui o Winsock, entao o build MSVC nao muda.

#ifndef APPSERVER_NET_COMPAT_H
#define APPSERVER_NET_COMPAT_H

#ifdef _WIN32

    // Exatamente o include que appserver.c/appclient.c tinham: com _WINSOCKAPI_ ja definido
    // (winsock ja incluido por outro header) nao se inclui de novo -- trocar a ordem aqui
    // arriscaria o conflito winsock 1 x winsock 2 num build que hoje funciona.
    #ifndef _WINSOCKAPI_
    #define _WINSOCKAPI_
    #include <ws2tcpip.h>
    #endif
    #pragma comment(lib, "ws2_32.lib")

#else

    #include <sys/types.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <errno.h>
    #include <stdint.h>
    #include <stdlib.h>
    #include <pthread.h>
    #include <fcntl.h>

    typedef int SOCKET;
    #define INVALID_SOCKET  (-1)
    #define SOCKET_ERROR    (-1)
    #define closesocket     close
    #define WSAGetLastError() (errno)
    #define WSAEINTR        EINTR
    #define InetNtop        inet_ntop
    #define SD_BOTH         SHUT_RDWR
    #define SD_SEND         SHUT_WR

    // Winsock precisa de WSAStartup/WSACleanup; os BSD sockets nao.
    typedef struct { int unused; } WSADATA;
    #define MAKEWORD(a, b)          ((unsigned short)(((unsigned char)(a)) | (((unsigned short)(unsigned char)(b)) << 8)))
    #define WSAStartup(ver, data)   ((void)(ver), (void)(data), 0)
    #define WSACleanup()            ((void)0)

#endif

// O servidor guarda o descritor num void* (Handle). Estas conversoes tornam isso explicito
// e sem aviso de truncamento nos dois sistemas.
#define NET_HANDLE_TO_SOCKET(h)  ((SOCKET)(uintptr_t)(h))
#define NET_SOCKET_TO_HANDLE(s)  ((void*)(uintptr_t)(s))

// Tempo maximo que um recv() BLOQUEANTE espera (clientes de teste; o servidor nao bloqueia).
static inline void net_set_recv_timeout(SOCKET s, int ms)
{
#ifdef _WIN32
    DWORD t = (DWORD)ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof(t));
#else
    struct timeval tv; tv.tv_sec = ms / 1000; tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

// ---- modo nao bloqueante (reator) --------------------------------------------------------
// O reator nunca pode ficar parado num recv/send/accept: o socket vira nao bloqueante e
// "nao ha dados agora" volta como erro de would-block, que nao e erro de conexao.
static inline int net_set_nonblocking(SOCKET s, int on)
{
#ifdef _WIN32
    u_long v = on ? 1 : 0;
    return ioctlsocket(s, FIONBIO, &v) == 0;
#else
    int fl = fcntl(s, F_GETFL, 0);
    if (fl < 0) return 0;
    fl = on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK);
    return fcntl(s, F_SETFL, fl) == 0;
#endif
}

// O ultimo recv/send/accept falhou so porque nao havia nada a fazer AGORA?
static inline int net_last_error_would_block(void)
{
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

// Porta de escuta reutilizavel logo apos reiniciar. No Linux, sem isto, o bind falha
// (EADDRINUSE) por ~60 s enquanto as conexoes da execucao anterior estao em TIME_WAIT.
// No Windows NAO se liga: la SO_REUSEADDR deixa outro processo roubar a porta em uso, e o
// bind ja ignora TIME_WAIT por padrao.
static inline void net_set_listen_reuse(SOCKET s)
{
#ifdef _WIN32
    (void)s;
#else
    int v = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &v, sizeof(v));
#endif
}

// Sem Nagle: respostas pequenas (cabecalho de SSE, evento curto) saem na hora.
static inline void net_set_nodelay(SOCKET s)
{
    int v = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&v, sizeof(v));
}

// send() que nao gera SIGPIPE no Linux quando o outro lado ja fechou (no Windows nao existe
// o sinal). Sem isto, escrever num cliente que caiu mata o PROCESSO.
#ifdef _WIN32
    #define NET_SEND_FLAGS 0
#else
    #ifdef MSG_NOSIGNAL
        #define NET_SEND_FLAGS MSG_NOSIGNAL
    #else
        #define NET_SEND_FLAGS 0
    #endif
#endif

#endif  // APPSERVER_NET_COMPAT_H
