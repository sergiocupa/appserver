# Refatoração media/codecs/gateway — histórico e pendências

Handoff para retomar em nova sessão. **Nada commitado.** Build atual: **VERDE** (Debug|x64).

---

## Objetivo geral
Separar em camadas, dependência **one-way**:

```
codecs  ←  MediaGateway  ←  MediaFragmenter  ←  appservertester
(codec)     (conversão)      (orquestração)       (app/teste)
```

- **codecs**: implementações de codec + adaptadores + decode + structs de codec.
- **MediaGateway** (a criar): conversão `convert_input → bus → convert_output`, usa codecs.
- **MediaFragmenter**: orquestra o MediaGateway; contém demux/builders MP4.

---

## O que já foi feito (nesta sessão)

### Codecs multiplataforma / build
- **libyuv** virou lib build-once: `codecs/libyuv.vcxproj` (StaticLibrary Debug+Release, `/MP`, 15 `.cc`), na pasta de solution "Codecs" **sem `Build.0`**. `appservertester` linka `libyuv.lib`. Corrige rescale das renditions (`HAVE_LIBYUV`).
- **codecs** agora é **StaticLibrary em Debug E Release** (antes Release era DLL+`.def`). Sem `codecs.def`/Link. `SvtAv1Enc.lib`/`x265-static.lib` são linkados pelos **consumidores** (appservertester), não embutidos.
- `appservertester.vcxproj`: `/MP` ligado (build caiu de ~5min → ~30s). Release linka `SvtAv1Enc.lib;x265-static.lib` e não copia mais DLL.
- **Removida `codecs.lib` velha** em `x64/Debug` (resquício pré-OutDir) que o linker pegava antes de `codecs\x64\Debug` → causava `unresolved`.
- `CMakeLists.txt` multiplataforma dos codecs (Linux x64/ARM64) validado antes via WSL+QEMU (ver `codecs/CMAKE-README.md`).

### Etapa 1 — codecs absorveu a camada de codec (FEITO, verde)
- Movidos para **`codecs/media/`**: `media_codec.c/.h` (interface `MediaEncoder`/`MediaDecoder` + structs `MediaPacket/MediaFrame/MediaEncoderParams`) e os 5 adaptadores `codec_h264/h265/vp9/av1/opus.c`.
- `codecs.vcxproj`: defines `HAVE_LIBOPUS/LIBVPX/SVTAV1/X265/OPENH264`; include dirs svtav1 API, x265, openh264, xplatbase src, de265, `media`.
- `appservertester` não compila mais esses 6 (linka de `codecs.lib`); ganhou include `codecs\media`.

### Etapa 1b — decode H26x + structs de imagem → codecs (FEITO, verde)
- Criado **`codecs/media/codec_video.h`**: structs `MediaType/DecoderInstance/ImagePlane/ImagePlaneList/MediaBuffer` + protótipos `h26x_decode_frames/decoder_create/decoder_release` + `imagep_list_*`.
- Criado **`codecs/media/codec_video.c`**: decode H26x (openh264/de265) + `imagep_list_*` self-contained (movidos de `VideoCodecWrapper.c` e `MediaFragmenterType.c`).
- `MediaFragmenter/src/VideoCodecWrapper.c` **reescrito**: só as funções de builder (`h26x_create_annexb`, `h26x_put_single_frame`, `h26x_put_fragment`) que usam `VideoMetadata`/builders.
- `MediaFragmenter/src/MediaFragmenterType.h`: as 5 structs viraram `#include "codec_video.h"`.
- `MediaFragmenter/src/MediaFragmenterType.c`: `imagep_list_*` removidas (agora no codecs).
- `MediaFragmenter.vcxproj`: include `codecs\media` adicionado.
- **Resultado:** `codecs` self-contained (sem dep de MediaFragmenter); MediaFragmenter → codecs (one-way). Build verde (codecs + MediaFragmenter + appservertester).

### Paralelismo da fragmentação DASH (no `sync_gateway.c`, hoje em appservertester/media)
- Caminho ativo p/ DASH-WebM sem áudio: **`gw_dash_segpar`** (decode-once + buffer de K segmentos + tasks `(rendition × segmento)` no pool do xplatbase, 1 thread/task). Progresso e "pronto" **por pista**. **32min → ~16min.**
- Threads por codec calculados pelo **limite real** (`codec_thread_limit`): VP9=tile columns por largura; AV1/H265=cores; H264=~4 (slices). Orçamento = **núcleos físicos** (`phys_cores`), escalonador rolante empacota as maiores primeiro.
- **Código morto** `gw_dash_phased` + `pr_run_one` ainda no arquivo (re-decodificava 6× por pista; foi substituído pelo segpar). **Chamam `source_mp4_open`** — remover antes da Etapa 2.
- **H264 threading revertido** (`codec_h264.c`): `iMultipleThreadIdc`/`SM_FIXEDSLCNUM_SLICE` causava crash (`pNalLengthInByte` inválido no `h26x_recv`); voltou ao single-slice estável.
- **Crash H264 = mismatch de ABI do OpenH264 (CORRIGIDO):** header `deps/openh264/include` era **2.6.0** mas a DLL/lib vendorizada é **2.5.1** (`openh264-2.5.1-win64.dll`). O 2.6.0 adicionou `float rPsnr[3]` ao FINAL de `SLayerBSInfo`; como `SFrameBSInfo` embute `sLayerInfo[128]` **inline**, o stride entre camadas mudou (40→56) e desalinhou `sLayerInfo[l>=1]`. Num IDR (2 camadas), `pNalLengthInByte` da camada 1 era lido no offset errado → `{iSubSeqId=0, iNalCount=1}` = `0x1_00000000` → AV em `h264_recv:65`. **O crash reproduz em SERIAL** (não era concorrência — a tentativa de gate no pool por codec foi revertida). Fix: comentado `rPsnr[3]` em `codec_app_def.h` p/ casar o layout com a 2.5.1. VP9/AV1 não sofrem (compilados do fonte). Bug **pré-existente** (path H264/HLS nunca validado; o testado era DASH-WebM). **Fix definitivo:** alinhar versão header↔DLL (trocar a DLL/lib por 2.6.0 OU usar headers 2.5.1).
- **GOTCHA de build:** alterar `deps/openh264/include/.../codec_app_def.h` **NÃO** dispara recompilação incremental do projeto `codecs` (dependência de header não rastreada). Rodar **Rebuild do `codecs`** (`msbuild codecs\codecs.vcxproj -t:Rebuild ...`) e depois relinkar o appservertester.
- **H264 sem GOP periódico → HLS de 1 segmento gigante (CORRIGIDO):** o `h264_encoder_open` não setava `uiIntraPeriod` → OpenH264 emitia **só 1 IDR no início**. O corte de segmento HLS/MP4 (`emit_vpkt`/`sink->Cut`) só corta em **keyframe** após `seg_ms`; sem IDR periódico, nunca cortava → `<name>-0.m4s` com o vídeo inteiro (311s) e `#EXTINF:311` vs `#EXT-X-TARGETDURATION:6` → player não inicia. Fix: `param.uiIntraPeriod = fps*2` (GOP ~2s). (DASH-WebM não sofria: o segpar usa 1 encoder por segmento, keyframe implícito no início de cada.) `codecs` não é ProjectReference do appservertester → após mexer em `codec_*.c`, buildar `codecs` e depois relinkar appservertester.
- **H264 travava no 2º IDR (~2s) = `eSpsPpsIdStrategy` (CORRIGIDO):** com IDR periódico, o player tocava o 1º GOP e morria com `PIPELINE_ERROR_DECODE` no 2º keyframe. O default do OpenH264 **incrementa o id de SPS/PPS a cada IDR** (INCREASING_ID); o muxer põe SPS/PPS só no `avcC` (id 0) e o `h26x_tl` remove os SPS/PPS in-band das amostras → IDRs 2+ (id 1,2,…) ficam sem SPS → decode falha. Fix: `param.eSpsPpsIdStrategy = CONSTANT_ID` em `h264_encoder_open`. **Validado end-to-end** (hls.js 1.7.1 no browser: buffer até 103s sem erro, seeks a 10/30/60/90s `readyState=4` sem erro, frames decodificados/renderizados). Diagnóstico usou harness `web/hlstest.html` (removido) + parser Python dos moof/mdat.
- **Menor (não corrigido):** `sink_hls.c` hardcoda `CODECS="avc1.640028"` (High 4.0) no master.m3u8, mas as pistas são **Baseline** (`0x42`) com levels variados (144p=1.2 … 1080p=4.0). hls.js tolera (over-declara), tocou normal. Ideal: derivar `avc1.PPCCLL` por rendition dos bytes [1..3] do `avcC` (`t->conv.cfg`).
- **Troca de resolução HLS não pegava (CORRIGIDO, front-end — 2 causas):**
  1. **Causa raiz — `open_hls` preferia HLS NATIVO ao hls.js.** A ordem checava `video.canPlayType('application/vnd.apple.mpegurl')` **antes** de `Hls.isSupported()`. Chromium/Edge (e o browser embutido) reportam `canPlayType('mpegurl')="maybe"` → entrava no ramo nativo (`video.src=master`), deixando **`this.hls=null`**. Aí `on_resolution_changed` (condição `this._hlsMaster && this.hls?.levels?.length`) falhava e caía no `else` → status "**{h}p ainda está sendo fragmentada**", sem trocar nada. HLS nativo não expõe API JS p/ fixar nível. Fix: **inverter a ordem** em `open_hls` — `Hls.isSupported()` primeiro (dá `currentLevel`), nativo só como fallback (Safari). Confirmado no browser: agora `this.hls` existe, `levels=[144…1080]`, e a troca alcança o ramo master.
  2. **Imediatismo do quadro exibido.** `currentLevel` recarrega o buffer só do `currentTime` pra frente; com o vídeo inteiro bufferizado (arquivos locais) ou pausado, o quadro atual não re-decoda. Fix: **nudge** mínimo em `video.currentTime` (`±0.05s`, clampado) após setar o nível. Tocando, entra no novo nível na hora (confirmado: seek pós-troca → `videoWidth` 1920→256).
  - Só JS, servido estático → **hard-refresh (Ctrl+F5)**, sem rebuild. O hls.js ordena níveis por bitrate ASC (0=144p…5=1080p); o mapeamento por `height` no player já lida com isso.
- **`appclient_send` endurecido (CORRIGIDO):** fazia `send()` **único** e só tratava `< 0` como erro — um `send()` parcial (retorna menos que `length`) truncaria respostas grandes (segmentos HLS de MB); e o caminho websocket **vazava** o buffer de `websocket_encode_text` (alocado via `memop_alloc_raw`, nunca liberado). Fix (`appserver/src/appclient.c`): helper `appclient_send_all` com laço até escoar todos os bytes (retry em `WSAEINTR`) usado nos dois caminhos, + `memop_free_raw(webs)`. Smoke test: master `200` e segmento 1080p de 3.67MB entregue **completo**. **Nota:** o travamento observado nos testes (curl `http=000` com processo vivo, sob MUITAS conexões do browser) provavelmente vem de **outra** frente — thread-por-conexão + `send()` blocking numa conexão cujo peer parou de ler prende a thread; endurecer isso (timeout/limite de conexões, ou I/O não-bloqueante) fica pendente e é separado deste fix.

### Frontend (web) — correções já aplicadas
- `VideoViewer.html`: passou a carregar `hls.min.js` + `dash.all.min.js` (só o index tinha) → player DASH funciona.
- `video-player.js`: troca de resolução DASH corrigida (mapeia por altura + fallback ordenado + retry); progresso **por pista** (evento com `name`); guarda em `track-done`; tempo total de fragmentação exibido (elemento `#frag-time`).
- `hls_controller.c`: emite `GW_EV_PROGRESS` por pista (`{name,percent}`) e global; `elapsed` no `done`.

---

## Estado atual dos arquivos

Cadeia one-way realizada: **codecs ← MediaGateway ← MediaFragmenter ← appservertester**.

- `codecs/media/`: `media_codec.c/.h`, `codec_opus/vp9/av1/h264/h265.c`, `codec_video.c/.h`.
- `MediaGateway/media/` (nucleo de conversao): `sync_gateway.c/.h`, `media_sink.c/.h`, `sink_mp4/hls/dash.c`, `mux_webm.c/.h`, `mux_mp4.c/.h`, `audio_aac.c/.h`, `mp4_timing.c/.h`, `h26x_util.c/.h`, `gw_types.h`, `gw_base.h`, `media_time.h`, `media_source.h` (interface), `pipeline.h` (PipeTrack).
- `MediaFragmenter/src/` (orquestracao, Etapa 3): `media_source.c` (impl MP4 via demux), `pipeline.c`, `embed_ops.c` + `MediaFragmenter/include/embed_ops.h`. Mais `VideoCodecWrapper.c`: só builders (annexb/fragment).
- `test/appservertester/`: só `appservertester.c`, `hls_controller.c` (a pasta `media/` agora só tem os `.md` de doc).

---

## Etapa 2 — criar MediaGateway (FEITO, Debug|x64 verde)
- **Código morto removido** de `sync_gateway.c`: `gw_dash_phased`, `pr_run_one`, `pr_emit`, struct `PrJob` (chamavam `source_mp4_open`/`mp4_video_info`). Removido também o `#include "mp4_timing.h"` órfão. Ativo continua `gw_dash_segpar` (decode-once).
- **Novo projeto `MediaGateway/MediaGateway.vcxproj`** (StaticLibrary, Debug+Release x64, GUID `{6A7E0A00-…-A7E0}`, `OutDir=$(ProjectDir)x64\$(Configuration)`, define `HAVE_LIBYUV`). Include dirs via `$(ProjectDir)../` (robusto, não usa `$(SolutionDir)`): `media`, `../codecs/media`, xplatbase `/src` e `/include`, libyuv include.
- **Arquivos movidos** de `test/appservertester/media/` → **`MediaGateway/media/`**: `sync_gateway.c/.h`, `media_sink.c/.h`, `sink_mp4/hls/dash.c`, `mux_webm.c/.h`, `mux_mp4.c/.h`, `audio_aac.c/.h`, `mp4_timing.c/.h`, `h26x_util.c/.h` + headers `gw_types.h`, `gw_base.h`, `media_time.h`, `media_source.h` (interface), `pipeline.h` (PipeTrack).
- Em `sync_gateway.c`: `#include "MediaFragmenter.h"` → `#include "codec_video.h"`. MediaGateway **não** inclui nada de MediaFragmenter (one-way OK).
- **Ficaram na orquestração** (`test/appservertester/media/`): `media_source.c` (impl MP4 via demux), `pipeline.c`, `embed_ops.c/.h`.
- **`appservertester.vcxproj`**: removidos os 10 `.c` movidos (só compila `pipeline.c`/`embed_ops.c`/`media_source.c`); ProjectReference → MediaGateway; include dirs `$(SolutionDir)MediaGateway` (p/ os `media/xxx.h` do `hls_controller.c`) **e** `$(SolutionDir)MediaGateway\media` (p/ includes bare dos `.c` de orquestração); link `MediaGateway.lib` antes de `codecs.lib`, com dir `$(SolutionDir)MediaGateway\x64\$(Configuration)`.
- **`appserver.sln`**: MediaGateway adicionado COM `Build.0` (Debug/Release x64).
- **Validação:** `msbuild appserver.sln -t:appservertester -p:Configuration=Debug -p:Platform=x64` → **EXIT=0, zero erros**. `hls_controller.c` não precisou de edição (os `media/*.h` resolvem via `$(SolutionDir)MediaGateway`).
- **GOTCHA de validação:** compilar o `.vcxproj` do appservertester/MediaFragmenter **direto** (sem a `.sln`) quebra porque `$(SolutionDir)` fica indefinido — sempre validar **pela `.sln`**.
- **PCH Release do MediaFragmenter — CORRIGIDO:** `MediaFragmenter.vcxproj` Release|x64 tinha `PrecompiledHeader=Use pch.h` (Debug|x64 usa `NotUsing`) e ainda faltavam `AdditionalIncludeDirectories`/`<Lib>`. Release|x64 foi **alinhado ao Debug|x64** (NotUsing + include dirs codecs/sdl2/openh264/libde265 + Lib SDL2/de265/openh264). MediaFragmenter Release|x64 agora **compila** (verificado: erro saiu de C1010 → estágio de link).
- **Pendente p/ Release (Etapa 4, dependências build-once):** o link Release do appservertester ainda quebra por libs ausentes em Release — `yason.lib`/`dashstream.lib` só existem em `x64\Debug` (o link Release aponta yason para `x64\Debug`), e faltam `codecs/libyuv/SvtAv1Enc/x265-static` Release. Gerar via `build-codecs.bat` + buildar `yason`/`streammanager` Release e corrigir os lib dirs Release do appservertester quando chegar a Etapa 4. Baseline continua **Debug|x64 verde**.

