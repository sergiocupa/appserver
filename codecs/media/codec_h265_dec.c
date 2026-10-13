//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Decode de H.265 pelo libde265. Este arquivo e o UNICO lugar da camada de codecs que
//  inclui de265.h -- e o que permite ele virar o projeto codec_h265.
//
//  Saiu do codec_video.c, que tinha isto junto com o decode de H.264.

#include "dec_select.h"
#include "codec_parallel.h"
#include <stdio.h>
#include <string.h>
#include "de265.h"

// ============================================================================================
//  POR QUE A REORDENACAO E FEITA AQUI
//
//  O libde265 devolve os quadros na ordem de apresentacao, mas perde UM por grupo quando o
//  fluxo tem quadros B: o ultimo apresentado de cada grupo nao sai, nem no esvaziamento final
//  (medido num 1080p de 24 s: 602 pacotes, 593 quadros, 9 quadros-chave no arquivo). Quem
//  consome da a cada quadro o proximo tempo da fila, entao a perda aparecia como falta no fim
//  do video e deslocava o conteudo a cada grupo.
//
//  Avisando o fim de quadro (de265_push_end_of_frame) depois de CADA pacote, o decoder devolve
//  o quadro daquele pacote na hora e nao perde nenhum -- mas para de reordenar: saem na ordem
//  de decodificacao. Entao a ordem de apresentacao e reconstruida aqui:
//    - cada pacote vai com uma etiqueta, que volta junto com o quadro (de265_get_image_PTS);
//    - a ordem de apresentacao do quadro (POC) e lida do cabecalho da primeira fatia do pacote;
//    - os quadros esperam numa fila ordenada e saem quando ha mais deles do que o fluxo
//      declara que pode reordenar (sps_max_num_reorder_pics) -- a regra da norma (C.5.2.2).
//  Num quadro-chave que fecha o grupo (IDR/BLA) a fila e esvaziada antes: tudo o que esta nela
//  e anterior a ele.
// ============================================================================================

#define H265_FILA 32

typedef struct { ImagePlane* img; int poc; } H265Sai;
typedef struct { int64_t tag; int poc; } H265Pend;

typedef struct
{
    de265_decoder_context* ctx;

    // do SPS e do PPS: o que e preciso para chegar ao POC no cabecalho da fatia
    uint8_t sps_ok[16], sps_log2_poc[16], sps_sep_plano[16], sps_reord[16];
    uint8_t pps_ok[64], pps_sps[64], pps_saida_flag[64], pps_bits_extra[64];

    int      prev_lsb, prev_msb, tem_prev;   // referencia para o POC (ultimo quadro de camada 0)
    int      ult_poc;                        // ultimo POC atribuido (para quando nao der para ler)
    int      max_reord;                      // quantos quadros o fluxo pode segurar para reordenar
    int64_t  seq;                            // etiqueta do proximo pacote

    H265Pend pend[H265_FILA]; int npend;     // pacotes entregues cujo quadro ainda nao voltou
    H265Sai  fila[H265_FILA]; int nfila;     // quadros decodificados, em ordem de POC
}
H265Dec;

static void* h265_dec_create(void)
{
    H265Dec* d = (H265Dec*)memop_alloc_raw(sizeof(H265Dec));
    if (!d) return NULL;
    memset(d, 0, sizeof(*d));
    d->max_reord = 4;   // ate o primeiro SPS dizer o valor do fluxo

    d->ctx = de265_new_decoder();
    if (!d->ctx) { memop_free_raw(d); return NULL; }

    // Habilitar verificacao de erros e tolerancia
    de265_set_parameter_bool(d->ctx, DE265_DECODER_PARAM_SUPPRESS_FAULTY_PICTURES, 0);
    de265_set_parameter_bool(d->ctx, DE265_DECODER_PARAM_DISABLE_DEBLOCKING, 0);
    de265_set_parameter_bool(d->ctx, DE265_DECODER_PARAM_DISABLE_SAO, 0);

    // Threads de trabalho do libde265. Era 4 fixo, "ajustar conforme necessario" --
    // numero sem origem, que nao acompanhava nem a maquina nem a resolucao.
    //
    // Aqui vale so a regra da maquina do H.265 (1 x nucleos fisicos): o teto de geometria
    // depende da altura do quadro, que so se conhece depois do primeiro NAL. E a regra foi
    // medida no ENCODE (x265); o decode do libde265 nao foi medido, entao fica no mesmo
    // numero em vez de ganhar um palpite proprio.
    de265_start_worker_threads(d->ctx, codec_threads(0, codec_nucleos_fisicos(), 0));

    return d;
}

