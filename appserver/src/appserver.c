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


#include "../include/appserver.h"
#include "http/http_conexao.h"
#include "net/net_servidor.h"
#include "event_server.h"
#include "utils/callback_binder.h"
#include "utils/message_assembler.h"
#include "http/http_resposta.h"
#include "http/http_parser.h"
#include "utils/activity_binder.h"
#include "utils/health_monitor.h"   // painel de saude embutido
#include "yason.h"
#include "utils/websocket_util.h"

#include "utils/net_compat.h"   // Winsock no Windows, BSD sockets no Linux

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>



#ifdef _WIN32
  #define WSAEMFILE_COMPAT WSAEMFILE
  static void thread_sleep_ms_compat(int ms) { Sleep((DWORD)ms); }
#else
  #include <time.h>
  #define WSAEMFILE_COMPAT EMFILE
  static void thread_sleep_ms_compat(int ms) { struct timespec t = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&t, 0); }
#endif

#ifndef _WIN32
  #include <sys/stat.h>
#endif
int ServerInitialized = false;
MessageField HTTP_HEADER_ALLOW_HEADERS;
MessageField HTTP_HEADER_ALLOW_METHODS;
AppServerList Servers;



// Definido em health_controller.c. Declarado aqui para nao criar um header so por
// causa de um simbolo interno da biblioteca.
Element* appserver_health_route(Message* request);
int  appserver_rota_modo(Message* request);
void appserver_received(Message* request);

#ifdef _WIN32
static long long agora_us_srv(void)
{
    static LARGE_INTEGER f; LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (long long)(c.QuadPart / f.QuadPart) * 1000000LL + (long long)((c.QuadPart % f.QuadPart) * 1000000LL / f.QuadPart);
}
#else
static long long agora_us_srv(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000000LL + t.tv_nsec / 1000;
}
#endif

// Aprende rota curta que demora. Acima de RotaLentaUs soma, abaixo subtrai (0..8); com 3 a
// rota passa a rodar fora do reator, e volta quando o contador zera. Corrida entre threads
// aqui so erra a contagem por um, nao corrompe nada.
static void aprende_rota(AppServerInfo* s, FunctionBind* b, long long dur_us)
{
    if (!b || b->Longa) return;
    int lim = s->Config.RotaLentaUs > 0 ? s->Config.RotaLentaUs : 1000;
    int v = atomic_get_inline(&b->Lentas);
    if (dur_us > lim) { if (v < 8) atomic_set_inline(&b->Lentas, v + 1); if (v + 1 >= 3) b->Lenta = true; }
    else if (v > 0) { atomic_set_inline(&b->Lentas, v - 1); if (v - 1 == 0) b->Lenta = false; }
}


// Libera uma arvore devolvida por handler (o yason nao exporta essa funcao).
void appserver_element_free(Element* e)
{
    if (!e) return;
    for (int i = 0; i < e->Children.Count; i++) appserver_element_free(e->Children.Items[i]);
    memop_free_raw(e->Children.Items);
    string_release(&e->Name);
    string_release(&e->Value);
    string_release(&e->Comment);
    memop_free_raw(e);
}

static void send_response_status(Message* request, HttpStatusCode status, const char* msg)
{
    int msgz = msg ? (int)strlen(msg) : 0;
    ResourceBuffer rb = { .Data = (byte*)msg, .Length = msgz, .Type = TEXT_PLAIN };
    appserver_http_response_send(request->Client->Server, request, status, &rb, 0, 0);
}

void send_response_server_error(Message* request, const char* msg)
{
    // Era sizeof(msg): o TAMANHO DO PONTEIRO, 8 em x64. Todo erro 500 saia com
    // Content-Length 8 e o texto cortado, o que escondia qual das causas tinha sido.
    int msgz = msg ? (int)strlen(msg) : 0;
    ResourceBuffer rb = { .Data = (byte*)msg, .Length = msgz, .Type = TEXT_PLAIN };
    appserver_http_response_send(request->Client->Server, request, HTTP_STATUS_INTERNAL_ERROR, &rb, 0, 0);
}


void _ReportRequest(Message* request)
{
    const char* a = message_command_titule(request->Cmd);
    const char* tp = request->ContentType != CONTENT_TYPE_NONE ? http_tipo_texto(request->ContentType) : "<nenhum>";

    StringX b;
    string_init(&b);
    if (request->Route.Count > 0)
    {
        int CNT = request->Route.Count - 1;
        int ix = 0;
        while (ix < CNT)
        {
            string_append_s(&b, request->Route.Items[ix]);
            string_append_char(&b, '/');
            ix++;
        }
        string_append_s(&b, request->Route.Items[ix]);
    }

    printf("REQUEST  | Client: %d | Method: %s | Route: '%s' | Type: %s | Content Length: %lld\n", (int)(intptr_t)request->Client->Handle, a, b.Content, tp, (long long)request->ContentLength);
    string_release_data(&b);
}

void ReportRequest(Message* request)
{
    if (request->Client && request->Client->Server && request->Client->Server->Config.LogRequests)
        _ReportRequest(request);
}


int WsaInit()
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        printf("Erro na inicializa��o do Winsock. C�digo: %d\n", WSAGetLastError());
        return 1;
    }
    return 0;
}




void appserver_create_default_headers()
{
    memset(&HTTP_HEADER_ALLOW_HEADERS, 0, sizeof(MessageField));
    memset(&HTTP_HEADER_ALLOW_METHODS, 0, sizeof(MessageField));

    string_init(&HTTP_HEADER_ALLOW_HEADERS.Name);
    string_init(&HTTP_HEADER_ALLOW_HEADERS.Param.Value);
    string_appends(&HTTP_HEADER_ALLOW_HEADERS.Name, "Access-Control-Allow-Headers", (int)strlen("Access-Control-Allow-Headers"), 0, (int)strlen("Access-Control-Allow-Headers"));
    string_appends(&HTTP_HEADER_ALLOW_HEADERS.Param.Value, "Content-Type, Authorization", (int)strlen("Content-Type, Authorization"), 0, (int)strlen("Content-Type, Authorization"));

    string_init(&HTTP_HEADER_ALLOW_METHODS.Name);
    string_init(&HTTP_HEADER_ALLOW_METHODS.Param.Value);
    string_appends(&HTTP_HEADER_ALLOW_METHODS.Name, "Access-Control-Allow-Methods", (int)strlen("Access-Control-Allow-Methods"), 0, (int)strlen("Access-Control-Allow-Methods"));
    string_appends(&HTTP_HEADER_ALLOW_METHODS.Param.Value, "GET, POST, OPTIONS", (int)strlen("GET, POST, OPTIONS"), 0, (int)strlen("GET, POST, OPTIONS"));
}


