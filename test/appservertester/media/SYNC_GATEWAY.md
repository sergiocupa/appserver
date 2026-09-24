# Gateway de Sincronização de Mídias — desenho (para aprovação)

Status: **proposta / não implementado**. Nenhum `.c` novo foi escrito ainda.
Objetivo deste documento: fixar interfaces, contrato de timebase, política de drift,
modelo de threads e layout de arquivos **antes** de codar.

---

## 1. Objetivo e não-objetivos

**Objetivo.** Tornar a conversão genérica: **qualquer codec de entrada → qualquer codec
de saída**, entregando os fragmentos no formato esperado (DASH / HLS / MP4), com **A/V
sincronizado por um relógio comum**. O mesmo núcleo deve aceitar uma **fonte contínua**
(câmera / stream ao vivo), não só um arquivo finito.

**Não-objetivos (agora).**
- Não reimplementar decoders que não temos (a matriz de codecs de entrada é limitada — ver §12).
- Não cobrir legendas/múltiplas trilhas de áudio na fase 1 (1 vídeo + 1 áudio).
- Não fazer B-frames corretos até `ctts` existir nos muxers (ver §12).

---

## 2. Topologia

Hoje o pipeline está **preso a arquivo** (lê `FrameIndexList` inteiro e itera todos os
frames). O gateway quebra isso em 4 estágios plugáveis ligados por um núcleo de sync:

```
[MediaSource] --demux--> GwPacket(v/a)
    --[Decode]--> MediaFrame (YUV/PCM), pts_us em timebase comum
        --[SyncCore]--> ordena por DTS, alinha fragmento a keyframe, corrige drift
            --[Encode]--> GwPacket (codec de saída)
                --[MediaSink]--> fragmentador (DASH/HLS/MP4), VOD ou LIVE
```

Passthrough: se `codec_entrada == codec_saida` para um stream, o estágio Decode+Encode é
**pulado** (só remux) — é o que o `embed_build_hls` passthrough já fazia.

---

## 3. Contrato de timebase

- **Timebase comum = microssegundos** (`int64_t`, `mtime_us`). Todo `GwPacket`/frame carrega
  `Pts`/`Dts` em µs. Escolha: evita a confusão de timescales por trilha; `int64` em µs cobre
  ~292 mil anos.
- Conversão para o timescale do container é feita **no sink** (MP4 vídeo = ms/1000; áudio = rate; WebM = ns).
- `MTIME_NONE = INT64_MIN` marca "sem timestamp".
- A fonte é responsável por entregar `Pts`/`Dts` já em µs (o `source_mp4` converte do `stts`/`mp4_timing`).
- **Stream mestre do relógio**: vídeo em VOD; **áudio em live/câmera** (clock de captura de áudio é mais estável) — ver §7.

```c
// media_time.h
typedef int64_t mtime_us;
#define MTIME_NONE  INT64_MIN
static inline int64_t us_to_ms(mtime_us t){ return t/1000; }
static inline int64_t us_to_scale(mtime_us t,int ts){ return (int64_t)( (double)t*ts/1000000.0 + 0.5 ); }
```

---

## 4. Estruturas de dados

```c
// gw_types.h
typedef enum { MSTREAM_VIDEO, MSTREAM_AUDIO } MediaStreamType;

typedef struct {
    MediaStreamType Type;
    MediaCodec      Codec;                 // codec de ENTRADA
    int    Width, Height; double Fps;      // vídeo
    int    SampleRate, Channels;           // áudio
    const uint8_t* Extra; int ExtraLen;    // SPS/PPS/VPS/ASC (opcional, para decode/passthrough)
} MediaStreamInfo;

typedef struct {                            // pacote codificado (entrada ou saída)
    int       Stream;                       // índice do stream
    uint8_t*  Data; int Size;               // válido até a próxima chamada do produtor
    mtime_us  Pts, Dts;
    int       KeyFrame;
} GwPacket;
```

`MediaFrame` (YUV/PCM) é o já existente em `media_codec.h`; adiciono só a convenção de que
`Pts` do frame passa a ser **µs** dentro do gateway (o encoder recebe µs e devolve µs).