static ImagePlane* image_from_h265(const struct de265_image* img)
{
    uint8_t* src[3]; int st[3];
    for (int k = 0; k < 3; k++) src[k] = (uint8_t*)de265_get_image_plane(img, k, &st[k]);
    return imagep_copy_new(src, st, de265_get_image_width(img, 0), de265_get_image_height(img, 0));
}

// ---- leitura de bits (cabecalhos) --------------------------------------------------------

typedef struct { const uint8_t* d; int n, pos; int erro; } H265Bits;

static int hb_u(H265Bits* b, int bits)
{
    int v = 0;
    for (int i = 0; i < bits; i++)
    {
        if (b->pos >= b->n * 8) { b->erro = 1; return 0; }
        v = (v << 1) | ((b->d[b->pos >> 3] >> (7 - (b->pos & 7))) & 1);
        b->pos++;
    }
    return v;
}

static int hb_ue(H265Bits* b)   // Exp-Golomb sem sinal
{
    int zeros = 0;
    while (!b->erro && hb_u(b, 1) == 0) { if (++zeros > 24) { b->erro = 1; return 0; } }
    return zeros ? ((1 << zeros) - 1 + hb_u(b, zeros)) : 0;
}

// Corpo do NAL (depois do cabecalho de 2 bytes) sem os bytes de prevencao de emulacao.
static int h265_rbsp(const uint8_t* nal, int len, uint8_t* out, int cap)
{
    int n = 0, zeros = 0;
    for (int i = 2; i < len && n < cap; i++)
    {
        if (zeros >= 2 && nal[i] == 3) { zeros = 0; continue; }
        zeros = nal[i] == 0 ? zeros + 1 : 0;
        out[n++] = nal[i];
    }
    return n;
}

static void h265_le_sps(H265Dec* d, const uint8_t* nal, int len)
{
    uint8_t rb[256]; H265Bits b = { rb, h265_rbsp(nal, len, rb, (int)sizeof(rb)), 0, 0 };
    hb_u(&b, 4);                                   // sps_video_parameter_set_id
    int sub = hb_u(&b, 3);                         // sps_max_sub_layers_minus1
    hb_u(&b, 1);                                   // sps_temporal_id_nesting_flag
    // profile_tier_level: 96 bits gerais, depois o que cada subcamada declara
    for (int i = 0; i < 12; i++) hb_u(&b, 8);
    int perfil[8], nivel[8];
    for (int i = 0; i < sub; i++) { perfil[i] = hb_u(&b, 1); nivel[i] = hb_u(&b, 1); }
    if (sub > 0) for (int i = sub; i < 8; i++) hb_u(&b, 2);
    for (int i = 0; i < sub; i++)
    {
        if (perfil[i]) for (int k = 0; k < 11; k++) hb_u(&b, 8);
        if (nivel[i])  hb_u(&b, 8);
    }
    int id = hb_ue(&b);                            // sps_seq_parameter_set_id
    int croma = hb_ue(&b);                         // chroma_format_idc
    int sep = (croma == 3) ? hb_u(&b, 1) : 0;      // separate_colour_plane_flag
    hb_ue(&b); hb_ue(&b);                          // largura, altura
    if (hb_u(&b, 1)) { hb_ue(&b); hb_ue(&b); hb_ue(&b); hb_ue(&b); }   // janela de conformidade
    hb_ue(&b); hb_ue(&b);                          // profundidade de bits (luma, croma)
    int log2_poc = hb_ue(&b) + 4;                  // log2_max_pic_order_cnt_lsb_minus4
    int por_camada = hb_u(&b, 1);                  // sps_sub_layer_ordering_info_present_flag
    int reord = 0;
    for (int i = (por_camada ? 0 : sub); i <= sub; i++) { hb_ue(&b); reord = hb_ue(&b); hb_ue(&b); }   // vale o da camada mais alta
    if (b.erro || id < 0 || id > 15 || log2_poc > 16) return;
    d->sps_ok[id] = 1; d->sps_log2_poc[id] = (uint8_t)log2_poc; d->sps_sep_plano[id] = (uint8_t)sep;
    d->sps_reord[id] = (uint8_t)(reord > H265_FILA - 2 ? H265_FILA - 2 : reord);
}

