//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Implementacoes de net_poll.h. Ver as regras de uso no header.
//
//  A memoria interna vem do memory_pool do xplatbase (memop_*), como no resto do servidor.

#include "net_poll.h"
#include "atomics.h"
#include "memory_pool.h"
#include <stdlib.h>
#include <string.h>

#define NET_POLL_LOTE 256   // eventos lidos do sistema por espera

#if defined(__linux__)
// =========================================================================================
//  Linux: epoll
// =========================================================================================
#include <sys/epoll.h>
#include <sys/eventfd.h>

struct NetPoll
{
    int         Ep;
    int         Wake;        // eventfd
    xatomic_int Pendente;    // 1 = ja ha um acordar a caminho
};

// Marca o eventfd no epoll. Qualquer endereco que nunca seja um Dado valido serve.
static char g_sentinela_wake;

NetPoll* net_poll_criar(void)
{
    NetPoll* p = (NetPoll*)memop_calloc_raw(1, sizeof(NetPoll));
    if (!p) return 0;
    p->Ep   = epoll_create1(EPOLL_CLOEXEC);
    p->Wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (p->Ep < 0 || p->Wake < 0) { net_poll_destruir(p); return 0; }

    struct epoll_event e; memset(&e, 0, sizeof(e));
    e.events = EPOLLIN; e.data.ptr = &g_sentinela_wake;
    if (epoll_ctl(p->Ep, EPOLL_CTL_ADD, p->Wake, &e) != 0) { net_poll_destruir(p); return 0; }
    return p;
}

void net_poll_destruir(NetPoll* p)
{
    if (!p) return;
    if (p->Wake >= 0) close(p->Wake);
    if (p->Ep >= 0)   close(p->Ep);
    memop_free_raw(p);
}

bool net_poll_definir(NetPoll* p, SOCKET s, unsigned interesse, void* dado)
{
    // Desarmar e TIRAR do epoll: com interesse 0 o epoll ainda reporta EPOLLHUP/EPOLLERR,
    // e por nivel isso se repetiria sem parar enquanto a tarefa roda (reator a 100%).
    if (interesse == 0)
    {
        if (epoll_ctl(p->Ep, EPOLL_CTL_DEL, s, 0) != 0 && errno != ENOENT) return false;
        return true;
    }
    struct epoll_event e; memset(&e, 0, sizeof(e));
    if (interesse & NET_POLL_LER)      e.events |= EPOLLIN;
    if (interesse & NET_POLL_ESCREVER) e.events |= EPOLLOUT;
    e.data.ptr = dado;
    if (epoll_ctl(p->Ep, EPOLL_CTL_MOD, s, &e) == 0) return true;
    if (errno != ENOENT) return false;
    return epoll_ctl(p->Ep, EPOLL_CTL_ADD, s, &e) == 0;
}

void net_poll_remover(NetPoll* p, SOCKET s)
{
    epoll_ctl(p->Ep, EPOLL_CTL_DEL, s, 0);
}

int net_poll_esperar(NetPoll* p, NetPollEvento* ev, int max, int timeout_ms)
{
    struct epoll_event lote[NET_POLL_LOTE];
    if (max > NET_POLL_LOTE) max = NET_POLL_LOTE;
    if (max <= 0) return 0;

    int n = epoll_wait(p->Ep, lote, max, timeout_ms);
    if (n <= 0) return 0;   // tempo esgotado ou EINTR

    int k = 0;
    for (int i = 0; i < n; i++)
    {
        if (lote[i].data.ptr == &g_sentinela_wake)
        {
            // Drena e SO DEPOIS zera. Na ordem inversa (zerar, drenar) um acordar que caisse no
            // meio punha Pendente em 1 e mandava o sinal -- e o dreno o consumia: Pendente
            // ficava 1 sem sinal nenhum, todo acordar seguinte era suprimido e o reator so
            // andava pelo timeout (medido: p99 de 1 s e vazao de 100 req/s). Assim, quem
            // acordar durante o dreno ve Pendente = 1 e nao sinaliza, mas o comando dele ja
            // esta na fila e e processado nesta volta (o reator processa depois de esperar).
            uint64_t v; while (read(p->Wake, &v, sizeof(v)) > 0) {}
            atomic_set_inline(&p->Pendente, 0);
            continue;
        }
        unsigned f = 0;
        if (lote[i].events & EPOLLIN)                f |= NET_POLL_LER;
        if (lote[i].events & EPOLLOUT)               f |= NET_POLL_ESCREVER;
        if (lote[i].events & (EPOLLHUP | EPOLLERR))  f |= NET_POLL_FIM;
        ev[k].Dado = lote[i].data.ptr;
        ev[k].Eventos = f;
        k++;
    }
    return k;
}