---

## 5. Interfaces

### 5.0 Modos de entrada (bypass / detecção / automático)

A entrada pode ser **crua** (frames sem codec — típico de câmera) ou **codificada**
(bitstream — arquivo ou câmera que já entrega H.264/H.265/MJPEG). Por isso cada stream
carrega `Raw` em `MediaStreamInfo`:

- `Raw = 1` → **bypass de decode**: `GwPacket.Data` é o frame cru empacotado (`GwPixFmt`
  I420/NV12/YUYV/RGB24 no vídeo; PCM S16 no áudio). O estágio Decode é pulado.
- `Raw = 0` → bitstream; `Codec` identifica (ou foi auto-detectado).

A câmera (`CameraParams.Mode`) escolhe `SRC_AUTO` / `SRC_RAW` / `SRC_ENCODED`. Em `AUTO`,
o sniffer `gw_sniff_stream()` decide por 1 buffer: start code Annex-B → H.264/H.265 (por
tipo de NAL), `FF D8` → MJPEG, senão → cru. Assim "mesmo se câmera, detecta o tipo de
stream", com a opção automática embutida.

### 5.1 MediaSource (pull)
```c
// media_source.h
typedef struct MediaSource MediaSource;
struct MediaSource {
    void* Ctx;
    int  (*Info)(MediaSource*, MediaStreamInfo* out, int max, int* count);
    int  (*Read)(MediaSource*, GwPacket* pkt);   // 1=ok, 0=EOF, <0=erro; live: bloqueia
    int  (*IsLive)(MediaSource*);
    void (*Close)(MediaSource*);
};
MediaSource* source_mp4_open(const char* path);           // fase 1 (reembala o demux atual)
MediaSource* source_camera_open(const CameraParams*);     // fase 2
```

### 5.2 MediaSink (push)
```c
// media_sink.h
typedef struct {                             // descreve uma trilha de saída já configurada
    MediaStreamType Type; MediaCodec Codec;
    int Width, Height; double Fps;           // vídeo
    int SampleRate, Channels;                // áudio
    const uint8_t* Extra; int ExtraLen;      // avcC/hvcC/OpusHead/ASC gerado no encode
    const char* Name;                        // rótulo da rendition (ex.: "720p")
} MediaTrackOut;

typedef struct MediaSink MediaSink;
struct MediaSink {
    void* Ctx;
    int  (*Start)(MediaSink*, const MediaTrackOut* tracks, int count);
    int  (*Write)(MediaSink*, int track, const GwPacket* pkt);  // sink COPIA se bufferizar
    int  (*Cut)(MediaSink*, mtime_us at);      // fecha fragmento corrente (fronteira de segmento)
    void (*Close)(MediaSink*);                 // finaliza (ENDLIST / patch mdat / MPD final)
};
```

### 5.3 Perfil de saída
```c
// gw_types.h
typedef enum { CONT_DASH_WEBM, CONT_DASH_MP4, CONT_HLS_FMP4, CONT_MP4_FILE } Container;
typedef enum { GW_VOD, GW_LIVE } GwMode;
typedef struct {
    Container   Container;
    MediaCodec  VideoCodec;                 // saída (H264/H265/VP9/AV1)
    MediaCodec  AudioCodec;                 // saída (OPUS p/ webm; AAC passthrough p/ mp4/hls)
    int         SegmentMs;                  // duração alvo do segmento
    GwMode      Mode;
    const PipeTrack* Renditions; int RenditionCount;   // multi-resolução (0 = fonte)
} MediaProfile;
```

### 5.4 Orquestrador
```c
// sync_gateway.h
typedef struct { volatile int Stop; } GwControl;   // parada cooperativa (live)
int gateway_run(MediaSource* src, const MediaProfile* profile, MediaSink* sink, GwControl* ctl);
```

---

## 6. Núcleo de sincronização (VOD)

`gateway_run`:

1. `src->Info` → escolhe 1 stream de vídeo + 1 de áudio.
2. Abre **decoders** por codec de entrada (`media_decoder_open`); áudio via `audio_aac`/decoder.
   Detecta **passthrough** por stream (in==out) e nesse caso não abre decode/encode.
   Se `MediaStreamInfo.Raw` = 1 (câmera crua), **pula o decode** (bypass): o `GwPacket` já é
   o frame — vai direto ao scale/encode.
3. Abre **encoders** de saída (`media_encoder_open`) quando as dimensões do 1º frame são
   conhecidas. Multi-rendition = N encoders de vídeo (+ scale por libyuv) + 1 de áudio.
   `Extra` (avcC/hvcC/OpusHead) é capturado do 1º pacote e vira `MediaTrackOut.Extra` →
   `sink->Start`.
4. Laço principal:
   - `src->Read(pkt)` → roteia ao decoder do stream.
   - Decode → 0..n frames com `Pts` (µs). Enfileira em **filas ordenadas por stream**.
   - **Condição de emissão** (garantia de intercalação): só emite frame cujo `Pts ≤ menor Pts
     de cabeça entre todos os streams ativos` (ou o stream chegou a EOF). Isso garante ordem
     temporal entre trilhas sem esperar o arquivo inteiro.
   - Vídeo emitido: scale (se rendition≠fonte) → encode → pacote → **fronteira de fragmento**:
     se `KeyFrame && (pts - seg_start ≥ SegmentMs*1000)` → `sink->Cut(pts)`; então
     `sink->Write(track_video, pkt)`.
   - Áudio emitido: encode (ou passthrough) → `sink->Write(track_audio, pkt)`. O corte de
     segmento de áudio é **guiado pelo tempo do vídeo** (o `Cut(at)` faz o sink de áudio
     drenar até `at`), mantendo segmentos alinhados entre trilhas e renditions.
5. EOF: drena decoders (`SendPacket(NULL)`), drena encoders (`SendFrame(NULL)`), `Cut` final,
   `sink->Close`.

### VFR
Nativo: os `Pts` por frame vêm exatos do demux (`mp4_video_pts_ms`). As durações (`stts`/
`trun`) são calculadas no sink a partir dos deltas de `Pts`.

---

## 7. Política de drift A/V

Mantemos um **relógio mestre** (§3). Para o áudio, o esperado é `pts_audio = amostras_emitidas / rate`.
Comparação a cada frame de áudio contra o mestre:

- `|erro| ≤ LIMIAR` (ex.: 40 ms): nada a fazer.
- áudio **atrasado** (falta som): **descarta** amostras/quadros até alcançar (ou acelera consumo).
- áudio **adiantado** (sobra buraco): **insere silêncio** (quadros PCM zerados) para preencher.
- Correções são graduais (um passo por fronteira) para não “pular” audível.

Em **câmera**, vídeo e áudio chegam em relógios independentes; o **áudio é o mestre** e o
vídeo é reamostrado no tempo (duplica/descarta frame) para casar. Em **arquivo**, o vídeo é o
mestre (PTS confiáveis) e o áudio é ajustado.

---

## 8. Passthrough (remux sem transcode)

Por stream, se `codec_entrada == codec_saida` **e** o container aceita, o gateway copia os
pacotes (ajustando só timestamps/fragmentação). Casos:
- Áudio **AAC → MP4/HLS**: passthrough (já implementado como base em `aac_open_raw`).
- Vídeo **H.264 → HLS/MP4** quando não há rescale: remux (o antigo `embed_build_hls`).
- Vídeo **HEVC → MP4**: cópia (o passthrough atual do convert).

---

## 9. Sinks

Cada sink reusa os muxers de baixo nível já existentes.

| Sink | Container | Muxer base | Manifesto |
|---|---|---|---|
| `sink_mp4`  | MP4 single-file | `mux_mp4` (`mp4_open2`/`mp4_open_hevc`) | — |
| `sink_dash` | WebM ou MP4 segmentado | `mux_webm` seg. / `mux_mp4` fMP4 | `.mpd` SegmentTemplate |
| `sink_hls`  | fMP4 | `mux_mp4` (`init`/`segment`) | `master.m3u8` + mídia + grupo de áudio |

