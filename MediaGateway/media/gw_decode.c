//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Implementacao do decode de video unificado. Ver gw_decode.h.
//
//  Ordem: GPU (hw_dec.h), quando a GPU expoe o perfil do codec; senao software -- h26x para
//  H264/H265, vtable de media_codec para VP9/AV1.
//
//  Queda da GPU para o software NO MEIO do fluxo so e segura antes do primeiro frame. Ate la
//  os pacotes entregues a GPU ficam guardados (ate GW_HW_STASH) e sao reenviados ao
//  software: um fluxo que o decoder da GPU recusa (perfil fora do suportado, 4:4:4, 10 bits)
//  segue no software sem perder nada. Depois do primeiro frame nao ha como refazer o que
//  ja passou, entao um erro ali e reportado como erro.

#include "gw_decode.h"
#include "codec_video.h"   // h26x_decoder_create/h26x_decode_frames + ImagePlaneList
#include "hw_dec.h"
#include <stdio.h>
#include <string.h>

#define GW_HW_STASH 64

struct GwVideoDec
{
    MediaCodec codec;
    int        accel;          // MediaAccel pedida

    // --- GPU ---
    HwDec*     hw;
    int        hw_frames;                  // frames ja entregues pela GPU
    int        flushed;                    // o drain ja foi pedido
    uint8_t*   stash[GW_HW_STASH];         // pacotes entregues a GPU antes do 1o frame
    int        stash_n[GW_HW_STASH];
    int        stash_count;
    int        stash_overflow;             // passou do limite: nao ha mais como cair

    // --- via h26x (H264/H265) ---
    DecoderInstance* h26x;
    ImagePlaneList*  list;     // reaproveitada entre pacotes; os itens sao liberados no ciclo
    int              next;     // indice do proximo frame a entregar

    // --- via vtable de media_codec (VP9/AV1) ---
    MediaDecoder*    gen;
    int              drained;  // 1 depois que o ReceiveFrame devolveu 0 neste pacote

    char             backend[192];
};

static int is_h26x(MediaCodec c) { return c == MEDIA_CODEC_H264 || c == MEDIA_CODEC_H265; }

int gw_vdec_available(MediaCodec codec)
{
    if (is_h26x(codec)) return 1;                  // openh264/de265 sempre linkados
    if (codec == MEDIA_CODEC_VP9 || codec == MEDIA_CODEC_AV1) return media_decoder_available(codec);
    return 0;
}

// Libera as ImagePlane do ciclo anterior: h26x_decode_frames aloca uma por frame.
static void h26x_cycle_reset(GwVideoDec* d)
{
    if (!d->list) return;
    for (int i = 0; i < d->list->Count; i++) GW_FREE(d->list->Items[i]);
    d->list->Count = 0;
    d->next = 0;
}

static const char* sw_name(MediaCodec c)
{
    switch (c)
    {
        case MEDIA_CODEC_H264: return "openh264";
        case MEDIA_CODEC_H265: return "libde265";
        case MEDIA_CODEC_VP9:  return "libvpx";
        case MEDIA_CODEC_AV1:  return "dav1d";
        default:               return "?";
    }
}

static int sw_open(GwVideoDec* d)
{
    if (is_h26x(d->codec))
    {
        d->h26x = h26x_decoder_create(d->codec == MEDIA_CODEC_H265 ? 265 : 264);
        d->list = imagep_list_new(4);
        if (!d->h26x || !d->list) return 0;
    }
    else
    {
        d->gen = media_decoder_open(d->codec);
        if (!d->gen) return 0;
    }
    snprintf(d->backend, sizeof(d->backend), "%s", sw_name(d->codec));
    return 1;
}

static int sw_send(GwVideoDec* d, const uint8_t* data, int size)
{
    if (d->h26x)
    {
        h26x_cycle_reset(d);
        if (!data || size <= 0)   // drain: os frames retidos para reordenacao
        {
            h26x_decoder_flush(d->h26x, d->list);
            return 0;
        }
        MediaBuffer in; in.Data = (uint_fast8_t*)data; in.Size = size; in.Max = size;
        h26x_decode_frames(d->h26x, &in, d->list);
        return 0;
    }

    d->drained = 0;
    if (!data || size <= 0) return d->gen->SendPacket(d->gen, 0);   // flush

    MediaPacket p; GW_ZERO(&p, sizeof(p));
    p.Data = (uint8_t*)data; p.Size = size;
    return d->gen->SendPacket(d->gen, &p);
}

static int sw_next(GwVideoDec* d, GwImage* out)
{
    if (d->h26x)
    {
        if (!d->list || d->next >= d->list->Count) return 0;
        ImagePlane* im = d->list->Items[d->next++];
        for (int i = 0; i < 3; i++) { out->Planes[i] = (uint8_t*)im->Planes[i]; out->Strides[i] = im->Strides[i]; }
        out->Width = im->Width; out->Height = im->Height;
        return 1;
    }
    if (!d->gen || d->drained) return 0;

    MediaFrame f;
    int r = d->gen->ReceiveFrame(d->gen, &f);
    if (r != 1) { d->drained = 1; return r < 0 ? -1 : 0; }

    out->Planes[0] = f.Y; out->Strides[0] = f.StrideY;
    out->Planes[1] = f.U; out->Strides[1] = f.StrideU;
    out->Planes[2] = f.V; out->Strides[2] = f.StrideV;
    out->Width = f.Width; out->Height = f.Height;
    return 1;
}