void appserver_init()
{
    if (ServerInitialized) return;

    // O servidor depende do xplatbase inicializado: pool de tarefas (reator), ganchos de
    // thread do memory_pool. Nao da para contar com a auto-init: no Windows ela e um ponteiro
    // na secao .CRT$XCU que o linker DESCARTA no Release (/OPT:REF), e o processo subia sem
    // pool nenhum. platform_init e idempotente: quem ja chamou (appservertester) nao sente.
    platform_init();

    WsaInit();
    appserver_create_default_headers();
    serverinfo_list_init(&Servers);

    ServerInitialized = true;
}


void appserver_shutdown()
{
    if (!ServerInitialized) return;

    health_monitor_shutdown();   // no-op se nunca foi iniciado; fecha a consulta do PDH


    //...


    ServerInitialized = false;
}


void appserver_http_response_send(AppServerInfo* server, Message* request, HttpStatusCode http_status, ResourceBuffer* object, HeaderAppender header_appender, void* appender_args)
{
    ResourceBuffer http;
    resource_buffer_init(&http);

    HttpCabecalho c = http_cabecalho(http_status, server->Config.AgentName);
    c.Tipo      = object ? object->Type : CONTENT_TYPE_NONE;
    c.Extra     = header_appender;
    c.ExtraArgs = appender_args;
    int n = (object && object->Data) ? object->Length : 0;
    // HEAD: mesmos cabecalhos (inclusive o Content-Length do recurso), sem o corpo.
    http_resposta_montar(&http, c, object ? object->Data : 0, n, request->Cmd != CMD_HEAD);

    if (server->Config.LogRequests)
        printf("RESPONSE | Client: %d | Status: %s | Type: %s | Content Length: %d\n", (int)(intptr_t)request->Client->Handle,
               http_status_texto(http_status), n ? http_tipo_texto(c.Tipo) : "", n);

    appclient_send(request->Client, http.Data, http.Length, false);

    // O buffer montado leva cabecalho + COPIA do corpo inteiro. Nunca era liberado: cada
    // resposta vazava o proprio tamanho -- um hls.min.js de 618 KB vazava 618 KB por pedido.
    resource_buffer_release(&http, true);
}

// Cabecalhos CORS da resposta ao OPTIONS (preflight). O codigo antigo passava o ARRAY de
// campos onde message_assembler_prepare espera uma FUNCAO (header_appender): o preflight de
// qualquer cliente de outra origem fazia o servidor saltar para dentro de dados.
static void append_cors_defaults(void* args, ResourceBuffer* http)
{
    (void)args;
    resource_buffer_append_format(http, "%s: %s\r\n", HTTP_HEADER_ALLOW_METHODS.Name.Content, HTTP_HEADER_ALLOW_METHODS.Param.Value.Content);
    resource_buffer_append_format(http, "%s: %s\r\n", HTTP_HEADER_ALLOW_HEADERS.Name.Content, HTTP_HEADER_ALLOW_HEADERS.Param.Value.Content);
}

void appserver_http_default_options(AppServerInfo* server, Message* request)
{
    appserver_http_response_send(server, request, HTTP_STATUS_OK, 0, append_cors_defaults, 0);
}

// Ate este tamanho o arquivo e lido para a memoria e enviado de uma vez (a resposta
// costuma sair direto, sem passar pelo reator). Acima, vai em streaming: blocos de 256 KB
// pela fila da conexao, e a memoria nao cresce com o tamanho do arquivo nem com o numero
// de downloads simultaneos.
#define WEB_EM_MEMORIA_MAX (256 * 1024)

// Arquivo estatico: tamanho e data de modificacao (os validadores do cache e do If-Range), numa
// chamada so e sem abrir o arquivo. Pasta ou outra coisa que nao seja arquivo comum: false.
typedef struct { int64 Tam; int64 Mod; } InfoArquivo;   // Mod = segundos desde 1970 (UTC)

static bool info_arquivo(const char* caminho, InfoArquivo* o)
{
#ifdef _WIN32
    struct _stat64 st;
    if (_stat64(caminho, &st) != 0 || (st.st_mode & _S_IFMT) != _S_IFREG) return false;
#else
    struct stat st;
    if (stat(caminho, &st) != 0 || !S_ISREG(st.st_mode)) return false;
#endif
    o->Tam = (int64)st.st_size;
    o->Mod = (int64)st.st_mtime;
    return true;
}

// Valor de um cabecalho do pedido (nome sem diferenciar maiusculas); 0 = ausente.
static const char* campo_pedido(Message* m, const char* nome, int* n)
{
    int ln = (int)strlen(nome);
    for (int i = 0; i < m->Fields.Count; i++)
    {
        MessageField* f = (MessageField*)m->Fields.Items[i];
        if ((int)f->Name.Length != ln || f->Raw.Length <= 0) continue;
        int k = 0;
        while (k < ln && tolower((unsigned char)f->Name.Content[k]) == tolower((unsigned char)nome[k])) k++;
        if (k == ln) { *n = (int)f->Raw.Length; return f->Raw.Content; }
    }
    return 0;
}

// Data HTTP (IMF-fixdate, RFC 7231): "Sun, 06 Nov 1994 08:49:37 GMT".
static const char* const DIAS_HTTP  = "SunMonTueWedThuFriSat";
static const char* const MESES_HTTP = "JanFebMarAprMayJunJulAugSepOctNovDec";

