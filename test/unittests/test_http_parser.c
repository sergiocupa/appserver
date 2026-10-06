//  Testes do parser HTTP incremental (appserver/src/http/http_parser.c).
//
//  O parser nao sabe de socket: aqui ele e alimentado direto, em pedacos escolhidos para
//  quebrar cabecalho e corpo em pontos ruins (1 byte, no meio do "\r\n\r\n", etc.).

#include "testes.h"
#include "../../appserver/src/http/http_parser.h"
#include "../../appserver/include/appserver.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
  #include <windows.h>
  static void pasta_temp(char* out, int cap) { char t[MAX_PATH]; GetTempPathA(MAX_PATH, t); size_t n = strlen(t); if (n && (t[n-1] == '\\' || t[n-1] == '/')) t[n-1] = 0; snprintf(out, cap, "%s", t); }
#else
  static void pasta_temp(char* out, int cap) { snprintf(out, cap, "/tmp"); }
#endif

#define MAX_MSG 16

typedef struct
{
    Message*       Msg[MAX_MSG];
    int            N;
    HttpStatusCode Erro;
    int            NErros;
}
Coleta;

static void ao_msg(void* ctx, Message* m) { Coleta* c = (Coleta*)ctx; if (c->N < MAX_MSG) c->Msg[c->N++] = m; else message_release(m); }
static void ao_err(void* ctx, HttpStatusCode s) { Coleta* c = (Coleta*)ctx; c->Erro = s; c->NErros++; }

static void coleta_libera(Coleta* c) { for (int i = 0; i < c->N; i++) message_release(c->Msg[i]); c->N = 0; }

static char g_temp[512];

static HttpParser* novo(Coleta* c, int64 max_cab, int64 max_corpo, int64 disco)
{
    memset(c, 0, sizeof(*c));
    pasta_temp(g_temp, sizeof(g_temp));
    HttpParserLimites l;
    l.MaxHeaderBytes = max_cab; l.MaxBodyBytes = max_corpo; l.BodyToDiskBytes = disco; l.TempDir = g_temp;
    return http_parser_criar(&l, ao_msg, ao_err, c);
}

static int rota_eh(Message* m, int i, const char* s)
{
    if (i >= m->Route.Count) return 0;
    StringX* x = (StringX*)m->Route.Items[i];
    return (int)strlen(s) == (int)x->Length && memcmp(x->Content, s, x->Length) == 0;
}

static long long tamanho_arquivo(const char* cam)
{
    FILE* f = fopen(cam, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END); long long t = ftell(f); fclose(f);
    return t;
}

void teste_parser_pedacos_de_um_byte(TestResult* r)
{
    t_start(r);
    Coleta c; HttpParser* p = novo(&c, 64 * 1024, 1 << 20, 1 << 20);
    const char* req = "POST /api/eco/x HTTP/1.1\r\nHost: a\r\nContent-Type: application/json\r\nContent-Length: 11\r\n\r\n{\"a\":12345}";
    int n = (int)strlen(req);
    for (int i = 0; i < n; i++)
    {
        T_ASSERT(r, http_parser_alimentar(p, (const byte*)req + i, 1), "parser recusou o byte %d", i);
        if (i < n - 1) T_ASSERT(r, c.N == 0, "entregou a mensagem antes do ultimo byte (byte %d)", i);
    }
    T_ASSERT(r, c.N == 1, "esperava 1 mensagem, veio %d", c.N);
    Message* m = c.Msg[0];
    T_ASSERT(r, m->Cmd == CMD_POST, "metodo errado (%d)", (int)m->Cmd);
    T_ASSERT(r, rota_eh(m, 0, "api") && rota_eh(m, 1, "eco") && rota_eh(m, 2, "x"), "rota errada");
    T_ASSERT(r, m->ContentLength == 11 && m->Content.Length == 11 && !memcmp(m->Content.Content, "{\"a\":12345}", 11), "corpo errado");
    T_ASSERT(r, app_corpo_arquivo(m) == 0, "corpo pequeno nao devia ir para disco");
    T_ASSERT(r, !http_parser_ocupado(p), "parser ficou ocupado depois da mensagem completa");
    coleta_libera(&c);
    http_parser_destruir(p);
}

