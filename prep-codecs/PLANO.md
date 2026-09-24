# Separar os codecs: um projeto, uma lib por codec

Pasta descartável, como a `prep-xplatbase`. O `.md` é a especificação e a lista de
verificação; cada fase só fecha com o build verde e os testes passando nos dois sistemas.

**Pré-requisito, já feito:** instância única do xplatbase (`prep-xplatbase/PLANO.md`). Sem
ela, um codec carregado como plugin teria o próprio pool de memória e o próprio pool de
tarefas.

---

## Onde estamos

O lado do **encoder** já está abstraído: [`enc_select.c`](../codecs/media/enc_select.c) tem
uma tabela que é a única fonte da verdade, e todo mundo entra por `media_encoder_open()`.
Nenhuma camada de cima chama `h264_encoder_open` direto.

O lado do **decoder** não está:

- [`codec_video.c`](../codecs/media/codec_video.c) mistura três coisas num arquivo só:
  decode H.264 (openh264), decode H.265 (de265) e os utilitários genéricos `imagep_list_*`.
- `h26x_decoder_create` / `h26x_decode_frames` são chamados direto por
  [`gw_decode.c`](../MediaGateway/media/gw_decode.c) e por
  [`embed_ops.c`](../MediaFragmenter/src/embed_ops.c) (3 pontos), e estão declarados até no
  `MediaFragmenter.h`.

## Uma correção de rumo, antes de começar

Eu tinha proposto **rotear o decode H26x pela vtable `MediaDecoder`** e reescrever os
chamadores. Não vou fazer assim, e o motivo importa:

- reescrever `embed_ops.c` e `gw_decode.c` para a vtable é mudança **no pipeline**, com risco
  real (o decode em lote tem comportamento próprio de reordenação e de posse dos planos);
- e **não é necessário** para separar as libs. Basta que `h26x_decoder_create` passe a ser
  uma função do **core**, que despacha para o backend registrado. Os chamadores não mudam,
  e nenhum deles fica dependendo de `codec_h264.lib` ou `codec_h265.lib`.

Unificar os chamadores na vtable continua sendo desejável, mas vira limpeza posterior, e
opcional — não bloqueia nada.

---

## Fase 1 - Quebrar o `codec_video.c` e criar a tabela de decoders — **FEITA**

Arquivos novos, ainda dentro do projeto `codecs`:

| arquivo | conteúdo |
|---|---|
| `image_plane.c` | `imagep_list_*` e `imagep_copy_new` |
| `dec_select.h/.c` | `H26xDecBackend`, a tabela e o ciclo de vida do `DecoderInstance` |
| `codec_h264_dec.c` | decode openh264 |
| `codec_h265_dec.c` | decode de265 |

- [x] `codec_video.c` deixou de existir; `codec_video.h` (a API pública) não mudou
- [x] A tabela em `dec_select.c` é a única fonte da verdade do decode H26x
- [x] `wels/` e `de265.h` só aparecem nos dois arquivos de backend (e no `codec_h264.c`,
      que é o encoder)
- [x] 17 testes verdes nos dois sistemas

**Três defeitos que apareceram no recorte e foram corrigidos:**

1. `h26x_decoder_create` devolvia um `DecoderInstance` com `Instance == NULL` quando o
   decoder falhava ao subir, e o decode seguinte fazia deref nele. Agora falha é falha:
   devolve `NULL`, que todos os chamadores já tratam.
2. O caminho de falha do `h264_decoder_create` soltava o `ISVCDecoder` com
   `memop_free_raw` — ponteiro que quem alocou foi o openh264, não o pool. Nos dois
   caminhos de erro.
3. No decode de H.265, chroma diferente de 4:2:0 saía com `return -2` **sem**
   `de265_release_next_picture` — a imagem ficava presa no decoder.

## Fase 2 - Projetos separados — **FEITA**

O projeto `codecs` compilava **344 arquivos**, e só 14 eram nossos: o resto é opus, libvpx
e dav1d. Por isso o recorte ficou em dois níveis, seguindo a convenção que o repositório já
usava para `libyuv`, `x265-static` e `SvtAv1Enc`:

| projeto | arquivos | o que é |
|---|---|---|
| `codec_core` | 5 | `media_codec.c`, `enc_select.c`, `dec_select.c`, `image_plane.c`, `cpu_features.c` |
| `codec_h264` | 2 | encoder + decoder openh264 |
| `codec_h265` | 2 | encoder x265 + decoder de265 |
| `codec_vp9` | 1 | adaptador |
| `codec_av1` | 1 | adaptador |
| `codec_opus` | 1 | adaptador |
| `codec_hw` | 2 | Media Foundation (fora do Windows, só os stubs) |
| `libopus` | 137 | terceiros |
| `libvpx` | 164 | terceiros (inclui os 24 `.asm` do nasm) |
| `libdav1d` | 99 | terceiros (inclui os 46 `.asm`) |

