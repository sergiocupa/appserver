//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Tipos do gateway: pacote, info de stream, perfil de saida, pista de saida.
//  Ver media/SYNC_GATEWAY.md.

#pragma once
#include "gw_base.h"
#include "media_codec.h"   // MediaCodec
#include "pipeline.h"      // PipeTrack (renditions)

typedef enum { MSTREAM_VIDEO = 0, MSTREAM_AUDIO = 1 } MediaStreamType;

// Formato de pixel dos frames CRUS (bypass de decode). Empacotamento contiguo em GwPacket.Data.
typedef enum { GW_PIX_NONE = 0, GW_PIX_I420, GW_PIX_NV12, GW_PIX_YUYV, GW_PIX_RGB24 } GwPixFmt;

typedef struct
{
    MediaStreamType Type;
    // Raw=1: frames CRUS (sem codec) -> o gateway PULA o decode (bypass). Codec fica NONE.
    // Raw=0: bitstream codificado -> Codec identifica (ou foi auto-detectado).
    int             Raw;
    MediaCodec      Codec;                 // codec de ENTRADA (NONE se Raw)
    GwPixFmt        PixFmt;                 // formato do frame cru (video, quando Raw)
    int    Width, Height; double Fps;      // video
    int    SampleRate, Channels;           // audio (PCM S16 quando Raw)
    const uint8_t* Extra; int ExtraLen;    // SPS/PPS/VPS/ASC (opcional; so Raw=0)
    // Video de arquivo CORTADO sem recodificar: quanto do comeco a lista de edicao esconde
    // (us; 0 = nada). Esses quadros saem da fonte com tempo negativo -- precisam passar pelo
    // decoder, mas nao sao apresentados. Copiar sem recodificar nao consegue esconde-los num
    // formato de segmentos, entao o gateway recodifica quando isto e maior que zero.
    mtime_us PrerollUs;
    int      PrerollFrames;                // quantos quadros estao nesse trecho (0 = nao se sabe)
} MediaStreamInfo;

typedef struct                              // pacote codificado (entrada ou saida)
{
    int       Stream;                       // indice do stream de origem
    uint8_t*  Data; int Size;               // valido ate a proxima chamada do produtor
    mtime_us  Pts, Dts, Dur;                // Dur = duracao do sample (us); 0 se desconhecida
    int       KeyFrame;
} GwPacket;

typedef enum { CONT_DASH_WEBM, CONT_DASH_MP4, CONT_HLS_FMP4, CONT_MP4_FILE,
               CONT_LIVE_MEM   // ao vivo: fMP4 numa janela em memoria (live_store), sem disco
             } Container;
typedef enum { GW_VOD = 0, GW_LIVE = 1 } GwMode;

// O que fazer com arquivo CORTADO sem recodificar (a lista de edicao esconde o comeco) quando o
// pedido e de copia (mesma resolucao e codec) num formato de SEGMENTOS. Num MP4 de arquivo
// unico nada disto se aplica: a copia leva a propria lista de edicao.
typedef enum
{
    // Recodifica o video inteiro. A saida comeca no quadro exato e toca em qualquer player;
    // custa o tempo e a perda de uma recodificacao.
    GW_CUT_RECODE = 0,
    // Recodifica SO o primeiro grupo de quadros (o que tem o trecho escondido) e copia o
    // resto. Comeca no quadro exato e o grosso do video sai intacto. Como os dois pedacos tem
    // parametros de codificacao diferentes, eles vao DENTRO das amostras (entrada 'avc3' no
    // H.264, 'hev1' no H.265), o que nem todo player aceita. Outro codec cai em GW_CUT_RECODE.
    GW_CUT_SMART,
    // Copia tudo. Os quadros escondidos continuam nos segmentos (os seguintes dependem deles),
    // mas COMPRIMIDOS no tempo: 2 ms cada, em vez da duracao real. O trecho de 2,2 s vira um
    // decimo de segundo no comeco, em qualquer player; e o manifesto ainda manda comecar depois
    // dele (EXT-X-START no HLS, ancora #t= na URL do DASH) para quem respeita. Nao gasta encoder.
    GW_CUT_COPY,
    // IGNORA o corte: mostra tudo o que esta gravado, desde o primeiro quadro. A linha do tempo
    // inteira (video e audio) anda o tanto que a origem escondia, entao nada fica antes de zero
    // e o sincronismo se mantem. Vale em toda saida -- copia, recodificacao, escada, arquivo
    // unico --, porque aqui a pergunta nao e "como copiar", e "o que mostrar". A saida fica mais
    // longa que o video exibido pelo arquivo original.
    GW_CUT_SHOW_ALL
}
GwCutMode;

typedef struct
{
    Container   Container;
    MediaCodec  VideoCodec;                 // saida (H264/H265/VP9/AV1); NONE = herda a entrada
    MediaCodec  AudioCodec;                 // saida (OPUS p/ webm; AAC passthrough p/ mp4/hls)
    int         SegmentMs;                  // duracao alvo do segmento
    GwMode      Mode;

    // ---- parametros de saida ----------------------------------------------
    // Convencao: 0 = "nao escolhido, herda da entrada". Quando TODOS sao 0/NONE e nao ha
    // renditions, o gateway trabalha em BYPASS (remux puro, sem decode/encode) -- e o que
    // torna "nada selecionado" barato de verdade em vez de um reencode disfarcado.
    int         Width, Height;              // 0 = resolucao da entrada
    int         Fps;                        // 0 = taxa da entrada; senao reamostra por PTS
    int         BitrateBps;                 // 0 = estimado pela resolucao

    const PipeTrack* Renditions; int RenditionCount;   // multi-resolucao (0 = usa os campos acima)

    GwCutMode   CutFileMode;                // arquivo cortado em pedido de copia (ver GwCutMode)

    // CONT_LIVE_MEM: o LiveStore que recebe os segmentos. Fica aqui porque quem abre o sink
    // e' o gateway (media_sink_open), e o destino ao vivo nao e' um caminho em disco.
    void* Live;
} MediaProfile;

typedef struct                              // uma pista de saida ja configurada (para o sink)
{
    MediaStreamType Type; MediaCodec Codec;
    int Width, Height; double Fps;          // video
    int Bandwidth;                          // bps (video: p/ BANDWIDTH do manifesto)
    int SampleRate, Channels;               // audio
    const uint8_t* Extra; int ExtraLen;     // avcC/hvcC/OpusHead/ASC gerado no encode
    const char* Name;                       // rotulo da rendition (ex.: "720p")
    // Video copiado de arquivo cortado (GW_CUT_COPY): a apresentacao comeca aqui (us); o que
    // vem antes e o trecho que a origem esconde. 0 = comeca no inicio.
    mtime_us StartUs;
    // 1 = os parametros de codificacao (SPS/PPS, e VPS no H.265) mudam no meio da pista e vao
    // dentro das amostras (GW_CUT_SMART): entrada 'avc3' em vez de 'avc1', 'hev1' em vez de 'hvc1'.
    int InBandParams;
} MediaTrackOut;