// Corpo grande EM MEMORIA (abaixo do limite de disco): o buffer nasce do tamanho do
// Content-Length e os pedacos sao copiados direto. Pedacos de tamanhos variados, o 1o grudado
// no cabecalho, e outra requisicao logo atras do corpo: o corpo tem de sair inteiro e exato, e
// a seguinte, intacta.
void teste_parser_corpo_grande_em_memoria(TestResult* r)
{
    t_start(r);
    enum { CORPO = 300000 };
    Coleta c; HttpParser* p = novo(&c, 64 * 1024, 1 << 20, 1 << 20);
    int cab = 0;
    char* req = (char*)malloc(CORPO + 512);
    cab = snprintf(req, 512, "POST /api/up HTTP/1.1\r\nContent-Length: %d\r\n\r\n", CORPO);
    for (int i = 0; i < CORPO; i++) req[cab + i] = (char)('a' + (i * 7) % 26);
    int n = cab + CORPO;
    n += snprintf(req + n, 512, "GET /api/depois HTTP/1.1\r\nHost: a\r\n\r\n");
    int pos = 0, k = 0;
    static const int pedacos[] = { 3000, 1, 65536, 7, 12345, 100000, 2, 40000 };
    while (pos < n)
    {
        int q = pedacos[k++ % 8]; if (q > n - pos) q = n - pos;
        T_ASSERT(r, http_parser_alimentar(p, (const byte*)req + pos, q), "parser recusou o pedaco em %d", pos);
        pos += q;
    }
    T_ASSERT(r, c.N == 2, "esperava 2 mensagens, veio %d", c.N);
    Message* m = c.Msg[0];
    T_ASSERT(r, app_corpo_arquivo(m) == 0, "corpo abaixo do limite nao devia ir para disco");
    T_ASSERT(r, m->Content.Length == CORPO && !memcmp(m->Content.Content, req + cab, CORPO), "corpo diferente do enviado");
    T_ASSERT(r, m->Content.Content[CORPO] == 0, "corpo sem o terminador");
    T_ASSERT(r, c.Msg[1]->Cmd == CMD_GET && rota_eh(c.Msg[1], 1, "depois"), "a requisicao seguinte chegou errada");
    free(req);
    coleta_libera(&c);
    http_parser_destruir(p);
}

void teste_parser_pipelining_em_ordem(TestResult* r)
{
    t_start(r);
    Coleta c; HttpParser* p = novo(&c, 64 * 1024, 1 << 20, 1 << 20);
    char buf[4096]; int n = 0;
    for (int k = 1; k <= 5; k++)
    {
        n += snprintf(buf + n, sizeof(buf) - n, "POST /api/r%d HTTP/1.1\r\nContent-Length: %d\r\n\r\n", k, k);
        memset(buf + n, '0' + k, (size_t)k); n += k;
    }
    n += snprintf(buf + n, sizeof(buf) - n, "GET /api/fim HTTP/1.1\r\n\r\n");
    T_ASSERT(r, http_parser_alimentar(p, (const byte*)buf, n), "parser recusou o lote");
    T_ASSERT(r, c.N == 6, "esperava 6 mensagens num unico envio, veio %d", c.N);
    for (int k = 1; k <= 5; k++)
    {
        char nome[8]; snprintf(nome, sizeof(nome), "r%d", k);
        Message* m = c.Msg[k - 1];
        T_ASSERT(r, rota_eh(m, 1, nome), "mensagem %d fora de ordem", k);
        T_ASSERT(r, m->Content.Length == k && m->Content.Content[0] == '0' + k, "corpo da mensagem %d errado", k);
    }
    T_ASSERT(r, rota_eh(c.Msg[5], 1, "fim") && c.Msg[5]->Cmd == CMD_GET, "ultima mensagem errada");
    coleta_libera(&c);
    http_parser_destruir(p);
}

