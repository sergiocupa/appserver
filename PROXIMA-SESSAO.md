# Passagem de sessão — vídeo (MediaGateway / MediaFragmenter / player)

Estado em **2026-10-13**. Escrito para quem continuar o trabalho (pessoa ou agente) sem o
histórico da conversa. Tudo o que está aqui foi medido nesta sessão, salvo onde diz o contrário.

## 1. Estado do repositório

- Branch `main`, último commit `9cc5fd1` (a sequência de vídeo anterior **já está commitada**).
- **Não commitado:** o trabalho da segunda à quinta rodadas de 2026-10-13 (pendências 1 a 6
  da lista antiga, mais a remoção do `embed_ops` e o silêncio em 5.1) — arquivos modificados,
  2 novos e 2 removidos (`git status` mostra a lista).
- Submódulos: `appserver/submodules/yason` em `c8ff0be`; `appserver/submodules/xplatbase` em `fd4db2a`.
- Servidor de teste **parado**. Sessões de teste apagadas; a biblioteca tem só os dois vídeos
  de antes (`29ae719e3df1e2bd`, `98652b380317f30a`), com o HLS e o `master.mpd` **repreparados
  com o código novo** (texto de codec por resolução). Os DASH VP9/AV1 deles não foram refeitos
  (nada mudou nesse caminho para esses arquivos).
- Só o **Release x64** foi compilado e testado no Windows. O **Debug não foi recompilado**.
- Suítes com os binários finais: **Windows 93/93**, **Linux 93/93** (4 pulados: bancada e
  codificador por placa).

### Arquivos novos (precisam de `git add`)
`MediaGateway/media/aac_silence.h`, `test/unittests/test_video.c`

### Arquivos removidos
`MediaFragmenter/src/embed_ops.c`, `MediaFragmenter/include/embed_ops.h` (nenhuma função era
chamada). Saíram também do `MediaFragmenter.vcxproj`, do `.filters` e do `CMakeLists.txt`. Os
documentos antigos que os citam (`REFACTOR-NOTES.md`, `prep-codecs/PLANO.md`,
`test/appservertester/media/*.md`) ficaram como estão: são histórico.

### Arquivos modificados, por assunto
| Assunto | Arquivos |
|---|---|
| Áudio atrasado: silêncio inicial | `aac_silence.h`, `sink_hls.c`, `aud_fmp4.c` |
| Arquivo cortado recodifica; cópia começa em zero | `gw_types.h` (`PrerollUs`), `MediaFragmenter/src/media_source.c`, `sync_gateway.c` |
| Texto de codec do H.264 por resolução | `sink_hls.c` (`vcodec_str`, `codecs` em cada `Representation`) |
| `embed_ops` removido inteiro | `embed_ops.c/.h` (apagados), `MediaFragmenter.vcxproj/.filters`, `CMakeLists.txt`, `hls_controller.c` |
| Testes | `test_video.c`, `testes.h`, `registro.cpp`, `runner_main.c`, `unittests.vcxproj`, `CMakeLists.txt` |

## 2. Observações para o commit

1. **Não incluir** os submódulos que já estavam sujos antes (`appserver/submodules/utility`,
   `streammanager/submodules/*`): não são deste trabalho.