## PENDÊNCIA — Etapa 2 (histórico do plano; já executado acima)

### Bloqueio descoberto
A **entrada** do gateway é acoplada ao MediaFragmenter:
- `media_source.c` guarda `FrameIndexList*` (tipo do MediaFragmenter) na struct e usa `mp4builder_get_frames` (demux MP4 do MediaFragmenter).
- `pipeline.c` e `embed_ops.c` são orquestração (usam `mp4builder_*`, `FrameIndexList`, `VideoMetadata`).

Mover esses para o MediaGateway criaria MediaGateway → MediaFragmenter (circular).

### Caminho recomendado (Option B — injeção da fonte)
`gateway_run(MediaSource* src, ...)` **já recebe a fonte por injeção**; o `gw_dash_segpar` decodifica lendo do `src` passado. Então:

1. **Remover código morto** `gw_dash_phased` + `pr_run_one` de `sync_gateway.c` (chamam `source_mp4_open` → arrastam `media_source.c`). O ativo é `gw_dash_segpar`.
2. **MediaGateway** (novo `MediaGateway/MediaGateway.vcxproj`, StaticLibrary, na solution COM `Build.0`) recebe: `sync_gateway.c`, `media_sink.c`, `sink_mp4/hls/dash.c`, `mux_webm.c`, `mux_mp4.c`, `audio_aac.c`, `mp4_timing.c`, `h26x_util.c` + headers `gw_types.h`, `gw_base.h`, `media_sink.h`, `sync_gateway.h`, `mux_webm.h`, `mp4_timing.h`, `pipeline.h` (só `PipeTrack`), **`media_source.h` (só a interface)**.
   - Nos movidos, trocar `#include "MediaFragmenter.h"` → `#include "codec_video.h"` (tipos/decode vêm dos codecs). `sync_gateway` já inclui `media_codec.h`.
   - Include dirs: `MediaGateway/media`, `../codecs/media`, xplatbase `/src` **e** `/include`, libyuv include. Define `HAVE_LIBYUV`.
3. **Ficam na orquestração** (appservertester/MediaFragmenter): `media_source.c` (impl MP4 via demux), `pipeline.c`, `embed_ops.c`. O orquestrador abre `source_mp4_open` e passa ao `gateway_run`.
4. **Wiring:** MediaGateway na `appserver.sln` (com `Build.0`), `appservertester` com ProjectReference → MediaGateway (build order + link) e remove da compilação os `.c` movidos; mantém `embed_ops.c`, `pipeline.c`, `media_source.c`. Include `MediaGateway\media` no appservertester (para `embed_ops`/`pipeline`/`media_source` acharem os headers do gateway).
5. Depois seguem as Etapas 3 e 4 (abaixo).

---

## Etapa 3 — MediaFragmenter orquestra o MediaGateway (FEITO, Debug|x64 verde)
- **Movidos** `test/appservertester/media/` → **`MediaFragmenter/src/`**: `media_source.c` (impl MP4 via demux `mp4builder`), `pipeline.c`, `embed_ops.c`. **`embed_ops.h`** → **`MediaFragmenter/include/`** (API pública de orquestração).
- **`pipeline.h`/`media_source.h` NÃO moveram** — ficam em `MediaGateway/media` (o gateway precisa de `PipeTrack`/interface `MediaSource`; os protótipos `pipeline_*`/`source_mp4_open` são inócuos lá). Dependência one-way preservada.
- **`MediaFragmenter.vcxproj`**: adicionados os 3 `.c` (+ `include\embed_ops.h`); ProjectReference → MediaGateway (ordem de build); em Debug|x64 **e** Release|x64 os include dirs `include`, `MediaGateway\media`, xplatbase `/src`+`/include`, libyuv `vpx\third_party\libyuv\include`, e o define **`HAVE_LIBYUV`** (senão pipeline/embed perdem o rescale libyuv). Os `.c` movidos usavam bare `#include "MediaFragmenter.h"` → resolvido pelo novo include dir `MediaFragmenter\include`.
- **`appservertester.vcxproj`**: removidos os 3 `.c` da compilação (agora só `appservertester.c`/`hls_controller.c`). Os símbolos vêm de MediaFragmenter.lib (orquestração) + MediaGateway.lib (conversão) + codecs.lib.
- **`hls_controller.c`**: includes trocados para bare (`pipeline.h`, `mp4_timing.h`, `sync_gateway.h` via `MediaGateway\media`; `embed_ops.h` via `MediaFragmenter\include`).
- **Ordem de link** (validada): libs de ProjectReference entram antes das `AdditionalDependencies`, e a ordem dos `ProjectReference` no vcxproj define a ordem relativa → MediaFragmenter.lib → MediaGateway.lib → codecs.lib (referrer antes do provider). `media_source.c` referencia `aac_*`/`mp4_video_info` do MediaGateway; resolvido no link final do appservertester.
- **Validação:** `msbuild appserver.sln -t:appservertester -p:Configuration=Debug -p:Platform=x64` → **EXIT=0**. Solution **inteira** em Debug|x64 também **verde** (sem quebra colateral em appserver/streammanager/testes).

## PENDÊNCIA — Etapa 4: appservertester só consome
Objetivo: o app de teste vira só cliente da orquestração.

1. **`appservertester.vcxproj`**: remover da compilação os `.c` que foram para MediaGateway/MediaFragmenter; manter só o código do app (`appservertester.c`, `hls_controller.c`, e o que for específico do teste).
2. Se `pipeline.c`/`embed_ops.c` foram para o MediaFragmenter (Etapa 3), remover daqui; senão manter.
3. Links do appservertester (ordem importa p/ static libs): `MediaFragmenter.lib` → `MediaGateway.lib` → `codecs.lib` → `libyuv.lib;SvtAv1Enc.lib;x265-static.lib;de265.lib;openh264.lib` + `yason/dashstream/xplatbase/SDL2`.
4. `hls_controller.c` deve chamar a API de orquestração (MediaFragmenter) em vez de montar `MediaProfile`+`gateway_run` direto — se quiser a separação completa; caso contrário pode continuar chamando `gateway_run` (MediaGateway) diretamente.
5. Build final da solution inteira em Debug e Release; validar runtime (fragmentar DASH/HLS, tocar, trocar resolução).

## Ordem de execução sugerida (resumo)
1. Etapa 2 (Option B) — criar MediaGateway com o núcleo de conversão + remover `gw_dash_phased`/`pr_run_one`.
2. Etapa 3 — MediaFragmenter passa a orquestrar (ProjectReference + mover `pipeline.c`/`embed_ops.c` se desejado).
3. Etapa 4 — appservertester só consome; build Debug+Release; validar runtime.
Cada etapa: **build verde antes de seguir** (não dá p/ testar runtime automaticamente — validar no VS a cada etapa).

### Gotchas de build já conhecidos
- Saída determinística: pinar `<OutDir>$(ProjectDir)x64\$(Configuration)\</OutDir>` (como codecs/libyuv) e limpar `.lib` velhas em `x64\Debug` para o linker não pegar a errada.
- Static lib não linka deps; quem linka é o consumidor final (appservertester): `codecs.lib;libyuv.lib;SvtAv1Enc.lib;x265-static.lib;de265.lib;openh264.lib;MediaGateway.lib`.
- `codec_video.h` não existe umbrella `libyuv.h` — usar `libyuv/scale.h`.
- `small` é macro do Windows (rpc) — não usar como identificador.

---

## Como compilar os codecs (build-once)
`codecs\build-codecs.bat` (SvtAv1Enc → x265-static → codecs → libyuv, Debug+Release). Ou botão direito nos projetos da pasta "Codecs". O build normal (F5) só linka.

## Feedback de progresso do DASH/VP9 (CORRIGIDO)
- **Sintoma:** ao fragmentar DASH·VP9 a UI ficava "congelada" por minutos (barras paradas, sem reação) — não dava pra saber se travou ou estava só lento. VP9 em software é **legitimamente lento** (~16min p/ o vídeo de 5min; gargalo é o encode VP9).
- **Causa:** `gw_dash_segpar` (`sync_gateway.c`) só emitia `GW_EV_PROGRESS` no **loop pós-leitura** (últimos KBUF=3 segmentos). O **loop de leitura** (decode+dispatch) é atrelado ao ritmo do encode pelo backpressure (SP_ACQUIRE espera slot livre) e leva quase todo o tempo — sem emitir nada. `done[r]` incrementava, mas só era lido no fim.
- **Fix:** emitir `GW_EV_PROGRESS` **global** (`e.Pts = pkt.Pts`, ~1x/segundo de conteúdo) dentro do loop de leitura, igual ao caminho HLS per-frame. O `sse_feedback` calcula `%` = Pts/duração. Validado: 23 eventos em 65s, subindo 6→7% (coerente com ~16min). Rebuild: `MediaGateway` + relink `appservertester`.
- **Diagnóstico "hung vs lento":** processo vivo + CPU alta (+344 CPU-s/30s ≈ 11 núcleos) + chunks `.webm` avançando em rajadas (233→305 em 90s) = **está codificando, não travado**. Só faltava o feedback.