- [x] Dez `.vcxproj`, um por projeto, todos saindo em `codecs\x64\<Config>\` — os
      consumidores não mudaram de caminho, só de nome de lib
- [x] `Build.0` declarado para todos (o `codecs` de antes **não tinha**, e por isso a
      `codecs.lib` de Release era de 8 de setembro)
- [x] Dependências declaradas na solution — sem isso o link de `unittests` e
      `appservertester` acontecia antes das libs existirem (`LNK1104 codec_core.lib`)
- [x] O projeto `codecs` saiu da solution; o `.vcxproj` continua no disco, pode apagar
- [x] No Linux, `codecs_media` virou sete alvos e um alvo `INTERFACE` com o mesmo nome:
      quem linkava não mudou de linha
- [x] Build limpo e 17 testes verdes nos dois sistemas

**Efeito colateral bom:** cada codec agora compila sozinho. Mexer no `codec_av1.c` recompila
1 arquivo, não 344.

## Cobertura: um teste de ida e volta por codec

A suíte cobria VP9, AV1 e H.264. **H.265 e Opus não tinham teste nenhum** — e o
`codec_h265_dec.c` foi um dos arquivos que a separação criou. Um recorte que ninguém
exercita não está verificado, está só compilando.

`test/unittests/test_codecs.c` codifica 12 quadros sintéticos, decodifica o que saiu e
confere três coisas: que veio quadro, que as dimensões batem, e que **saíram pelo menos
tantos quadros quanto entraram** (perder os últimos no flush por B-frame retido já
aconteceu aqui e custou frames em toda conversão).

| caso | encode | decode |
|---|---|---|
| `Codecs.IdaEVoltaH264` | openh264 | openh264 |
| `Codecs.IdaEVoltaH265` | x265 | libde265 |
| `Codecs.IdaEVoltaVp9` | libvpx | libvpx |
| `Codecs.IdaEVoltaAv1` | SVT-AV1 | dav1d |
| `Codecs.EncodeOpus` | libopus | — (não há decoder de áudio nesta camada) |

Os pacotes são entregues **um a um** ao decoder: VP9 e AV1 esperam um pacote por chamada, e
concatenar tudo (o que Annex-B aceita) não serviria para eles.

**`T_SKIP` no framework.** Um teste que saía cedo porque faltava um codec ou um hardware
passava calado, indistinguível de um que verificou tudo — olhando o verde não dava para
saber o que foi de fato exercitado. Agora ele se marca como pulado, com o motivo, e isso
aparece no Gerenciador de Testes e na saída do executor do Linux.

Conferido: **nenhum dos 22 testes pula** nesta máquina, nos dois sistemas. O verde é verde
de verdade.

## Transicao entre codecs no gateway

Cada teste de codec, sozinho, abre o encoder, usa e fecha. O que nao estava coberto e a
**troca**: fechar o SVT-AV1 e abrir o x265 no mesmo processo, ir de 160x120 para 320x240,
de 15 para 30 fps. E ai que sobra estado — buffer dimensionado para a resolucao anterior,
decoder que nao soltou o que reteve, tabela global apontando para memoria que ja saiu.

`test/unittests/test_gateway_matrix.c` percorre tres matrizes, **cada uma duas vezes**:

| caso | o que alterna |
|---|---|
| `GatewayMatriz.AlternaCodec` | saida H.264 -> VP9 -> H.265 -> AV1 -> H.264 |
| `GatewayMatriz.AlternaResolucaoEFps` | 160x120@15 -> 320x240@30 -> 96x72@10 -> 240x176@24 -> 160x120@15 |
| `GatewayMatriz.AlternaEntrada` | entrada VP9 -> AV1 -> AV1 -> VP9 (troca o DECODER) |

Cada rodada verifica tres coisas:

1. o gateway terminou sem evento de erro e com evento de conclusao;
2. a saida **decodifica**, e na resolucao daquela rodada — e assim que lixo de uma transicao
   aparece (quando o container e arquivo unico; nos segmentados, que ha segmento escrito);
3. no fim, que a **segunda volta inteira nao deixou alocacao viva a mais** que a primeira.
   Se cada transicao vazasse um pedaco, a conta cresceria a cada volta.

Resultado: verdes nos dois sistemas, 10 a 12 s cada. Nenhuma transicao gerou erro nem sobra
de memoria.

## Fase 3 — Descritor de plugin — **FEITA**

Cada projeto de codec passou a exportar **um único símbolo**: um `CodecPluginSet` com os
backends que ele traz. O núcleo não conhece mais função de codec nenhuma.

### Antes e depois

| o núcleo carregava | agora |
|---|---|
| `h264_encoder_open`, `h265_encoder_open`, `vp9_encoder_open`, `av1_encoder_open`, `opus_encoder_open` | — |
| `h264_decoder_open`, `h265_decoder_open`, `vp9_decoder_open`, `av1_decoder_open` | — |
| `h264_dec_backend`, `h265_dec_backend` | — |
| `mf_hw_encoder_open`, `mf_hw_probe`, `hw_dec_probe` | — |
| a faixa de SIMD de cada lib, os `#ifdef HAVE_*`, o texto de cada backend | no projeto do próprio codec |
| **três tabelas** (`g_enc`, `g_dec`, `BACKENDS`) | **uma lista de seis `extern`**, um por projeto |