static void data_http(int64 t, char* out, int cap)
{
    time_t tt = (time_t)t; struct tm g;
#ifdef _WIN32
    if (gmtime_s(&g, &tt) != 0) { out[0] = 0; return; }
#else
    if (!gmtime_r(&tt, &g)) { out[0] = 0; return; }
#endif
    snprintf(out, (size_t)cap, "%.3s, %02d %.3s %04d %02d:%02d:%02d GMT", DIAS_HTTP + 3 * g.tm_wday, g.tm_mday,
             MESES_HTTP + 3 * g.tm_mon, g.tm_year + 1900, g.tm_hour, g.tm_min, g.tm_sec);
}

static int le_num(const char* v, int n, int* i, int digitos)
{
    int x = 0, k = 0;
    while (*i < n && k < digitos && v[*i] >= '0' && v[*i] <= '9') { x = x * 10 + (v[(*i)++] - '0'); k++; }
    return k == digitos ? x : -1;
}

// Le uma data HTTP no formato que o proprio servidor manda (IMF-fixdate), que e o que o
// navegador devolve em If-Modified-Since / If-Range. Outro formato: false (condicao ignorada).
static bool le_data_http(const char* v, int n, int64* t)
{
    if (n < 29 || v[3] != ',' || v[4] != ' ') return false;
    int i = 5, d, y, H, M, S, mes = -1;
    if ((d = le_num(v, n, &i, 2)) < 0 || v[i++] != ' ') return false;
    for (int k = 0; k < 12; k++) if (!strncmp(v + i, MESES_HTTP + 3 * k, 3)) { mes = k; break; }
    if (mes < 0) return false;
    i += 3; if (v[i++] != ' ') return false;
    if ((y = le_num(v, n, &i, 4)) < 0 || v[i++] != ' ') return false;
    if ((H = le_num(v, n, &i, 2)) < 0 || v[i++] != ':') return false;
    if ((M = le_num(v, n, &i, 2)) < 0 || v[i++] != ':') return false;
    if ((S = le_num(v, n, &i, 2)) < 0 || i + 4 > n || strncmp(v + i, " GMT", 4)) return false;
    struct tm g; memset(&g, 0, sizeof(g));
    g.tm_year = y - 1900; g.tm_mon = mes; g.tm_mday = d; g.tm_hour = H; g.tm_min = M; g.tm_sec = S;
#ifdef _WIN32
    time_t tt = _mkgmtime(&g);
#else
    time_t tt = timegm(&g);
#endif
    if (tt == (time_t)-1) return false;
    *t = (int64)tt;
    return true;
}

// If-None-Match: "*" ou lista de etiquetas; comparacao FRACA (ignora o W/), como a RFC 7232
// manda para este cabecalho.
static bool etag_na_lista(const char* v, int n, const char* etag)
{
    int le = (int)strlen(etag), i = 0;
    while (i < n)
    {
        while (i < n && (v[i] == ' ' || v[i] == '\t' || v[i] == ',')) i++;
        if (i >= n) break;
        if (v[i] == '*') return true;
        if (i + 1 < n && v[i] == 'W' && v[i + 1] == '/') i += 2;
        int ini = i;
        if (i < n && v[i] == '"') { i++; while (i < n && v[i] != '"') i++; if (i < n) i++; }
        else while (i < n && v[i] != ',') i++;
        if (i - ini == le && !memcmp(v + ini, etag, (size_t)le)) return true;
    }
    return false;
}

// If-Range: so vale a faixa se o arquivo ainda e o mesmo que o cliente tem. Etiqueta: comparacao
// FORTE (uma fraca, W/, nunca casa); data: igual a de modificacao. Sem casar, a resposta e o
// arquivo inteiro (200): o cliente que retomava um download recebe o arquivo novo, em vez de um
// pedaco dele grudado no antigo.
static bool if_range_vale(const char* v, int n, const char* etag, int64 mod)
{
    if (n > 0 && v[0] == '"') return n == (int)strlen(etag) && !memcmp(v, etag, (size_t)n);
    if (n > 1 && v[0] == 'W' && v[1] == '/') return false;
    int64 t;
    return le_data_http(v, n, &t) && t == mod;
}

// Pedido parcial (RFC 7233): "bytes=a-b", "bytes=a-", "bytes=-n", separados por virgula.
// Players de video (MP4) pedem uma faixa para avancar no meio do arquivo; leitores de PDF pedem
// varias de uma vez. Devolve k > 0 = k faixas satisfaziveis em f[]; -1 = nenhuma dentro do
// arquivo (416); 0 = sem Range ou um que se ignora (outra unidade, sintaxe invalida, mais de
// FAIXAS_MAX, ou faixas sobrepostas pedindo mais que o arquivo): 200 com o arquivo inteiro, que
// a RFC permite. Faixas fora do arquivo no meio de outras validas sao descartadas.
#define FAIXAS_MAX 16
typedef struct { int64 Ini, Fim; } Faixa;

static int faixas_pedidas(const char* v, int n, int64 total, Faixa* f)
{
    if (!v || n < 7 || strncmp(v, "bytes=", 6)) return 0;
    v += 6; n -= 6;
    int k = 0, pedidas = 0, i = 0; int64 soma = 0;
    while (i < n)
    {
        while (i < n && (v[i] == ' ' || v[i] == '\t')) i++;
        int64 a = -1, b = -1;
        if (i < n && v[i] >= '0' && v[i] <= '9') { a = 0; while (i < n && v[i] >= '0' && v[i] <= '9') a = a * 10 + (v[i++] - '0'); }
        if (i >= n || v[i] != '-') return 0;
        i++;
        if (i < n && v[i] >= '0' && v[i] <= '9') { b = 0; while (i < n && v[i] >= '0' && v[i] <= '9') b = b * 10 + (v[i++] - '0'); }
        while (i < n && (v[i] == ' ' || v[i] == '\t')) i++;
        if (i < n && v[i] != ',') return 0;
        if (i < n) i++;
        if (++pedidas > FAIXAS_MAX) return 0;
        if (a < 0 && b < 0) return 0;              // "-" sozinho
        if (a >= 0 && b >= 0 && b < a) return 0;   // sintaticamente invalida: ignora o Range
        int64 x, y;
        if (a < 0)
        {
            if (b == 0 || total == 0) continue;    // "bytes=-0": nada satisfaz
            x = b >= total ? 0 : total - b; y = total - 1;
        }
        else
        {
            if (a >= total) continue;
            x = a; y = (b < 0 || b >= total) ? total - 1 : b;
        }
        f[k].Ini = x; f[k].Fim = y; k++; soma += y - x + 1;
    }
    if (pedidas == 0) return 0;
    if (k == 0) return -1;
    if (k > 1 && soma > total) return 0;
    return k;
}