static void h265_le_pps(H265Dec* d, const uint8_t* nal, int len)
{
    uint8_t rb[64]; H265Bits b = { rb, h265_rbsp(nal, len, rb, (int)sizeof(rb)), 0, 0 };
    int id = hb_ue(&b), sps = hb_ue(&b);
    hb_u(&b, 1);                                   // dependent_slice_segments_enabled_flag
    int saida = hb_u(&b, 1);                       // output_flag_present_flag
    int extra = hb_u(&b, 3);                       // num_extra_slice_header_bits
    if (b.erro || id < 0 || id > 63 || sps < 0 || sps > 15) return;
    d->pps_ok[id] = 1; d->pps_sps[id] = (uint8_t)sps; d->pps_saida_flag[id] = (uint8_t)saida; d->pps_bits_extra[id] = (uint8_t)extra;
}

// POC do quadro, lido da PRIMEIRA fatia. Devolve 1 e preenche *poc; 0 se nao deu para ler
// (parametros que nao chegaram, fatia que nao e a primeira).
static int h265_le_poc(H265Dec* d, const uint8_t* nal, int len, int tipo, int camada, int* poc)
{
    uint8_t rb[64]; H265Bits b = { rb, h265_rbsp(nal, len, rb, (int)sizeof(rb)), 0, 0 };
    if (!hb_u(&b, 1)) return 0;                    // first_slice_segment_in_pic_flag
    if (tipo >= 16 && tipo <= 23) hb_u(&b, 1);     // no_output_of_prior_pics_flag
    int pps = hb_ue(&b);
    if (b.erro || pps < 0 || pps > 63 || !d->pps_ok[pps] || !d->sps_ok[d->pps_sps[pps]]) return 0;
    int sps = d->pps_sps[pps];
    d->max_reord = d->sps_reord[sps];

    int lsb = 0, msb = 0;
    if (tipo != 19 && tipo != 20)                  // IDR: POC zero, sem campo
    {
        hb_u(&b, d->pps_bits_extra[pps]);
        hb_ue(&b);                                 // slice_type
        if (d->pps_saida_flag[pps]) hb_u(&b, 1);   // pic_output_flag
        if (d->sps_sep_plano[sps])  hb_u(&b, 2);   // colour_plane_id
        lsb = hb_u(&b, d->sps_log2_poc[sps]);
        if (b.erro) return 0;

        int max = 1 << d->sps_log2_poc[sps];
        if (tipo >= 16 && tipo <= 18) msb = 0;     // BLA: recomeca
        else if (!d->tem_prev)        msb = 0;
        else if (lsb < d->prev_lsb && d->prev_lsb - lsb >= max / 2) msb = d->prev_msb + max;
        else if (lsb > d->prev_lsb && lsb - d->prev_lsb >  max / 2) msb = d->prev_msb - max;
        else                                                        msb = d->prev_msb;
    }
    *poc = msb + lsb;

    // serve de referencia para os proximos: camada 0, e nem RASL/RADL nem quadro que ninguem usa
    int descartavel = (tipo < 16 && (tipo % 2) == 0) || (tipo >= 6 && tipo <= 9);
    if (camada == 0 && !descartavel) { d->prev_lsb = lsb; d->prev_msb = msb; d->tem_prev = 1; }
    return 1;
}

// ---- fila de saida --------------------------------------------------------------------------

static void h265_solta(H265Dec* d, int manter, ImagePlaneList* images)   // deixa 'manter' na fila
{
    int n = d->nfila - manter;
    if (n <= 0) return;
    for (int i = 0; i < n; i++) imagep_list_add(images, d->fila[i].img);
    for (int i = n; i < d->nfila; i++) d->fila[i - n] = d->fila[i];
    d->nfila -= n;
}

static void h265_guarda(H265Dec* d, ImagePlane* img, int poc, ImagePlaneList* images)
{
    if (d->nfila == H265_FILA) h265_solta(d, H265_FILA - 1, images);   // fila cheia: sai o mais antigo
    int i = d->nfila;
    while (i > 0 && d->fila[i - 1].poc > poc) { d->fila[i] = d->fila[i - 1]; i--; }
    d->fila[i].img = img; d->fila[i].poc = poc; d->nfila++;
}