2. Os avisos "LF will be replaced by CRLF" são só de fim de linha, inofensivos.
3. **Mudanças de comportamento que quem usa a saída precisa saber:**
   - Pedir a mesma resolução (cópia) de um arquivo **cortado sem recodificar** agora
     **recodifica** no HLS/DASH: a saída começa no quadro exato e o evento de início de pista
     diz o motivo. O MP4 de arquivo único continua copiando.
   - Na cópia em segmentos o vídeo começa em **zero**; os segmentos passam a ter deslocamento
     de composição **negativo** (`trun` versão 1). Tocou no hls.js 1.7.1 e no dash.js 4.7.4
     (Chromium). **Não testado no Safari.**
   - Áudio que entra atrasado ganha quadros AAC de **silêncio** no começo da faixa (AAC-LC,
     1 a 6 canais; 7.1, HE-AAC e taxa explícita ficam como antes).
   - `master.mpd`: o `codecs` do vídeo saiu do `AdaptationSet` e foi para cada `Representation`.
   - `MediaStreamInfo` ganhou o campo `PrerollUs`.
   - **Arquivo cortado tem três modos** (`MediaProfile.CutFileMode`; no servidor de teste,
     `"cutFiles"` no `appservertester.json`): `recode` (padrão, recodifica tudo), `smart`
     (recodifica só o primeiro grupo de quadros e copia o resto; H.264 e H.265) e `copy` (copia
     tudo; o trecho escondido fica comprimido e o manifesto manda começar depois dele). Sem
     codificador, cai em `copy`.
   - No modo `smart` a entrada de amostra é **`avc3`** (H.264) ou **`hev1`** (H.265), com os
     parâmetros dentro das amostras, e o texto de codec acompanha. No modo `copy` o trecho
     escondido fica comprimido a 2 ms por quadro, o HLS leva `#EXT-X-START` e a URL do DASH
     registrada na sessão leva a âncora `#t=segundos`.
   - O modo pode vir da **sessão** (`cutFiles` no `session.json`, seletor na tela) e vale mais
     que o do servidor.
   - `#EXT-X-TARGETDURATION` passa a ser o maior segmento real quando ele excede o alvo.
   - O DASH VP9 também segura o corte (último segmento não fica minúsculo).
   - O sink do HLS **segura cada corte** até o segmento seguinte ter tamanho mínimo (um quarto
     do alvo, entre 200 ms e 1 s); resto menor que isso no fim do vídeo é juntado ao segmento
     anterior. As durações do `EXTINF` passaram a ser exatas (4,000 em vez de 3,999).
   - `MediaTrackOut` ganhou `StartUs` e `InBandParams`; `mux_mp4` ganhou `MP4_KIND_AVC3`;
     `H26xToLen` ganhou `keep_ps`.
4. Sugestão de título:
   `video: silencio inicial no audio atrasado, arquivo cortado recodifica em segmentos, copia em HLS comeca em zero, texto de codec do H.264 por resolucao, embed_ops removido; 12 testes de video`

## 3. Pendências

### Feito na segunda rodada de 2026-10-13 (eram as pendências 1 a 6)
1. **Áudio atrasado**: silêncio até o primeiro pacote. Medido com áudio em 0,500 s: a faixa
   passou a começar em 0,0124 s, o primeiro pacote real ficou na amostra 22050 e o player
   começa em 0 (HLS) / 0,023 s (DASH e DASH VP9).
2. **Testes automáticos**: 12 casos em `test_video.c` (classe `Video`), cada um gerando a
   própria entrada. Prova inversa feita: desfazendo as correções, 6 deles falham.
3. **Arquivo cortado em segmentos**: recodifica. Medido: 577 quadros (antes 629), começo em 0,
   PSNR médio 42,2 dB contra a origem (30,7 dB deslocando um quadro de propósito).
4. **Um quadro depois do zero**: resolvido com composição negativa. Primeira amostra em 0,000 s.
5. **Texto de codec do H.264**: montado do `avcC` de cada resolução.
6. **`embed_transcode_webm`**: removida.

### Feito na terceira rodada de 2026-10-13
- **`embed_ops.c/.h` removidos inteiros**: as outras quatro funções também não tinham chamador.
- **Silêncio em 5.1**: quadro montado com SCE + CPE + CPE + LFE (o que os players usam põe o
  canal de graves como canal comum e o decoder avisa). Decodifica como 5.1 sem aviso a 32,
  44,1, 48 e 96 kHz. Medido com áudio 5.1 entrando em 0,476 s: 20 quadros de 26 bytes, primeiro
  pacote real na amostra 20992, player começa em 0 (HLS) / 0,023 s (DASH).
- **Biblioteca repreparada** em HLS: os dois vídeos agora declaram `avc1.4D40xx` por resolução.

### Feito na quarta rodada de 2026-10-13
- **Último segmento de um quadro só**: resolvido no sink do HLS. Medido no trecho de 24 s:
  6 segmentos (5 de 96 quadros e 1 de 97) em vez de 7 com o último de 1 quadro.