// Validadores do arquivo (ETag forte = tamanho-data, Last-Modified) na resposta 200 em memoria.
typedef struct { const char* ETag; const char* Mod; } Validadores;

static void cabecalho_arquivo(void* args, ResourceBuffer* http)
{
    Validadores* v = (Validadores*)args;
    resource_buffer_append_string(http, "Accept-Ranges: bytes\r\n");
    if (v) resource_buffer_append_format(http, "ETag: %s\r\nLast-Modified: %s\r\n", v->ETag, v->Mod);
}

static void envia_cabecalho(Message* request, const HttpCabecalho* c)
{
    ResourceBuffer h; resource_buffer_init(&h);
    http_resposta_cabecalho(&h, c);
    appclient_send(request->Client, h.Data, h.Length, false);
    resource_buffer_release(&h, true);
}

// Cabecalho de uma parte do multipart/byteranges (o corpo comeca com CRLF: preambulo vazio).
static int parte_cabecalho(char* out, int cap, const char* fronteira, const char* tipo, const Faixa* f, int64 total)
{
    return snprintf(out, (size_t)cap, "\r\n--%s\r\nContent-Type: %s\r\nContent-Range: bytes %lld-%lld/%lld\r\n\r\n",
                    fronteira, tipo, (long long)f->Ini, (long long)f->Fim, (long long)total);
}

bool appserver_web_process(AppServerInfo* server, Message* request)
{
    StringX path;
    string_init(&path);
    ContentTypeOption tipo = CONTENT_TYPE_NONE;
    if (!binder_web_path(&request->Route, &server->AbsLocal, &path, &tipo)) { string_release_data(&path); return false; }

    InfoArquivo info;
    if (!info_arquivo(path.Content, &info)) { string_release_data(&path); return false; }
    int64 tam = info.Tam;
    char etag[48], mod[40];
    snprintf(etag, sizeof(etag), "\"%llx-%llx\"", (unsigned long long)tam, (unsigned long long)info.Mod);
    data_http(info.Mod, mod, sizeof(mod));
    bool com_corpo = request->Cmd != CMD_HEAD;
    int n = 0; const char* v;

    // 304 (RFC 7232, secao 6): If-None-Match decide; sem ele, If-Modified-Since. O navegador
    // revalida o que ja tem no cache e o arquivo nao trafega de novo.
    bool igual = false;
    if ((v = campo_pedido(request, "If-None-Match", &n))) igual = etag_na_lista(v, n, etag);
    else if ((v = campo_pedido(request, "If-Modified-Since", &n))) { int64 t; igual = le_data_http(v, n, &t) && info.Mod <= t; }
    if (igual)
    {
        HttpCabecalho c = http_cabecalho(HTTP_STATUS_NOT_MODIFIED, server->Config.AgentName);
        c.Tamanho = -1; c.ETag = etag; c.UltimaMod = mod;
        envia_cabecalho(request, &c);
        if (server->Config.LogRequests)
            printf("RESPONSE | Client: %d | Status: 304 | %s\n", (int)(intptr_t)request->Client->Handle, path.Content);
        string_release_data(&path);
        return true;
    }

    Faixa fx[FAIXAS_MAX]; int k = 0;
    if ((v = campo_pedido(request, "Range", &n)))
    {
        int ni = 0; const char* vi = campo_pedido(request, "If-Range", &ni);
        if (!vi || if_range_vale(vi, ni, etag, info.Mod)) k = faixas_pedidas(v, n, tam, fx);
    }

    if (k == -1 || k == 1)
    {
        // 206 com o trecho (sai pela fila da conexao, como o arquivo grande) ou 416
        HttpCabecalho c = http_cabecalho(k == 1 ? HTTP_STATUS_PARTIAL_CONTENT : HTTP_STATUS_RANGE_NOT_SATISFIABLE, server->Config.AgentName);
        c.AceitaFaixa = true; c.FaixaTotal = tam; c.ETag = etag; c.UltimaMod = mod;
        if (k == 1) { c.Tipo = tipo; c.Tamanho = fx[0].Fim - fx[0].Ini + 1; c.FaixaIni = fx[0].Ini; c.FaixaFim = fx[0].Fim; }
        envia_cabecalho(request, &c);
        if (server->Config.LogRequests)
            printf("RESPONSE | Client: %d | Status: %d | Range: %lld-%lld/%lld\n", (int)(intptr_t)request->Client->Handle,
                   (int)c.Status, (long long)(k == 1 ? fx[0].Ini : 0), (long long)(k == 1 ? fx[0].Fim : 0), (long long)tam);
        if (k == 1 && com_corpo) appclient_send_file(request->Client, path.Content, fx[0].Ini, fx[0].Fim - fx[0].Ini + 1);
        string_release_data(&path);
        return true;
    }

    if (k > 1)
    {
        // 206 multipart/byteranges: cada faixa com o proprio Content-Type e Content-Range. O
        // corpo sai pela fila: cabecalho da parte, trecho do arquivo, ..., fronteira final.
        static xatomic_int seq;
        char fronteira[48], tipo_mp[96], parte[256];
        snprintf(fronteira, sizeof(fronteira), "appserver_faixas_%08x%08x", (unsigned)atomic_add_inline(&seq, 1), (unsigned)(info.Mod ^ tam));
        snprintf(tipo_mp, sizeof(tipo_mp), "multipart/byteranges; boundary=%s", fronteira);
        const char* tipo_parte = http_tipo_texto(tipo);
        int64 total = 0;
        for (int i = 0; i < k; i++) total += parte_cabecalho(parte, sizeof(parte), fronteira, tipo_parte, &fx[i], tam) + (fx[i].Fim - fx[i].Ini + 1);
        int nfim = snprintf(parte, sizeof(parte), "\r\n--%s--\r\n", fronteira);
        total += nfim;
        HttpCabecalho c = http_cabecalho(HTTP_STATUS_PARTIAL_CONTENT, server->Config.AgentName);
        c.AceitaFaixa = true; c.TipoTexto = tipo_mp; c.Tamanho = total; c.ETag = etag; c.UltimaMod = mod;
        envia_cabecalho(request, &c);
        if (server->Config.LogRequests)
            printf("RESPONSE | Client: %d | Status: 206 | %d faixas | Content Length: %lld\n", (int)(intptr_t)request->Client->Handle, k, (long long)total);
        if (com_corpo)
        {
            for (int i = 0; i < k; i++)
            {
                int np = parte_cabecalho(parte, sizeof(parte), fronteira, tipo_parte, &fx[i], tam);
                appclient_send(request->Client, (byte*)parte, np, false);
                if (!appclient_send_file(request->Client, path.Content, fx[i].Ini, fx[i].Fim - fx[i].Ini + 1)) break;
            }
            nfim = snprintf(parte, sizeof(parte), "\r\n--%s--\r\n", fronteira);
            appclient_send(request->Client, (byte*)parte, nfim, false);
        }
        string_release_data(&path);
        return true;
    }

    if (tam <= WEB_EM_MEMORIA_MAX)
    {
        ResourceBuffer buffer;
        memset(&buffer, 0, sizeof(ResourceBuffer));
        Validadores val = { etag, mod };
        bool ok = binder_get_web_resource(&request->Route, &server->AbsLocal, &buffer);
        if (ok) appserver_http_response_send(server, request, HTTP_STATUS_OK, &buffer, cabecalho_arquivo, &val);
        if (buffer.Data) memop_free_raw(buffer.Data);
        string_release_data(&path);
        return ok;
    }

    HttpCabecalho c = http_cabecalho(HTTP_STATUS_OK, server->Config.AgentName);
    c.Tipo    = tipo;
    c.Tamanho = tam;
    c.AceitaFaixa = true; c.ETag = etag; c.UltimaMod = mod;
    envia_cabecalho(request, &c);
    if (server->Config.LogRequests)
        printf("RESPONSE | Client: %d | Status: OK | Type: %s | Content Length: %lld (streaming)\n",
               (int)(intptr_t)request->Client->Handle, http_tipo_texto(tipo), (long long)tam);
    // HEAD: so o cabecalho (com o Content-Length do arquivo)
    if (com_corpo) appclient_send_file(request->Client, path.Content, 0, tam);
    string_release_data(&path);
    return true;
}