// Corpo acima do limite de memoria: vai para arquivo conforme chega, e o arquivo e apagado
// quando a mensagem e liberada.
void teste_parser_corpo_grande_vai_para_disco(TestResult* r)
{
    t_start(r);
    enum { CORPO = 300 * 1024, PEDACO = 3001 };
    Coleta c; HttpParser* p = novo(&c, 64 * 1024, 1 << 20, 64 * 1024);
    char cab[256];
    int nc = snprintf(cab, sizeof(cab), "POST /api/up HTTP/1.1\r\nContent-Length: %d\r\n\r\n", CORPO);
    byte* corpo = (byte*)malloc(CORPO);
    for (int i = 0; i < CORPO; i++) corpo[i] = (byte)(i * 7 + 3);

    // cabecalho + comeco do corpo no mesmo pedaco, como costuma chegar
    byte* prim = (byte*)malloc((size_t)nc + 100);
    memcpy(prim, cab, (size_t)nc); memcpy(prim + nc, corpo, 100);
    T_ASSERT(r, http_parser_alimentar(p, prim, nc + 100), "recusou o primeiro pedaco");
    free(prim);
    T_ASSERT(r, http_parser_ocupado(p), "com corpo pela metade o parser devia estar ocupado");
    for (int i = 100; i < CORPO; i += PEDACO)
    {
        int k = CORPO - i < PEDACO ? CORPO - i : PEDACO;
        T_ASSERT(r, http_parser_alimentar(p, corpo + i, k), "recusou o pedaco em %d", i);
    }
    T_ASSERT(r, c.N == 1, "esperava 1 mensagem, veio %d", c.N);
    Message* m = c.Msg[0];
    const char* arq = app_corpo_arquivo(m);
    T_ASSERT(r, arq != 0, "corpo de %d bytes devia estar em disco", CORPO);
    T_ASSERT(r, m->Content.Length == 0, "corpo em disco nao deveria estar tambem em memoria");
    T_ASSERT(r, m->ContentLength == CORPO, "ContentLength errado");
    T_ASSERT(r, tamanho_arquivo(arq) == CORPO, "arquivo com %lld bytes, esperava %d", tamanho_arquivo(arq), CORPO);

    FILE* f = fopen(arq, "rb");
    byte* lido = (byte*)malloc(CORPO);
    size_t nl = f ? fread(lido, 1, CORPO, f) : 0;
    if (f) fclose(f);
    T_ASSERT(r, nl == CORPO && memcmp(lido, corpo, CORPO) == 0, "conteudo do arquivo difere do enviado");
    free(lido);

    char copia[1024]; snprintf(copia, sizeof(copia), "%s", arq);
    coleta_libera(&c);
    T_ASSERT(r, tamanho_arquivo(copia) < 0, "temporario ficou para tras depois de liberar a mensagem: %s", copia);
    free(corpo);
    http_parser_destruir(p);
}

void teste_parser_corpo_salvar_move_o_arquivo(TestResult* r)
{
    t_start(r);
    Coleta c; HttpParser* p = novo(&c, 64 * 1024, 1 << 20, 1024);
    char req[8192];
    int n = snprintf(req, sizeof(req), "POST /api/up HTTP/1.1\r\nContent-Length: 5000\r\n\r\n");
    memset(req + n, 'z', 5000); n += 5000;
    T_ASSERT(r, http_parser_alimentar(p, (const byte*)req, n), "recusou");
    T_ASSERT(r, c.N == 1, "sem mensagem");
    Message* m = c.Msg[0];
    char tmp[1024]; snprintf(tmp, sizeof(tmp), "%s", app_corpo_arquivo(m) ? app_corpo_arquivo(m) : "");
    T_ASSERT(r, tmp[0], "corpo devia estar em disco");

    char destino[1024]; snprintf(destino, sizeof(destino), "%s/appsrv_teste_destino.bin", g_temp);
    remove(destino);
    T_ASSERT(r, app_corpo_salvar(m, destino), "app_corpo_salvar falhou");
    T_ASSERT(r, tamanho_arquivo(destino) == 5000, "destino com tamanho errado");
    T_ASSERT(r, tamanho_arquivo(tmp) < 0, "o temporario devia ter sido MOVIDO, nao copiado");
    T_ASSERT(r, app_corpo_arquivo(m) == 0, "depois de salvo o arquivo nao e mais da mensagem");
    coleta_libera(&c);
    T_ASSERT(r, tamanho_arquivo(destino) == 5000, "liberar a mensagem apagou o arquivo do chamador");
    remove(destino);

    // corpo em memoria: app_corpo_salvar grava
    n = snprintf(req, sizeof(req), "POST /api/up HTTP/1.1\r\nContent-Length: 10\r\n\r\n0123456789");
    T_ASSERT(r, http_parser_alimentar(p, (const byte*)req, n), "recusou o pequeno");
    T_ASSERT(r, c.N == 1 && app_corpo_arquivo(c.Msg[0]) == 0, "pequeno devia ficar em memoria");
    T_ASSERT(r, app_corpo_salvar(c.Msg[0], destino) && tamanho_arquivo(destino) == 10, "gravar corpo em memoria falhou");
    remove(destino);
    coleta_libera(&c);
    http_parser_destruir(p);
}

