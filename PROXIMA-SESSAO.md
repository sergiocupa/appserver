# Passagem de sessão — vídeo (MediaGateway / MediaFragmenter / player)

Estado em **2026-10-13**. Escrito para quem continuar o trabalho (pessoa ou agente) sem o
histórico da conversa. Tudo o que está aqui foi medido nesta sessão, salvo onde diz o contrário.

## 1. Estado do repositório

- Branch `main`, último commit `8118849`. **Nada desta sequência está commitado.**
- 30 arquivos modificados + 4 novos (2.344 linhas a mais), mais o ponteiro do submódulo do yason.
- Submódulos: `appserver/submodules/yason` em `c8ff0be` (commit novo, já no remoto, **ponteiro
  ainda não commitado no appserver**); `appserver/submodules/xplatbase` em `fd4db2a` (já commitado).
- Servidor de teste e proxy **parados**. Pastas e sessões de teste apagadas.
- Só o **Release x64** foi compilado e testado. O **Debug não foi recompilado** nesta sessão.

### Arquivos novos (precisam de `git add`)
`MediaGateway/media/aud_fmp4.c`, `aud_fmp4.h`, `mpd_util.h`, `test/appservertester/appservertester.json`

### Arquivos modificados, por assunto
| Assunto | Arquivos |
|---|---|
| Áudio no DASH, manifestos com tempo real | `sink_hls.c`, `sink_dash.c`, `mux_webm.c/.h`, `sync_gateway.c`, `aud_fmp4.*`, `mpd_util.h`, `MediaGateway.vcxproj`, `CMakeLists.txt` |
| Lista de edição e tempos do MP4 | `mp4_timing.c/.h`, `MediaFragmenter/src/media_source.c`, `sync_gateway.c` |
| MP4 de arquivo único (composição, lista de edição, VP9/AV1/H.265) | `mux_mp4.c/.h`, `sink_mp4.c`, `h26x_util.c` |
| Vazamentos | `MediaFragmenterType.c`, `Mp4Builder.c`, yason `c8ff0be` |
| Sessão e biblioteca | `frag_session.c/.h`, `hls_controller.c/.h`, `appservertester.c`, `appservertester.json`, `appservertester.vcxproj` |
| Servidor HTTP | `appserver.c` (`Cache-Control: no-cache` nos estáticos), `http_resposta.c`, `server_type.h` (status 409) |
| Player | `web/components/ui/player/video-player.js/.html/.css` |
| Teste | `test/unittests/test_pipeline.c` (espera `manifest-vp9.mpd`) |

## 2. Observações para o commit

1. **Incluir o ponteiro do yason** (`appserver/submodules/yason` → `c8ff0be`) junto com o código:
   sem ele o servidor volta a perder centenas de blocos por leitura de `session.json`.
2. **Não incluir** os submódulos que já estavam sujos antes desta sequência
   (`appserver/submodules/utility`, `streammanager/submodules/*`): não são deste trabalho.
3. Este arquivo (`PROXIMA-SESSAO.md`) é nota de passagem; commitar ou não é escolha sua.
4. Os avisos "LF will be replaced by CRLF" são só de fim de linha, inofensivos.
5. **Depois de atualizar o yason é preciso recompilar a `yason.lib`**: o `appservertester.vcxproj`
   liga a biblioteca pelo nome, sem referência ao projeto. Compilar só o servidor mantém a
   biblioteca antiga. Use "Recompilar Solução", ou compile `appserver\submodules\yason\yason\yason.vcxproj`.