bool websocket_handshake_received(AppServerInfo* server, Message* request)
{
    if (request->SecWebsocketKey && request->SecWebsocketKey->Length > 0)
    {
        bool found = binder_route_exist(server->BindList, server->Prefix, &request->Route, &(int){ -1 }) != 0;
        if (found)
        {
            request->Client->IsWebSocketMode = true;
            appserver_http_response_send(server, request, HTTP_STATUS_SWITCHING_PROTOCOLS, 0,  websocket_handshake_prepare, request->SecWebsocketKey);
        }
        else
        {
            appserver_http_response_send(server, request, HTTP_STATUS_NOT_FOUND, 0, 0,0);
        }
        return true;
    }
    return false;
}



// Onde a requisicao roda (ROTA_CURTA / LENTA / LONGA). Mesmo criterio de busca do despacho
// (rota, depois extensao).
int appserver_rota_modo(Message* request)
{
    if (request->Protocol == AOTP) return ROTA_CURTA;
    AppServerInfo* server = request->Client->Server;
    int rest = -1;
    FunctionBind* bind = binder_route_exist(server->BindList, server->Prefix, &request->Route, &rest);
    if (!bind && request->Route.Count > 0)
        bind = binder_extension_exist(server->BindList, server->Prefix, (StringX*)request->Route.Items[request->Route.Count - 1]);
    if (!bind) return ROTA_CURTA;
    return bind->Longa ? ROTA_LONGA : bind->Lenta ? ROTA_LENTA : ROTA_CURTA;
}