void net_poll_acordar(NetPoll* p)
{
    int esperado = 0;
    if (!atomic_cas_inline(&p->Pendente, &esperado, 1)) return;
    uint64_t um = 1;
    ssize_t r = write(p->Wake, &um, sizeof(um));
    (void)r;
}

#else
// =========================================================================================
//  Windows (WSAPoll) e demais POSIX (poll): registro proprio em vetor
// =========================================================================================
//  WSAPoll/poll nao guardam estado: a cada espera o vetor de pollfd e montado com os
//  sockets ARMADOS. O custo e O(n) por espera -- com n limitado por MaxClients (centenas)
//  isso e desprezivel perto da chamada ao sistema. Se um dia passar de milhares de conexoes
//  no Windows, o caminho e IOCP; a interface acima nao muda.

#ifdef _WIN32
    typedef WSAPOLLFD NetPollFd;
    #define NET_SYS_POLL(v, n, t) WSAPoll((v), (ULONG)(n), (t))
    #define NET_EV_IN   POLLRDNORM     // WSAPoll recusa POLLPRI; POLLRDNORM e o "tem dado"
    #define NET_EV_OUT  POLLWRNORM
#else
    #include <poll.h>
    typedef struct pollfd NetPollFd;
    #define NET_SYS_POLL(v, n, t) poll((v), (nfds_t)(n), (t))
    #define NET_EV_IN   POLLIN
    #define NET_EV_OUT  POLLOUT
#endif

typedef struct
{
    SOCKET   S;
    unsigned Interesse;
    void*    Dado;
}
NetPollItem;

struct NetPoll
{
    NetPollItem* Itens;
    int          Qtd, Cap;
    NetPollFd*   Fds;          // montado a cada espera; Fds[0] e o de acordar
    int*         Mapa;         // Fds[i] (i >= 1) -> indice em Itens
    int          FdsCap;
    xatomic_int  Pendente;
#ifdef _WIN32
    SOCKET       Wake;         // UDP conectado a si mesmo
#else
    int          WakeLer, WakeEscrever;   // pipe
#endif
};

static bool wake_abrir(NetPoll* p)
{
#ifdef _WIN32
    // Winsock nao tem pipe nem eventfd que o WSAPoll aceite: o jeito portavel e um socket
    // UDP de loopback mandando para si mesmo.
    p->Wake = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (p->Wake == INVALID_SOCKET) return false;
    struct sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    int alen = sizeof(a);
    if (bind(p->Wake, (struct sockaddr*)&a, sizeof(a)) != 0) return false;
    if (getsockname(p->Wake, (struct sockaddr*)&a, &alen) != 0) return false;
    if (connect(p->Wake, (struct sockaddr*)&a, sizeof(a)) != 0) return false;
    return net_set_nonblocking(p->Wake, 1) != 0;
#else
    int fd[2];
    if (pipe(fd) != 0) { p->WakeLer = p->WakeEscrever = -1; return false; }
    p->WakeLer = fd[0]; p->WakeEscrever = fd[1];
    fcntl(p->WakeLer, F_SETFL, fcntl(p->WakeLer, F_GETFL, 0) | O_NONBLOCK);
    fcntl(p->WakeEscrever, F_SETFL, fcntl(p->WakeEscrever, F_GETFL, 0) | O_NONBLOCK);
    return true;
#endif
}

static void wake_drenar(NetPoll* p)
{
    char b[64];
#ifdef _WIN32
    while (recv(p->Wake, b, (int)sizeof(b), 0) > 0) {}
#else
    while (read(p->WakeLer, b, sizeof(b)) > 0) {}
#endif
}

NetPoll* net_poll_criar(void)
{
    NetPoll* p = (NetPoll*)memop_calloc_raw(1, sizeof(NetPoll));
    if (!p) return 0;
#ifdef _WIN32
    p->Wake = INVALID_SOCKET;
#endif
    if (!wake_abrir(p)) { net_poll_destruir(p); return 0; }
    return p;
}

