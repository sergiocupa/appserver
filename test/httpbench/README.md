# httpbench — medicao do servidor HTTP

Mede o appserver **de fora**, como cliente, e compara duas versoes com rodadas pareadas.

- `bench_server` — servidor minimo (ping, eco de corpo, rota lenta, SSE, arquivos estaticos).
  Mede o servidor, nao as aplicacoes em cima dele.
- `httpbench` — sobe o `bench_server` como processo filho (um processo novo por cenario) e
  mede vazao/latencia do lado do cliente e **threads, CPU e memoria do servidor**.

## Uso

```
httpbench --exe <bench_server> [--exe <outro bench_server>] [--rotulo A] [--rotulo B]
          --web <pasta> [--rodadas N] [--saida <prefixo>] [--filtro cen1,cen2] [--porta P]
          [--env-a K=V] [--env-b K=V]
```

- Uma versao: mediana / min / max de cada metrica.
- Duas versoes: rodadas pareadas com ordem alternada; efeito de Hodges-Lehmann (B/A), IC 95%
  por bootstrap, Wilcoxon exato + Benjamini-Hochberg, veredito com margem de
  nao-inferioridade (vazao 5%, p50 10%, p99 15%, recursos 10%).
- `--env-a` / `--env-b K=V` (ate 4 cada): variavel de ambiente so do servidor daquela versao.
  Com o MESMO `--exe` nas duas, compara duas configuracoes do mesmo binario (isola o efeito
  de uma opcao, sem diferenca de codigo nem de caminho do executavel).
- `HTTPBENCH_DEBUG=1`: threads / memoria / CPU do servidor ao fim de cada cenario.
- Saida: `<prefixo>.txt` (tabela) e `<prefixo>.tsv` (bruto: versao, rodada, metrica, valor).

## Cenarios

| cenario | o que mede |
|---|---|
| processo | threads, memoria e CPU com o servidor parado |
| ping_ka_c1 / ping_ka_c32 | GET pequeno, keep-alive, 1 e 32 clientes |
| ping_nova_c8 | conexao nova a cada requisicao |
| estatico_64k_c8 / estatico_4m_c4 | arquivo estatico pequeno e grande |
| post_1m_c4 | corpo de 1 MB |
| pipeline | 20 requisicoes num unico envio: respostas na ordem? |
| ociosas_200 | 200 conexoes paradas: threads, CPU, memoria |
| sse_50 / sse_200 / sse_1000 | 50, 200 e 1000 assinantes SSE: threads, CPU, atraso de entrega (lido por uma thread do cliente: com 1000, a leitura do proprio cliente entra no atraso, igual para as duas versoes) |
| lento_mix | rotas de 200 ms misturadas com pings |
| cliente_lento | 20 clientes que pedem 4 MB e nao leem |
| upload_200m | upload de 200 MB: vazao e pico de memoria |

Cada cenario tambem registra `cpu_us_req` (CPU do servidor por requisicao) e
`rss_cresce_MB` (crescimento de memoria no periodo).

## bench_server: variaveis de ambiente

| variavel | efeito |
|---|---|
| `BENCH_PERFIL=economia\|performance` | perfil do pool (`Config.PerfilPool`); sem ela, o padrao da biblioteca |
| `BENCH_SSE_VOLTA_US=N` | `Config.SseVoltaUs` (0 = entrega SSE sempre em serie) |
| `BENCH_SSE_MAX_PISTA=N` | `Config.SseMaxPorPista` |
| `BENCH_PUB_MS=N` | intervalo do publicador do relogio SSE (padrao 100 ms) |
| `BENCH_PUB_LOG=1` | a cada 50 publicacoes, no stderr: tempo de `app_publicar` (entrega a todos) p50/p90/max e pistas |

## Windows: Topaz OFD (Warsaw)

Nesta maquina o modulo de seguranca de banco injeta `wslbscr64.dll` (+ USER32/SHELL32/GDI) no
processo conforme o CAMINHO do executavel, e passa a injetar em executaveis novos depois de
algumas execucoes: +4 MB de memoria parada. Antes de comparar, conferir os modulos dos dois
servidores; o mais seguro e o mesmo binario nas duas versoes (`--env-a`/`--env-b`).