void appserver_received(Message* request)
{
    if (request->Protocol == AOTP)
	{
		appserver_received_aotp(request);
		return;
	}

    AppServerInfo* server = request->Client->Server;

    ReportRequest(request);

    if(websocket_handshake_received(server, request)) return;

    if (request->Cmd == CMD_OPTIONS)
    {
        appserver_http_default_options(server, request);
        return;
    }
    // Rota registrada primeiro: com ela, nada de procurar arquivo estatico. A busca custa uma
    // consulta ao sistema de arquivos que FALHA (GetFileAttributes + abertura) a cada GET de
    // API -- medido no Windows: com o reator atendendo os pings em serie, era a maior parte do
    // tempo dele (70-95% ocupado em CreateFile/NtQueryAttributesFile), p99 do ping +49% com
    // clientes lentos. Arquivo estatico e rota por extensao seguem na ordem de antes.
    int rest = -1;
    FunctionBind* bind = binder_route_exist(server->BindList, server->Prefix, &request->Route, &rest);

    if (!bind && (request->Cmd == CMD_GET || request->Cmd == CMD_HEAD) && appserver_web_process(server, request))
    {
        return;
    }

    // Rota registrada por EXTENSAO (app_add_receiver_extension): a API era publica, mas
    // ninguem consultava o registro -- registrar um handler de ".m4s" nao tinha efeito.
    if (!bind && request->Route.Count > 0)
        bind = binder_extension_exist(server->BindList, server->Prefix, (StringX*)request->Route.Items[request->Route.Count - 1]);

    // HEAD numa rota de API: responder exigiria RODAR o handler (com os efeitos colaterais
    // dele) so para descartar o corpo. Recusa com 405 em vez disso.
    if (bind && request->Cmd == CMD_HEAD)
    {
        send_response_status(request, HTTP_STATUS_METHOD_NOT_ALLOWED, "HEAD nao suportado nesta rota.");
        return;
    }

    if (bind)
    {
        if (request->ContentLength > 0 && request->Content.Length > 0)
        {
            int first = 0;
            while (first < request->Content.Length &&
                  (request->Content.Content[first] == ' ' || request->Content.Content[first] == '\t' ||
                   request->Content.Content[first] == '\r' || request->Content.Content[first] == '\n'))
            {
                first++;
            }

            bool looks_json = first < request->Content.Length &&
                (request->Content.Content[first] == '{' || request->Content.Content[first] == '[');

            if (request->ContentType == APPLICATION_JSON || looks_json)
            {
                request->ContentType = APPLICATION_JSON;
                request->Object = yason_parse(request->Content.Content, request->Content.Length, TREE_TYPE_JSON);
            }
        }

        MessageMatchReceiverCalback func = bind->CallbackFunc;
        long long t0 = agora_us_srv();
        Element* result = func(request);
        aprende_rota(server, bind, agora_us_srv() - t0);

        // O handler pode ter transmitido a resposta ele mesmo (ex.: SSE). O canal acaba com o
        // handler: fecha o fluxo e a conexao.
        if (request->StreamHandled)
        {
            if (request->Canal) { app_canal_fechar(request->Canal); request->Canal = 0; }
            return;
        }

        if (result)
        {
            // Tudo aqui era vazado a cada resposta JSON: a struct do buffer, os bytes UTF-8, a
            // string renderizada e a propria arvore devolvida pelo handler.
            ResourceBuffer rb;
            memset(&rb, 0, sizeof(rb));
            StringX* json = yason_render(result, 1);
            size_t json_len = 0;   // string_utf8_to_bytes grava size_t; Length e int
            rb.Data   = json ? string_utf8_to_bytes(json->Content, &json_len) : 0;
            rb.Length = (int)json_len;
            rb.Type   = APPLICATION_JSON;
            appserver_http_response_send(server, request, HTTP_STATUS_OK, &rb, 0, 0);

            if (rb.Data) memop_free_raw(rb.Data);
            if (json) { string_release(json); memop_free_raw(json); }
            // O handler pode ter devolvido o proprio corpo da requisicao; esse vai embora com a
            // mensagem (message_release), e liberar aqui tambem seria dupla liberacao.
            if ((void*)result != request->Object) appserver_element_free(result);
            return;
        }
        else if (request->Response)
        {
            if (request->Response->ContentType != CONTENT_TYPE_NONE)
            {
                ResourceBuffer rb;
                rb.Data   = request->Response->Content.Data;
                rb.Length = request->Response->Content.Length;
                rb.Type   = request->Response->ContentType;
                // O STATUS e' o que o handler gravou na resposta. Estava chumbado em 200:
                // todo 400/404/500 dos controllers chegava ao cliente como sucesso, com o
                // erro escondido no corpo -- o front chegou a contornar isso adivinhando
                // pelo texto, e um player HLS nao tem como adivinhar.
                HttpStatusCode status = request->Response->Status > 0
                                      ? (HttpStatusCode)request->Response->Status : HTTP_STATUS_OK;
                appserver_http_response_send(server, request, status, &rb, 0, 0);
            }
            else
            {
                send_response_server_error(request, "The content type was not defined.");
            }
        }
        else
        {
            send_response_server_error(request, "Instantiating a response is required if no return object is defined.");
        }
    }
    else
    {
        // Rota inexistente e 404, nao 500: o servidor nao falhou, o recurso e que nao existe.
        // Com 500 todo favicon.ico e toda sondagem de navegador apareciam como erro interno.
        send_response_status(request, HTTP_STATUS_NOT_FOUND, "Recurso nao encontrado.");
    }
}






// ---- configuracao ---------------------------------------------------------------------
// Unico lugar que le variavel de ambiente do servidor. Antes cada modulo lia a sua (o
// parser lia APPSERVER_MAX_BODY_MB, o health lia APPSERVER_HEALTH_ALLOW_REMOTE), cada um
// com seu cache estatico: nao havia como dois servidores no mesmo processo diferirem.
static const char* env_ler(const char* nome, char* buf, int cap)
{
#ifdef _WIN32
    char* v = 0; size_t n = 0;
    if (_dupenv_s(&v, &n, nome) != 0 || !v) return 0;
    snprintf(buf, cap, "%s", v);
    free(v);
    return buf;
#else
    const char* v = getenv(nome);
    if (!v) return 0;
    snprintf(buf, cap, "%s", v);
    return buf;
#endif
}

AppServerConfig appserver_config_default(void)
{
    AppServerConfig c;
    memset(&c, 0, sizeof(c));
    c.AgentName           = "appserver";
    c.Port                = 8080;
    c.Prefix              = "api";
    c.WebContentPath      = 0;
    c.EnableHealthMonitor = false;
    c.HealthAllowRemote   = false;
    c.MaxClients          = 512;
    c.IdleTimeoutMs       = 75000;            // mesma ordem do keep-alive do nginx
    c.MaxHeaderBytes      = 64 * 1024;
    c.MaxBodyBytes        = (int64)1024 * 1024 * 1024;
    c.BodyToDiskBytes     = (int64)8 * 1024 * 1024;
    c.TempDir             = 0;
    c.MaxOutputQueueBytes = (int64)8 * 1024 * 1024;
    c.SseHeartbeatMs      = 15000;
    c.Pool                = 0;
    c.LogRequests         = true;
    c.LongWorkers         = 16;
    c.RotaLentaUs         = 1000;
    c.MaxSincronoPorVolta = 1;
    c.PerfilPool          = -1;     // a biblioteca nao impoe perfil: a aplicacao escolhe
    c.JobsEmPerformance   = true;
    c.SseVoltaUs          = 1000;   // o mesmo orcamento do reator: ate ~50 assinantes, nenhuma thread acorda
    c.SseMaxPorPista      = 64;

    char v[64];
    if (env_ler("APPSERVER_MAX_BODY_MB", v, sizeof(v)))
    {
        long long mb = atoll(v);
        if (mb > 0) c.MaxBodyBytes = (int64)mb * 1024 * 1024;
    }
    if (env_ler("APPSERVER_HEALTH_ALLOW_REMOTE", v, sizeof(v))) c.HealthAllowRemote = (v[0] == '1');
    return c;
}