- `Start` cria as trilhas/inits; `Write` acumula amostras do segmento corrente (copiando os
  bytes, pois `GwPacket.Data` é volátil); `Cut(at)` fecha o segmento (grava `.m4s`/chunk,
  atualiza playlist/MPD); `Close` finaliza (`ENDLIST` / `mdat` patch / MPD estático).
- Multi-rendition: `sink_hls`/`sink_dash` recebem N trilhas de vídeo + 1 de áudio; o áudio é
  **uma rendition compartilhada** (grupo `AUDIO` no HLS; AdaptationSet de áudio no DASH).

---

## 10. Live (fase 2 — desenhado, não codado)

- `source_camera`: thread produtora enche um **anel limitado** de `GwPacket`; `Read` desenfileira
  (bloqueia se vazio; descarta o mais antigo se cheio → teto de latência).
- Encoders com **GOP = SegmentMs** (keyframe periódico) para cortes alinhados.
- Sinks em `GW_LIVE` mantêm só os últimos **K** segmentos em disco e reescrevem o manifesto a
  cada `Cut`:
  - **DASH**: `MPD@type=dynamic`, `availabilityStartTime`, `timeShiftBufferDepth`,
    `minimumUpdatePeriod`, `startNumber` avançando.
  - **HLS**: playlist de mídia **sem** `ENDLIST`, `#EXT-X-MEDIA-SEQUENCE` crescente, janela
    deslizante de segmentos; `#EXT-X-MAP` fixo.
- Parada cooperativa via `GwControl.Stop`.

---

## 11. Modelo de threads

- **VOD**: 1 thread (laço pull). Simples e determinístico.
- **LIVE (câmera)**: 1 thread de captura (produtora) + 1 thread `gateway_run` (consumidora),
  ligadas pelo anel limitado. Sinks são escritos só na thread consumidora.
- **Multi-rendition**: encode é pesado; fase 3 pode paralelizar 1 worker por rendition. Fase 1
  é sequencial (correto e simples primeiro).

---

## 12. Matriz de codecs e limitações (honestas)

**Entrada (decode disponível hoje):**
- Vídeo: H.264 / H.265 (via MediaFragmenter), AV1 (dav1d). **VP9 decode ausente.**
- Áudio: **AAC** (fdk-aac) e passthrough. Outros (MP3/Opus-in) não cobertos.

**Saída (encode disponível hoje):** H.264 (OpenH264), H.265 (x265), VP9 (libvpx), AV1
(SVT-AV1); áudio Opus (libopus) e AAC passthrough.

⇒ “qualquer→qualquer” é real **dentro dessa matriz**; fora dela o gateway retorna erro claro
ou cai para passthrough. Adicionar um codec = 1 módulo novo, sem tocar no núcleo.

**Pendências de correção que o gateway expõe:**
- **B-frames (DTS≠PTS)**: os muxers MP4 ainda não escrevem `ctts`. Enquanto isso, encoders são
  configurados sem B-frames, ou o gateway rejeita entradas com reordenação. Corrigir `ctts` é
  pré-requisito para “qualquer H.264/H.265 com B-frames”.
- Live/manifestos dinâmicos: detalhes de `UTCTiming`/latência ficam para a fase 2.

---

## 13. Migração dos handlers atuais

Adaptadores finos por cima do gateway (comportamento idêntico ao de hoje):

| Handler | Vira |
|---|---|
| `convert_file` (vp9/av1/h264/h265) | `source_mp4` + perfil `CONT_MP4_FILE`/`CONT_DASH_WEBM` + `sink_*` |
| `dash_stream_video` | `source_mp4` + `CONT_DASH_*` + `sink_dash` |
| `hls_stream_video`  | `source_mp4` + `CONT_HLS_FMP4` (multi-rendition) + `sink_hls` |