void teste_parser_limites_e_erros(TestResult* r)
{
    t_start(r);
    Coleta c; HttpParser* p;

    // cabecalho sem fim e maior que o limite -> 431
    p = novo(&c, 1024, 1 << 20, 1 << 20);
    char grande[4096]; memset(grande, 'a', sizeof(grande));
    memcpy(grande, "GET / HTTP/1.1\r\nX: ", 19);
    T_ASSERT(r, !http_parser_alimentar(p, (const byte*)grande, sizeof(grande)), "cabecalho grande foi aceito");
    T_ASSERT(r, c.NErros == 1 && c.Erro == HTTP_STATUS_HEADERS_TOO_LARGE, "esperava 431 (veio %d, %d vez(es))", (int)c.Erro, c.NErros);
    T_ASSERT(r, !http_parser_alimentar(p, (const byte*)"GET / HTTP/1.1\r\n\r\n", 18), "depois do erro devia recusar tudo");
    T_ASSERT(r, c.NErros == 1 && c.N == 0, "erro avisado mais de uma vez ou mensagem entregue depois do erro");
    http_parser_destruir(p);

    // corpo acima do maximo -> 413, sem esperar o corpo chegar
    p = novo(&c, 64 * 1024, 1000, 1 << 20);
    const char* r413 = "POST /x HTTP/1.1\r\nContent-Length: 1001\r\n\r\n";
    T_ASSERT(r, !http_parser_alimentar(p, (const byte*)r413, (int)strlen(r413)), "corpo acima do maximo foi aceito");
    T_ASSERT(r, c.Erro == HTTP_STATUS_PAYLOAD_TOO_LARGE, "esperava 413 (veio %d)", (int)c.Erro);
    http_parser_destruir(p);

    // Content-Length invalido -> 400
    p = novo(&c, 64 * 1024, 1 << 20, 1 << 20);
    const char* r400 = "POST /x HTTP/1.1\r\nContent-Length: -5\r\n\r\n";
    T_ASSERT(r, !http_parser_alimentar(p, (const byte*)r400, (int)strlen(r400)), "Content-Length negativo foi aceito");
    T_ASSERT(r, c.Erro == HTTP_STATUS_BAD_REQUEST, "esperava 400 (veio %d)", (int)c.Erro);
    http_parser_destruir(p);
}

// Conexao cai no meio de um upload grande: o temporario nao pode ficar para tras.
void teste_parser_destruir_no_meio_apaga_temporario(TestResult* r)
{
    t_start(r);
    Coleta c; HttpParser* p = novo(&c, 64 * 1024, 1 << 20, 1024);
    char req[4096];
    int n = snprintf(req, sizeof(req), "POST /api/up HTTP/1.1\r\nContent-Length: 100000\r\n\r\n");
    memset(req + n, 'q', 2000); n += 2000;
    T_ASSERT(r, http_parser_alimentar(p, (const byte*)req, n), "recusou");
    T_ASSERT(r, c.N == 0 && http_parser_ocupado(p), "a mensagem nao podia estar completa");

    char tmp[1024];
    const char* a = http_parser_arquivo_parcial(p);
    T_ASSERT(r, a != 0, "corpo grande pela metade devia estar num temporario");
    snprintf(tmp, sizeof(tmp), "%s", a);
    T_ASSERT(r, tamanho_arquivo(tmp) >= 0, "temporario nao existe: %s", tmp);
    http_parser_destruir(p);
    T_ASSERT(r, tamanho_arquivo(tmp) < 0, "conexao caiu e o temporario ficou para tras: %s", tmp);
}