// Copia de string que vive enquanto o servidor viver (o processo, na pratica).
static const char* cfg_copia(const char* s)
{
    if (!s) return 0;
    size_t n = strlen(s);
    char* r = (char*)memop_calloc_raw(1, n + 1);
    if (r) memcpy(r, s, n);
    return r;
}

static void cfg_resolve_temp(AppServerInfo* info)
{
    const char* t = info->Config.TempDir;
    char buf[1024];
    if (!t || !t[0])
    {
#ifdef _WIN32
        DWORD n = GetTempPathA((DWORD)sizeof(buf), buf);
        t = (n > 0 && n < sizeof(buf)) ? buf : ".";
#else
        t = env_ler("TMPDIR", buf, sizeof(buf));
        if (!t || !t[0]) t = "/tmp";
#endif
    }
    snprintf(info->TempDir, sizeof(info->TempDir), "%s", t);
    size_t n = strlen(info->TempDir);   // sem a barra final: quem monta o caminho poe a sua
    while (n > 1 && (info->TempDir[n - 1] == '/' || info->TempDir[n - 1] == '\\'))info->TempDir[--n] = 0;
    info->Config.TempDir = info->TempDir;
}


AppServerInfo* appserver_create(const AppServerConfig* config, FunctionBindList* bind_list)
{
    appserver_init();

    AppServerConfig cfg = config ? *config : appserver_config_default();
    if (!cfg.AgentName) cfg.AgentName = "appserver";
    if (!cfg.Prefix)    cfg.Prefix    = "";
    const char* agent_name       = cfg.AgentName;
    const int   port             = cfg.Port;
    const char* prefix           = cfg.Prefix;
    const char* web_content_path = cfg.WebContentPath;
    const bool  enable_health_monitor = cfg.EnableHealthMonitor;

    AppServerInfo* info = serverinfo_create();
    info->BindList  = bind_list;
    info->Config    = cfg;
    info->Config.AgentName      = cfg_copia(cfg.AgentName);
    info->Config.Prefix         = cfg_copia(cfg.Prefix);
    info->Config.WebContentPath = cfg_copia(cfg.WebContentPath);
    info->Config.Pool           = cfg.Pool;
    info->Port                  = port;
    cfg_resolve_temp(info);

    // Rota de saude EMBUTIDA: /<prefixo>/health, so se o chamador pedir. Entra na lista
    // dele para que a aplicacao a tenha sem registrar nada. Desligada, nem o monitor
    // inicializa -- e isso importa, porque o init do PDH carrega pdh.dll, d3d11.dll e
    // dxgi.dll no processo. Ver health_controller.c, inclusive a ressalva de que a rota
    // ainda nao passa por autenticacao.
    //
    // ANTES do laco logo abaixo, de proposito: e ele quem da um Thung.Client a cada bind.
    // Registrada depois, a rota entrava na lista com esse campo nulo, e o servidor travava
    // ao atender por ela -- foi o que aconteceu, e custou caro achar.
    if (enable_health_monitor)
    {
        health_monitor_init();
        if (info->BindList) app_add_receiver(info->BindList, "health", appserver_health_route, true);
    }
    info->IsRunning = true;
	info->Events    = event_list_create();

    // TO-DO: somente para teste. depois que gerenciador de sesseao pronto, nao precisa este loop de atribui��o de client
    // Passar referencia do servidor
    if (info->BindList)
    {
        int ix = 0;
		while (ix < info->BindList->Count)
		{
			FunctionBind* bind = info->BindList->Items[ix];

            // TO-DO: somente para teste, para nao gerar erro, depois tem que remover.
            bind->Thung.Client = (AppClientInfo*)memop_calloc_raw(1, sizeof(AppClientInfo));
            bind->Thung.Client->Server = info;

           // bind->Function
			
           // bind. assembler_method_bind_(void* sender, void* client)

			ix++;
		}
    }

    string_init(&info->AgentName);
    string_appends(&info->AgentName, agent_name, (int)strlen(agent_name), 0, (int)strlen(agent_name));
    WsaInit();
    info->Prefix = string_split_cstr(prefix, (int)strlen(prefix), (char)0x2F);

    if (web_content_path && web_content_path[0] != '\0')
    {
        // Caminho ABSOLUTO, resolvido agora. Guardado como veio ("web"), ele valia em relacao
        // ao diretorio de trabalho de cada momento: o servidor so achava os arquivos se fosse
        // iniciado de dentro da pasta certa (o nome do campo ja era AbsLocal).
        char abs_path[4096];
        const char* usar = web_content_path;
#ifdef _WIN32
        if (_fullpath(abs_path, web_content_path, sizeof(abs_path))) usar = abs_path;
#else
        if (realpath(web_content_path, abs_path)) usar = abs_path;
#endif
        string_appends(&info->AbsLocal, usar, (int)strlen(usar), 0, (int)strlen(usar));
        // O caminho relativo continua valendo em relacao ao diretorio de trabalho NA SUBIDA.
        // Iniciado de outra pasta, o servidor subia "normal" e so falhava arquivo por arquivo
        // (500, depois 404). Agora o engano aparece na primeira linha do log.
        printf("Conteudo web: %s\n", info->AbsLocal.Content);
#ifdef _WIN32
        DWORD atr = GetFileAttributesA(info->AbsLocal.Content);
        int existe = atr != INVALID_FILE_ATTRIBUTES && (atr & FILE_ATTRIBUTE_DIRECTORY);
#else
        struct stat st; int existe = stat(info->AbsLocal.Content, &st) == 0 && S_ISDIR(st.st_mode);
#endif
        if (!existe) printf("AVISO: a pasta de conteudo web nao existe: %s (diretorio de trabalho errado?)\n", info->AbsLocal.Content);
    }

    serverinfo_list_add(&Servers, info);

    // Pista longa (utils/pista.c, via http_pista_longa): threads proprias para rotas longas e
    // jobs, criadas sob demanda ate LongWorkers e REUSADAS -- thread de vida curta deixava
    // memoria presa no memory_pool (medido: ~24 KB por requisicao no servidor antigo). Sem
    // trabalho elas dormem bloqueadas: nao giram como o pool de tarefas, e nao ha monitor.
    thread_mutex_init_inline(&info->ClientsLock);
    thread_mutex_init_inline(&info->CanaisLock);
    thread_mutex_init_inline(&info->TopicosLock);
    thread_mutex_init_inline(&info->PerfilLock);
    info->PerfilPedidos = 0;
    // Perfil do pool de tarefas (pool_perfil do xplatbase). Vale para o pool inteiro -- se for
    // o global, para tudo que o usa no processo (codecs inclusive): por isso o padrao (-1) nao
    // mexe, e a aplicacao decide.
    if (info->Config.PerfilPool >= 0)
    {
        if (info->Config.Pool) pool_perfil_relative(info->Config.Pool, info->Config.PerfilPool);
        else pool_perfil(info->Config.PerfilPool);
    }
    info->Despachar = appserver_received;
    info->Classificar = appserver_rota_modo;
    info->PistaLonga = 0;

    // O reator sobe por ULTIMO: um cliente rapido nao pode ser despachado com o servidor
    // pela metade (Prefix, AbsLocal, rotas).
    char recusa[256];
    int nr = http_resposta_recusa(recusa, sizeof(recusa), HTTP_STATUS_SERVICE_UNAVAILABLE);
    NetConfig nc;
    memset(&nc, 0, sizeof(nc));
    nc.Porta        = port;
    nc.MaxConexoes  = info->Config.MaxClients;
    nc.OciosoMs     = info->Config.IdleTimeoutMs;
    nc.MaxFilaSaida = info->Config.MaxOutputQueueBytes;
    nc.Pool         = info->Config.Pool;
    nc.RecusaCheio  = nr > 0 ? recusa : 0;
    nc.MaxSincronoVolta = info->Config.MaxSincronoPorVolta;
    info->Net = net_servidor_criar(&nc, http_conexao_protocolo(), info);
    if (!info->Net)
    {
        printf("Erro ao iniciar o servidor na porta %d.\n", port);
        return NULL;
    }

    printf("Server instantiated '%s' | Port: %d | Prefix: '%s'\n", agent_name, port, prefix);
    return info;
}



