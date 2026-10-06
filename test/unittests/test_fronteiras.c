//  Fronteiras entre as camadas do servidor, verificadas no CODIGO-FONTE.
//
//  As camadas so valem se ninguem as atravessar "so desta vez". Este teste le os fontes e
//  falha, com arquivo e linha, quando uma regra e quebrada:
//
//    L0/L1 (appserver/src/net)   : unica que fala com sockets
//    L2    (appserver/src/http)  : HTTP; nao conhece rotas, topicos nem jobs
//    L3    (appserver/src/*.c)   : aplicacao do servidor; nao mexe em socket
//    L4    (test/appservertester): controllers; so appserver.h
//
//  Regras:
//    R1  API de socket (send/recv/accept/closesocket/shutdown/poll/epoll/select) so em net/
//    R2  linha de status HTTP ("HTTP/1.1 ...") so no formatador (http/http_resposta.c)
//    R3  controllers nao incluem internos (src/net, src/http, net_compat) nem usam o socket
//    R4  net/ nao conhece HTTP (nem http/, nem appserver.h, nem "HTTP/1")
//    R5  http/ nao chama a aplicacao (topicos, jobs, rotas)

#include "testes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <dirent.h>
#endif

static char g_raiz[1024];

// Raiz do repositorio a partir deste arquivo: <raiz>/test/unittests/test_fronteiras.c
static int acha_raiz(void)
{
    snprintf(g_raiz, sizeof(g_raiz), "%s", __FILE__);
    for (char* p = g_raiz; *p; p++) if (*p == '\\') *p = '/';
    for (int k = 0; k < 3; k++)
    {
        char* b = strrchr(g_raiz, '/');
        if (!b) return 0;
        *b = 0;
    }
    return 1;
}

typedef struct { char Arq[256]; int Linha; char Trecho[120]; const char* Regra; } Violacao;
static Violacao g_v[64];
static int g_nv;

static void viola(const char* regra, const char* arq, int linha, const char* l)
{
    if (g_nv >= 64) return;
    Violacao* v = &g_v[g_nv++];
    v->Regra = regra; v->Linha = linha;
    snprintf(v->Arq, sizeof(v->Arq), "%s", arq + strlen(g_raiz) + 1);
    int n = 0; while (*l == ' ' || *l == '\t') l++;
    while (l[n] && l[n] != '\n' && l[n] != '\r' && n < 110) { v->Trecho[n] = l[n]; n++; }
    v->Trecho[n] = 0;
}

// 'tok(' como chamada: antes dele nao pode vir letra, digito, '_' nem '.' / '>' (membro).
static int chama(const char* l, const char* tok)
{
    size_t n = strlen(tok);
    for (const char* p = strstr(l, tok); p; p = strstr(p + 1, tok))
    {
        char a = p == l ? ' ' : p[-1];
        if (isalnum((unsigned char)a) || a == '_' || a == '.' || a == '>') continue;
        if (p[n] == '(') return 1;
        const char* q = p + n; while (*q == ' ') q++;
        if (*q == '(') return 1;
    }
    return 0;
}

// Linha de comentario (so o comeco importa: o codigo do projeto comenta com // no inicio).
static int comentario(const char* l)
{
    while (*l == ' ' || *l == '\t') l++;
    return (l[0] == '/' && (l[1] == '/' || l[1] == '*')) || l[0] == '*';
}

typedef void (*VerificaLinha)(const char* arq, int linha, const char* l);

static void varre_arquivo(const char* arq, VerificaLinha fn)
{
    FILE* f = fopen(arq, "rb");
    if (!f) return;
    char l[4096]; int n = 0;
    while (fgets(l, sizeof(l), f)) { n++; if (!comentario(l)) fn(arq, n, l); }
    fclose(f);
}

static int termina(const char* s, const char* suf) { size_t a = strlen(s), b = strlen(suf); return a >= b && !strcmp(s + a - b, suf); }