void net_poll_destruir(NetPoll* p)
{
    if (!p) return;
#ifdef _WIN32
    if (p->Wake != INVALID_SOCKET) closesocket(p->Wake);
#else
    if (p->WakeLer >= 0)      close(p->WakeLer);
    if (p->WakeEscrever >= 0) close(p->WakeEscrever);
#endif
    memop_free_raw(p->Itens); memop_free_raw(p->Fds); memop_free_raw(p->Mapa);
    memop_free_raw(p);
}

static int item_indice(NetPoll* p, SOCKET s)
{
    for (int i = 0; i < p->Qtd; i++) if (p->Itens[i].S == s) return i;
    return -1;
}

bool net_poll_definir(NetPoll* p, SOCKET s, unsigned interesse, void* dado)
{
    int i = item_indice(p, s);
    if (i < 0)
    {
        if (p->Qtd == p->Cap)
        {
            int cap = p->Cap ? p->Cap * 2 : 64;
            NetPollItem* n = (NetPollItem*)memop_realloc_raw(p->Itens, (size_t)cap * sizeof(NetPollItem));
            if (!n) return false;
            p->Itens = n; p->Cap = cap;
        }
        i = p->Qtd++;
        p->Itens[i].S = s;
    }
    p->Itens[i].Interesse = interesse;
    p->Itens[i].Dado = dado;
    return true;
}

void net_poll_remover(NetPoll* p, SOCKET s)
{
    int i = item_indice(p, s);
    if (i < 0) return;
    p->Itens[i] = p->Itens[--p->Qtd];   // ordem nao importa
}

int net_poll_esperar(NetPoll* p, NetPollEvento* ev, int max, int timeout_ms)
{
    if (p->FdsCap < p->Qtd + 1)
    {
        int cap = p->Qtd + 1 + 64;
        NetPollFd* f = (NetPollFd*)memop_realloc_raw(p->Fds, (size_t)cap * sizeof(NetPollFd));
        if (!f) return 0;
        p->Fds = f;
        int* m = (int*)memop_realloc_raw(p->Mapa, (size_t)cap * sizeof(int));
        if (!m) return 0;
        p->Mapa = m;
        p->FdsCap = cap;
    }

    int n = 0;
#ifdef _WIN32
    p->Fds[n].fd = p->Wake;
#else
    p->Fds[n].fd = p->WakeLer;
#endif
    p->Fds[n].events = NET_EV_IN; p->Fds[n].revents = 0; n++;

    for (int i = 0; i < p->Qtd; i++)
    {
        unsigned in = p->Itens[i].Interesse;
        if (!in) continue;   // desarmado: nem entra na espera
        p->Fds[n].fd = p->Itens[i].S;
        p->Fds[n].events = (short)(((in & NET_POLL_LER) ? NET_EV_IN : 0) | ((in & NET_POLL_ESCREVER) ? NET_EV_OUT : 0));
        p->Fds[n].revents = 0;
        p->Mapa[n] = i;
        n++;
    }

    int r = NET_SYS_POLL(p->Fds, n, timeout_ms);
    if (r <= 0) return 0;

    if (p->Fds[0].revents)
    {
        wake_drenar(p);
        atomic_set_inline(&p->Pendente, 0);   // DEPOIS de drenar; ver a versao epoll
    }

    int k = 0;
    for (int j = 1; j < n && k < max; j++)
    {
        short re = p->Fds[j].revents;
        if (!re) continue;
        unsigned f = 0;
        if (re & NET_EV_IN)                          f |= NET_POLL_LER;
        if (re & NET_EV_OUT)                         f |= NET_POLL_ESCREVER;
        if (re & (POLLHUP | POLLERR | POLLNVAL))     f |= NET_POLL_FIM;
        ev[k].Dado = p->Itens[p->Mapa[j]].Dado;
        ev[k].Eventos = f;
        k++;
    }
    // Os que nao couberam em max continuam prontos: por nivel, voltam na proxima espera.
    return k;
}

void net_poll_acordar(NetPoll* p)
{
    int esperado = 0;
    if (!atomic_cas_inline(&p->Pendente, &esperado, 1)) return;
    char um = 1;
#ifdef _WIN32
    send(p->Wake, &um, 1, 0);
#else
    ssize_t r = write(p->WakeEscrever, &um, 1);
    (void)r;
#endif
}

#endif
