//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Sockets e thread de accept, portaveis entre Windows (Winsock) e Linux (BSD sockets).
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
    #include <process.h>     // _beginthread
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

    typedef int SOCKET;
    #define INVALID_SOCKET  (-1)
    #define SOCKET_ERROR    (-1)
    #define closesocket     close
    #define WSAGetLastError() (errno)
    #define WSAEINTR        EINTR
    #define InetNtop        inet_ntop
    #define SD_BOTH         SHUT_RDWR

    // Winsock precisa de WSAStartup/WSACleanup; os BSD sockets nao.
    typedef struct { int unused; } WSADATA;
    #define MAKEWORD(a, b)          ((unsigned short)(((unsigned char)(a)) | (((unsigned short)(unsigned char)(b)) << 8)))
    #define WSAStartup(ver, data)   ((void)(ver), (void)(data), 0)
    #define WSACleanup()            ((void)0)

    // _beginthread(fn, pilha, arg): thread "solta" que termina sozinha. Aqui, pthread
    // desanexada. O trampolim existe porque a assinatura da pthread devolve void*.
    typedef struct { void (*fn)(void*); void* arg; } NetThreadStart;

    static void* net_thread_trampoline(void* p)
    {
        NetThreadStart s = *(NetThreadStart*)p;
        free(p);
        s.fn(s.arg);
        return NULL;
    }

    static inline uintptr_t _beginthread(void (*fn)(void*), unsigned stack_size, void* arg)
    {
        (void)stack_size;
        NetThreadStart* s = (NetThreadStart*)malloc(sizeof(NetThreadStart));
        if (!s) return (uintptr_t)-1;
        s->fn = fn; s->arg = arg;
        pthread_t th;
        if (pthread_create(&th, NULL, net_thread_trampoline, s) != 0) { free(s); return (uintptr_t)-1; }
        pthread_detach(th);
        return (uintptr_t)th;
    }

#endif

// O servidor guarda o descritor num void* (Handle). Estas conversoes tornam isso explicito
// e sem aviso de truncamento nos dois sistemas.
#define NET_HANDLE_TO_SOCKET(h)  ((SOCKET)(uintptr_t)(h))
#define NET_SOCKET_TO_HANDLE(s)  ((void*)(uintptr_t)(s))

#endif  // APPSERVER_NET_COMPAT_H