static void stash_clear(GwVideoDec* d)
{
    for (int i = 0; i < d->stash_count; i++) GW_FREE(d->stash[i]);
    d->stash_count = 0;
}

static void stash_push(GwVideoDec* d, const uint8_t* data, int size)
{
    if (d->hw_frames > 0 || d->stash_overflow || d->accel == MEDIA_ACCEL_HARDWARE) return;
    uint8_t* c = d->stash_count < GW_HW_STASH ? (uint8_t*)GW_ALLOC(size) : 0;
    if (!c) { stash_clear(d); d->stash_overflow = 1; return; }
    memcpy(c, data, (size_t)size);
    d->stash[d->stash_count] = c;
    d->stash_n[d->stash_count] = size;
    d->stash_count++;
}

// Troca a GPU pelo software antes do primeiro frame, reenviando o que a GPU ja tinha recebido.
static int fall_back(GwVideoDec* d)
{
    if (d->accel == MEDIA_ACCEL_HARDWARE || d->hw_frames > 0 || d->stash_overflow) return 0;
    hw_dec_close(&d->hw);
    if (!sw_open(d)) return 0;

    int ok = 1;
    if (d->h26x)
    {
        // Os frames de TODOS os pacotes reenviados se acumulam na lista (sem o reset por pacote
        // do sw_send), e o gw_vdec_next entrega todos.
        h26x_cycle_reset(d);
        for (int i = 0; i < d->stash_count; i++)
        {
            MediaBuffer in; in.Data = (uint_fast8_t*)d->stash[i]; in.Size = d->stash_n[i]; in.Max = d->stash_n[i];
            h26x_decode_frames(d->h26x, &in, d->list);
        }
        if (d->flushed) h26x_decoder_flush(d->h26x, d->list);
    }
    else
    {
        // VP9/AV1: o decoder da vtable so devolve os frames do ULTIMO pacote enviado, entao so
        // da para reenviar se a GPU tinha recebido um unico pacote.
        if (d->stash_count > 1) ok = 0;
        else if (d->stash_count == 1 && sw_send(d, d->stash[0], d->stash_n[0]) < 0) ok = 0;
        else if (d->flushed && sw_send(d, 0, 0) < 0) ok = 0;
    }
    stash_clear(d);
    return ok;
}

GwVideoDec* gw_vdec_open_accel(MediaCodec codec, int accel)
{
    GwVideoDec* d = (GwVideoDec*)GW_CALLOC(1, sizeof(GwVideoDec));
    if (!d) return 0;
    d->codec = codec;
    d->accel = accel;

    if (accel != MEDIA_ACCEL_SOFTWARE)
    {
        d->hw = hw_dec_open(codec);
        if (d->hw) snprintf(d->backend, sizeof(d->backend), "%s", hw_dec_name(d->hw));
    }
    if (!d->hw && (accel == MEDIA_ACCEL_HARDWARE || !gw_vdec_available(codec) || !sw_open(d)))
    {
        gw_vdec_close(&d);
        return 0;
    }
    return d;
}

GwVideoDec* gw_vdec_open(MediaCodec codec) { return gw_vdec_open_accel(codec, MEDIA_ACCEL_AUTO); }

int gw_vdec_send(GwVideoDec* d, const uint8_t* data, int size)
{
    if (!d) return -1;
    if (d->hw)
    {
        const int flush = (!data || size <= 0);
        if (flush) d->flushed = 1;
        else       stash_push(d, data, size);
        if (hw_dec_send(d->hw, data, size) == 0) return 0;
        return fall_back(d) ? 0 : -1;
    }
    return sw_send(d, data, size);
}

int gw_vdec_next(GwVideoDec* d, GwImage* out)
{
    if (!d || !out) return -1;
    GW_ZERO(out, sizeof(*out));

    if (d->hw)
    {
        uint8_t* p[3]; int st[3], w = 0, h = 0;
        int r = hw_dec_next(d->hw, p, st, &w, &h);
        if (r == 1)
        {
            if (++d->hw_frames == 1) stash_clear(d);   // a GPU funcionou: nao ha mais para onde cair
            for (int i = 0; i < 3; i++) { out->Planes[i] = p[i]; out->Strides[i] = st[i]; }
            out->Width = w; out->Height = h;
            return 1;
        }
        if (r == 0) return 0;
        if (!fall_back(d)) return -1;
    }
    return sw_next(d, out);
}

void gw_vdec_close(GwVideoDec** pd)
{
    if (!pd || !*pd) return;
    GwVideoDec* d = *pd;

    if (d->hw)   hw_dec_close(&d->hw);
    stash_clear(d);
    if (d->h26x) h26x_decoder_release(&d->h26x);
    if (d->list) { h26x_cycle_reset(d); imagep_list_release(&d->list, 1); }
    if (d->gen)  d->gen->Close(d->gen);

    GW_FREE(d);
    *pd = 0;
}

const char* gw_vdec_backend(const GwVideoDec* d) { return d ? d->backend : ""; }