void app_add_receiver(FunctionBindList* list, const char* route, MessageMatchReceiverCalback function, bool with_callback)
{
    binder_list_add_receiver(list, route, function, with_callback);
}
void app_add_receiver_longa(FunctionBindList* list, const char* route, MessageMatchReceiverCalback function)
{
    binder_list_add_receiver(list, route, function, true);
    list->Items[list->Count - 1]->Longa = true;
}
// ---- perfil do pool: pedidos de performance -----------------------------------------------
// Com o servidor em economia, quem precisa de vazao por um tempo (um job de video) pede
// performance; o ultimo a terminar devolve o perfil configurado. Por contagem: varios jobs
// ao mesmo tempo nao se atropelam. Sem perfil configurado (-1), nao faz nada.
static void aplica_perfil(AppServerInfo* s, int perfil)
{
    if (s->Config.Pool) pool_perfil_relative(s->Config.Pool, perfil);
    else pool_perfil(perfil);
}

void app_perfil_performance_inicio(AppServerInfo* s)
{
    if (!s || s->Config.PerfilPool != POOL_PERFIL_ECONOMIA) return;
    thread_mutex_lock_inline(&s->PerfilLock);
    if (s->PerfilPedidos++ == 0) aplica_perfil(s, POOL_PERFIL_PERFORMANCE);
    thread_mutex_unlock_inline(&s->PerfilLock);
}

void app_perfil_performance_fim(AppServerInfo* s)
{
    if (!s || s->Config.PerfilPool != POOL_PERFIL_ECONOMIA) return;
    thread_mutex_lock_inline(&s->PerfilLock);
    if (s->PerfilPedidos > 0 && --s->PerfilPedidos == 0) aplica_perfil(s, POOL_PERFIL_ECONOMIA);
    thread_mutex_unlock_inline(&s->PerfilLock);
}

// Diagnostico: a rota foi aprendida como lenta? -1 = rota nao existe.
int app_rota_lenta(AppServerInfo* s, const char* rota)
{
    if (!s || !s->BindList || !rota) return -1;
    for (int i = 0; i < s->BindList->Count; i++)
    {
        FunctionBind* b = s->BindList->Items[i];
        char nome[256]; int k = 0;
        for (int j = 0; j < b->Route.Count && k < 250; j++)
        {
            StringX* x = (StringX*)b->Route.Items[j];
            if (j) nome[k++] = '/';
            for (int q = 0; q < (int)x->Length && k < 250; q++) nome[k++] = x->Content[q];
        }
        nome[k] = 0;
        if (!strcmp(nome, rota)) return b->Lenta ? 1 : 0;
    }
    return -1;
}

void app_add_receiver_extension(FunctionBindList* list, const char* extension, MessageMatchReceiverCalback function, bool with_callback)
{
    binder_list_add_receiver_extension(list, extension, function, with_callback);
}
void app_add_web_resource(FunctionBindList* list, const char* route, MessageMatchReceiverCalback function)
{
    binder_list_add_web_resource(list, route, function);
}
MessageEmitterCalback app_add_emitter(FunctionBindList* list, const char* route)
{
    return binder_list_add_emitter(list, route);
}