//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Rota de saude EMBUTIDA na biblioteca: qualquer aplicacao que chame appserver_create
//  ganha /<prefixo>/health sem registrar nada. Fica aqui, e nao no appservertester,
//  porque saude e do SERVIDOR -- e a camada mais alta que conhece o processo inteiro.
//
//      GET  /api/health             amostra unica, JSON. A UI consulta em intervalo.
//      POST /api/health/leak-scan   dispara a varredura de alcancabilidade e devolve
//                                   o fim do mem_leak_watch.log.
//
//  Por que consulta periodica e nao SSE: o painel quer UM numero a cada segundo, nao um
//  fluxo de eventos. SSE nesta casa e para job longo com progresso (ver hls_controller);
//  usar aqui seria segurar uma conexao por aba aberta para transmitir um snapshot.
//
//  AVISO 1 -- leak-scan e PERIGOSO num processo com xplatbase duplicado.
//  Medido: num processo limpo a varredura volta em ~0,4 s e o servidor segue atendendo,
//  inclusive com consultas concorrentes. Mas quando o appservertester carrega
//  codec_h264_plugin.dll, entra junto a Xplatbase.dll e o detector do proprio xplatbase
//  acusa "SEGUNDA INSTANCIA NO MESMO PROCESSO" -- duas copias, cada uma com seu pool e seu
//  registro de threads. Nesse estado a varredura TRAVOU o servidor (sem resposta em 20 s).
//  Faz sentido: ela suspende as threads registradas e percorre os spans do pool, e com
//  dois registros ela suspende thread de uma copia enquanto le o pool da outra.
//  Enquanto a duplicacao existir, a varredura fica atras de confirmacao no painel.
//
//  AVISO 2: esta rota NAO tem autenticacao. Ela expoe contadores internos do processo
//  (memoria, CPU, caminho do log de vazamento). Antes de expor este servidor fora de uma
//  rede de confianca, ela precisa entrar no mesmo controle das demais rotas de api.

#include "../include/appserver.h"
#include "server_type.h"
#include "utils/health_monitor.h"

#include <stdio.h>
#include <string.h>

/* O fim do log basta para ver a varredura que acabou de rodar; o arquivo inteiro pode
 * ter megabytes de historico e nao cabe numa resposta de painel. */
#define LEAK_TAIL_BYTES 8192


static void rota_em(Message* message, int idx, char* out, int out_size)
{
    if (out_size > 0) out[0] = 0;
    if (idx < 0 || idx >= message->Route.Count) return;
    StringX* s = (StringX*)message->Route.Items[idx];
    int n = s->Length < out_size - 1 ? s->Length : out_size - 1;
    memcpy(out, s->Content, n);
    out[n] = 0;
}

static int indice_de(Message* message, const char* alvo)
{
    int tam = (int)strlen(alvo);
    for (int i = 0; i < message->Route.Count; i++)
    {
        StringX* s = (StringX*)message->Route.Items[i];
        if (s->Length == tam && memcmp(s->Content, alvo, tam) == 0) return i;
    }
    return -1;
}

/* Escapa o que vai dentro de uma string JSON. O log de vazamento tem barras invertidas
 * (caminhos) e quebras de linha: jogar cru no JSON produziria resposta invalida. */
static int json_escapa(const char* src, int src_len, char* dst, int dst_size)
{
    int o = 0;
    for (int i = 0; i < src_len && o < dst_size - 7; i++)
    {
        unsigned char c = (unsigned char)src[i];
        switch (c)
        {
            case '"':  dst[o++] = '\\'; dst[o++] = '"';  break;
            case '\\': dst[o++] = '\\'; dst[o++] = '\\'; break;
            case '\n': dst[o++] = '\\'; dst[o++] = 'n';  break;
            case '\r': dst[o++] = '\\'; dst[o++] = 'r';  break;
            case '\t': dst[o++] = '\\'; dst[o++] = 't';  break;
            default:
                if (c < 0x20) { o += snprintf(dst + o, (size_t)(dst_size - o), "\\u%04x", c); }
                else dst[o++] = (char)c;
        }
    }
    dst[o] = 0;
    return o;
}

static void responde_json(Message* message, int status, const char* json, int len)
{
    message->Response = message_response_create_content(status, APPLICATION_JSON, (char*)json, len);
}


Element* appserver_health_route(Message* message)
{
    int base = indice_de(message, "health");
    if (base < 0)
    {
        const char* e = "{\"error\":\"rota invalida\"}";
        responde_json(message, HTTP_STATUS_BAD_REQUEST, e, (int)strlen(e));
        return 0;
    }

    char acao[40];
    rota_em(message, base + 1, acao, sizeof(acao));

    /* ---- varredura de vazamento, sob demanda ---- */
    if (strcmp(acao, "leak-scan") == 0)
    {
        static char log[LEAK_TAIL_BYTES];
        static char esc[LEAK_TAIL_BYTES * 2 + 8];
        static char json[LEAK_TAIL_BYTES * 2 + 256];

        int n = health_monitor_leak_scan(log, (int)sizeof(log));
        json_escapa(log, n, esc, (int)sizeof(esc));

        int len = snprintf(json, sizeof(json),
            "{\"ran\":%s,\"bytes\":%d,\"log\":\"%s\"}",
            n > 0 ? "true" : "false", n, esc);
        responde_json(message, HTTP_STATUS_OK, json, len);
        return 0;
    }

    /* ---- amostra ---- */
    {
        HealthSample h;
        char json[1024];

        if (!health_monitor_sample(&h))
        {
            const char* e = "{\"error\":\"amostra indisponivel\"}";
            responde_json(message, HTTP_STATUS_INTERNAL_ERROR, e, (int)strlen(e));
            return 0;
        }

        int len = snprintf(json, sizeof(json),
            "{"
              "\"cpu\":{\"percent\":%.1f,\"cores\":%d},"
              "\"memory\":{"
                  "\"rssBytes\":%llu,\"virtualBytes\":%llu,"
                  "\"poolLiveBlocks\":%lld,\"poolAllocCount\":%llu,\"poolFreeCount\":%llu,"
                  "\"poolReservedBytes\":%llu,\"poolCachedChunks\":%llu,\"poolPurgeCount\":%llu"
              "},"
              "\"gpu\":{\"available\":%s,\"percent\":%.1f,\"videoPercent\":%.1f,\"memoryBytes\":%llu},"
              "\"leak\":{\"level\":%d,\"floorDelta\":%lld,\"floorPerMin\":%lld,\"windowSec\":%d},"
              "\"leakWatch\":{\"available\":%s}"
            "}",
            h.CpuPercent, h.CpuCores,
            (unsigned long long)h.RssBytes, (unsigned long long)h.VirtualBytes,
            (long long)h.PoolLiveBlocks,
            (unsigned long long)h.PoolAllocCount, (unsigned long long)h.PoolFreeCount,
            (unsigned long long)h.PoolReservedBytes, (unsigned long long)h.PoolCachedChunks,
            (unsigned long long)h.PoolPurgeCount,
            h.GpuAvailable ? "true" : "false", h.GpuPercent, h.GpuEncodePercent,
            (unsigned long long)h.GpuMemoryBytes,
            h.LeakLevel, (long long)h.LeakFloorDelta, (long long)h.LeakFloorPerMin, h.LeakWindowSec,
            h.LeakWatchAvailable ? "true" : "false");

        responde_json(message, HTTP_STATUS_OK, json, len);
        return 0;
    }
}