## Codecs de terceiros movidos p/ dentro de `codecs/` (FEITO, Debug|x64 verde)
Antes, os fontes de terceiros ficavam em `MediaFragmenter/deps/` e o `codecs.vcxproj` listava 327 `.c` que fisicamente moravam lá (camada de baixo "subindo" na pasta de uma camada de cima). Agora cada codec real tem pasta física própria dentro de `codecs/`:
- `codecs/opus`, `codecs/vpx` (compilados no `codecs.vcxproj`), `codecs/openh264`, `codecs/libde265` (prebuilt linkados), `codecs/svtav1`, `codecs/x265` (CMake), `codecs/tools` (nasm). `codecs/media` continua com os adaptadores.
- **SDL2 NÃO moveu** (não é codec; é render do MediaFragmenter) → `MediaFragmenter/deps/sdl2` (única coisa que sobrou em `deps/`).
- **svtav1/x265 regenerados** com CMake (VS bundle 3.31.6) no novo local: `cmake -S codecs/<c>[/source] -B .../\_vsbuild -G "Visual Studio 17 2022" -A x64` + as opções do CMakeCache (svtav1: BUILD_APPS/DEC/SHARED/TESTING=OFF + nasm em `codecs/tools`; x265: ENABLE_ASSEMBLY/CLI/SHARED=OFF, HIGH_BIT_DEPTH=OFF). Isso gerou **GUIDs novos** → `appserver.sln` reapontado (path + GUID) para `SvtAv1Enc {7A6CCE8D-…}` e `x265-static {062D87F6-…}`. Saídas: SvtAv1Enc.lib → `codecs/svtav1/Bin/{Debug,Release}`; x265-static.lib → `codecs/x265/_vsbuild/{Debug,Release}`.
- **Caminhos atualizados** em: `codecs.vcxproj` (relativo: `opus/`, `vpx/`, `svtav1/Source/API`, `x265/source`, `x265/_vsbuild`, `tools/nasm.exe`), `libyuv.vcxproj`, `MediaGateway`, `MediaFragmenter`, `appservertester`, `DashServerTest`, `H26XDesktopRendererTest`, `build-codecs.bat`.
- **Filtros (pastas virtuais)** gerados: `codecs.vcxproj.filters` (media/opus/vpx + subpastas) e `libyuv.vcxproj.filters` — os `.c` aparecem agrupados por codec no Solution Explorer. (Cosméticos; o build não os usa.)
- **Validação:** codecs + libyuv (Rebuild), svtav1 e x265 (regenerados, buildam), appservertester, e a **solution inteira em Debug|x64 → verde**. Runtime das DLLs OK (openh264/de265/SDL2 já em `x64/Debug`).
- **GOTCHAS desta operação:** (1) o VS precisa estar FECHADO (handles na árvore `deps`); (2) `deps` é untracked no git — sem "desfazer", validar antes de apagar; (3) `sed` do git bash NÃO casa `\` de forma confiável — usar Python `.replace()` literal para paths com backslash; (4) o `yason.lib` deu LNK1104 transitório no full-build paralelo — passou no retry.
- **Pendente (Release):** só o Debug foi validado; para Release rodar `codecs\build-codecs.bat` (já reaponta os novos locais) — gera svtav1/x265 + codecs + libyuv Release.

## Pendências menores (opcionais)
- DASH ~16min: se quiser mais, menos renditions / AV1 preset alto / hardware. Gargalo é VP9 software.
- H264/H265 threading: `codec_thread_limit` tem os casos, mas `codec_h264` está single-slice (revert do crash); H265 (x265) honra `Threads` via `pools`.
- HLS/H264 e "Converter" H265 não passam pelo escalonador rolante (só DASH-WebM).

---

# Ferramenta de preparacao de video — Fase 0 (estabilidade) e Fase 1 (sessoes)

Baseline: **Debug|x64 verde** (solution inteira). Nada commitado.

## Plano acordado (7 fases)
0. Estabilidade (sem feature nova) · 1. Sessoes de fragmentacao · 2. Enumeracao de dispositivos
(rota unica, MSMF + V4L2) · 3. Fonte de camera · 4. Perfil de saida completo (FPS/bitrate/bypass)
· 5. Cobertura de codecs na ENTRADA (VP9 via libvpx, AV1 via **dav1d**, demux WebM) · 6. Backends
hardware→GPU→SIMD→threads com **SDKs de vendor (NVENC/QSV/AMF)** + VAAPI · 7. Endurecimento,
Release, CMake Linux. Decisoes do usuario: vendor SDKs, dav1d, **Linux em paralelo desde a Fase 0**.

## Fase 0 — FEITO
- **HANG corrigido (`gw_dash_segpar`)**: `seg_no` era incrementado no `SP_ACQUIRE`; um slot
  adquirido e nunca despachado (EOF logo apos o acquire) deixava `seg_no` > nro real de tasks e a
  espera final (`done >= seg_no`) nunca terminava. Agora `seg_no` avanca no `SP_DISPATCH`.
- **Busy-waits eliminados**: os dois `for (volatile int z...)` viraram
  `thread_wait_sleep_for_inline` (xwait_t da xplatbase); as tasks acordam a main
  (`thread_wait_wake_inline`) ao terminar. Teto de 20ms (backpressure) / 50ms (espera final).
- **Cancelamento cooperativo real**: `SpCtx.abort`/`failed`; `sp_task` sai cedo e nao cria chunk;
  o laco de leitura propaga. Novo evento **`GW_EV_CANCELLED`** (gw_base.h) + `gw_cancelled()`.
- **Saida nao e publicada quando abortada**: novo campo `MediaSink.Aborted` — `sink_hls`,
  `sink_dash` e `sink_mp4` liberam tudo mas NAO escrevem manifesto/playlist/mp4 nem emitem
  `GW_EV_DONE`. No segpar, o `manifest.mpd` tambem nao e escrito. Um manifesto apontando para
  segmentos inexistentes era o que "contaminava" a sessao seguinte.
- **Erros deixam de ser mudos**: `sp_seg_append` devolve 1/0/-1 (era `return` mudo em 3 casos);
  falha de encoder/mux na task marca `failed` e emite `GW_EV_ERROR`.
- **`gw_path.{h,c}` (novo, MediaGateway/media)**: `gw_path_join/normalize/mkdir_p/rmtree/
  file_exists/dir_exists/dir_list`, Win32 + POSIX. Todos os caminhos montados passaram a usar
  `'/'` (o Windows aceita) — mux_webm, sink_dash/hls/mp4, sync_gateway, hls_controller.
- **`system("if exist ... rmdir /s /q ...")` removido** do `hls_prepare_video` (abria shell,
  bloqueava a thread, nao reportava erro, Windows-only) -> `gw_rmtree`.

## Fase 1 — backend FEITO (UI pendente)
- **`frag_session.{h,c}`** (MediaFragmenter): uma subpasta por sessao sob `web/hls`, com
  `session.json` renderizado pelo **yason**. Estado 100% em DISCO (`idle|ready|running|done|
  cancelled|error`) + registro em memoria apenas dos jobs EM VOO (id -> `GwControl`, sob mutex).
  API: create/list/read/delete/dir/exists, setters de input/output/result/tracks,
  `job_begin`/`job_end`/`cancel`/`running`, e `frag_session_clear_output` (limpa a saida
  PRESERVANDO o session.json).
- **`yason_build.h`** (MediaFragmenter/include): montagem de arvore yason
  (`yb_root_object/str/int/num/bool/object/array/array_object/render/free`) — substitui o JSON
  por `sprintf_s` em `char[2048]`, que truncava calado.
- **`session_controller.{c,h}`** (appservertester) + rota `session`:
  `POST /api/session/create`, `GET /api/session/list|get/<id>|cancel/<id>|delete/<id>`.
  (O appserver nao encaminha verbo nem query-string, por isso a acao vai no caminho.)
- **`run_gateway_session()`** em `hls_controller.c`: as 3 rotas de fragmentacao passaram a rodar
  sob o registro de jobs, com `GwControl` de verdade (antes passavam `0` — fechar o EventSource
  no browser nao parava nada). Emite `{"type":"cancelled"}` no SSE.
- `hls_prepare_video` preserva o `session.json` (usa `frag_session_clear_output` quando a pasta e
  uma sessao; `gw_rmtree` no modo legado com `X-Hls-Folder`).

### Validado em runtime (nao so build)
create/list/get/cancel/delete OK; upload+probe numa sessao preserva o session.json; DASH·VP9
cancelado aos ~25s -> estado `cancelled`, SSE `cancelled`, **nenhum `manifest.mpd` escrito**,
`delete` remove a pasta inteira com os chunks parciais.

## BUGS ENCONTRADOS NO SUBMODULO yason (NAO corrigidos aqui — repo separado)
1. **Parser JSON ignora aspas ao tokenizar.** `:` `,` `{` `[` dentro de uma string quebram o
   padrao do `json_parse_object` e o **campo inteiro some** na releitura. Foi o que fazia
   `"created":"2026-09-09T02:00:11Z"` desaparecer. Contorno aplicado: timestamps em ISO 8601
   BASICO (`20260909T020011Z`) e `yb_str_plain()` trocando esses caracteres por `_` no que sera
   relido. **Efeito colateral visivel**: um nome de sessao com `:` ou `,` vindo do corpo da
   requisicao e descartado pelo parser (cai no fallback = id).
2. **Array/objeto VAZIO nao renderiza nada** (`json_render_array` so escreve com
   `Children.Count > 0`) -> gerava `"tracks":}`, JSON invalido. Contorno: `yb__fix_empty()` em
   `yb_render` converte nos vazios em campo literal `[]` / `{}`.
3. `json_render`: `if (root->Type = NODE_TYPE_OBJECT)` — atribuicao no lugar de comparacao; o no
   raiz e sempre forcado a objeto (array na raiz nao funciona).

## Pendente
- Fase 0 item 8: baseline de memoria por sessao (`GW_EV_MEM` ja existe; falta expor/assertar).
- Fase 1: UI (botao "Nova sessao", lista de sessoes, cancelar/excluir) e migrar o front de
  `X-Hls-Folder` para o id de sessao.
- Fases 2..7 conforme o plano.

### Gotcha de ferramenta (nao do projeto)
Heredoc do bash comendo `\` em scripts Python inline: usar `chr(92)` ou gravar o script em
arquivo antes de rodar.

---

# Fase 2 — Enumeracao de dispositivos (FEITO, validado em runtime)

## Modelo
Uma consulta so devolve a ARVORE inteira `device -> stream -> resolucao -> fps`. As opcoes
NAO sao independentes (a mesma camera da 1080p@30 em MJPG e so 5fps em YUY2); tres rotas
separadas por parametro entregariam combinacoes que o driver recusa.

## Arquivos
- **`MediaGateway/media/device_enum.{h,c}` (novo)** — backend **Media Foundation** (Windows) e
  **V4L2** (Linux) no mesmo arquivo, produzindo a mesma estrutura. Tabela de FOURCC unica
  (`device_map_fourcc`) marca `Raw`/`Codec`/`PixFmt` e um flag **`Supported`**: MJPG, VP9 e AV1
  aparecem na lista mas desabilitados (nao ha decoder JPEG/VP9/dav1d na cadeia hoje) — melhor
  do que sumir com a opcao e o usuario achar que a camera nao a oferece.
- **`test/appservertester/device_controller.{c,h}` (novo)** + rota `media/devices`:
  `GET /api/media/devices` e `GET /api/media/devices/<sessao>`. Com sessao, o `source.mp4` entra
  como um device de `kind:"file"` com UMA combinacao — mesmo shape das cameras, entao a UI tem
  um caminho de codigo so e apenas desabilita os campos.
- `captureAvailable` distingue "maquina sem camera" (true + lista vazia) de "MF/V4L2 nao subiu"
  (false). Antes qualquer falha na cadeia devolvia lista vazia, indistinguivel.

## Detalhes de implementacao que custaram tempo
- **`MFGetAttributeSize`/`MFGetAttributeRatio` sao inline SO em C++**; em C viram LNK2019. Os
  dois atributos sao UINT64 com dois UINT32 empacotados -> `mf_get_pair` le com
  `IMFMediaType_GetUINT64` e desempacota. Sem dependencia extra.
- **Subtipo -> FOURCC por padrao de GUID**: os subtipos de video do MF sao
  `{FOURCC-0000-0010-8000-00AA00389B71}`; derivar de `Data1` cobre qualquer formato, inclusive
  os que nao estao na lista de GUIDs nomeados do SDK.
- **`DevList` tem ~3 MB — NAO cabe na pilha.** Declarado como local, derrubava o processo
  (thread de requisicao tem 1 MB). `device_list_new/free` alocam no memory_pool.
- **fps em ordem decrescente**: a camera expoe um segundo stream descriptor (pino de foto) a
  1 fps; sem ordenar, 1 fps podia ser a primeira opcao da UI.

## BUG CORRIGIDO no `yason_build.h` (era JSON invalido)
O render do yason escreve o valor CRU entre aspas — nao escapa nada. O id de dispositivo do
Windows (`\?\usb#...\global`) saia com barras sem escape e o `JSON.parse` do browser falhava
(`Invalid \uXXXX escape`). Agora `yb_str`/`yb_array_str` escapam (`"`, `\`, controles -> `\uXXXX`)
via `yb__append_escaped`. Divisao clara: **`yb_str` = saida para o browser (escapada)**;
**`yb_str_plain` = valor que sera RELIDO pelo parser do yason (sanitizado)**.

## Validacao em runtime
- **Windows**: `GET /api/media/devices` -> "Integrated Camera" com 3 formatos (NV12/MJPG/YUY2),
  1280x720, fps `[30,1]` — confere exatamente com o dump direto do MF. Saida passa no
  `json.load`. Com sessao, o `source.mp4` aparece como device `file` (h264 1920x1080 @25, 311.5s).
- Harness de verificacao do MF (`scratchpad/mftest.c`, fora do repo): confirmou
  `MFStartup`/`MFEnumDeviceSources`/`ActivateObject`/`PresentationDescriptor`/`MediaTypeHandler`
  todos `S_OK`, e o reconhecimento do padrao de GUID.
- **Linux (WSL Ubuntu 24.04)**: `device_enum.c` + `gw_path.c` compilam e RODAM. `gw_path`
  verificado (mkdir_p recursivo, rmtree recursivo, dir_list, file/dir_exists, rmtree idempotente);
  `device_map_fourcc` confere para NV12/MJPG/H264/YUYV. V4L2 devolve 0 devices (WSL2 nao tem
  `/dev/video*`) — o backend nao foi exercitado contra hardware real.

## Ressalvas honestas
- O caminho de camera foi validado com **uma** camera (1 resolucao, 3 formatos). Cameras com
  varias resolucoes/faixas continuas de fps nao foram exercitadas.
- V4L2 compila e roda, mas **sem hardware** — nao ha garantia contra um driver real ainda.
- **Bloqueadores pre-existentes do build Linux da xplatbase** (nao sao deste trabalho):
  `src/numeric.c` linha 3 tem um `]` sobrando no `#include <stdlib.h>]`, e
  `numeric_parse_double` tem `char*` no .c contra `const char*` no .h (erro no gcc). Precisam
  ser corrigidos no submodulo antes do build Linux completo da Fase 7.

---

# Fase 0, item 8 — instrumentacao de memoria (FEITO) + VAZAMENTO REAL CORRIGIDO

## As duas ferramentas, e para que serve cada uma
1. **`memop_get_stats()` (balanco)** — responde "acumula entre sessoes?". Gravado agora no
   `session.json` como `memory: {liveDelta, osReservedDelta, allocs, frees}`, medido entre
   `frag_session_job_begin` e `job_end`.
2. **`mem_leak_watch` (alcancabilidade)** — responde "ha blocos que ninguem mais alcanca?",
   com backtrace de onde foram criados.

## DESCOBERTA sobre o mem_leak_watch
**O `platform_init()` JA chama `mem_leak_watch_start(NULL)`** (xplatbase.c:60) — chamar de novo
na aplicacao e no-op ("ja rodando"). O que faltava era DISPARAR: os limiares padrao sao
**70%/90% da RAM fisica**, entao o scan automatico praticamente nunca acontece. A integracao
correta e `mem_leak_watch_scan_now()` na fronteira que interessa. Agora, com
`set MEDIA_LEAK_WATCH=1`, o scan roda ao FIM DE CADA sessao (`frag_session_set_leak_scan`).
O log sai em `x64/Debug/mem_leak_watch.log` (TSV: ts_ms/level/kind/span/size/used/age_ms/site).

## VAZAMENTO ENCONTRADO E CORRIGIDO — um buffer POR FRAME de video
`h264_create_single_frame` / `h265_create_single_frame` (e as versoes `_fragment`) faziam
`output->Data = memop_alloc_raw(total_size)` a cada chamada, **trocando o ponteiro sem liberar
o anterior**. Os **6 chamadores** (`media_source.c:68`, `pipeline.c:138`, `embed_ops.c` x4)
reusam UM `MediaBuffer` no laco e liberam UM so no fim -> vazava um buffer por frame lido.
Num video de 311s/25fps sao ~7.800 buffers.

**Correcao:** novo `mbuffer_ensure(MediaBuffer*, size_t)` (MediaFragmenterType) cresce o buffer
do chamador preservando o ponteiro quando ja cabe; os 4 sites dos builders passaram a usa-lo.
Conserta os 6 chamadores de uma vez e ainda tira um malloc por frame do caminho quente.

**Correcao do ponto de medicao:** `run_gateway_session` agora assume a posse da fonte e faz
`src->Close(src)` ANTES do `job_end`. Antes o balanco era medido com a fonte MP4 ainda aberta
(indice de frames + buffers), contabilizando-a como sobra da sessao. Tambem garante que a fonte
fecha em todos os caminhos.

## Medicoes (3 sessoes seguidas, cada uma: upload + DASH/VP9 + cancelamento aos 15s)
| | liveDelta por sessao | candidatos por scan | bytes |
|---|---|---|---|
| **antes** | +715, +612, +612 (positivo, repetindo) | 171 -> 281 -> 391 | 17,9 MB |
| **depois** | -22901, -23016, -23016 (estavel) | 85 -> 181 -> 273 | 938 KB |

Sumiram do relatorio: `mnalu_list_init` (186 spans / 11,7 MB), `h264_create_single_frame`
(102 / 4,2 MB), `mframe_new` (9 / 538 KB). O `osReservedDelta` tambem passou a CAIR entre
rodadas (33 MB -> 8 MB -> 4 MB): o pool reusa segmentos em cache em vez de reservar mais.

## O QUE SOBROU (nao corrigido — outro subsistema)
O crescimento residual **nao e do pipeline de midia, e por REQUISICAO HTTP**. Experimento:
200 `GET /api/session/list` entre dois scans levaram o relatorio de ~90 para **2.122 candidatos**
(~10 spans / ~16 KB por requisicao). Origens: `string_init`, `list_create`,
`resource_buffer_init`, `yason_string_new`, `yason_element_array_init`, `appclient_received`,
`message_create` — ou seja, `Message`/`Response`/`Element` por requisicao no `appserver`.
Fica com o endurecimento do servidor HTTP (Fase 7).
**Ressalva:** o `mem_leak_watch` nao varre `.data`/`.bss` como raiz, entao objetos mantidos
apenas por uma lista estatica aparecem como falso positivo. De qualquer forma o crescimento e
linear e sem teto, o que e problema com ou sem alcancabilidade.

## BUG #4 do yason (contornado): campo NAO-STRING em ULTIMA posicao some na releitura
Em `json_parse_object`, um valor nao-string na ultima posicao cai no ramo
`element4->Token == '}'`, que so tem `// TO-TO: implementar campo no fim do objeto` — o campo e
**descartado**. String em ultima posicao funciona. Como o `session.json` e relido a cada edicao,
`frees` sumia na primeira releitura e `allocs` na seguinte. Contorno: `yb_seal_for_reparse()`
fecha cada objeto com um campo string `"_":""` antes de gravar o que sera relido.

## Limitacoes do mem_leak_watch que valem para ler o log (do proprio header)
- so varre spans de **size-class (<=16KB)**: os buffers de FRAME sao blocos LARGE e **nao entram**
  na varredura — para eles o sinal e o `memory` do session.json;
- nao usa `.data`/`.bss` como raiz -> estaticos viram falso positivo;
- so enxerga threads criadas via `thread_create()` da lib (as do appserver passam, ok);
- marca por SPAN inteiro, nao por bloco.

---

# Correcao do yason na ORIGEM (E:\git\libs\yason) — commit abd150f

Cinco defeitos no caminho JSON, mais dois achados durante a validacao. Todos corrigidos no
repositorio de origem, nao contornados.

1. **Tokenizador ignorava aspas.** `json_index_tokens` procurava qualquer um de `":,{}[]`
   sem saber se estava dentro de string; `"2026-09-09T02:00:11Z"` gerava tokens `:` no meio e
   o CAMPO INTEIRO sumia na leitura, sem erro. Reescrito como scanner ciente de aspas, que
   tambem decodifica escapes (`\" \ \/ \b \f \n \r \t \uXXXX`, com par surrogate). O formato
   dos tokens e o mesmo, entao os parsers de objeto/array nao mudaram.
2. **Render nao escapava nada.** Valor copiado cru entre aspas -> aspa/barra/controle geravam
   JSON invalido. Novo `json_append_escaped`, aplicado a valores **e a chaves**.
3. **Valor nao-string na ULTIMA posicao de objeto era descartado** (ramo `'}'` so tinha um
   "TO-TO"). `{"a":1}` perdia o campo; num arquivo relido a cada edicao, os numericos sumiam
   um a um.
4. **Array/objeto VAZIO nao renderizava nada** -> `"tracks":`. Agora `[]` e `{}`.
5. **`if (root->Type = NODE_TYPE_OBJECT)`** — atribuicao no lugar de comparacao; array na raiz
   nunca era renderizado como array.

Achados na validacao (tambem corrigidos):
6. A virgula de array criava campo fantasma depois de um item ja criado
   (`["a","b"]` -> `["a",,"b",,]`).
7. Array aninhado dentro de array nao era renderizado.

**Validacao:** harness de round-trip (`scratchpad/yasontest.c` + `yasontest2.c`, fora do repo),
parse->render->parse: delimitadores dentro de string, numero/bool/null em ultima posicao,
containers vazios, arrays de numeros/strings/objetos, objetos aninhados, escapes em valor e em
chave, `\uXXXX`, e estabilidade do render em duas voltas. **0 falhas.**

## Contornos REMOVIDOS do appserver (viraram nocivos)
Com o render escapando, o escape manual do `yason_build.h` escaparia DUAS vezes. Removidos:
`yb__append_escaped`, `yb_str_plain` (6 call sites -> `yb_str`), `yb_seal_for_reparse`,
`yb__fix_empty`. Os timestamps do `session.json` voltaram ao **ISO 8601 completo**
(`2026-09-09T16:56:06Z`); o id da sessao continua `[A-Za-z0-9_-]` mas agora e formatado a
parte (`strftime "%Y%m%d-%H%M%S"`), nao derivado do timestamp por filtragem.

## Testes de regressao no yasontester (commit 7b30e0b)
Os defeitos falhavam em SILENCIO, entao viraram teste no proprio repo do yason:
`yasontester/json_tests.c`, um bloco por defeito, sempre comparando texto/valor.
Grupos: `delimitador`, `fim-objeto`, `vazio`, `raiz-array`, `escape`, `array-item`,
`aninhado`, `documento`. O `tester.c` roda a bateria JSON ANTES do round-trip do `.cfg`
que ja existia e devolve 10 se algum caso falhar. **36 casos, 0 falhas.**

**Escrever os testes revelou um OITAVO defeito**, que a correcao anterior nao pegava:
`json_parse_array` nao tinha ramo para `'['`, entao um array aninhado nao era reconhecido
e seus tokens vazavam para o laco do array de fora -- `[[1,2],[3,4]]` virava `[1,2]`.
O render ja tratava o caso; faltava o parser. Corrigido em 7b30e0b.

**Build do yasontester:** o xplatbase aninhado do yason esta pinado em **v145** (nao
instalado). Buildar com `-p:PlatformToolset=v143`:
`msbuild yason.sln -t:yasontester -p:Configuration=Debug -p:Platform=x64 -p:PlatformToolset=v143`
(no submodulo dentro do appserver esse arquivo tem uma alteracao LOCAL para v143, por isso
la ele compila sem o override).

## Submodulos
Working tree dos dois submodulos apontando para `7b30e0b`
(`appserver/submodules/yason` e `streammanager/submodules/yason`, este ultimo vinha de
`aa5e888`, mais antigo). O **gitlink ainda nao foi commitado** e **nada foi enviado ao GitHub**.

## GOTCHA DE BUILD (novo, importante)
`msbuild appserver.sln -t:yason:Rebuild` constroi tambem o **xplatbase aninhado do yason**
(copia antiga, sem `memop_zero_raw`) e ele sobrescreve `x64\Debug\Xplatbase.lib`, quebrando o
link de tudo com `LNK2001: memop_zero_raw`. Depois de mexer no yason: buildar a **solution
inteira** (a ordem correta prevalece) ou rodar `-t:Xplatbase:Rebuild` em seguida.
Obs.: `strings` NAO serve para conferir simbolos de `.lib` — use
`dumpbin /linkermember:1 <lib> | findstr <simbolo>`.

## Validado em runtime apos a troca
- nome de sessao com `:`, `,` e aspas escapadas sobrevive ao parse do corpo da requisicao,
  ao disco e a releitura;
- `created`/`updated` em ISO completo preservados;
- `"tracks":[]` valido; `json.load` aceita todas as respostas;
- id de dispositivo do Windows (`\?\usb#...\global`) valido e **sem escape duplo**;
- soak de 2 sessoes: `liveDelta` estavel (-22949, -22949), cancelamento e limpeza OK.

---

# Alinhamento ao Visual Studio 2026 (toolset v145, VCProjectVersion 18.0)

**Toolsets desta maquina** (cada instalacao oferece UM):
`VS 2022 Community -> v143` | `VS 18 (2026) BuildTools -> v145`.
Conferir com `ls <VS>\MSBuild\Microsoft\VC\v1??\Platforms\x64\PlatformToolsets` (as pastas
`VC\v170`/`v180` sao a versao do SISTEMA DE BUILD, nao o toolset -- confundir as duas leva a
conclusao errada de que "v145 nao existe").

**msbuild a usar daqui em diante:**
`"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\MSBuild.exe"`

## appserver.sln (13 projetos) — TODOS em v145 / 18.0
Proprios: appserver, streammanager, MediaFragmenter, H26XDesktopRendererTest, DashServerTest,
appservertester, codecs, libyuv, MediaGateway.
Submodulos (edicao TEMPORARIA na working tree, sera substituida ao atualizar o submodulo):
Xplatbase e yason.
Gerados por CMake: `SvtAv1Enc` e `x265-static` **e os ~40 sub-projetos** (encoder, common,
cpuinfo, clog, FASTFEAT, ASM_AVX2/AVX512, C_DEFAULT...). Alinhar so o projeto de topo NAO
basta: as sub-libs continuam no toolset antigo e entram no `.lib` final misturando CRT.
`codecs\build-codecs.bat` repontado para o `vcvars64.bat` do VS 2026.

**Libs de codec RECONSTRUIDAS em v145** (SvtAv1Enc, x265-static, codecs, libyuv): elas estao
fora do build da solution (sem `Build.0`) e ficariam em v143 sem rebuild explicito.

## DEFEITO PRE-EXISTENTE CORRIGIDO: duas copias do xplatbase, mesma saida
A solution tem o `Xplatbase` de `appserver\submodules\xplatbase` E, via ProjectReference do
yason, o de `appserver\submodules\yason\yason\submodules\xplatbase` -- **mesmo GUID, mesmo
OutDir default**. Resultado: em serie uma sobrescreve o `Xplatbase.lib` da outra (foi a causa
do `LNK2001: memop_zero_raw` que apareceu ao rodar `-t:yason:Rebuild`), e em paralelo o
compilador falha com **C1041** no `.pdb` compartilhado -- por isso um `-t:Rebuild` da solution
inteira nunca funcionou.
**Correcao:** `Xplatbase.vcxproj` passou a fixar saida propria
(`$(ProjectDir)$(Platform)\$(Configuration)\`), mesmo padrao de codecs/libyuv/MediaGateway.
Aplicado no repo de ORIGEM `E:\git\libs\xplatbase` e, temporariamente, nas duas copias dentro
do appserver. **Agora `-t:Rebuild` da solution inteira passa.**

## Repos de origem (E:\git\libs) — para o usuario commitar e enviar
- **xplatbase**: `Xplatbase.vcxproj` v143/v145 misturados (Release|x64 estava em v143) -> tudo
  v145 + OutDir proprio; `bench.vcxproj` e os 3 testers do `Xplatbase.sln` -> v145.
  `Xplatbase.sln` (Rebuild Debug|x64): **verde**. NAO toquei em `bench/mimalloc` (terceiros).
- **yason**: `yason.vcxproj`, `yasontester.vcxproj` -> v145/18.0; `yason.sln` cabecalho
  `Visual Studio Version 17` -> `18`. Rebuild + `yasontester`: **36 casos, 0 falhas**.

## Validacao
- `appserver.sln -t:Rebuild Debug|x64` com o msbuild do VS 2026: **verde**.
- Runtime: sessao com `:`/`,`/aspas, `created` ISO completo, `/api/media/devices` OK.
- Nenhum `<PlatformToolset>v143` restante em nenhum projeto DAS SOLUTIONS.

## Fora de escopo (ainda em v143, nao entram em nenhuma solution)
`appserver/submodules/utility/*` (10 projetos, submodulo separado -- origem provavel
`E:\git\libs\utility`) e `E:\git\libs\xplatbase\bench\mimalloc` (vendorizado de terceiros).

## Pendencia real: regenerar os projetos CMake
`SvtAv1Enc`/`x265-static` foram alinhados por EDICAO dos `.vcxproj`; o `CMakeCache.txt` ainda
diz `Visual Studio 17 2022` + `CMAKE_GENERATOR_INSTANCE` do VS 2022. Qualquer `cmake` futuro
reverte tudo para v143 em silencio. O certo e regenerar com
`-G "Visual Studio 18 2026" -A x64` (o cmake do VS 2026 tem o gerador), mas isso exige apagar
os `_vsbuild` -- e **o Visual Studio precisa estar FECHADO** (2 `devenv.exe` seguravam handles
e o `mv` deu "Permission denied"). Regenerar tambem troca os GUIDs, entao `appserver.sln`
precisa ser repontado depois (ja aconteceu uma vez, ver secao dos codecs).
Opcoes originais preservadas em `codecs/svtav1/_vsbuild/CMakeCache.txt` e
`codecs/x265/_vsbuild/CMakeCache.txt`:
  svtav1: BUILD_APPS/DEC/SHARED/TESTING=OFF, ENABLE_AVX512=ON, SVT_AV1_LTO=OFF, nasm em codecs/tools
  x265:   ENABLE_ASSEMBLY/CLI/SHARED/PIC/PPA/VTUNE/HDR10_PLUS/ALPHA/MULTIVIEW/SCC_EXT/SVT_HEVC=OFF,
          HIGH_BIT_DEPTH=OFF, STATIC_LINK_CRT=OFF

## Projetos CMake REGENERADOS para VS 2026 (pendencia anterior — FEITA)
`SvtAv1Enc` (119 projetos) e `x265-static` (39 projetos) regerados com
`-G "Visual Studio 18 2026" -A x64` pelo **cmake do VS 2026 (4.3.1)**. Agora o
`CMakeCache.txt` diz VS 2026 e um `cmake` futuro NAO reverte mais para v143.

- **Os GUIDs NAO mudaram** (o CMake os deriva de forma deterministica do nome/caminho):
  `SvtAv1Enc {7A6CCE8D-...}` e `x265-static {062D87F6-...}` continuam batendo com a
  `appserver.sln`. Nao foi preciso repontar nada, ao contrario do que a regeracao anterior
  exigiu.
- **O Visual Studio NAO precisou ser fechado.** As duas instancias abertas bloqueavam
  RENOMEAR a pasta `_vsbuild` (o `mv` dava "Permission denied"), mas nao impedem apagar
  `CMakeCache.txt` + `CMakeFiles/` e regerar no lugar -- que e tudo o que o CMake pede para
  trocar de gerador.

### Dois bloqueios do CMake 4.x (o do VS 2022 era 3.31 e aceitava tudo)
1. **`CMAKE_POLICY_VERSION_MINIMUM=3.5` obrigatorio** nos dois projetos: os
   `cmake_minimum_required` deles sao antigos demais para o CMake 4.
2. **x265 nao configurava de jeito nenhum**: `CMakeLists.txt` fixa `CMP0025` e `CMP0054` em
   **OLD**, modo que o CMake 4.x REMOVEU ("may not be set to OLD behavior"). Corrigido em
   `codecs/x265/source/CMakeLists.txt` (vendorizado, untracked): so fixa OLD enquanto
   `CMAKE_VERSION < 4.0`, senao NEW. Continua valido em CMake 3.x. Em MSVC as duas sao
   inocuas -- CMP0025 so muda o nome reportado do Clang da Apple; CMP0054 faz `if()` parar de
   re-dereferenciar argumentos entre aspas.

### Comandos exatos (para repetir)
```
CM="C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

svtav1: -S codecs/svtav1 -B codecs/svtav1/_vsbuild -G "Visual Studio 18 2026" -A x64
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBUILD_APPS=OFF -DBUILD_DEC=OFF
        -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF -DENABLE_AVX512=ON -DSVT_AV1_LTO=OFF
        -DCMAKE_ASM_NASM_COMPILER="E:/git/appserver/codecs/tools/nasm.exe"

x265:   -S codecs/x265/source -B codecs/x265/_vsbuild -G "Visual Studio 18 2026" -A x64
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DENABLE_ASSEMBLY=OFF -DENABLE_CLI=OFF
        -DENABLE_SHARED=OFF -DENABLE_PIC=OFF -DENABLE_PPA=OFF -DENABLE_VTUNE=OFF
        -DENABLE_HDR10_PLUS=OFF -DENABLE_ALPHA=OFF -DENABLE_MULTIVIEW=OFF
        -DENABLE_SCC_EXT=OFF -DENABLE_SVT_HEVC=OFF -DHIGH_BIT_DEPTH=OFF -DSTATIC_LINK_CRT=OFF
```

### Validacao
`SvtAv1Enc.lib` e `x265-static.lib` reconstruidos (Debug|x64); `appserver.sln -t:Rebuild`
**verde**; runtime OK. **So Debug|x64 foi gerado/validado** -- as libs Release dos dois
codecs continuam as antigas; rodar `codecs\build-codecs.bat` quando o Release for necessario.

---

# Fase 1 — UI de sessoes (FEITO, validado no browser)

O campo livre "Pasta de destino" (`#hls-folder`) foi REMOVIDO. A pasta deixou de ser um nome
digitado: agora e o **id da sessao**, criado pelo backend, e e ele que vai no `X-Hls-Folder`
e nas rotas de fragmentacao/conversao.

## O que a UI ganhou (`video-player.html/.css/.js`)
- **Seletor de sessao** + campo de nome inline + botoes **Nova sessao** / **Excluir**.
- **Barra de estado**: `<id> - criada em <ISO>` + chip colorido com o estado do session.json
  (idle/ready/running/done/cancelled/error).
- **Botao Cancelar**, visivel so durante a fragmentacao; chama
  `GET /api/session/cancel/<id>`.
- Evento SSE **`cancelled`** tratado como terminal DISTINTO de `done`: a saida nao foi
  publicada, entao nao ha manifesto para abrir.
- `reset_for_new_session()` limpa plano de pistas, progresso, player e combo de resolucao ao
  trocar/criar/excluir sessao -- sem isso a tela ficava com dados da sessao anterior.
- Sem sessao selecionada, o envio de video fica bloqueado (nao ha onde gravar).

## Tres defeitos encontrados e corrigidos durante o teste no browser
1. **`hidden` nao escondia o botao.** `[hidden]{display:none}` e regra de USER-AGENT e perde
   por especificidade para qualquer classe -- `.btn` tem `display:inline-flex`. O Cancelar
   nascia visivel. O CSS ja tinha um remendo pontual para o mesmo problema
   (`.frag-time[hidden]`); trocado por uma regra global `[hidden]{display:none!important}`.
2. **`prompt()`/`confirm()` nao servem aqui.** Bloqueiam o componente, nao dao para estilizar
   e sao descartados em ambiente automatizado (o clique em "Nova sessao" virava no-op
   silencioso). Nome -> campo inline; exclusao -> DOIS cliques no proprio botao
   ("Excluir" -> "Confirmar?", rearmado em 4s).
3. **Corrida no estado da sessao.** O gateway emite o evento terminal e SO DEPOIS o servidor
   grava o estado final; recarregar a lista na hora do evento lia "running" e o chip mentia.
   Novo `refresh_sessions_settled(id)`: espera `running:false` (ate 20 tentativas de 400ms)
   antes de reler a lista.

## Validacao (browser real, nao so build)
Criar sessao com nome `Teste: um, dois` -> nome preservado; ordenacao mais-recente-primeiro;
exclusao em 2 cliques com a selecao caindo para a sessao restante; upload de 101 MB pelo
input real; 6 pistas em progresso; chip vai `idle` -> `running`; **Cancelar** -> status
"Fragmentacao interrompida. A saida NAO foi publicada.", chip `cancelled`, botao some, upload
reabilitado, e **nenhum `manifest.mpd` no disco** (32 chunks parciais, sem manifesto).

## Pendente da Fase 1
Nada. As Fases 3 (camera) e 4 (grupos Entrada/Saida) sao as proximas e usam esta base.

---

# Fase 3 — Fonte de camera (FEITO, validado contra hardware real)

**`MediaGateway/media/source_camera.c` (novo)** — `source_camera_open` de verdade (o stub em
`MediaFragmenter/src/media_source.c` foi removido). Fica no MediaGateway, junto do
`device_enum`: nao depende do demux MP4, entao nao ha motivo para morar na orquestracao.

- **Windows**: `IMFSourceReader`. Escolhe entre os tipos NATIVOS do device por pontuacao
  (resolucao pedida > formato > fps), ignorando o que `device_map_fourcc` marca como nao
  suportado.
- **Linux**: V4L2 com mmap (4 buffers), `S_FMT`/`S_PARM` respeitando o que o driver devolve.
- **Decisao central: a fonte NORMALIZA todo formato cru para I420.** O gateway ja consome
  I420 contiguo (e o que o decode de arquivo produz); fazer a camera falar a mesma lingua
  evita espalhar NV12/YUY2/RGB24 por decode, escala e encode. Conversao por **libyuv**
  (vetorizada), nao laco proprio.
- Stream CODIFICADO (H.264/H.265 de camera) passa direto, sem tocar nos bytes.
- Contrato de buffer: o bitstream fica valido ate a proxima leitura (buffer MF travado /
  buffer V4L2 nao devolvido), que e exatamente o que `MediaSource` promete.

**libyuv:** `convert.cc` + `rotate*.cc` + `video_common.cc` NAO estavam na compilacao (so
scale/row/planar_functions). Sem eles, `NV12ToI420` e companhia ficam como simbolo nao
resolvido. Adicionados ao `libyuv.vcxproj`.

**Validacao (camera real, `scratchpad/camtest.c`):** Integrated Camera, NV12 1280x720@30 ->
`Info` reporta I420 1280x720; **10/10 frames com 1.382.400 bytes exatos** (1280*720*1.5);
**9/9 frames com conteudo diferente do anterior** (dados vivos, nao frame congelado); pts
avancando; fecha limpo.

# Fase 4 — Perfil de saida completo (FEITO, validado fim a fim)

## Backend
- `MediaProfile` ganhou **Width/Height/Fps/BitrateBps**; `PipeTrack` ganhou **Fps**.
  Convencao: **0 = "nao escolhido, herda da entrada"**.
- **Perfil EFETIVO** resolvido UMA vez no inicio do `gateway_run` (era heranca reinventada em
  tres lugares, com bitrate chumbado em cada um).
- **BYPASS estrito**: passthrough so quando NADA muda -- mesmo codec, mesma resolucao, mesma
  taxa e pista unica. Antes bastava o codec bater.
- **Reamostragem de taxa (`GwFpsGate`)**: decide QUAIS frames seguem, por PTS; ligada nos dois
  caminhos (por-frame e segment-parallel) ANTES da escala/encode, para nao gastar trabalho em
  frame que sera descartado. O encoder passa a receber o fps de SAIDA (com o da entrada, o
  codec calcularia GOP e rate control para uma taxa que nao existe).
- Sessao guarda a config: nova rota **`POST /api/session/config/<id>`**
  (`{input:{...},output:{...}}`) + leitores `frag_session_get_input/get_output`. As tres rotas
  de fragmentacao aplicam via `apply_session_output`. Resolucao fixa escolhida a mao zera as
  renditions (escada adaptativa e o outro modo; misturar entregaria uma escada que ignora o
  pedido).

## UI
Grupos **ENTRADA** e **SAIDA**. A entrada vem inteira de `/api/media/devices` e e ENCADEADA
(trocar o codec repopula resolucoes; trocar a resolucao repopula os fps) -- as opcoes nao sao
independentes. Arquivo desabilita os campos (vem do proprio video). Formato nao suportado
aparece marcado e desabilitado. Saida tem "(do formato)/(adaptativa)/(da entrada)/(automatico)"
como vazio, e uma nota que reflete a politica de bypass.

## Validacao fim a fim
- Config gravada e relida do `session.json` (entrada: camera + link simbolico + NV12 1280x720@30;
  saida: h264 640x360 @15 800kb/s).
- **Conversao com parametros forcados, relida pelo parser do proprio projeto**:
  pedido 640x360@5 -> obtido **640x360 @ 4,515 fps**, duracao preservada (311,6s).
  Resolucao exata; a taxa bate com o que o decode entrega (ver defeito abaixo).

# DEFEITO PRE-EXISTENTE ENCONTRADO: o pipeline perde ~8% dos frames

Medido ao validar a Fase 4, **sem forcar fps nenhum**:

| | frames | fps |
|---|---|---|
| fonte (`stts`) | 7788 | 25,000 |
| saida convertida | ~7139 | **22,920** |

**649 frames (8,3%) somem em toda conversao** -- e sempre sumiram; nao tem relacao com a
reamostragem (com o gate ligado, 4,515 / 22,92 = 1/5,08, ou seja o gate esta correto em
relacao ao que recebe).

**Localizado**: um `printf` temporario em `source_mp4_open` mostrou
`demux frames=7788  pts_entries=7788` -- ou seja, **o demux entrega tudo**. A perda esta no
**decode** (`h26x_decode_frames` / openh264), que devolve menos imagens do que recebe pacotes.
Suspeitas a investigar: frames antes do primeiro IDR, ausencia de flush no fim do stream
(explicaria poucas unidades, nao 649) e descarte por ocultacao de erro do openh264.
Entra na Fase 5, que ja mexe no decode.

---

# Fase 5 (parcial) — a perda de 8% dos frames: CAUSA ACHADA E CORRIGIDA

## O caminho ate a causa (duas hipoteses minhas estavam ERRADAS)
1. **"E o demux"** -> NAO. `printf` temporario em `source_mp4_open`:
   `demux frames=7788  pts_entries=7788` = tudo que o `stts` promete.
2. **"E o ramo de erro do decode"** (`if (state != dsErrorFree && state != dsFramePending)
   return -1;` em `put_decode_h264`) -> NAO. Instrumentado, o decode saiu **limpo**:
   `pacotes=7788 imagens=7786 descartados=0`, todos os estados `dsErrorFree`. As 2 imagens
   que faltam sao a latencia do openh264 no fim do stream (nao ha flush no EOF) -- 0,03%.
3. **A causa real: o ENCODER descartava frames.** `h264_encoder_open` nao setava
   `bEnableFrameSkip`; o default do OpenH264 para `CAMERA_VIDEO_REAL_TIME` +
   `RC_BITRATE_MODE` e **descartar frames para segurar o bitrate**. Faz sentido em
   transmissao ao vivo por link limitado, nao ao preparar arquivo.

## Correcao e medicao
`codecs/media/codec_h264.c`: **`param.bEnableFrameSkip = false;`**

| | fps da saida | frames |
|---|---|---|
| fonte | 25,000 | 7788 |
| antes | 22,920 | ~7139 |
| **depois** | **24,997** | **7786** |

Perda 8,3% -> **0,01%**. As 2 imagens restantes sao a latencia de fim de stream do decoder.

**Efeito colateral: a reamostragem de FPS da Fase 4 ficou exata.** O mesmo teste que dava
`pedido 5 -> obtido 4,515` agora da **`5,000` (erro 0,0%)**. O gate sempre esteve certo; ele
so reproduzia a perda do encoder.

`codecs/media/codec_vp9.c`: `cfg.rc_dropframe_thresh = 0` EXPLICITO. O default do libvpx ja e
0, mas deixar implicito foi exatamente o que custou 8% no H.264. (x265 `X265_RC_ABR` e SVT-AV1
nao descartam frames.) **Nao medido em runtime para VP9** -- so o H.264 foi medido.

## Decoder VP9 implementado
`vp9_decoder_open` era `return 0; // TODO`. Os fontes do decoder (`vp9/decoder/*`,
`vp9_dx_iface.c`) **ja estavam no `codecs.vcxproj`**; faltava so o adaptador. A saida e
copiada para buffer PROPRIO em I420 contiguo: a `vpx_image` pertence ao decoder e vale ate o
proximo `vpx_codec_decode`, enquanto o contrato do `MediaDecoder` e "valido ate a proxima
chamada".

**Validado (`scratchpad/vp9test.c`)**: 30 frames sinteticos encode->decode, **30/30
decodificados**, dimensoes preservadas, erro medio do plano Y = **0,24**.

## Fase 5 FECHADA — o decode do gateway generalizado + demux WebM

Os dois itens que faltavam eram um par: o decoder de VP9 existia e passava no teste, mas
nenhum caminho do gateway chegava nele, e nenhum demux da casa lia `.webm`. Um sem o outro
nao entregava nada.

### `MediaGateway/media/gw_decode.{h,c}` — decode unificado

`sync_gateway.c` chamava `h26x_decode_frames()` direto nos dois caminhos (segment-parallel
e sequencial), o que amarrava o pipeline a openh264/de265. Agora ha uma interface unica:

- H264/H265 -> `h26x_decoder_create` / `h26x_decode_frames` (openh264 / libde265)
- VP9/AV1   -> `media_decoder_open` (vtable `SendPacket`/`ReceiveFrame`)

A interface e de **iterador** (`gw_vdec_send` + `gw_vdec_next`), nao de lista, por um
motivo concreto: o decoder de VP9 reaproveita UM buffer de saida entre frames, entao uma
lista com dois frames do mesmo pacote teria os dois apontando para o mesmo pixel.

`gateway_run` valida a disponibilidade do decoder **uma vez, antes de abrir sink/pool/
arquivos**. Antes, um codec sem decoder fazia o laco descartar todo pacote de video em
silencio e a saida sair vazia, sem erro nenhum.

### `MediaGateway/media/demux_webm.c` — demux WebM/Matroska

Streaming por `FILE*` (nao carrega o arquivo inteiro); so `Info` e `Tracks`, que somam
poucos KB antes do primeiro Cluster, vao para memoria de uma vez. Le
`SimpleBlock` e `BlockGroup/Block`, `TimestampScale`, `DefaultDuration` (unica fonte de FPS
no WebM -- e o gateway precisa do FPS antes do primeiro frame).

Nao trata lacing (so aparece em audio de quadro curto: Vorbis/MP3; Opus em WebM sai sem)
nem faixa cifrada/comprimida.

### `MediaGateway/media/source_file.c` — escolhe o demux pelo CONTEUDO

`source_file_open()` decide por assinatura (`1A 45 DF A3` = Matroska, `ftyp` no offset 4 =
ISO-BMFF), com a extensao so como desempate. Os tres call sites do `hls_controller.c`
passaram de `source_mp4_open` para ele.

## Quatro defeitos que so apareceram ao montar o caminho de ponta a ponta

**1. CodecID do AV1 errado no mux (afetava a saida, nao so a entrada).** `sink_dash.c` e
`sync_gateway.c` escreviam **`V_AV01`** como CodecID do Matroska. Esse valor nao existe no
Matroska -- e o fourcc do ISO-BMFF/DASH, outro namespace. **Todo WebM AV1 que a ferramenta
produziu ate agora tem um CodecID que nenhum player reconhece.** Corrigido para `V_AV1` na
escrita; a leitura aceita os dois, para nao invalidar o que ja esta gravado.

**2. dav1d: EAGAIN descartava o pacote e vazava a referencia.** `av1d_send` ignorava
`DAV1D_ERR(EAGAIN)`, que significa "fila cheia, o buffer NAO foi consumido, reenvie". O
pacote sumia em silencio e o `Dav1dData` vazava. Agora o resto pendente fica no contexto e
`ReceiveFrame` reenvia antes de declarar que nao ha mais imagem.

**3. Faltava o DRAIN do decoder no EOF.** Decoder com reordenacao (o dav1d segura frames na
fila interna) so devolve os ultimos quando para de receber entrada. Sem o flush o video
saia com os frames finais faltando, sem nenhum aviso. Medido: **26 de 30** frames antes,
**30 de 30** depois. Os frames drenados nao tem PTS proprio, entao seguem a cadencia da
entrada a partir do ultimo pacote lido -- empilhar todos no mesmo timestamp faria o
segmento final ter varios frames simultaneos.

**4. `imagep_list_release(list, 1)` ignorava `free_items`.** O parametro era `(void)`-ado.
Cada frame decodificado aloca uma `ImagePlane` (ver `put_decode_h264/h265`) e **nenhuma era
liberada**, em todos os chamadores -- todos passam 1. Havia ate um `TODO(verificar)` em
`pipeline.c:216` perguntando exatamente isso.

## H264/H265 dentro de MKV: length-prefixed, nao Annex-B

Em Matroska o H26x vai em NALs prefixados por tamanho (avcC/hvcC), e openh264/libde265
esperam Annex-B. Sem converter, o decoder recebe bytes de comprimento no lugar do start
code e nao decodifica nada, em silencio. O demux le o `CodecPrivate`, extrai o tamanho do
prefixo e os parameter sets, e converte cada bloco -- injetando SPS/PPS/VPS em cada
keyframe (o decoder pode ser aberto no meio do arquivo, e repetir e barato).

## Disponibilidade de encode e decode viraram perguntas diferentes

`media_codec_available()` respondia pelo encoder e estava sendo usada para decidir decode.
As duas metades vem de libs **diferentes** em dois codecs: AV1 codifica com SVT-AV1 e
decodifica com dav1d (a Intel removeu o decoder do SVT), e H265 codifica com x265 mas o
decoder da camada ainda e um stub. Agora existe `media_decoder_available()`, e e ela que
`gw_decode` e `device_map_fourcc` consultam. Com isso VP9 e AV1 deixaram de ser
`Supported=0` chumbado em `device_map_fourcc` -- a resposta vem do registro de codecs.

## Validacao (medida, nao estimada)

`scratchpad/webmtest.c` — encode -> mux WebM -> `source_file_open` -> `gw_vdec`:

| caso | pacotes | decodificados |
|---|---|---|
| VP9 (`V_VP9`) | 30 | **30** |
| AV1 (`V_AV1`) | 30 | **30** |
| AV1 (`V_AV01`, id legado) | 30 | **30** |

`scratchpad/mkvtest.c` — H.264 em MKV (avcC): 30 pacotes, **todos entregues em Annex-B**,
**30 decodificados**.

`scratchpad/gwtest.c` — ponta a ponta por `gateway_run()`, com escala (320x240 -> 160x120) e
reamostragem (30 -> 15 fps), **3 rodadas identicas** para expor acumulo entre sessoes:

| entrada | saida | container | resultado |
|---|---|---|---|
| VP9 | VP9 | DASH-WebM | OK |
| AV1 | VP9 | DASH-WebM | OK (troca de codec) |
| VP9 | AV1 | DASH-WebM | OK |
| VP9 | H.264 | HLS-fMP4 | OK (caminho sequencial) |
| AV1 | H.264 | MP4 | OK (caminho sequencial) |

**15 execucoes, todas verdes, `alloc - free` constante em 3** -- sem acumulo entre sessoes,
que era o requisito de estabilidade.

## Continua PENDENTE

- `h265_decoder_open` ainda e `return 0`. **Nao e lacuna funcional**: `gw_decode` roteia
  H265 para a via h26x/libde265, entao HEVC de entrada decodifica. So falta se alguem
  quiser HEVC pela vtable do `MediaDecoder`.
- Fase 6: NVENC/QSV/AMF + VAAPI, com a cascata de fallback.
- Fase 7: Linux.

## dav1d vendorizado SEM meson (FEITO, decodifica AV1 de verdade)

`codecs/dav1d/` (fonte da VideoLAN) + **`codecs/dav1d.vcxproj` escrito a mao**. Nenhuma
ferramenta nova: sem meson, sem Python, sem ninja.

**Numeros reais** (medidos, nao estimados): 25 `.c` compilados uma vez, **13 `_tmpl.c`
compilados DUAS vezes** (`-DBITDEPTH=8` e `=16`, cada entrada com `ObjectFileName` proprio,
senao os objetos colidem) + `src/win32/thread.c` = **51 objetos**. Para comparar, o
`codecs.vcxproj` ja lista 286 arquivos a mao. Minha descricao anterior de que o build do
dav1d seria "hostil a fazer a mao" estava exagerada.

Os headers que o meson geraria estao escritos a mao em `codecs/dav1d/build/`:
- **`config.h`** — as ~49 decisoes viram `#ifdef` (arquitetura, plataforma, alocacao
  alinhada). **`HAVE_ASM=0`**: build C puro, portavel para x64/ARM e Windows/Linux. Ligar o
  assembly depois exige tambem gerar `config.asm` e montar os 47 `.asm` com o NASM que ja
  esta vendorizado.
- **`vcs_version.h`** — so alimenta `dav1d_version()`.

Duas armadilhas resolvidas no caminho:
1. **`<stdatomic.h>` no MSVC** exige `/experimental:c11atomics`. O proprio dav1d traz um
   substituto em `include/compat/msvc`; pondo essa pasta ANTES das do sistema no include
   path, o shim e encontrado e a flag experimental nao e necessaria (e o que o meson faz).
2. **`src/win32/thread.c`** (shim de pthreads sobre a API do Windows) nao esta em `src/*.c`;
   sem ele o link quebra com LNK2019 em `dav1d_pthread_create`/`dav1d_pthread_once`.

**Integrado ao build**: `appserver.sln` (pasta Codecs, **sem Build.0**, como as demais libs
build-once) e `codecs/build-codecs.bat` (passo 3/5). Antes disso o dav1d so compilava se
chamado a mao -- um clone limpo quebraria no link.

# DEFEITO GRAVE ENCONTRADO: o encoder AV1 NUNCA produziu video valido

Ao testar o round-trip SVT-AV1 -> dav1d, 30 frames viraram **um unico pacote de 39 bytes**,
com o SVT imprimindo `Invalid API input buffer size detected` a cada quadro.

**Causa**, localizada no fonte do SVT (`Source/Lib/Globals/enc_handle.c`):
```c
read_size = SIZE_OF_ONE_FRAME_IN_BYTES(w, h, csp, is_16bit);
if (app_hdr->p_buffer != NULL && read_size > app_hdr->n_filled_len)  -> entrada INVALIDA
```
`codec_av1.c` passava **`n_filled_len = 0`**, entao `read_size > 0` era sempre verdadeiro e
**todo frame era rejeitado**. Ou seja, a opcao **DASH · AV1 da UI nunca gerou video valido**.

**Correcao**: `c->in->n_filled_len = w * h * 3 / 2` (formula do proprio SVT para 4:2:0 8-bit).

| | pacotes | bytes | decodificados | erros do SVT |
|---|---|---|---|---|
| antes | 1 | 39 | 1 | 30 |
| **depois** | **30** | **11.708** | **30** | **0** |

Corrigido tambem: `av1_recv` devolvia o buffer VAZIO de EOS do SVT como se fosse pacote, o
que punha uma amostra de tamanho zero no muxer (e o dav1d recusa: `in->sz > 0 ... failed in
dav1d_send_data`).

# GOTCHAS DE BUILD (dois novos, ambos me enganaram)

1. **`grep " error "` NAO serve para dizer se o build passou.** Quando o MSBuild CRASHA, nao
   ha linha com " error " e a contagem da zero -- eu li varios "builds verdes" que nao
   compilaram nada. Usar **codigo de saida** (`echo EXIT=$?`) ou conferir o artefato.
2. **O MSBuild de linha de comando do VS 2026 esta quebrado nesta maquina**:
   `System.TypeLoadException: IsTrueInterpolatedStringHandler ... Microsoft.Build.Framework,
   Version=15.1.0.0` em `AssemblyResources.GetString`. Ate `MSBuild.exe -version` falha. A
   pasta `MSBuild/Current/Bin` do VS 18 foi reescrita durante a sessao (atualizacao em
   segundo plano). **A IDE compila normalmente**; so o CLI quebrou.
   **Contorno que funciona** (EXIT=0), usado daqui em diante:
   ```
   "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" \
       <alvo> -p:Configuration=Debug -p:Platform=x64 \
       -p:VCTargetsPath="C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Microsoft\VC\v180\"
   ```
   O conserto de verdade e **Reparar** o VS 18 Build Tools no Visual Studio Installer.

# Fase 6 — Selecao de backend por recurso disponivel (hardware -> GPU -> SIMD -> threads)

Pedido do usuario: "selecao por recurso disponivel, tentando pela ordem
hardware -> GPU -> SIMD -> threads; escolher o mais performatico; e se tem suporte a win,
linux, x64 e ARM".

## Levantamento ANTES de escrever codigo (e o que ele mudou)

Maquina: **NVIDIA GTX 750** (Maxwell 1a geracao), NVENC API 12.2. O Media Foundation expoe
o *NVIDIA H.264 Encoder MFT* como hardware — **so H.264**; HEVC e AV1 dao zero. Nenhuma
runtime AMD ou Intel. O `nvEncodeAPI.h` NAO esta na maquina (vem no Video Codec SDK, nao no
CUDA Toolkit).

Consequencia: dos SDKs de vendor escolhidos no inicio, so o NVENC/H.264 seria testavel. O
**Media Foundation** cobre NVIDIA/Intel/AMD e tambem Windows ARM64 (Qualcomm) num backend
so, com os headers do Windows SDK que o `device_enum.c` ja usa — e e testavel aqui. Os SDKs
nativos continuam possiveis como degrau extra acima dele.

## Arquitetura

- **`codecs/media/enc_select.{h,c}`** — tabela unica de backends + a cascata. A mesma
  tabela alimenta `media_encoder_open()` (escolher) e `media_encoder_list()` (relatar), entao
  o que a UI mostra e o que o encode faz nao divergem. Cada backend DECLARA SO/arquitetura
  (Windows/Linux, x64/ARM64) e e SONDADO em runtime.
- **`MediaEncoderParams.Accel`**: `AUTO` (0, padrao — os 10 call sites nao mudaram),
  `SOFTWARE`, `HARDWARE` (estrito: devolve NULL em vez de disfarcar com software).
- **`MediaEncoder.Backend/Tier/BackendDetail`**: quem atendeu. Vai no `GW_EV_TRACK_START`,
  no SSE (`"encoder"`) e na linha de cada pista na UI.
- **`codecs/media/hw_mf.c`** — degrau HARDWARE no Windows. Protocolo ASSINCRONO do MFT
  traduzido para SendFrame/ReceivePacket, fila interna de pacotes, **prazo de 5s em toda
  espera** (driver travado nao segura o job), I420 -> NV12, GOP de 2s, **sem B-frames**,
  SPS/PPS injetados no keyframe quando o MFT so os poe no tipo de saida.
- **`codecs/media/cpu_features.c`** — CPUID + XGETBV (checa o SO salvando YMM/ZMM, nao so o
  bit da CPU). **Conferido contra `IsProcessorFeaturePresent` nas 6 ISAs: identico.**
- **`/api/media/devices`** ganhou `platform` (SO, arch, SIMD da CPU), `encoders` e `decoders`
  — por codec, os backends na ordem de tentativa, com degrau, disponibilidade, motivo quando
  pulado, SO/arch declarados e o `selected`. Continua uma rota so para toda a configuracao.

### Degrau GPU (compute) esta VAZIO — de proposito, e dito

Nenhuma lib da pilha codifica por shader/CUDA/OpenCL. O NVENC nao e esse degrau: e bloco
fixo dentro da GPU, entao conta como HARDWARE.

### Segment-parallel fica em SOFTWARE, mesmo com hardware disponivel

Ele abre UM encoder por segmento x rendition, em paralelo. A GeForce limita sessoes
simultaneas (medido abaixo: 8), entao parte das tasks cairia para software — e isso
misturaria encoders DENTRO de uma mesma rendition, com cabecalhos de sequencia diferentes
(o init segment do AV1 carrega um so). Ali o degrau que escala e o THREADS. Os demais call
sites (`pipeline.c`, `embed_ops.c`, gateway sequencial) abrem um encoder de vida longa por
pista, entao o AUTO escolhe hardware sem esse risco.

## Matriz nesta maquina (saida do seletor)

| codec | backend | degrau | aqui | SO | arch | detalhe |
|---|---|---|---|---|---|---|
| H.264 | mf-hw | hardware | **sim** | win | x64, arm64 | NVIDIA H.264 Encoder MFT |
| H.264 | openh264 | simd | sim | win, linux | x64, arm64 | AVX2 |
| H.265 | mf-hw | hardware | nao | win | x64, arm64 | sem encoder HEVC nesta GPU |
| H.265 | x265 | simd | sim | win, linux | x64, arm64 | AVX2 (Windows; no Linux ainda threads — ver "x265 com assembly") |
| VP9 | mf-hw | hardware | nao | win | x64, arm64 | sem encoder VP9 nesta GPU |
| VP9 | libvpx | simd | sim | win, linux | x64, arm64 | AVX2 |
| AV1 | mf-hw | hardware | nao | win | x64, arm64 | sem encoder AV1 nesta GPU |
| AV1 | svt-av1 | simd | sim | win, linux | x64, arm64 | AVX2 |

Decoders: openh264 simd, libvpx simd, **dav1d threads** (`HAVE_ASM=0`), **libde265
"binario pre-compilado: SIMD nao verificavel"** — ele entra como `de265.lib`/`libde265.dll`
prontos, e eu tinha escrito que era "compilado sem SSE" sem verificar; corrigido para dizer
so o que se sabe.

## Defeitos encontrados no caminho (todos corrigidos e re-medidos)

**1. Vazamento de handles do encoder de hardware — acha por bissecao, nao por palpite.**
150 ciclos abrir/codificar/fechar: 346 -> 667 handles, crescendo. Minha primeira hipotese
(faltava `IMFShutdown::Shutdown` no MFT assincrono) foi aplicada — e **nao resolveu**
(308 -> 1013). Bissecao, 100 ciclos por variante:

| variante | handles/ciclo |
|---|---|
| CoInit + MFStartup/MFShutdown | 0 |
| + MFTEnumEx | 0 |
| + ativar/desligar o MFT, plataforma derrubada a cada ciclo | +11,8 a +13,3 |
| ciclo completo do encoder, plataforma derrubada | +9,0 a +10,2 |
| ciclo completo, COM + MF vivos pelo processo | +0,02 |
| MF vivo pelo processo, COM por sessao | **+0,03** |
| COM vivo na thread, MF por sessao | **+8,1** |

Causa: **derrubar a plataforma MF (`MFShutdown`) depois que o MFT do driver foi ativado** — o
driver prende recursos a ela. O COM por thread nao vaza. Correcao: a plataforma MF sobe uma
vez por processo (`mf_platform_ready`, thread-safe) e nao desce. O `IMFShutdown` ficou,
porque e contrato da API, mas nao era a causa.

**2. Drain do DECODER inexistente no caminho h26x (bug meu da Fase 5).** O `gw_decode` fazia
`return 0` no flush de H.264/H.265. Medido: **149/150** no H.264 do NVENC, **59/60**, e
**26/30** no HEVC. Vale para qualquer entrada com B-frames (video de celular/camera): os
ultimos frames sumiam em toda conversao. Novo `h26x_decoder_flush()` (openh264:
`FlushFrame` + `END_OF_STREAM`; libde265: `de265_flush_data`).

**3. Condicao de corrida pre-existente no decode H.265.** O codigo guardava o ponteiro do
plano e chamava `de265_release_next_picture` logo em seguida, com o libde265 rodando **4
threads de trabalho** — o buffer podia ser reescrito enquanto o gateway lia. Agora cada frame
e copiado para memoria propria (struct + pixels num bloco so) antes de soltar a imagem. O
teste detecta frame repetido (aliasing): 0.

**4. x265 com B-frames.** O preset `ultrafast` liga `bframes=3`
(`x265/source/common/param.cpp`), e os sinks assumem DTS == PTS. `param->bframes = 0`, o
mesmo contrato do encoder de hardware.

**5. Duas funcoes do `codec_video.c` caiam do fim sem `return`** (`put_decode_h265`,
`h26x_decode_frames`): devolviam lixo.

## Limite de sessoes, medido

12 encoders H.264 AUTO abertos ao mesmo tempo: **8 no hardware, 4 cairam para software, 0
sem encoder, 12/12 codificaram** com todos abertos. E exatamente o cenario que a queda POR
ABERTURA (e nao por processo) tinha de cobrir.

## Validacao final (medida, depois de todas as correcoes)

`scratchpad/hwtest.c` — **0 falhas**:

| verificacao | resultado |
|---|---|
| H.264 AUTO escolhe o hardware | mf-hw (NVIDIA H.264 Encoder MFT) |
| pacotes = frames | 150 / 150 |
| SPS no 1o pacote | sim |
| PTS estritamente crescente (sem B-frames) | sim |
| IDR periodico, GOP 2s | 3 keyframes, em 0 / 60 / 120 |
| decodifica de volta | **150 / 150** (antes 149), 0 frames repetidos |
| SOFTWARE forcado | openh264 [simd AVX2], 60/60 |
| HARDWARE forcado | mf-hw, **60 / 60** (antes 59) |
| HEVC AUTO sem hardware | caiu para x265, **30 / 30** (antes 26), PTS crescente |
| HEVC HARDWARE sem hardware | NULL (nao disfarca com software) |
| 12 encoders H.264 simultaneos | 8 hardware + 4 software, 12/12 codificaram |
| 20 ciclos: memory_pool / threads / handles | 0 -> 0 / 44 -> 44 / **392 -> 392** |
| gateway HLS-fMP4 e MP4 -> H.264 | reporta `mf-hw [hardware] NVIDIA H.264 Encoder MFT` |
| gateway DASH-WebM -> VP9 | reporta `libvpx [simd] AVX2 (segment-parallel)` |

`scratchpad/hleak.c` — 150 ciclos abrir/codificar/fechar o hardware:

| ciclo | 1 | 25 | 50 | 75 | 100 | 125 | 150 |
|---|---|---|---|---|---|---|---|
| handles, **antes** | 346 | 356 | 370 | 398 | 443 | 527 | 667 |
| handles, **depois** | 371 | 374 | 374 | 375 | 375 | 376 | 376 |

`scratchpad/cputest.c` — deteccao de ISA identica a `IsProcessorFeaturePresent` nas 6
(SSE2, SSSE3, SSE4.1, AVX, AVX2 sim; AVX-512 nao).

## Continua PENDENTE

- **x265 com assembly no build Linux.** No Windows foi religado (secao "x265 com assembly",
  abaixo); o `codecs/CMakeLists.txt:83` ainda forca `ENABLE_ASSEMBLY=OFF` no Linux.
- **dav1d com `HAVE_ASM=0`**: exige gerar `config.asm` e montar os `.asm` com o mesmo NASM.
- **Linux (Fase 7)**: VAAPI (x64) e V4L2-M2M (ARM SoCs) como degrau HARDWARE. A tabela ja
  aceita backends so-Linux, mas nenhum CMakeLists compila `media/*.c` ainda.
- **Decode por hardware**: fora do escopo por ora. O custo da conversao e dominado pelo
  encode, e aproveitar decode de hardware exige manter os frames em superficie de GPU ate o
  encoder (D3D11/VAAPI) — so compensa com encode de hardware na mesma GPU.
- **Controle fino do NVENC nativo** (lookahead, AQ, multi-pass): o MFT expoe rate control,
  GOP e B-frames, nao tudo. Entra como degrau extra acima do `mf-hw` se fizer falta.

## x265 com assembly (Fase 6, "escolher o mais performatico")

**Causa real do build sem assembly**: o `CMakeCache.txt` do `x265/_vsbuild` registrava
`NASM_EXECUTABLE-NOTFOUND`, e o CMake do x265 desliga o assembly em silencio quando nao acha
o NASM. Nao foi escolha. O NASM 2.16.03 ja estava no repo (`codecs/tools/nasm.exe`; o x265
exige >= 2.13).

**Incidente durante a correcao — registrado porque foi erro meu.** A primeira reconfiguracao
falhou, e em seguida eu compilei com `-t:Rebuild`, que LIMPA antes de compilar: o
`x265/_vsbuild/Debug/x265-static.lib` foi apagado e o link Debug da solution ficou quebrado
ate a restauracao. A falha em si nao tinha relacao com o NASM: o CMake nao achava a instancia
do VS 18 no registro do Installer ("instance is not known to the Visual Studio Installer, and
no 'version=' field was given") — o mesmo registro quebrado que ja derrubava o MSBuild de
linha de comando. Correcao: `CMAKE_GENERATOR_INSTANCE` com `version=18.10.12201.205`
(chave `InstallationVersion` de `Common7/IDE/devenv.isolation.ini`). Com isso:
`Found Nasm 2.16.03`, `ENABLE_ASSEMBLY=ON`, 324 entradas `.asm` no projeto (antes, 0).

**Resultado**: as duas libs compilaram com assembly — `Debug/x265-static.lib` restaurada
(14,5 MB) e `Release/x265-static.lib` refeita (8,6 MB; antes 3,6 MB, sem os kernels). O
seletor agora lista `x265 [simd] AVX2`, e o `hwtest` passou com 0 falhas.

| mesmo teste (30 frames 320x240, build Debug) | fps | QP medio | kb/s |
|---|---|---|---|
| antes: sem assembly, `bframes=3` do preset | 14,99 | 19,84 | 163,69 |
| depois: com assembly, `bframes=0` | **70,59** | 13,50 | 212,94 |

**Ressalva honesta sobre o 4,7x**: a comparacao mistura DUAS mudancas. Entre as medicoes
entrou tambem o `bframes=0` (correcao do DTS == PTS), que por si so reduz trabalho do encoder
— o QP e o bitrate diferentes mostram que a configuracao de encode mudou junto. Isolar so o
assembly exigiria recompilar o x265 sem ele mantendo `bframes=0`; nao foi feito. E 30 frames
com a inicializacao incluida e amostra pequena. O ganho do assembly e real (324 kernels
SSE2..AVX2 no lugar de C puro), mas o numero exato atribuivel a ele nao foi medido.

**Seletor**: a tabela de backends e compartilhada entre Windows e Linux, mas os dois builds do
x265 diferem — declarar SIMD sem condicao faria o seletor afirmar SIMD num binario Linux sem
assembly. A entrada agora depende da plataforma (`X265_ISA_*` em `enc_select.c`): SIMD
(SSE2..AVX2) no Windows, THREADS no Linux. Os kernels AVX-512 existem, mas o proprio x265 so
os usa se pedidos explicitamente, entao o teto declarado e AVX2.

**`build-codecs.bat`**: o comentario apontava o NASM em `MediaFragmenter\deps\tools\`, que nao
existe. Corrigido para `codecs\tools\`, e agora documenta o comando exato para regenerar o
`_vsbuild` do x265 com assembly e `version=`.

**Continua PENDENTE (Fase 7)**: `codecs/CMakeLists.txt:83` ainda forca
`set(ENABLE_ASSEMBLY OFF CACHE BOOL "" FORCE)` no build Linux. Ligar exige nasm (x64) ou gas
(arm64) no host, e nao ha como compilar nem testar isso daqui. O caminho do NASM no cache do
`_vsbuild` e absoluto, mas o `_vsbuild` nao e versionado, entao so vale nesta maquina.

# Fase 7 — Linux, decode por GPU e assembly do dav1d

Escopo pedido: "Faça só fase 7, Decode por GPU, Assembly do dav1d. O restante não implemente
ou execute." Nada foi commitado.

## Ambiente levantado antes de escrever codigo

- WSL2 com Ubuntu 24.04 (x86_64): gcc 13.3, cmake 3.28, ninja, nasm 2.16.01, cross-compilador
  aarch64 e `qemu-aarch64-static`, `sudo` sem senha, repositorio legivel em `/mnt/e`.
- Pacotes instalados COM AUTORIZACAO do usuario: pkg-config, libopenh264-dev 2.4.1,
  libde265-dev 1.0.15, libva-dev 2.20, vainfo, mesa-va-drivers 25.2.8.
- Varredura `gcc -fsyntax-only` em todos os modulos antes do porte: os erros tinham poucas
  causas raiz (um `#include <wtypes.h>` derrubava 17 arquivos; Winsock em 3; `process.h`; o
  `const` da xplatbase; `x265_config.h`, que e gerado pelo CMake do x265).

## Build Linux

- **`CMakeLists.txt` na raiz** (novo). As listas de fontes espelham os `.vcxproj`, para os
  dois sistemas compilarem o mesmo codigo. xplatbase e yason por caminho configuravel
  (`XPLATBASE_DIR`/`YASON_DIR`), padrao nos submodules.
- **`codecs/CMakeLists.txt` estava quebrado**: apontava para `MediaFragmenter/deps/src`, que
  nao existe mais (as deps foram movidas para `codecs/`). Corrigido.
- **`cmake/linux/msvc_compat.h`** (novo), forcado com `-include` so nos alvos do projeto e so
  no Linux: as ~100 chamadas da CRT "segura" do MSVC (`sprintf_s`, `fopen_s`, `_stricmp`...),
  `WINAPI`/`__stdcall`, `Sleep`, `GetTickCount64`. Sao renomeacoes mecanicas; editar cada
  chamada seria ruido, e o build MSVC fica intocado.
- **O que e estrutural tem `#ifdef` explicito no fonte**: sockets (`appserver/src/utils/
  net_compat.h`, novo), a thread de accept (`_beginthread` sobre pthread), os headers do
  Windows, o simulador de player com SDL.
- **`-fcommon`**: o MSVC aceita a mesma global definida em varios `.c` via header; o gcc >= 10
  recusa. Mantem o comportamento do Windows.
- **openh264 e libde265 vem do sistema** (pkg-config): no Windows sao binarios prontos, sem
  fonte no repo.
- **dav1d com assembly tambem no Linux**: os mesmos 46 `.asm`, montados com `nasm -f elf64`.

## Bugs reais achados no porte (valem para Windows tambem)

1. **Condicao de corrida na subida do servidor** (`appserver.c`): a thread de accept era
   disparada ANTES de `info->Handle` receber o socket. Se ela chegasse ao `accept()` primeiro,
   usava um handle vazio — no Windows o accept falhava e a thread morria em silencio.
   Funcionava por sorte de temporizacao.
2. **`WsaInit` caia do fim sem `return`**: devolvia lixo no caminho de sucesso.
3. **`appserver_create` fazia `return 1;` numa funcao que devolve ponteiro** (3 caminhos de
   erro de socket): quem chamava recebia um "servidor" invalido em vez de NULL.
4. **`program_util.h`, ramo Linux**: `readlink(..., sizeof(path) - 1)` num ponteiro (7 bytes de
   buffer) e escrita num `const char*`. Codigo sem chamadores hoje, mas quebrava a compilacao.
5. **xplatbase (corrigido NA ORIGEM, `E:\git\libs\xplatbase`)**: `#include <stdlib.h>]` com um
   `]` sobrando e `numeric_parse_double` com `char*` no `.c` e `const char*` no `.h`. O push
   e a atualizacao do submodule ficam com o usuario; o build Linux usa a origem corrigida via
   `XPLATBASE_DIR` ate la.

## Servidor HTTP: bugs de memoria achados pelo porte (valem para Windows TAMBEM)

Os avisos do gcc que o gcc 14 transforma em erro (`-Wincompatible-pointer-types`,
`-Wint-conversion`, `-Wimplicit-function-declaration`) nao foram tratados como cosmeticos: cada um
foi lido contra a assinatura real da funcao e o tipo real do campo. Os mesmos pontos ja apareciam
no MSVC como C4024/C4047/C4133 — os bugs existiam antes do porte.

**Prova antes de corrigir** (`scratchpad/httptest.py` contra o binario Windows SEM as correcoes,
cada caso rodado em processo separado para um crash nao esconder o seguinte):

| caso | resultado ANTES |
|---|---|
| preflight `OPTIONS` | **processo morreu**: codigo `3221225477` = `0xC0000005` (violacao de acesso) |
| upgrade de WebSocket | **processo morreu**: mesmo `0xC0000005` |
| `GET` com query string | HTTP 200, processo de pe — o erro de tipo (#2) e real, mas **nao produziu crash** neste teste |

| # | onde | defeito | efeito |
|---|---|---|---|
| 1 | `appserver.c` (OPTIONS) | o ARRAY de campos ia no lugar da FUNCAO `header_appender` | **crash confirmado** no preflight CORS |
| 2 | `message_parser.c` | `method_param_populate` recebe `StringX*`, os 4 chamadores passavam o texto cru | le bytes da URL como se fossem ponteiro (leitura invalida); **sem crash demonstrado** |
| 3 | `message_parser.c` | `string_init_copy(&campo)` com campo `StringX*` | 32 bytes escritos sobre 3 ponteiros; **crash confirmado** no upgrade de WebSocket |
| 4 | `websocket_util.c` | GUID inventado no lugar do FIXO do RFC 6455, e `"&s"` no lugar de `"%s"` | o handshake de WebSocket **nunca** funcionou |
| 5 | `websocket_util.c` | `int` recebendo `size_t` (8 bytes) de `string_utf8_to_bytes` | escrita alem da variavel na pilha |
| 6 | `message_parser.c` | `int*` gravando em `Length` `uint64` | metade alta do tamanho com lixo |
| 7 | `message_assembler.c` | devolvia o NUMERO 500 como ponteiro de string | crash ao imprimir status desconhecido |
| 8 | `activity_binder.c` | `return 1` numa funcao que devolve `FunctionBind*` | ponteiro 0x1 para quem chamasse |
| 9 | `server_type.c` | `Route` (lista) liberado como string em `bind_release` | liberacao errada |
| 10 | `server_type.c` | `message_release` nunca liberava os campos de WebSocket | (necessario apos o #3) |
| 11 | `appserver.c`/`event_server.c` | `size_t` gravado em `ResourceBuffer.Length` (`int`) | escrita de 8 bytes em campo de 4 |
| 12 | `MediaFragmenterType.c` | `mbuffer_append_uint64` recebia `uint32_t` e fazia `>> 56` | deslocamento indefinido nos campos de 64 bits |
| 13 | `Mp4Builder.c` | `h264_create_fragment` sem prototipo | declaracao implicita (gcc 14: erro) |

Mais: `op` do WebSocket gravado no byte baixo de um ponteiro; `event_list_init` na lista de campos
(layout igual, trocado pela funcao certa); casts `(void**)`/`(byte**)` desnecessarios;
`const` nos literais de string. `Mp4BuilderTest.c` (teste sem chamadores, que chama funcoes
inexistentes) ficou fora do build Linux.

**Nao corrigido, e dito**: `binder_extension_exist` alem do `return` — nao tem chamadores e
confunde lista e string nas outras duas chamadas; a logica que ela deveria ter nao e deduzivel
do codigo, entao nao foi inventada.

**Achado depois das correcoes: a chave do WebSocket nunca chegava inteira.** Com os crashes
corrigidos, o `101` saia mas o `Sec-WebSocket-Accept` continuava errado. SHA-1/base64/handshake
foram testados isolados (`scratchpad/wstest.c`) e estavam certos; uma busca exaustiva sobre
substrings mostrou que o servidor fazia o hash de `"="`. Causa: o parser quebra TODO valor de
cabecalho em parametros `nome=valor` (`message_field_param_add`), e a chave base64 termina com
`=` de padding. A `MessageField` nao guardava o valor cru. Correcao: campo `Raw` na
`MessageField` (valor apos `:`, sem espacos nas pontas), preenchido em `message_parser_field`,
liberado em `message_field_release`, e lido para `Sec-WebSocket-Key`/`Accept` ANTES do teste de
`Param.Value` (uma chave sem `=` nem entraria naquele ramo).

**Depois das correcoes** (`httptest.py`, mesmo teste nos dois sistemas, 0 falhas em ambos):

| caso | Windows | Linux |
|---|---|---|
| preflight `OPTIONS` | 200 com os cabecalhos CORS, processo de pe | idem |
| `GET` com query string | 200, de pe | idem |
| WebSocket, vetor do RFC 6455 | `101` + `s3pPLMBiTxaQ9kYGzzhZRbK+xOo=` | idem |
| WebSocket, 2 chaves com `+` e `/` (Accept esperado calculado em Python) | corretos | corretos |
| criar/listar/apagar sessao | OK | OK |

Avisos C4024/C4047/C4133 nos arquivos corrigidos: 10 -> 2 (os dois restantes sao da
`binder_extension_exist`, acima).

## Resultado no Linux (WSL2 Ubuntu 24.04, x86-64)

- `cmake -G Ninja` + build: **EXIT=0**, `appservertester` de 16,5 MB.
- `webmtest`: VP9 e AV1 (`V_AV1` e `V_AV01`) 30/30 decodificados.
- `gwtest`: 15 rodadas, `memory_pool` constante (vivos=3 em todas).
- `httptest`: 0 falhas (tabela acima); `/api/media/devices` informa `linux/x64` com encoders
  openh264/x265/libvpx/svt-av1 e decoders openh264/libde265/libvpx/dav1d.
- **x265 no Linux usa threads** (a lista declara isso); assembly do x265 no Linux continua
  desligado, fora do escopo desta fase.
- **SVT-AV1 grava a lib dentro da arvore de fontes** (`codecs/svtav1/Bin/Release`), comportamento
  do CMake dele; o CMake da raiz aponta para la.
- **libde265 do sistema** e binario pre-compilado: uso de SIMD nao verificavel daqui.

**Pendente com o usuario (push nas origens e atualizacao dos submodules)**:
- `E:\git\libs\xplatbase`: `numeric.c` (o `]` sobrando e o `const` de `numeric_parse_double`).
- `E:\git\libs\yason`: `yason_compat.c` ganhou `fopen_s` para nao-Windows (sem ele o link
  falhava). Ate o push, o build Linux usa as origens via `XPLATBASE_DIR`/`YASON_DIR`.

**Nao testado**: ARM64 (so compilacao cruzada disponivel; nao foi executado nesta fase), outras
GPUs (AMD/Intel).

## Decode por GPU

### Windows: FEITO e validado

**Levantamento antes de escrever codigo**: nao ha "MFT de decode de hardware" nesta pilha
(`MFTEnumEx` com `MFT_ENUM_FLAG_HARDWARE` devolve 0 decoders para os 4 codecs). O caminho da
GPU e o MFT da Microsoft (sincrono, D3D11-aware) recebendo um `IMFDXGIDeviceManager`: ai ele
decodifica via DXVA. A GTX 750 expoe perfil DXVA de H.264 e de HEVC (com NV12), nao de VP9 nem
AV1 — e nao ha decoder HEVC do Media Foundation instalado (a extensao HEVC da loja nao esta
presente). **Por GPU nesta maquina: so H.264.**

- **`codecs/media/hw_dec.h` + `hw_dec_mf.c`** (novos): so aceita a GPU quando o
  `ID3D11VideoDevice` expoe o perfil DXVA do codec. Sem essa checagem, um MFT de extensao
  (VP9/AV1 da loja) aceitaria o gerenciador e decodificaria em SOFTWARE numa GPU sem o perfil — e
  o seletor diria "GPU". Frame lido de volta por `IMF2DBuffer::Lock2D`, NV12 -> I420, recorte
  pela `MF_MT_MINIMUM_DISPLAY_APERTURE` (a superficie vem alinhada: 1080 -> 1088).
- **`gw_decode.c` reescrito**: GPU primeiro, software depois. **Queda para o software no meio do
  fluxo antes do primeiro frame**: ate la os pacotes entregues a GPU ficam guardados (ate 64) e
  sao reenviados ao software — um fluxo que o decoder da GPU recuse (perfil nao suportado,
  4:4:4, 10 bits) segue no software sem perder frame. Depois do primeiro frame nao ha como
  refazer o que passou, entao um erro ali e reportado como erro.
- **Plataforma MF**: a mesma `mf_platform_ready` do encoder (viva pelo processo), exportada.
- **`media_decoder_list`**: linhas `dxva` (Windows) e `vaapi` (Linux) antes do software.
- **Gateway**: o `TRACK_START` passou a dizer tambem o decoder:
  `mf-hw [hardware] NVIDIA H.264 Encoder MFT | decode: Microsoft H264 Video Decoder MFT via DXVA (...)`.

**Validacao** (`scratchpad/gpudectest.c`, bitstream FIXO `rt_bench_h264.bin`, 300 frames
1280x720) — **0 falhas**:

| verificacao | resultado |
|---|---|
| H.264 AUTO usa a GPU | `Microsoft H264 Video Decoder MFT via DXVA (NVIDIA GeForce GTX 750)` |
| frames | GPU 300 / software 300 |
| **saida da GPU x software, frame a frame** | **identica em todos os 300** (H.264 e bit-exato: qualquer erro de NV12/pitch/recorte apareceria) |
| AV1 AUTO (sem perfil DXVA) | caiu para o dav1d, 30/30 |
| AV1 HARDWARE forcado | NULL (nao disfarca com software) |

**Desempenho** (maquina ociosa, build Debug, melhor de 3, inclui ler o frame de volta da GPU e
converter NV12 -> I420):

| H.264 1280x720, 300 frames | fps |
|---|---|
| openh264 (software) | 82,9 |
| **DXVA na GTX 750** | **176,9 (~2,1x)** |

**Estabilidade** — 200 ciclos abrir/decodificar/fechar a GPU (`scratchpad/gpucycles.c`):

| ciclo | 1 | 25 | 50 | 75 | 100 | 150 | 200 |
|---|---|---|---|---|---|---|---|
| threads | 41 | 40 | 40 | 43 | 43 | 43 | 43 |
| handles | 387 | 387 | 387 | 388 | 388 | 389 | 389 |
| memory_pool (alloc-free) | 0 | 0 | 0 | 0 | 0 | 0 | 0 |

As threads estabilizam em 43 (pool do MF/driver), sem crescer com as sessoes. Nenhum ciclo perdeu
frame. (A curva longa foi medida de proposito: 50 ciclos mostravam 40 -> 42, e foi exatamente
num teste curto que o vazamento de handles do encoder tinha se escondido.)

Regressao no mesmo build: `hwtest` 0 falhas, `gwtest` 15 rodadas com memoria constante,
`webmtest` e `mkvtest` OK.

## Assembly do dav1d

Ligado em x86-64, no Windows (`codecs.vcxproj`) e no Linux (CMake), sem meson:

- **`codecs/dav1d/build/config.asm`** escrito a mao com os valores do `meson.build` para x86-64
  (`private_prefix=dav1d`, `PIC=1`, `STACK_ALIGNMENT=16`, `FORCE_VEX_ENCODING=0`).
- **`config.h`**: `HAVE_ASM=1` so em x86-64. ARM64 continua C puro (os `.S` de ARM nao sao
  montados).
- **46 `.asm`** (a lista do meson: 9 comuns + 16 de 8 bits + 21 de 16 bits) + `src/x86/cpu.c`.

Bench com bitstream FIXO em disco (`rt_bench_av1.bin`, 640x360, 300 frames), build Debug,
maquina OCIOSA, melhor de 3 execucoes:

| | AV1 640x360 | H.264 1280x720 (controle, nao mudou) |
|---|---|---|
| antes: dav1d em C puro | 32,7 fps | 82,8 fps |
| depois: dav1d com assembly | **122,4 / 120,2 fps (~3,7x)** | 82,9 fps |

Round-trip AV1 (encode SVT-AV1 -> WebM -> demux -> dav1d): 30/30 nos tres casos.

**Tres tropecos no caminho, todos meus:**
1. **Colisao de objeto.** `dav1d/src/cpu.c` e `dav1d/src/x86/cpu.c` geravam o mesmo `cpu.obj`
   (aviso MSB8027); um apagava o outro e sumiam `dav1d_init_cpu`/`dav1d_cpu_flags` no link.
   Corrigido com `ObjectFileName` proprio.
2. **XML invalido.** O comentario que coloquei explicando (1) tinha `--`, proibido em comentario
   XML: o `codecs.vcxproj` deixou de carregar. Corrigido; desde entao todo `.vcxproj` editado
   passa por um parser XML antes do build.
3. **Medicao contaminada.** A primeira medicao rodou junto do build Linux (ninja em todos os
   nucleos) e mostrou o H.264 "caindo" 35% sem nenhuma mudanca no caminho dele. Refeita com a
   maquina ociosa (acima); os benches passaram a rodar separados de qualquer build.

A lista de decoders agora declara o dav1d como SIMD com piso SSSE3 (os kernels "sse" do dav1d
exigem SSSE3) e teto AVX2: ha kernels AVX-512, mas o dav1d so os usa com o conjunto AVX-512
ICL, que a deteccao de CPU nao distingue do AVX512F.

## Decode por GPU no Linux: NAO implementado — e por que

1. **Nao e testavel nesta maquina.** O driver VAAPI d3d12 do WSL instala, mas falha na
   inicializacao (`d3d12_drv_video.so init failed`, "resource allocation failed") mesmo com
   log detalhado; nao ha `/dev/dri`.
2. **E outra ordem de trabalho.** O VAAPI nao recebe o bitstream pronto: a aplicacao precisa
   fazer o parse de SPS/PPS/cabecalhos de slice e preencher os buffers de parametros ela mesma
   (e o que o ffmpeg faz com o parser proprio dele) — milhares de linhas por codec. No Windows
   o decode por GPU foi viavel porque o MFT da Microsoft faz esse parse.

A lista de decoders ja declara `vaapi` (Linux) como "nao compilado neste build", entao a matriz
de suporte fica honesta.

# Camera ao vivo (HLS fragmentado em MEMORIA)

Pedido: com a fonte em camera nao ha arquivo; a fragmentacao acontece em memoria e SO na
resolucao escolhida na barra do player. ENTRADA fica toda desativada; a SAIDA oferece codec,
FPS e bitrate (resolucao nao, porque quem escolhe e a barra do player).

## Pecas novas

- **`MediaGateway/media/live_store.c/.h`**: janela deslizante de segmentos fMP4 em RAM
  (6 segmentos) + a playlist HLS ao vivo. Um produtor (thread do gateway) e N leitores (as
  threads das requisicoes), sob um mutex curto; a leitura devolve COPIA para um `send()`
  lento nunca travar a captura. Nada e' gravado em disco: ao vivo nao tem fim, e uma pasta
  que cresce para sempre so criaria lixo que ninguem apaga.
- **`MediaGateway/media/sink_live.c`**: `MediaSink` que monta os mesmos moof/mdat do HLS VOD
  e empurra na janela. Container novo `CONT_LIVE_MEM`; o destino vai no perfil
  (`MediaProfile.Live`) porque quem abre o sink e' o proprio gateway.
- **`mux_mp4`**: `mp4_build_init` / `mp4_build_segment` montam as caixas em MEMORIA; os
  antigos `mp4_write_*` passaram a ser esses mais um `fwrite`. Um so lugar monta MP4.
- **`test/appservertester/live_controller.c`**: `/api/live/start|stop|status/<sessao>` e
  `/api/live/media/<sessao>/{live.m3u8,init.mp4,seg-N.m4s}`. A fragmentacao roda em thread
  propria (`thread_create`): segurar a requisicao ate o fim nunca devolveria a pagina. A
  configuracao vem do `session.json` gravado pela UI -- sem uma segunda via de parametros.
- **UI**: com camera, o botao vira **Transmitir/Parar**, a barra do player vira a resolucao
  de SAIDA (trocar reinicia camera e encoder), e ENTRADA fica desativada.

## Dois bugs achados por causa do ao vivo (valem para o resto do servidor)

1. **O servidor respondia 200 para tudo** (`appserver.c`): o status gravado pelo handler era
   ignorado e todo 400/404/500 chegava como sucesso, com o erro escondido no corpo -- o front
   ja contornava isso adivinhando pelo texto. Um player HLS nao tem como adivinhar. Agora a
   resposta usa `Response->Status`.
2. **Playlist vazia respondia 404** nos primeiros segundos (antes do primeiro segmento), e o
   hls.js esgotava as tentativas e desistia antes de a camera render imagem. Agora a playlist
   e' valida desde o inicio, so sem segmentos.

Tambem: o corte de segmento exigia o alvo CHEIO; um keyframe que chega um instante antes era
recusado e o segmento ia ate o proximo -- com GOP igual ao alvo isso DOBRAVA a duracao. Passou
a ter 25% de tolerancia (vale para VOD tambem; `gwtest`, `webmtest` e `mkvtest` seguem OK).

## Medido nesta maquina (Integrated Camera + GTX 750)

| | |
|---|---|
| encoder | `mf-hw [hardware] NVIDIA H.264 Encoder MFT` |
| resolucoes testadas | 1280x720 e 640x360 (escolhidas na barra do player) |
| janela | 6 segmentos; memoria estavel (os antigos saem quando os novos entram) |
| latencia ate a borda | ~12 s |
| parar | encerra a thread, libera a janela e o `status` volta a `live:false` |

## Limitacoes (o que NAO esta feito)

- **Segmento sai com ~3,6 s, nao 2 s**: o codigo pede GOP de 2 s ao encoder, mas o NVENC
  entrega keyframe a cada ~107 quadros e o corte so pode acontecer em keyframe. Reduzir de
  verdade exige forcar IDR na fronteira do segmento (o encoder ainda nao expoe isso). E'
  tambem o que segura a latencia em ~12 s.
- **Sem audio** no ao vivo (a captura aqui e so video).
- **Uma resolucao por vez** -- que e o pedido; nao ha escada ao vivo.
- **So HLS/H.264**. DASH ao vivo cabe depois sobre os MESMOS segmentos (sao CMAF), mas o sink
  DASH atual e WebM/VP9, e nao ha VP9/AV1 por hardware nesta maquina para tempo real.
- **Nao testado no Linux** (a captura V4L2 existe no `source_camera.c`, mas nao rodou aqui) e
  nao testado com mais de uma transmissao simultanea (o limite no codigo e 4).
- A reproducao aparece pausada quando a aba esta oculta: e o navegador pausando midia em aba
  escondida, nao o fluxo.

---

# Instancia unica do xplatbase (preparacao para os plugins de codec)

Plano, fases e resultados medidos estao em `prep-xplatbase/PLANO.md` -- aquela pasta e
descartavel; o que interessa a longo prazo esta aqui e no README do xplatbase.

## O problema

O pool de memoria (`static MemPool G`), o registro de threads e o pool de tarefas
(`static ThreadPool* GlobalPool`) sao variaveis **file-static**. Cada modulo que linka o
xplatbase estaticamente ganha a propria copia: dois pools, memoria alocada de um lado
invisivel do outro, e bloco liberado do lado errado = crash. Como o proximo passo e carregar
codec como plugin, isso precisava ser resolvido ANTES, nao depois.

## O que mudou

- **Xplatbase.vcxproj unificado**: 8 configuracoes, `Debug`/`Release` (estatico) e
  `Debug DLL`/`Release DLL` (compartilhado, saindo em `$(Platform)\$(Configuration)\dll\`).
  Um projeto so; quem consome decide o modo. Antes havia a ideia de um projeto irmao -- foi
  descartada, duplicava manutencao.
- **Macro de export em tres modos** (`xplatbase.h`): `XPLATBASE_BUILD_SHARED` (dllexport),
  `XPLATBASE_USE_SHARED` (dllimport), nenhuma das duas (estatico, como sempre foi).
- **`src/xplat_instance.c` (novo)**: registra a instancia no sistema operacional -- mapeamento
  nomeado + mutex nomeado no Windows, arquivo com `flock` no `TMPDIR` no POSIX, sempre por
  pid. A **segunda** copia acha a primeira, denuncia e fica **inerte** (nao cria segundo
  pool). Politica via `XPLATBASE_DUPLICATE_FATAL`: aborta em Debug por padrao, so relata em
  Release.
- **Modulo fixado na memoria** (`GET_MODULE_HANDLE_EX_FLAG_PIN` / `RTLD_NODELETE`): descarregar
  o modulo com threads do pool vivas matava o processo. Foi assim que o executor de testes
  morria no fim da rodada.
- **CMake**: `-DXPLATBASE_SHARED=ON`, visibilidade escondida e `$ORIGIN` no rpath.

## Regra que passa a valer

Quando houver mais de um modulo usando xplatbase no mesmo processo, **todos linkam a versao
compartilhada**. E Debug nao se mistura com Release entre modulos -- o layout das structs e a
ABI aqui.

## Testes

Os testes antigos foram refeitos em **C**, integrados ao Gerenciador de Testes do Visual
Studio (CppUnitTest: a logica em `.c`, o registro em `registro.cpp`) e ao **CTest** no Linux
(`runner_main.c`). Mesmos arquivos `.c` nos dois sistemas. **17 casos, verdes nos dois.**

Os que provam a instancia unica estao em `test/unittests/test_instancia_modulos.c`:

| caso | o que prova |
|---|---|
| `ModuloCompartilhadoUsaMesmoPool` | o que o modulo aloca aparece na contabilidade do host |
| `ModuloUsaMesmoPoolDeTarefas` | tarefa submetida pelo modulo roda em worker que o host conhece |
| `ModuloCompartilhadoMesmasThreads` | thread criada no modulo aparece no `thread_enum` do host |
| `DuplicataEDenunciada` | modulo que linka estatico **de proposito** e pego pelo detector |
| `CicloCarregaDescarrega` | 50 cargas/descargas sem crescer thread, handle nem memoria |

O caso negativo e o que sustenta os outros: detector que nunca foi visto acusando nao vale
nada.

## Armadilhas encontradas no caminho (custaram tempo)

- `x64\Debug\Xplatbase.lib` **velha** (de setembro) na raiz da solution sombreava a lib de
  importacao nova; o link passava e o binario carregava a copia errada.
- O yason tinha um **xplatbase aninhado com o mesmo ProjectGuid**: o mapeamento de
  configuracoes da solution colidia (`MSB8013`). Resolvido removendo a `ProjectReference`
  do yason e dando ao yason a **propria** macro `YASON_API` (ele declarava a API dele com
  `XPLATBASE_API`, o que gerava `__imp_yason_parse` sem definicao).
- `ProjectReference` propaga a `.lib` estatica; foi preciso `<LinkLibraryDependencies>false`.
- No Linux, `Dl_info`/`dladdr` exigem `_GNU_SOURCE` **antes** dos includes -- o `-include` do
  `msvc_compat.h` entrava na frente e sumia com a declaracao.

## Pendente / nao feito (de proposito)

- **Aborto em Debug nao e validado por teste**: abortar derruba o executor junto. O teste
  desliga a politica e verifica a deteccao; validar o aborto exigiria processo filho.
- **`mem_leak_watch` nao foi exercitado atravessando modulo** -- fica para quando o primeiro
  plugin existir.
- **yason**: `StaticLibrary` em `Debug|x64` e `DynamicLibrary` em `Release|x64`. Parece
  descuido, nao foi mexido.
- O post-build de varios projetos chama `pwsh.exe`, que **nao existe nesta maquina**: o build
  passa, mas cada projeto imprime o erro. Nao foi tocado.

## Estado da regressao (Fase 7)

| item | Windows | Linux |
|---|---|---|
| build completo, Debug | limpo | limpo (Ninja, WSL2) |
| build completo, Release | falta `dashstream.lib`, abaixo | -- |
| 17 testes | verdes em Debug e em Release | verdes (CTest) |
| `httptest` no servidor real | 0 falhas | 0 falhas |

## Defeitos de build que o Release cobrou (ANTERIORES a esta preparacao)

O Debug escondia todos: os artefatos ja estavam no disco de builds antigos.

- **`codecs` nao tem `Build.0` em `Release|x64`** -- so `ActiveCfg`. A solution nunca constroi
  o projeto nessa configuracao e quem linka em Release pega a `codecs.lib` que estiver no
  disco; aqui era de **8 de setembro**, sem `media_decoder_available`, `hw_dec_*` nem
  `enc_tier_name`. Construi o projeto a mao (11,9 MB -> 18,9 MB). **O `Build.0` NAO foi
  reposto** -- pesa no tempo de build e a decisao e sua.
- **`streammanager` em `Release|x64`**: sem `AdditionalIncludeDirectories` (o Debug tem) e
  `DynamicLibrary` sendo que as outras tres configuracoes sao `StaticLibrary`. Como DLL, o
  link cobrava `platform_init`, `list_add`, `string_append`, que nenhum projeto desta
  solution produz. Corrigidos os dois, senao nao havia como medir o Release. **Confira**: e
  projeto fora do escopo desta preparacao.
- **`appservertester` linka `dashstream.lib` e nao ha projeto `dashstream` na solution.** Em
  Debug funciona porque sobrou uma `dashstream.lib` em `x64\Debug`, de um build antigo; em
  Release o link para. Ou a dependencia sai do `appservertester`, ou o projeto volta para a
  solution. **Unica coisa que impede o Release de fechar verde.**
- **`yason` e `streammanager` tinham a MESMA assimetria**: `Release|x64` como
  `DynamicLibrary`, as demais como `StaticLibrary`. Nos dois casos a DLL nunca teve como
  linkar. Os dois viraram `StaticLibrary` (o do yason e na **origem**, precisa do seu push).
- **Nem `unittests` nem `appservertester` declaravam dependencia de `MediaFragmenter`,
  `MediaGateway` e `yason`** -- linkam essas `.lib` pelo nome, sem `ProjectReference`. Em
  build paralelo (ou logo apos um Rebuild) o link acontecia antes da lib existir:
  `LNK1104 MediaFragmenter.lib`. Declaradas em `appserver.sln`; conferido apagando as duas
  libs de proposito e refazendo o build paralelo -- limpo. `codecs` ficou **de fora** da
  lista de proposito: ele nao tem `Build.0` em nenhuma configuracao.
- **Ordem de build: a armadilha que mais voltou nesta refatoracao.** Varios projetos linkam
  `.lib` de OUTROS projetos da solution apenas pelo NOME, sem `ProjectReference` nem
  dependencia declarada. Com build paralelo -- ou logo depois de um Rebuild, que apaga as
  libs -- o link roda antes de a lib existir, e sai `LNK1104`. Apareceu quatro vezes, em
  quatro pares diferentes (`MediaFragmenter.lib`, `codec_core.lib`, `yason.lib`,
  `libopus.lib`). Todas as dependencias reais estao declaradas agora em `appserver.sln`.
  **Ficam de fora de proposito** `SvtAv1Enc`, `libyuv`, `x265-static` e `codecs`: eles nao
  tem `Build.0` em configuracao nenhuma (terceiros, construidos uma vez a mao), entao
  declarar dependencia deles nao teria efeito. O preco e conhecido: se um deles for apagado,
  o link falha e e preciso reconstruir o projeto a mao.
- **`appservertester` nao declarava dependencia do `yason`**: com build paralelo, linkava
  antes da lib existir. Declarada em `appserver.sln`.
