//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  O DESCRITOR que cada projeto de codec expoe.
//
//  Antes, o nucleo carregava tres tabelas com o nome das funcoes de cada codec
//  (`h264_encoder_open`, `av1_decoder_open`, `h265_dec_backend`, ...). Toda vez que um
//  codec entrava ou saia, o nucleo mudava -- que e exatamente o acoplamento que a separacao
//  em projetos veio desfazer.
//
//  Agora cada projeto exporta UM simbolo: um CodecPluginSet com os backends que ele traz.
//  O nucleo nao conhece funcao de codec nenhuma; ele percorre os conjuntos.
//
//  E o mesmo descritor que vira o export da DLL/.so quando o codec for carregado em tempo
//  de execucao: na Fase 4 muda o jeito de ACHAR o simbolo (dlsym/GetProcAddress em vez de
//  extern), nao o que ele contem.

#ifndef CODEC_PLUGIN_H
#define CODEC_PLUGIN_H

#include "media_codec.h"
#include "dec_select.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CODEC_PLUGIN_ABI 1

// Onde o backend roda. As mesmas mascaras do enc_select.h, repetidas aqui para o descritor
// nao depender dele (quem implementa codec nao precisa saber da cascata de selecao).
#define CODEC_OS_WINDOWS   0x1
#define CODEC_OS_LINUX     0x2
#define CODEC_ARCH_X64     0x1
#define CODEC_ARCH_ARM64   0x2
#define CODEC_OS_ALL       (CODEC_OS_WINDOWS | CODEC_OS_LINUX)
#define CODEC_ARCH_ALL     (CODEC_ARCH_X64 | CODEC_ARCH_ARM64)

// O que o backend faz. Um mesmo codec costuma ter backends diferentes para cada lado
// (AV1 codifica com SVT-AV1 e decodifica com dav1d).
#define CODEC_ROLE_ENCODE  0x1
#define CODEC_ROLE_DECODE  0x2

// Ranking das ISAs x86 (maior = mais larga), para comparar "o que a CPU tem" com "ate onde
// a lib foi compilada". Fica aqui porque quem declara a faixa e o proprio codec.
enum { ISA_NONE = 0, ISA_SSE2, ISA_SSSE3, ISA_SSE41, ISA_AVX, ISA_AVX2, ISA_AVX512 };

typedef struct
{
    int         Abi;         // CODEC_PLUGIN_ABI -- conferido ao registrar
    MediaCodec  Codec;
    int         Role;        // CODEC_ROLE_*
    const char* Name;        // identificador estavel: "openh264", "x265", "dav1d", "mf-hw"
    const char* Note;        // texto para a UI e o log

    unsigned    Os, Arch;    // onde este backend roda
    int         Compiled;    // 1 = a lib entrou NESTE build
    int         Hardware;    // 1 = degrau de hardware (GPU), nao software

    // Faixa de SIMD que a lib usa. ISA_NONE/ISA_NONE = nao verificavel daqui.
    int         IsaMin, IsaMax;
    int         Neon;        // usa NEON no ARM64

    // ---- encode ----
    // detail recebe o que foi achado ("NVIDIA H.264 Encoder MFT"); backends de software
    // podem ignorar. NULL quando o backend nao codifica.
    MediaEncoder* (*EncoderOpen)(const MediaEncoderParams* p, char* detail, int size);

    // ---- decode ----
    MediaDecoder*         (*DecoderOpen)(void);   // vtable (pacote -> MediaFrame), ou 0
    const H26xDecBackend*  H26x;                  // decode em lote de H26x, ou 0
    const char*            DecDetail;             // texto fixo do lado do decode, ou 0

    // Sonda em runtime: > 0 = disponivel aqui e agora, e preenche detail. 0 = sempre
    // disponivel quando compilado (caso do software).
    int (*Probe)(MediaCodec codec, char* detail, int size);
}
CodecPlugin;

// O simbolo que CADA projeto de codec exporta -- um so.
typedef struct
{
    int                Abi;
    const char*        Module;   // "codec_h264", "codec_hw", ...
    int                Count;
    const CodecPlugin* Items;
}
CodecPluginSet;

// ---- carga dinamica --------------------------------------------------------
//  Um codec pode vir de duas formas, e o nucleo nao distingue as duas:
//    ESTATICA  - o conjunto esta linkado no binario (extern, ver codec_registry.c);
//    PLUGIN    - o conjunto vem de uma DLL/.so carregada em tempo de execucao.
//
//  O plugin exporta UMA funcao, com este nome, que devolve o mesmo CodecPluginSet de
//  sempre. Nada mais atravessa a fronteira do modulo.
//
//  O plugin NAO linka o nucleo: ele carrega sua propria copia dos utilitarios de imagem
//  (image_plane.c). Isso so e seguro porque o xplatbase e UNICO no processo -- o que um
//  modulo aloca, o outro libera. E exatamente o que a prep-xplatbase foi feita para
//  garantir, e o que os testes de modulo de la provam.
#define CODEC_PLUGIN_ENTRY_NAME "codec_plugin_entry"

typedef const CodecPluginSet* (*CodecPluginEntry)(void);

#ifdef _WIN32
  #define CODEC_PLUGIN_EXPORT __declspec(dllexport)
#else
  #define CODEC_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

// Usado no fim do arquivo de descritor de cada codec, quando ele e compilado como plugin.
#define CODEC_PLUGIN_DECLARE(conjunto)                                  \
    CODEC_PLUGIN_EXPORT const CodecPluginSet* codec_plugin_entry(void)  \
    { return &(conjunto); }

// ---- o que o nucleo usa ----------------------------------------------------
// Percorre todos os backends registrados, na ordem de tentativa (hardware antes de
// software). Devolve 0 quando acaba.
const CodecPlugin* codec_plugin_at(int index);
int                codec_plugin_count(void);

// Garante que o modulo que atende esse codec ja entrou (carga sob demanda). Quem abre um
// encoder/decoder chama isto antes de percorrer a lista; com tudo embutido, nao faz nada.
// MEDIA_CODEC_NONE = traz tudo o que houver (so as listas de diagnostico precisam).
void codec_plugin_ensure(MediaCodec codec);

// De qual conjunto este backend veio (0 = embutido no binario).
const CodecPluginSet* codec_plugin_set_of(const CodecPlugin* p);

// Contagem de uso: enquanto houver encoder/decoder aberto de um modulo carregado, ele nao
// pode ser descarregado. As duas primeiras trocam o Close por um desvio que devolve a
// contagem sozinho; as duas ultimas sao para quem gerencia o ciclo de vida na mao (o
// DecoderInstance do h26x). Com conjunto embutido, nao fazem nada.
void codec_use_track_encoder(const CodecPluginSet* cs, MediaEncoder* e);
void codec_use_track_decoder(const CodecPluginSet* cs, MediaDecoder* d);
void codec_use_begin(const CodecPluginSet* cs);
void codec_use_end(const CodecPluginSet* cs);

// Primeiro backend que atende esse codec nesse papel, ou 0.
const CodecPlugin* codec_plugin_find(MediaCodec codec, int role);

#ifdef __cplusplus
}
#endif
#endif // CODEC_PLUGIN_H