O descritor está em [`codec_plugin.h`](../codecs/media/codec_plugin.h); o registro, em
[`codec_registry.c`](../codecs/media/codec_registry.c).

- [x] `CodecPlugin` e `CodecPluginSet` definidos no núcleo
- [x] Cada projeto expõe o seu: `codec_set_h264`, `codec_set_h265`, `codec_set_vp9`,
      `codec_set_av1`, `codec_set_opus`, `codec_set_hw`
- [x] `enc_select.c` só **decide** — a cascata percorre os descritores
- [x] `dec_select.c` e `media_codec.c` idem
- [x] Nada no núcleo cita nome de função de codec (conferido por varredura)
- [x] 25 testes verdes nos dois sistemas, nenhum pulado

### Duas coisas que melhoraram de brinde

1. **As duas listas de diagnóstico viraram a mesma função.** `media_encoder_list` e
   `media_decoder_list` liam tabelas separadas (`g_enc` e `g_dec`) que podiam divergir; agora
   são a mesma varredura com o papel trocado (`CODEC_ROLE_ENCODE` / `CODEC_ROLE_DECODE`).
2. **O Opus deixou de ser exceção.** `media_encoder_open` tinha um desvio no topo só para
   áudio; agora ele entra na mesma varredura, porque o descritor dele diz que não tem degrau
   de hardware.

### Um cuidado que valeu a pena

O descritor do H.265 **não** publica `DecoderOpen`. A função existe (`h265_decoder_open`) mas
é um stub que sempre devolve 0; publicá-la faria `media_decoder_available(H265)` responder 1
enquanto `media_decoder_open(H265)` devolve 0 — o núcleo passaria a mentir. O HEVC de entrada
decodifica pela via h26x, que é outra coisa e está declarada no mesmo descritor.

### O que a Fase 4 vai precisar mudar

Só o **modo de achar o símbolo**: os seis `extern` do `codec_registry.c` viram varredura de
diretório + `GetProcAddress`/`dlsym` por um nome de símbolo conhecido. O conteúdo do
descritor não muda, e nenhum outro arquivo do núcleo é tocado — que era o objetivo desta fase.

## Fase 4 — Carga dinâmica — **FEITA**

**Os cinco codecs de software saíram do binário** e são carregados em tempo de execução:

| plugin | Windows | Linux | traz |
|---|---|---|---|
| H.264 | `codec_h264_plugin.dll` | `libcodec_h264.so` | openh264 (encode e decode) |
| H.265 | `codec_h265_plugin.dll` | `libcodec_h265.so` | x265 + libde265 |
| VP9 | `codec_vp9_plugin.dll` | `libcodec_vp9.so` | libvpx |
| AV1 | `codec_av1_plugin.dll` | `libcodec_av1.so` | SVT-AV1 + dav1d |
| Opus | `codec_opus_plugin.dll` | `libcodec_opus.so` | libopus |

No binário ficaram só o núcleo (`codec_core`) e o caminho de **hardware** (`codec_hw`,
Media Foundation / DXVA / VAAPI) — esse não é plugin porque o `gw_decode` fala com a GPU
direto, por outra via.

- [x] O descritor virou export de DLL/`.so` (`codec_plugin_entry`, um símbolo só)
- [x] Carga por demanda, e **descarga** com recusa quando há uso vivo
- [x] O plugin usa o xplatbase compartilhado — verificado por teste, não por suposição
- [x] 29 testes verdes nos dois sistemas, nenhum pulado

### Por que não precisou de "API de host"

O plugin **não linka o núcleo**: ele carrega a própria cópia dos utilitários de imagem. Isso
seria uma armadilha clássica — memória alocada num módulo, liberada noutro — se o xplatbase
não fosse único no processo. Como é (Fase 1 a 7 da `prep-xplatbase`), funciona. Foi o
trabalho daquela preparação pagando aqui.