static void varre_pasta(const char* rel, VerificaLinha fn)
{
    char dir[1300]; snprintf(dir, sizeof(dir), "%s/%s", g_raiz, rel);
#ifdef _WIN32
    char pad[1400]; snprintf(pad, sizeof(pad), "%s/*", dir);
    WIN32_FIND_DATAA d; HANDLE h = FindFirstFileA(pad, &d);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!termina(d.cFileName, ".c") && !termina(d.cFileName, ".h")) continue;
        char a[1700]; snprintf(a, sizeof(a), "%s/%s", dir, d.cFileName); varre_arquivo(a, fn);
    } while (FindNextFileA(h, &d));
    FindClose(h);
#else
    DIR* d = opendir(dir); if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != 0)
    {
        if (!termina(e->d_name, ".c") && !termina(e->d_name, ".h")) continue;
        char a[1700]; snprintf(a, sizeof(a), "%s/%s", dir, e->d_name); varre_arquivo(a, fn);
    }
    closedir(d);
#endif
}

static const char* SOCKET_API[] = { "send", "recv", "accept", "closesocket", "shutdown", "WSAPoll", "select", "epoll_wait", "epoll_ctl", 0 };

static void r1(const char* arq, int n, const char* l)
{
    for (int i = 0; SOCKET_API[i]; i++) if (chama(l, SOCKET_API[i])) { viola("R1 socket fora de net/", arq, n, l); return; }
}

static void r2(const char* arq, int n, const char* l)
{
    if (termina(arq, "http/http_resposta.c")) return;
    if (strstr(l, "\"HTTP/1.1 ")) viola("R2 resposta HTTP fora do formatador", arq, n, l);
}

static void r3(const char* arq, int n, const char* l)
{
    if (!strncmp(l, "#include", 8) && (strstr(l, "src/net") || strstr(l, "src/http") || strstr(l, "net_compat")))
        viola("R3 controller inclui interno do servidor", arq, n, l);
    if (strstr(l, "Client->Handle") || strstr(l, "Client->Net")) viola("R3 controller mexe na conexao", arq, n, l);
    r1(arq, n, l);
    r2(arq, n, l);
}

static void r4(const char* arq, int n, const char* l)
{
    if (!strncmp(l, "#include", 8) && (strstr(l, "http/") || strstr(l, "appserver.h") || strstr(l, "http_")))
        viola("R4 net/ inclui HTTP ou aplicacao", arq, n, l);
    if (strstr(l, "\"HTTP/1")) viola("R4 net/ fala HTTP", arq, n, l);
}

static void r5(const char* arq, int n, const char* l)
{
    static const char* APP[] = { "app_publicar", "app_assinar", "app_job_", "app_topico", "binder_", "appserver_received", 0 };
    for (int i = 0; APP[i]; i++)
        if (strstr(l, APP[i])) { viola("R5 http/ chama a aplicacao", arq, n, l); return; }
    if (!strncmp(l, "#include", 8) && strstr(l, "app_topico")) viola("R5 http/ inclui a aplicacao", arq, n, l);
    r1(arq, n, l);
}

static void l3(const char* arq, int n, const char* l) { r1(arq, n, l); r2(arq, n, l); }

void teste_fronteiras_entre_camadas(TestResult* r)
{
    t_start(r);
    T_ASSERT(r, acha_raiz(), "nao achou a raiz do repositorio a partir de %s", __FILE__);
    char marca[1200]; snprintf(marca, sizeof(marca), "%s/appserver/src/net/net_servidor.c", g_raiz);
    FILE* f = fopen(marca, "rb");
    if (!f) T_SKIP(r, "fontes nao acessiveis daqui (%s)", marca);
    fclose(f);

    g_nv = 0;
    varre_pasta("appserver/src", l3);            // L3: nada de socket, nada de HTTP a mao
    varre_pasta("appserver/src/utils", l3);
    varre_pasta("appserver/src/http", r5);       // L2
    varre_pasta("appserver/src/net", r4);        // L1 (socket permitido)
    varre_pasta("test/appservertester", r3);     // L4

    if (g_nv > 0)
    {
        char m[480]; int k = 0;
        for (int i = 0; i < g_nv && k < (int)sizeof(m) - 120; i++)
            k += snprintf(m + k, sizeof(m) - k, "%s%s:%d [%s] %s", i ? " | " : "", g_v[i].Arq, g_v[i].Linha, g_v[i].Regra, g_v[i].Trecho);
        T_ASSERT(r, 0, "%d violacao(oes): %s", g_nv, m);
    }
}