- **Arquivo cortado sem codificador**: modo `copy`. Medido: HLS e DASH começam em 2,167 s no
  player. O deslocamento de apresentação do DASH (`presentationTimeOffset`) foi testado e
  **não serve**: o Chrome descarta até o quadro-chave seguinte e o vídeo só começava em 4,708 s.
- **Recodificar só o primeiro grupo**: modo `smart`. Medido no trecho de 24 s: pronto em 2,0 s
  (7,5 s recodificando tudo), 577 quadros, os 113 primeiros recodificados (38 dB) e os 464
  restantes **idênticos** à origem; toca em HLS e DASH passando pela emenda, 0 quadros perdidos.
- **Testes**: mais 3 casos em `test_video.c` (15 no total); a prova inversa derruba os 3.
- **Como comparar conteúdo**: o filtro `psnr` do ffmpeg casa os quadros pelo tempo e erra com
  composição negativa (só casou 464 de 577). Comparar por **índice de quadro**: despejar os
  dois em `rawvideo` com `-fps_mode passthrough` e calcular fora.

### Feito na quinta rodada de 2026-10-13 (os cinco pontos que tinham ficado em aberto)
1. **`#EXT-X-TARGETDURATION`**: no fim da preparação a linha é regravada com o maior segmento
   real, quando ele passa do declarado. Medido na cópia do trecho de 10 s: declara 9 para um
   segmento de 8,958 s (antes 6).
2. **Último segmento no DASH VP9**: o defeito existia (12 segmentos de 2 s e um de 41 ms) e foi
   corrigido com a mesma espera do HLS. Medido: 12 segmentos, o último de 2,041 s. O DASH AV1
   não tinha o defeito. `GW_SP_KBUF` foi de 3 para 4 (um segmento fica retido).
3. **Modo de arquivo cortado por sessão**: campo `cutFiles` no `session.json`, aceito em
   `POST /api/session/config/<id>`, e um seletor "Arquivo cortado" na tela. Vale mais que o do
   servidor; vazio herda o do servidor.
4. **Modo `smart` em H.265**: entrada `hev1`, parâmetros (VPS/SPS/PPS) dentro das amostras; a
   troca para cópia só acontece em quadro-chave que fecha o grupo (IDR, não CRA/BLA). Medido
   num 1080p de 24 s: 578 quadros, 530 copiados idênticos à origem, pronto em 1,9 s.
5. **Modo `copy` sem depender do player**: os quadros escondidos ficam nos segmentos, mas
   comprimidos a 2 ms cada (2,167 s viram 0,104 s). `EXT-X-START` e a âncora `#t=` continuam,
   para quem respeita. Tocando desde o zero no navegador: sem travar, 0 quadros perdidos.
- **Testes**: mais 3 casos (18 de vídeo no total) e o modo por sessão dentro do caso de
  sessão; a prova inversa derruba um teste para cada um dos cinco pontos.

### Feito na sexta rodada de 2026-10-13 (lista A1 a A8)
| # | Item | Situação | Evidência e teste |
|---|---|---|---|
| A1 | libde265 perdia um quadro por grupo (H.265 com quadros B) | **Corrigido** | Arquivo real: recodificação com 578 de 578 quadros, alinhados (antes 569, deslocados). Teste `Video.DecodeH265ComQuadrosB`: com o decodificador antigo saem 27 de 30 |
| A2 | Quadro repetido na emenda do modo `smart` em H.265 | **Resolvido pelo A1** | Emenda com o quadro verdadeiro (38,9 dB; antes 31,0). Teste `Video.HlsCortadoH265QuadrosBEmendaInteira` decodifica a saída e acusa quadro repetido |
| A3 | Safari com composição negativa | **Não testado** (sem aparelho) | Vídeo `SAFARI-1` deixado pronto na biblioteca |
| A4 | Safari com `avc3` e `hev1` | **Não testado** (sem aparelho) | Vídeos `SAFARI-2` e `SAFARI-3` prontos. Teste `Video.PadraoUsaFormatoAceitoPorTodos` segura o padrão em `avc1`/`hvc1` sem composição negativa |
| A5 | DASH AV1 cortava a cada 6,7 s | **Corrigido** | Quadro-chave a cada 2 s, grupo fechado. Arquivo real: 12 segmentos de 2,04 s. Teste `Video.DashAv1SegmentosSeguemOAlvo` |
| A6 | A tela não lia o valor salvo | **Corrigido** | Ao trocar de sessão os campos da saída mostram o gravado. Conferido no navegador; **sem teste automático** (o projeto não tem teste de tela) |
| A7 | Modo `copy`: a duração cresce o trecho comprimido | **Aceito** (limite) | A cópia do teste dura 2,020 s (2,000 + 0,020). Verificação dentro de `Video.HlsCortadoCopiaMandaComecarDepois` |
| A8 | Modo `copy`: lampejo dos quadros escondidos | **Aceito** (limite) | Os quadros escondidos ficam nos primeiros 20 ms do teste. Mesma verificação |

