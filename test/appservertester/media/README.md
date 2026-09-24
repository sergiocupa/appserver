# media/ — codecs embutidos (sem ffmpeg)

Camada de codecs montada "do zero": **uma lib por codec**, um **wrapper central**
(`media_codec.c`) e um **enum de seleção** (`MediaCodec`). Embutida no servidor C.

## Arquivos
| Arquivo | Papel |
|---|---|
| `media_codec.h` | enum `MediaCodec`, `MediaFrame` (YUV), `MediaPacket`, interfaces `MediaEncoder`/`MediaDecoder`, wrapper |
| `media_codec.c` | wrapper central: `media_encoder_open(codec)` faz o dispatch por enum |
| `codec_vp9.c` | **VP9 encoder implementado** (libvpx) — módulo de referência |
| `codec_av1.c` | AV1 (SVT-AV1) — esqueleto p/ preencher |
| `codec_h264.c` | H.264 (OpenH264) encode+decode — esqueleto |
| `codec_h265.c` | **H.265 encoder implementado** (x265, `HAVE_X265`) — decode HEVC ainda via MediaFragmenter (**GPL/patente**) |
| `codec_opus.c` | Opus (libopus) — esqueleto |

Cada módulo é ativado por um `#define HAVE_*` no build. **Sem o define, o módulo
compila mas retorna `NULL` (indisponível)** — então o projeto sempre compila e cada
codec "liga" quando você linka a lib.

## Topologia (o pipeline "sem ffmpeg")
```
MP4 --[demux]--> pacote --[decoder]--> MediaFrame(YUV) --[scale]-->
    MediaFrame --[encoder]--> MediaPacket --[mux/segment]--> WebM/MP4/DASH
```
Esta camada cobre **decoder** e **encoder** (frame <-> pacote). Faltam 3 peças para
substituir o ffmpeg de ponta a ponta:

1. **Demux do MP4 de entrada** → `Bento4` ou `GPAC` (extrair os pacotes H.264/H.265).
2. **Decode da entrada** → `codec_h264.c` (OpenH264) / `codec_h265.c` (libde265).
3. **Mux + segmentação** → `libwebm` (WebM/VP9) + gerar `manifest.mpd` (DASH) você mesmo
   (é o que o `-f dash` do ffmpeg fazia). Para HLS, segmentar em fMP4 + `.m3u8`.

Também é útil um **scaler** (`libyuv`, BSD) para gerar cada resolução (I420 → I420 menor).

## Build (Windows / MSVC via vcpkg)
```bat
vcpkg install libvpx opus libyuv fdk-aac
:: opcionais: svt-av1 (encode AV1) dav1d (decode AV1/preview) openh264 ; x265/libde265 so p/ H.265 (GPL/patente)
:: defines correspondentes: HAVE_SVTAV1 / HAVE_DAV1D / HAVE_OPENH264 / HAVE_X265
```
No `.vcxproj` do appservertester:
- adicionar TODOS os `.c` de `media/` ao projeto:
  `media_codec.c codec_vp9.c codec_av1.c codec_h264.c codec_h265.c codec_opus.c mux_webm.c mux_mp4.c pipeline.c audio_aac.c mp4_timing.c embed_ops.c`
  e o **gateway de sync**: `media_source.c media_sink.c sink_mp4.c sink_hls.c sink_dash.c h26x_util.c sync_gateway.c`;
- **xplatbase**: adicionar o include dir (`...\Xplatbase\Xplatbase\include`) e linkar `Xplatbase.lib` — o gateway aloca SO pelo `memory_pool` (`memop_*`) e usa `thread_*`/`pool_*`;
- garantir include/link do **MediaFragmenter** (demux/decode h26x);
- **Include dirs / Lib dirs** do vcpkg;
- **Preprocessor Definitions** (só os que linkar):
  `USE_EMBEDDED_PIPELINE;HAVE_LIBVPX;HAVE_LIBYUV;HAVE_LIBOPUS;HAVE_FDK_AAC`;
- linkar `vpxmd.lib`, `opus.lib`, `yuv.lib`, `fdk-aac.lib` (+ SvtAv1/openh264 se usar).

O pipeline A/V completo (VP9+Opus/WebM/DASH, sem ffmpeg) exige, no minimo:
`USE_EMBEDDED_PIPELINE` + `HAVE_LIBVPX` + `HAVE_LIBYUV` + `HAVE_LIBOPUS` + `HAVE_FDK_AAC`.
Faltando qualquer um: cai para video-only ou (sem VP9/YUV) volta ao ffmpeg.

## Uso (esqueleto do loop de encode)
```c
#include "media/media_codec.h"

MediaEncoderParams pr = { .Codec=MEDIA_CODEC_VP9, .Width=1280, .Height=720,
                          .BitrateBps=3000000, .Fps=30, .SpeedPreset=8 };
MediaEncoder* enc = media_encoder_open(&pr);
if (!enc) { /* codec nao compilado: media_codec_available(MEDIA_CODEC_VP9)==0 */ }

MediaFrame frame; MediaPacket pkt;
// ... obter frame YUV do decoder+scaler ...
enc->SendFrame(enc, &frame);
while (enc->ReceivePacket(enc, &pkt) == 1)
    mux_write(&pkt);          // escrever no WebM/segmento
// no fim: drain
enc->SendFrame(enc, NULL);
while (enc->ReceivePacket(enc, &pkt) == 1) mux_write(&pkt);
enc->Close(enc);
```