`pipeline.c`/`embed_ops.c` viram cascas sobre `gateway_run`; os muxers permanecem como
escritores de baixo nível dos sinks. Remoção do código antigo só depois de paridade validada.

---

## 14. Propriedade / tempos de vida

- `GwPacket.Data` pertence ao produtor (source ou encoder), válido até a próxima chamada — o
  **sink copia** se acumula entre chamadas (a acumulação de segmento fMP4 já copia hoje).
- Decoders/encoders pertencem ao gateway; fechados no fim.
- O sink é dono dos arquivos de saída e do manifesto.

---

## 15. Plano em fases

- **Fase 0**: este documento (aprovação). ✔
- **Fase 1 (VOD)** — em código (não compilado):
  - ✔ `media_time.h`, `gw_base.h` (alloc xplatbase + eventos), `gw_types.h`
  - ✔ `media_source`(+`source_mp4`, bypass/auto-detect, `gw_sniff_stream`)
  - ✔ `media_sink` + `sink_mp4` + `sink_hls`; `sink_dash` = **stub** (próximo)
  - ✔ `h26x_util` (avcC/hvcC + Annex-B→length)
  - ✔ `sync_gateway` (decode/bypass/passthrough → scale → encode → Cut → sink) + **transcode
    de áudio AAC→Opus** (decoder AAC packet-fed em `audio_aac` + encoder Opus)
  - ✔ `sink_dash` (WebM VP9/AV1 + Opus, auto-segmentado, + `manifest.mpd`)
  - ✔ **religados os 3 handlers** via `gateway_run` + adaptador `GwEvent`→SSE (`sse_feedback`):
    `hls_stream_video` (HLS multi-rendition), `dash_stream_video` (DASH-WebM), `convert_file`
    h264/h265 (MP4).
  - **Fase 1 (VOD) fechada.** Holdover menor: `convert_file` vp9/av1 = arquivo `.webm` único
    (`embed_transcode_webm`), pois é saída single-file (não-DASH) — trivial de mover para um
    `sink_webm` single-file depois se quiser.
  - Sem live, sem B-frames. Drift A/V ativo fica p/ a Fase 2 (live); em VOD o áudio (passthrough
    ou AAC→Opus) alinha pelos Cut do vídeo.
- **Fase 2 (LIVE)**: manifestos dinâmicos + buffers limitados + pacing + `source_camera`.
- **Fase 3**: threads por rendition, `ctts`/B-frames, mais decoders de entrada (VP9-in etc.).

---

## 16. Layout de arquivos novos

```
media/
  media_time.h            # mtime_us + helpers de timebase
  gw_types.h              # GwPacket, MediaStreamInfo, MediaProfile, MediaTrackOut
  media_source.h / .c     # iface MediaSource + source_mp4
  media_source_camera.c   # source_camera (fase 2)
  media_sink.h / .c       # iface MediaSink + dispatch por Container
  sink_mp4.c              # MP4 single-file
  sink_dash.c             # DASH (webm/mp4 segmentado + .mpd)
  sink_hls.c              # HLS (fmp4 + m3u8 + grupo de áudio)
  sync_gateway.h / .c     # gateway_run + core de sync/drift
```

Muxers atuais (`mux_webm.c`, `mux_mp4.c`) e codecs (`codec_*.c`) permanecem sem mudança de
API; passam a ser chamados pelos sinks/estágios em vez de por `pipeline.c`/`embed_ops.c`.

---

## 17. Pontos a decidir (peço confirmação)

1. **Timebase µs** ok? (alternativa: `AVRational`/timescale por trilha — mais próximo do
   ffmpeg, porém mais verboso).
2. **Áudio como rendition única** (compartilhada) em HLS/DASH — confirma? (alternativa: áudio
   embutido em cada segmento de vídeo — mais simples, porém duplica o áudio N vezes).
3. Ordem das fases: **VOD com paridade primeiro** (recomendado) e câmera na fase 2 — ok?
4. Sem B-frames na fase 1 (encoders configurados `bframes=0`) até implementar `ctts` — aceitável?
```

Nada compilado/testado. Este é só o desenho.