- **Como o A1 foi resolvido** (`codecs/media/codec_h265_dec.c`): o aviso de fim de quadro é dado
  depois de cada pacote, o que faz o libde265 devolver o quadro na hora e sem perda, mas na
  ordem de decodificação. A ordem de apresentação é reconstruída no próprio arquivo: cada
  pacote leva uma etiqueta que volta com o quadro, a posição (POC) é lida do cabeçalho da
  primeira fatia, e os quadros saem de uma fila ordenada pela regra da norma
  (`sps_max_num_reorder_pics`). Vale para todo consumidor do decodificador, não só o gateway.
- **Dado de teste novo:** `test/unittests/dados_hevc_b.h`, um fluxo H.265 de 30 quadros com
  quadros B (8 KB), gerado uma vez com o ffmpeg porque o codificador do projeto não produz
  quadros B. O cinza de cada quadro diz a posição dele, o que permite conferir ordem e perda.
- **Preenchimento da emenda** (repetir o último quadro quando o decodificador devolve menos do
  que recebeu) continua no gateway, como rede de segurança; com o A1 corrigido ele não age.

### Feito na sétima rodada de 2026-10-13: a tela pergunta o que fazer com arquivo recortado
- **Popup ao fragmentar.** Se o vídeo enviado tem trecho escondido (lista de edição), o clique
  em fragmentar abre um popup com duas linhas na mesma escala -- "Gravado no arquivo" e
  "Exibido pelo arquivo", esta com o começo listrado -- e três botões: obedecer o corte,
  ignorar o corte e usar a linha do tempo toda, cancelar. A resposta vai para o campo
  "Arquivo cortado" e é gravada na sessão. Vale também para "Arquivo único".
- **Plano do vídeo** (`POST /api/video/prepare`) ganhou `"hidden": {"seconds", "frames",
  "recorded"}`; `seconds` 0 quando o arquivo não esconde nada.
- **Modo novo `showall`** (`GW_CUT_SHOW_ALL`): ignora o corte e mostra tudo o que está gravado,
  desde o primeiro quadro. A linha do tempo inteira (vídeo e áudio) anda o tanto que a origem
  escondia, então o sincronismo se mantém. Vale em **toda** saída (cópia, escada, DASH, arquivo
  único); os outros três modos só valem no pedido de cópia. Aceito em `cutFiles` da sessão e
  do `appservertester.json`.
- DASH e arquivo único passaram a ler o modo da sessão (antes só o HLS lia).
- **Medido** com o trecho de 24 s pela tela: ignorar dá 26,207 s no player; obedecer dá
  24,041 s; cancelar não inicia nada.
- **Testes:** `Video.CortadoLinhaDoTempoToda` (cópia: 60 quadros na cadência original, sem
  `EXT-X-START`; escada: 60 quadros por pista, contra 50 obedecendo). O popup em si foi
  conferido no navegador; **sem teste automático** (o projeto não tem teste de tela).
- Com isso o **A7 e o A8** deixam de ser o único caminho para quem quer copiar: quem prefere
  ver o trecho escondido inteiro, em velocidade normal, escolhe "linha do tempo toda".