// Roda o decoder ate ele pedir mais dados (ou, no fim do fluxo, ate nao sobrar nada) e poe na
// fila, pela ordem de apresentacao, cada quadro que ficar pronto. < 0 em erro.
static int h265_dec_roda(H265Dec* d, ImagePlaneList* images)
{
    for (int guard = 0; guard < 4096; guard++)
    {
        int more = 0;
        de265_error err = de265_decode(d->ctx, &more);

        if (err != DE265_OK && err != DE265_ERROR_WAITING_FOR_INPUT_DATA && !de265_isOK(err))
            fprintf(stderr, "Erro de decodificacao: %s\n", de265_get_error_text(err));   // segue: recolhe o que houver

        const struct de265_image* img;
        while ((img = de265_get_next_picture(d->ctx)) != 0)
        {
            enum de265_chroma chroma = de265_get_chroma_format(img);
            if (chroma != de265_chroma_420)
            {
                fprintf(stderr, "AVISO: Formato chroma nao suportado (%d)\n", chroma);
                de265_release_next_picture(d->ctx);
                return -2;
            }
            // o POC do pacote que gerou este quadro; os pacotes anteriores que nao renderam
            // quadro (o decoder pulou) saem da lista
            int64_t tag = (int64_t)de265_get_image_PTS(img);
            int poc = d->ult_poc + 1, k = -1;
            for (int i = 0; i < d->npend; i++) if (d->pend[i].tag == tag) { k = i; poc = d->pend[i].poc; break; }
            if (k >= 0) { for (int i = k + 1; i < d->npend; i++) d->pend[i - k - 1] = d->pend[i]; d->npend -= k + 1; }

            ImagePlane* image = image_from_h265(img);   // copia ANTES de soltar a imagem
            de265_release_next_picture(d->ctx);
            if (image) h265_guarda(d, image, poc, images);
        }
        if (!more || err == DE265_ERROR_WAITING_FOR_INPUT_DATA) break;
    }
    return 0;
}

static int h265_dec_decode(void* inst, uint_fast8_t* data, int len, ImagePlaneList* images)
{
    H265Dec* d = (H265Dec*)inst;
    if (!d || !data || len <= 0) return -1;

    // Parametros e a ordem de apresentacao deste pacote, lidos antes de entrega-lo.
    int tem_poc = 0, poc = 0, fecha_grupo = 0;
    for (int i = 0; i + 3 < len; )
    {
        if (!(data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)) { i++; continue; }
        int ini = i + 3, j = ini;
        while (j + 2 < len && !(data[j] == 0 && data[j + 1] == 0 && (data[j + 2] == 1 || (j + 3 < len && data[j + 2] == 0 && data[j + 3] == 1)))) j++;
        int fim = (j + 2 < len) ? j : len;
        if (fim - ini >= 3)
        {
            int tipo = (data[ini] >> 1) & 0x3F, camada = (data[ini + 1] & 7) - 1;
            if      (tipo == 33) h265_le_sps(d, data + ini, fim - ini);
            else if (tipo == 34) h265_le_pps(d, data + ini, fim - ini);
            else if (tipo <= 21 && !tem_poc)
            {
                tem_poc = h265_le_poc(d, data + ini, fim - ini, tipo, camada, &poc);
                if (tipo >= 16 && tipo <= 20) fecha_grupo = 1;   // IDR ou BLA
            }
        }
        i = fim;
    }
    if (!tem_poc) poc = d->ult_poc + 1;   // sem como ler: segue a ordem em que chegou
    d->ult_poc = poc;

    // Quadro-chave que fecha o grupo: o POC recomeca, e tudo o que esta na fila vem antes dele.
    if (fecha_grupo) h265_solta(d, 0, images);

    if (d->npend == H265_FILA) { for (int i = 1; i < d->npend; i++) d->pend[i - 1] = d->pend[i]; d->npend--; }
    d->pend[d->npend].tag = d->seq; d->pend[d->npend].poc = poc; d->npend++;

    de265_error err = de265_push_data(d->ctx, data, len, (de265_PTS)d->seq, NULL);
    d->seq++;
    if (err != DE265_OK)
    {
        fprintf(stderr, "Erro ao enviar dados: %s\n", de265_get_error_text(err));
        return -1;
    }
    de265_push_end_of_frame(d->ctx);   // o quadro deste pacote sai agora (ver o comentario do topo)

    int r = h265_dec_roda(d, images);
    if (r < 0) return r;

    h265_solta(d, d->max_reord, images);   // sai o que ja nao pode ser ultrapassado
    return 0;
}

static int h265_dec_flush(void* inst, ImagePlaneList* images)
{
    H265Dec* d = (H265Dec*)inst;
    if (!d) return -1;

    int antes = images->Count;
    de265_flush_data(d->ctx);
    h265_dec_roda(d, images);
    h265_solta(d, 0, images);
    return images->Count - antes;
}

static void h265_dec_release(void* inst)
{
    H265Dec* d = (H265Dec*)inst;
    if (!d) return;
    if (d->ctx) de265_free_decoder(d->ctx);
    for (int i = 0; i < d->nfila; i++) memop_free_raw(d->fila[i].img);
    memop_free_raw(d);
}

const H26xDecBackend h265_dec_backend =
{
    265, "libde265",
    h265_dec_create, h265_dec_decode, h265_dec_flush, h265_dec_release
};