### Abrir e carregar são a mesma chamada

Não há comando de carga para chamar antes: **quem pede o codec traz o módulo**.
`media_encoder_open`, `media_decoder_open` e `media_codec_available` resolvem sozinhos, e
carregam **só o módulo daquele codec** — não a pasta inteira. O arquivo é achado pela
convenção de nome, derivada do próprio `media_codec_name` ("H.264" → `codec_h264*`), então
o núcleo continua sem tabela de codecs própria.

A primeira versão carregava tudo na primeira consulta ao registro. Funcionava, mas trazia
para o processo módulos que ninguém tinha pedido — o contrário do que carga por demanda
deveria significar. Só as listas de diagnóstico (`media_encoder_list` com
`MEDIA_CODEC_NONE`) varrem a pasta toda, porque aí a pergunta é mesmo "o que existe?".

`codec_plugins_autoload(0)` desliga, para quem quiser carregar na mão. E consultar a lista
de módulos **não** dispara carga: ela responde o que está carregado agora.

A pasta procurada é a do **módulo**, não a do executável — dentro do `unittests.dll` a pasta
do executável é a do Visual Studio, e não a do binário recém-gerado.

### Descarga: o problema de verdade

Fechar o módulo com um encoder aberto derruba o processo na chamada seguinte — foi assim que
o executor de testes morria antes de o xplatbase fixar o próprio módulo. Por isso há
**contagem de uso**: ao abrir, o `Close` é desviado para um atalho que devolve a contagem;
enquanto houver encoder ou decoder vivo, `codec_plugin_unload` devolve `-1` e não descarrega.

| teste | o que prova |
|---|---|
| `Plugins.Carregado` | o módulo é achado sozinho e o Opus aparece como disponível |
| `Plugins.UsaOPoolDoHost` | o que o plugin aloca entra na contabilidade do host, e não há segunda instância de xplatbase |
| `Plugins.NaoSaiEmUso` | com encoder aberto, descarregar é **recusado**; depois do `Close`, a contagem zera |
| `Plugins.DescarregaEVolta` | sai, o Opus some da lista, volta, e volta a funcionar |

**Prova de que o teste testa:** tirando o arquivo do lugar, no Windows o `EncodeOpus` passa
de 40 ms para "PULADO" em 3 ms, e no Linux a suíte cai de 29/29 para 28/29 — o
`Plugins.Carregado` falha, como tem de falhar. Com o arquivo de volta, 29/29 nos dois.

### Como foi feito

O Opus foi o primeiro, por ser o de menor alcance (áudio, só encode): um erro de encanamento
apareceria isolado, e não misturado com decode, escala e hardware. Com ele funcionando, os
outros quatro saíram pela mesma receita — o `.vcxproj` do plugin é gerado a partir do
estático trocando três coisas (tipo do projeto, o define `CODEC_<X>_PLUGIN`, e as libs de
terceiros no link), e no Linux é uma função de CMake só.

Os plugins de H.264 e H.265 levam a **própria cópia** do `image_plane.c`, porque os backends
de decode em lote usam os utilitários de imagem que moram no núcleo. Levar a cópia, em vez
de linkar o núcleo, só é seguro porque o xplatbase é único no processo.

**Prova com os cinco:** tirando os cinco arquivos da pasta, a suíte cai de 29/29 para
**20 aprovados e 9 falhas** — exatamente os testes que precisam de codec (round-trips,
matriz do gateway, pipeline). Com os arquivos de volta, 29/29 nos dois sistemas. E o
servidor real, rodado pelo `httptest`, anuncia os quatro encoders e os quatro decoders e
passa com 0 falhas.

### Dois defeitos que a Fase 4 revelou

1. **A carga automática só disparava pela consulta ao registro.** Quem perguntasse ao
   carregador (`codec_plugins_count`) recebia "nenhum módulo" antes de qualquer plugin ter
   tido chance de entrar — resposta que dependia da ordem de chamada, não da realidade. Os
   testes novos caíram exatamente nisso. Corrigido junto com a mudança para carga dirigida:
   agora a resolução acontece em quem **abre** o codec, que é o único momento em que se sabe
   do que o processo precisa.
2. **`_GNU_SOURCE` no `.c` chega tarde no Linux**: o `-include msvc_compat.h` do `app_compat`
   entra antes do arquivo, e o `dladdr`/`Dl_info` some. Tem de ir pela linha de comando. É a
   segunda vez que esta armadilha aparece no projeto (a primeira foi no `mod_duplicado`).
