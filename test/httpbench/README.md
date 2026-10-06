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

`resultados/`: `base_*` (so o servidor antigo), `final_*` e `fim_*` (antes x depois, pareado),
`sse_fim_*` (entrega SSE em serie x em pistas, mesmo binario).