## Integração no servidor (substituir o ffmpeg)
Hoje `hls_controller.c` chama `ffmpeg` via `system()`. A migração é gradual:
1. **Encode** já dá para trocar por `media_encoder_open` (esta camada).
2. Enquanto **demux/decode/mux** não estão prontos, dá para manter o ffmpeg só para
   essas etapas — ou migrar tudo quando os módulos 1–3 acima existirem.
3. Quando o pipeline estiver completo, os handlers `dash_stream_video` / `convert_file`
   deixam de montar linha de comando e passam a rodar demux→decode→scale→encode→mux
   em processo, sem ffmpeg.

## Estado atual
- [x] Wrapper + enum + interfaces (encoder/decoder)
- [x] VP9 encoder (libvpx) + Opus encoder (libopus)
- [x] Mux WebM (EBML) + modo segmentado (init + chunks) para DASH
- [x] Pipeline MP4→VP9/WebM + `.mpd` (SegmentTemplate), integrado no `dash_stream_video` (`USE_EMBEDDED_PIPELINE`)
- [x] Audio: demux+decode AAC (fdk-aac) → Opus → mux A/V + `codecs="vp9,opus"`
- [x] AV1 encode (SVT-AV1) + decode (dav1d, preview); UI "DASH · AV1"
- [x] Afinação: fps real do demux + PTS por frame via `stts` (VFR) + keyframes por tempo
- [x] **SEM ffmpeg/ffprobe em todo o hls_controller**: prepare/DASH/HLS/convert usam so as libs embutidas
   - prepare/DASH/HLS/convert: metadados via `mp4_timing`; DASH via pipeline; HLS via `embed_build_hls` (passthrough fMP4); convert via `embed_transcode_webm` (VP9/AV1)
- [x] **H.264 reencode embutido (OpenH264)** no convert -> **MP4 single-file** (video-only, avcC+AVCC) via `embed_transcode_h264` + `mux_mp4` (`HAVE_OPENH264`)
- [x] **H.264 multi-res no HLS**: decode->scale(libyuv)->OpenH264->**fMP4 fragmentado** (`init-<name>.mp4` + `<name>-N.m4s`) + `master.m3u8` via `embed_build_hls_h264` + `mux_mp4`
- [x] **Audio AAC passthrough** nas saidas H.264 (sem re-encode, nativo Chrome/Safari): demux via `aac_open_raw`/`aac_read_raw` (independe de fdk-aac)
   - convert MP4: faixa `mp4a` no mesmo arquivo (`mp4_open2`/`mp4_write_audio`, video+audio em 2 chunks no `mdat`)
   - HLS: **rendition de audio unica** (`init-audio.mp4` + `audio-N.m4s` + `audio.m3u8`) via `#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="aud"`, compartilhada por todas as resolucoes
- [x] **H.265/HEVC reencode embutido (x265)** no convert -> MP4 (`hvc1`+`hvcC`, + AAC passthrough) via `embed_transcode_h265` + `mux_mp4` (`HAVE_X265`)
   - **GPL/patente**: linkar x265 torna o binario GPL (ou exige licenca comercial); fonte ja HEVC continua em passthrough (copia)
   - `hvcC` monta o profile_tier_level copiando 12 bytes do SPS (aproximacao) — validar contra HEVC real
- [x] **Gateway de sync (Fase 1 VOD)**: `source -> decode/bypass/passthrough -> scale -> encode -> Cut(keyframe) -> sink`,
   alocacao 100% via `memory_pool` (xplatbase) + eventos `GwEvent` (incl. `GW_EV_MEM` p/ monitor de vazamento).
   Ver **[SYNC_GATEWAY.md](SYNC_GATEWAY.md)**. Entrada com **bypass (frame cru)** e **auto-deteccao** (`gw_sniff_stream`).
   - handlers religados: `hls_stream_video` (HLS), `dash_stream_video` (DASH-WebM), `convert_file` h264/h265 (MP4)
   - sinks: `sink_mp4` (single-file), `sink_hls` (fMP4 multi-rendition + grupo AUDIO), `sink_dash` (WebM VP9/AV1 + Opus + MPD)
   - audio: AAC passthrough (MP4/HLS) e **AAC->Opus** (DASH) via decoder AAC packet-fed (`aac_dec_*`)
   - holdover: convert vp9/av1 = `.webm` single-file (`embed_transcode_webm`); Fase 2 = live/camera (`source_camera`, manifestos dinamicos)
- [ ] Validar em runtime (nada foi compilado/testado); ownership do MediaFragmenter; ctts (B-frames) no PTS; alinhamento ABR por `GopSize`; API dos codecs por versao