### Feito na oitava rodada de 2026-10-13: quadro-chave do H.265 e custo do AV1
- **H.265 (x265) com quadro-chave a cada 2 s**, grupo fechado, como já era no H.264 e passou a
  ser no AV1. O padrão do x265 era até 250 quadros (10 s): o HLS recodificado em H.265 saía num
  segmento só ou cortava em troca de cena. Teste `Video.HlsH265SegmentosSeguemOAlvo`: sem a
  correção, 4 s com alvo de 2 s dão 1 segmento; com ela, segmentos de até 2 s.
- **Quadro-chave do x265 passou a ser só IDR.** Antes um quadro I comum (de troca de cena) era
  reportado como quadro-chave, e o gateway podia cortar um segmento nele; os quadros seguintes
  podem depender de quadros anteriores a um I comum, então o segmento não decodificaria
  sozinho. O mesmo teste confere que cada segmento começa em IDR.
- **Custo do quadro-chave de 2 s no AV1, medido** (DASH AV1 em escada do trecho de 10 s,
  1080p, mesma máquina, alternando com/sem/com/sem/com):

  | | Tempo de preparo | Segmentos (1080p) | Tamanho total | PSNR médio |
  |---|---|---|---|---|
  | Padrão do codificador (antes) | 52,6 s e 52,6 s | 2 | 7,38 MB | 38,05 dB |
  | Quadro-chave a cada 2 s (agora) | 58,3, 58,4 e 57,3 s | 5 | 5,25 MB | 38,05 dB |

  O preparo ficou **cerca de 10% mais lento**; o arquivo ficou 29% menor com a mesma
  qualidade medida (PSNR em resolução reduzida, 320x180). Não investiguei por que fica menor;
  o padrão do codificador usa grupo aberto e outro intervalo, e o controle de taxa responde
  diferente.

### Safari (A3 e A4): PENDENTE por decisão do dono do projeto (2026-10-13)
Fica para quando houver um aparelho Apple. Não dá para fechar nesta máquina:
- **Safari para Windows** parou na versão 5.1.7 (2012): não toca HLS em fMP4 nem tem a base de
  mídia do Safari atual.
- **Não existe Safari para Linux/WSL.** Os navegadores WebKit de lá (GNOME Web, WebKit do
  Playwright) decodificam com o GStreamer, não com a base de mídia da Apple (AVFoundation), que
  é justamente o que está em dúvida; e não têm HLS nativo, que é como o iPhone toca.
- **O que serve:** um aparelho Apple na mesma rede, um Mac com acesso remoto (permite rodar o
  `mediastreamvalidator`) ou um serviço de aparelhos reais na nuvem.
- **Enquanto isso:** o padrão de arquivo cortado continua `recode`, que não usa `avc3`/`hev1`.
  A cópia de vídeo com quadros B (composição negativa) é comportamento padrão e segue sem
  verificação no Safari -- é o risco que fica em aberto.

### Teste no Safari (A3 e A4): como fazer
Três vídeos ficaram na biblioteca do servidor de teste, só para isso:
- `SAFARI-1` (`3003ca0f1359a257`): cópia de vídeo com quadros B, entrada `avc1`, composição negativa.
  **É o caminho padrão da cópia**, por isso é o mais importante.
- `SAFARI-2` (`533906c9257b81e6`): modo `smart` em H.264, entrada `avc3`.
- `SAFARI-3` (`a643f03c35162291`): modo `smart` em H.265, entrada `hev1`.
Abrir a página do servidor num aparelho Apple e tocar cada um em HLS. Conferir: começa do
início, a imagem não quebra depois de ~5 s (a emenda do `SAFARI-2`) e depois de 2 s (a do
`SAFARI-3`), e o salto na barra funciona. Pela rede local o servidor respondeu **403** a um
pedido feito desta máquina para o próprio endereço de rede; pode ser preciso liberar o acesso.
Para apagar depois: `POST /api/session/delete/<id>`.

### Em aberto
- **A3 e A4** (acima): dependem de um aparelho Apple.
- **A6 sem teste automático**: só verificação no navegador.
- Modo `copy`: A7 e A8 são limites aceitos (ver tabela).

### Não feito (igual a antes)
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
- **Composição negativa engana o ffprobe:** segmento copiado de fonte com quadros B (`trun`
  versão 1) aparece com o pts somado de um quadro. Ler `tfdt`/`trun` direto das caixas.
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