6. **Mudanças de comportamento que quem usa a saída precisa saber:**
   - DASH em WebM mudou de nome: `manifest-vp9.mpd` / `manifest-av1.mpd`, `init-vp9-<res>.webm`,
     `chunk-vp9-<res>-N.webm` (idem `av1`). O `manifest.mpd` antigo não é mais gerado.
   - O preparo HLS grava também `master.mpd` (DASH sobre os mesmos segmentos).
   - Conversão para VP9/AV1 sai em **MP4** (`output-vp9.mp4`, `output-av1.mp4`), não mais em WebM.
   - `x64\<config>\` passa a receber `openh264.dll` e `libde265.dll` no build.
   - O servidor lê `appservertester.json` ao lado do executável (`web`, `port`).
7. **Não há teste automático novo no appserver.** As suítes passam (Windows 69/69, Linux 69/69),
   mas nenhum caso delas cobre o que foi corrigido aqui. Ver pendência 2.
8. Sugestão de título, no estilo dos anteriores:
   `video: audio no DASH (CMAF + AAC em faixa propria), lista de edicao do MP4, copia sem recodificar com quadros B, VP9/AV1 em MP4, H.265 no HLS, vazamentos, biblioteca e recuperacao no player; yason c8ff0be`

## 3. Pendências

### Vale corrigir
1. **Áudio que entra atrasado na origem** (faixa com edição vazia). O arquivo gerado está certo
   (áudio em 0,5000 s no teste), mas o player começa onde o áudio entra e o início do vídeo não
   aparece. Correção sugerida: preencher o começo da faixa de áudio com quadros AAC de silêncio
   nos sinks (`sink_hls.c`, `aud_fmp4.c`; no MP4 único já há edição vazia e não precisa).
   Para reproduzir: `ffmpeg -i a.mp4 -itsoffset 0.5 -i a.mp4 -map 0:v -map 1:a -c copy atraso.mp4`.
2. **Testes automáticos** para o que foi feito: `mp4_edit_offsets` e `mp4_video_times_us`
   (arquivo normal, cortado e com edição vazia); cópia sem recodificar com quadros B (ctts e
   elst no MP4 de saída); init `hvc1` e texto de codec no HLS em H.265; `hvcC` com nível correto;
   manifestos com `SegmentTimeline` a 29,97 fps; formatos na sessão (`frag_session_set_format`).

### Limitações conhecidas (toleráveis, têm correção)
3. **HLS sem recodificar de arquivo cortado** mostra o trecho que a origem esconde (saída 2,2 s
   mais longa, em sincronismo). Só nessa combinação. Ver o comentário "BYPASS" em `sync_gateway.c`.
4. **HLS sem recodificar** começa um quadro (41,7 ms) depois do zero; áudio e vídeo andam juntos.
5. **Texto de codec do H.264** nos manifestos é fixo (`avc1.640028`) para todas as resoluções.
   Montar do `avcC` em `vcodec_str` (`sink_hls.c`), como já é feito para o H.265.
6. `embed_transcode_webm` (`MediaFragmenter/src/embed_ops.c`) ficou **sem uso**. Decidir se sai.

### Não feito
7. **Espera de buffer no ao vivo**: não validada. O ao vivo só aceita câmera e esta máquina não
   tem nenhuma listada em `GET /api/media/devices`.
8. **H.265 no ao vivo**: `sink_live.c` ainda grava o init como H.264. Não mexi (ao vivo reservado).
9. **Áudio no ao vivo**: reservado pelo dono do projeto para outra sessão (envolve protocolo).
10. **Sincronia "de ouvido"**: só foi medida por tempos e pacotes; ninguém ouviu.
11. `streammanager/submodules/yason` continua no commit antigo, sem a correção de vazamento
    (o streammanager está inativo por ora).
12. Na origem do yason (`E:\git\libs\yason`) o `yasontester` não liga pela solução (falta a
    `Xplatbase.lib`); o teste foi compilado direto das fontes com `cl`.

## 4. O que foi feito (resumo)

- **DASH com áudio:** AAC original em faixa fMP4 própria (`aud_fmp4`), sem conversão. DASH H.264
  via `master.mpd` sobre os segmentos do HLS; DASH VP9 pelo caminho paralelo por segmento; DASH
  AV1 com tempo real de cada pedaço. Todos os manifestos usam `SegmentTimeline`.
- **Lista de edição do MP4:** a fonte entrega apresentação e decodificação por quadro
  (`mp4_video_times_us`) e o deslocamento do áudio (`mp4_edit_offsets`). Quadro com tempo
  negativo é pré-rolo: decodifica e descarta. Medido no trecho cortado: 577 quadros como a
  origem, vídeo −0,3 ms, áudio 0,00 ms.
- **Cópia sem recodificar:** `mux_mp4` grava `ctts` e `edts/elst` (`mp4_write_video2`,
  `mp4_set_audio_start`); segmentos levam deslocamento de composição (`mp4_write_segment_cts`).
- **VP9/AV1 em MP4** com o AAC original (`mp4_open_kind`, `vpcC`, `av1C`).
- **H.265:** DLLs dos plugins copiadas no build; init `hvc1` (`mp4_write_init_kind`); `hvcC` sem
  os bytes de prevenção de emulação; texto `hvc1.1.6.L120.90`.
- **Vazamentos:** tokens do `json_parse` (yason), SPS/PPS da fonte e tabelas no caminho de erro.
- **Player:** biblioteca com um botão por formato; recuperação de queda mantendo a posição;
  espera de buffer mínimo; rebaixamento de qualidade em rede lenta (inclusive na primeira carga);
  aviso de carregamento na abertura; salto no DASH que pede de novo o segmento abandonado.

## 5. Como compilar, rodar e testar

```
MSBuild appserver\submodules\yason\yason\yason.vcxproj -p:Configuration=Release -p:Platform=x64 -p:SolutionDir=E:\git\appserver\
MSBuild test\appservertester\appservertester.vcxproj  -p:Configuration=Release -p:Platform=x64 -p:SolutionDir=E:\git\appserver\
MSBuild test\unittests\unittests.vcxproj              -p:Configuration=Release -p:Platform=x64 -p:SolutionDir=E:\git\appserver\
vstest.console.exe x64\Release\tests\unittests.dll /Platform:x64
```
- Servidor: executar `x64\Release\appservertester.exe` com a pasta atual em `test\appservertester`
  (porta 1234; a pasta web vem do `appservertester.json`).
- Linux (WSL): `cmake -S /mnt/e/git/appserver -B ~/build/appserver-fase4 -G Ninja -DCMAKE_BUILD_TYPE=Debug -DXPLATBASE_SHARED=ON`,
  `cmake --build ~/build/appserver-fase4`, `ctest` na pasta de build. Chamar o WSL pelo
  PowerShell (o Git Bash estraga caminhos `/mnt/...`).

### Rotas úteis
| Rota | Uso |
|---|---|
| `POST /api/video/prepare` (`X-Hls-Folder: <pasta>`, corpo = MP4) | envia o vídeo; pasta fora de sessão não entra na biblioteca |
| `GET /api/video/prepare-stream/<pasta>` | prepara HLS (e `master.mpd`) |
| `GET /api/video/prepare-dash/<pasta>/<vp9\|av1>` | prepara DASH em WebM |
| `GET /api/video/convert/<pasta>/<h264\|h265\|vp9\|av1>` | arquivo único |
| `POST /api/session/create`, `config/<id>`, `delete/<id>`; `GET /api/session/list` | sessões |
| `GET /api/health` | `memory.poolLiveBlocks` (medir vazamento) |
| `GET /api/media/devices` | encoders e decoders carregados, com o motivo quando falta |

## 6. Como medir (os scripts de teste foram apagados; recriar conforme precisar)

- **Sincronia pelo conteúdo:** vídeo pelos instantes dos cortes de cena
  (`ffmpeg -copyts -i X -vf "scale=320:180,select='gt(scene,0.30)',showinfo" -f null -`); áudio
  pelo `pts` e tamanho de cada pacote (`ffprobe -show_entries packet=pts_time,size`). O `ffmpeg`
  está em `E:\app\ffmpeg-master-latest-win64-gpl-shared\bin` (ferramenta de medição, não é
  dependência do projeto).
- **Sem `-copyts` a medida engana:** o ffmpeg desconta o tempo inicial do arquivo.
- **Lista de edição:** ler a caixa `elst` direto; o primeiro pacote que o ffprobe lista não é o
  deslocamento da faixa.
- **Manifesto contra arquivos:** conferir que cada segmento listado existe e começa no tempo
  declarado (`tfdt` no fMP4, `Timecode` do `Cluster` no WebM). Testar também a 29,97 fps.
- **Vazamento:** `poolLiveBlocks` antes e depois de N repetições. O `liveDelta` do
  `session.json` engana (a janela começa depois de a fonte ser aberta).
- **Rede lenta:** proxy TCP local 1235 → 1234 com limite em KB/s. Segmento já baixado volta do
  cache e faz o player achar que a rede é rápida: saltar para trecho não assistido ou repreparar.
- **Navegador:** o embutido reporta a página como oculta; o componente é
  `document.querySelector('video-player')`, com os controles no `shadowRoot`.

## 7. Dados de teste

- Biblioteca (`test/appservertester/web/hls`, fora do git): `29ae719e3df1e2bd` (3:51, 1080p,
  24 fps, com áudio: HLS, DASH, DASH VP9) e `98652b380317f30a` (20 s, 4K, 29,97 fps, sem áudio:
  HLS, DASH, DASH VP9, DASH AV1). `current` e `fluxo1` são anteriores a esta sequência.
- Trecho cortado de teste: `ffmpeg -ss 30 -t 24 -i <fonte de 3:51> -c copy clip24.mp4`
  (lista de edição: vídeo −2,167 s, áudio −2,229 s).