## Windows: inicializacao do xplatbase

Em Release, a auto-inicializacao do xplatbase (`.CRT$XCU`) e descartada pelo linker e o
processo sobe sem pool nem ganchos do memory_pool. O servidor novo chama `platform_init()`
sozinho; o binario "antes" guardado nao. Para comparar com a mesma inicializacao, os dois
rodam com a `Xplatbase.dll` de `x64/Release/bench_antes_init/` (compilada por `build.bat`,
chama `platform_init` no `DllMain`).

## Resultados

`resultados/` e local (ignorado pelo git): cada execucao grava `<prefixo>.txt` e `.tsv`. O
sufixo diz o sistema (`_windows`/`_linux`) e o perfil do pool (`_economia`/`_performance`);
`_15r` = 15 rodadas.

| prefixo | o que compara |
|---|---|
| `final2_*` | **original x atual, todos os cenarios (06/10, depois de todas as correcoes): a referencia** |
| `base_*` | so o servidor antigo (29/09) |
| `final_*`, `fim_*` | original x servidor novo em 29/09 e 05/10 (no Windows, `fim_*` tem o Warsaw injetado so no novo) |
| `volta_*`, `balanco_*`, `leitores_*`, `bateria_*` | variantes intermediarias do reator, entre si |
| `sse_fim_*` | entrega SSE em serie x em pistas, mesmo binario (`--env-a`) |
| `sse_justo_*` | SSE justo: servidor ANTIGO (commit 82169db + xplatbase 166831d) com uma thread por assinante mas UM evento para todos (publicador acorda as threads) x atual. `sse_justo_validacao_*` confirma que o antigo recompilado equivale ao original guardado. Contra o original puro o atraso nao e comparavel: la cada thread gerava o proprio evento, sem distribuicao |
| `sse_*` (sem `fim`) | primeira medicao das pistas SSE, AINDA com o defeito do pool corrigido no xplatbase b398dcd: os numeros de CPU nao valem |
| `pool_parado_*` | pool parado no perfil economia, antes x depois (xplatbase e0337cb) |
| `post_reserva_*` | corpo em memoria reservado pelo Content-Length, antes x depois |
| `arquivo_parado_*` | bloco do envio de arquivo solto quando o cliente para de ler (Windows), antes x depois |
| `ref_item2_*`, `ref_itens34_*` | REFERENCIAS (15 rodadas, original x base de 06/10) dos pontos ainda em aberto: Linux PERFORMANCE com conexao nova por requisicao, rotas lentas e clientes lentos; Windows clientes lentos e SSE com 50 assinantes. Base guardada em `x64/Release/bench_base_0610/` e `~/build/bench_base_0610/` (WSL): para medir uma correcao, comparar base x novo, pareado |
| `fim3_0710_*` | base de 06/10 x novo de 07/10 (rota antes do arquivo estatico, `Range`, xplatbase com timer solto em ECONOMIA parado e giro curto do core acordado sem tarefa), todos os cenarios. Novo guardado em `x64/Release/bench_novo_0710/` (exe + `Xplatbase.dll` da origem) |
| `orig3_0710_*` | original x novo de 07/10, so pings (`ping_ka_c1`, `ping_nova_c8`, `lento_mix`, `cliente_lento`) |
| `giro_0810_*` | xplatbase f2ecda3 x giro limitado no PERFORMANCE (quem submete nao acorda core se ja ha um girando; despertar em cadeia), mesmo appserver, todos os cenarios. `giro_conf_0810_*` = confirmacao com 10 rodadas dos pontos inconclusivos; `orig_giro_0810_*` = original x giro limitado, so pings |
| `range_cond_0810_*` | arquivos estaticos antes x depois de ETag/Last-Modified, 304 condicional, If-Range e varias faixas (multipart/byteranges): custo no caminho do estatico |
| `saida_tarefa_*` | envio com orcamento e tarefa de envio no pool, antes x depois; `saida_tarefa_original_*` = original x depois (arquivo de 4 MB) |
