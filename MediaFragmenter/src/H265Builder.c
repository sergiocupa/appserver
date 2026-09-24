#include "../../appserver/submodules/xplatbase/Xplatbase/Xplatbase/src/memory_pool.h"
#include "../../appserver/submodules/xplatbase/Xplatbase/Xplatbase/src/string_handler.h"
//  MIT License – Modified for Mandatory Attribution
//  
//  Copyright(c) 2025 Sergio Paludo
//
//  github.com/sergiocupa
//  
//  Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files, 
//  to use, copy, modify, merge, publish, distribute, and sublicense the software, including for commercial purposes, provided that:
//  
//     01. The original author’s credit is retained in all copies of the source code;
//     02. The original author’s credit is included in any code generated, derived, or distributed from this software, including templates, libraries, or code - generating scripts.
//  
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED.


#include "../include/MediaFragmenter.h"


// ──────────────────────────────────────────────────────────────────────────────
// Tipos de NAL Units para H.265/HEVC
// ──────────────────────────────────────────────────────────────────────────────
#define HEVC_NAL_TRAIL_N        0
#define HEVC_NAL_TRAIL_R        1
#define HEVC_NAL_TSA_N          2
#define HEVC_NAL_TSA_R          3
#define HEVC_NAL_STSA_N         4
#define HEVC_NAL_STSA_R         5
#define HEVC_NAL_RADL_N         6
#define HEVC_NAL_RADL_R         7
#define HEVC_NAL_RASL_N         8
#define HEVC_NAL_RASL_R         9
#define HEVC_NAL_BLA_W_LP       16
#define HEVC_NAL_BLA_W_RADL     17
#define HEVC_NAL_BLA_N_LP       18
#define HEVC_NAL_IDR_W_RADL     19
#define HEVC_NAL_IDR_N_LP       20
#define HEVC_NAL_CRA_NUT        21
#define HEVC_NAL_VPS            32
#define HEVC_NAL_SPS            33
#define HEVC_NAL_PPS            34
#define HEVC_NAL_AUD            35
#define HEVC_NAL_EOS_NUT        36
#define HEVC_NAL_EOB_NUT        37
#define HEVC_NAL_FD_NUT         38
#define HEVC_NAL_PREFIX_SEI     39
#define HEVC_NAL_SUFFIX_SEI     40


// Verifica se é um NAL de tipo IDR ou IRAP (Random Access Point)
static inline int hevc_is_irap(uint8_t nal_type)
{
    return nal_type >= HEVC_NAL_BLA_W_LP && nal_type <= HEVC_NAL_CRA_NUT;
}

// Verifica se é um NAL de tipo IDR especificamente
static inline int hevc_is_idr(uint8_t nal_type)
{
    return nal_type == HEVC_NAL_IDR_W_RADL || nal_type == HEVC_NAL_IDR_N_LP;
}

// Extrai o tipo de NAL do header H.265 (2 bytes)
// NAL header H.265: [forbidden_zero_bit(1) | nal_unit_type(6) | nuh_layer_id(6) | nuh_temporal_id_plus1(3)]
static inline uint8_t hevc_get_nal_type(uint8_t* nal_header)
{
    return (nal_header[0] >> 1) & 0x3F;
}


// ──────────────────────────────────────────────────────────────────────────────
// Cria buffer Annex-B com VPS + SPS + PPS para inicialização do decoder
// ──────────────────────────────────────────────────────────────────────────────
uint8_t* h265_create_annexb(VideoMetadata* meta, int* length)
{
    // H.265 requer VPS + SPS + PPS
    int vps_size = meta->Vps.Size;
    int sps_size = meta->Sps.Size;
    int pps_size = meta->Pps.Size;

    if (!meta->Vps.Data || vps_size == 0)
    {
        fprintf(stderr, "AVISO: VPS não disponível, tentando sem VPS\n");
        vps_size = 0;
    }

    if (!meta->Sps.Data || sps_size == 0)
    {
        fprintf(stderr, "ERRO: SPS não encontrado para H.265\n");
        *length = 0;
        return NULL;
    }

    if (!meta->Pps.Data || pps_size == 0)
    {
        fprintf(stderr, "ERRO: PPS não encontrado para H.265\n");
        *length = 0;
        return NULL;
    }

    // Calcular tamanho total: [start_code(4) + VPS] + [start_code(4) + SPS] + [start_code(4) + PPS]
    int total_size = 0;
    if (vps_size > 0) total_size += 4 + vps_size;
    total_size += 4 + sps_size;
    total_size += 4 + pps_size;

    *length = total_size;
    uint8_t* annexb = memop_alloc_raw(total_size);
    if (!annexb)
    {
        fprintf(stderr, "ERRO: Falha ao alocar buffer Annex-B (%d bytes)\n", total_size);
        *length = 0;
        return NULL;
    }

    int pos = 0;

    // VPS (se disponível)
    if (vps_size > 0)
    {
        annexb[pos++] = 0x00;
        annexb[pos++] = 0x00;
        annexb[pos++] = 0x00;
        annexb[pos++] = 0x01;
        memop_copy_raw(annexb + pos, meta->Vps.Data, vps_size);
        pos += vps_size;
    }

    // SPS
    annexb[pos++] = 0x00;
    annexb[pos++] = 0x00;
    annexb[pos++] = 0x00;
    annexb[pos++] = 0x01;
    memop_copy_raw(annexb + pos, meta->Sps.Data, sps_size);
    pos += sps_size;

    // PPS
    annexb[pos++] = 0x00;
    annexb[pos++] = 0x00;
    annexb[pos++] = 0x00;
    annexb[pos++] = 0x01;
    memop_copy_raw(annexb + pos, meta->Pps.Data, pps_size);

    return annexb;
}


// ──────────────────────────────────────────────────────────────────────────────
// Cria um único frame H.265 em formato Annex-B
// ──────────────────────────────────────────────────────────────────────────────
int h265_create_single_frame(FILE* f, FrameIndex* frame, VideoMetadata* metadata, MediaBuffer* output)
{
    if (!frame || frame->Nals.Count == 0)
    {
        fprintf(stderr, "Frame inválido ou sem NALs.\n");
        return -1;
    }

    if (!metadata || metadata->LengthSize < 1 || metadata->LengthSize > 4)
    {
        fprintf(stderr, "Metadata ou length_size inválido.\n");
        return -2;
    }

    // Verifica se frame contém IRAP (tipos 16-21 para H.265)
    int has_irap = 0;
    int has_vps = 0;
    int has_sps = 0;
    int has_pps = 0;

    for (int i = 0; i < frame->Nals.Count; i++)
    {
        uint8_t nal_type = frame->Nals.Items[i]->Type;
        if (hevc_is_irap(nal_type)) has_irap = 1;
        if (nal_type == HEVC_NAL_VPS) has_vps = 1;
        if (nal_type == HEVC_NAL_SPS) has_sps = 1;
        if (nal_type == HEVC_NAL_PPS) has_pps = 1;
    }

    // Se tem IRAP mas não tem VPS/SPS/PPS, precisamos injetar
    int need_inject_vps = (has_irap && !has_vps && metadata->Vps.Data && metadata->Vps.Size > 0);
    int need_inject_sps = (has_irap && !has_sps && metadata->Sps.Data && metadata->Sps.Size > 0);
    int need_inject_pps = (has_irap && !has_pps && metadata->Pps.Data && metadata->Pps.Size > 0);

    // Calcula tamanho necessário para Annex B:
    size_t total_size = 0;

    if (need_inject_vps)
    {
        total_size += 4 + metadata->Vps.Size;  // start code + VPS
    }
    if (need_inject_sps)
    {
        total_size += 4 + metadata->Sps.Size;  // start code + SPS
    }
    if (need_inject_pps)
    {
        total_size += 4 + metadata->Pps.Size;  // start code + PPS
    }

    for (int i = 0; i < frame->Nals.Count; i++)
    {
        total_size += 4 + frame->Nals.Items[i]->Size;
    }

    // Mesmo motivo do H264Builder: trocar output->Data vazava um buffer por frame.
    if (!mbuffer_ensure(output, total_size))
    {
        fprintf(stderr, "Falha na alocação de memória (%zu bytes).\n", total_size);
        return -3;
    }

    size_t pos = 0;

    // Injeta VPS antes do frame se necessário
    if (need_inject_vps)
    {
        output->Data[pos++] = 0x00;
        output->Data[pos++] = 0x00;
        output->Data[pos++] = 0x00;
        output->Data[pos++] = 0x01;
        memop_copy_raw(output->Data + pos, metadata->Vps.Data, metadata->Vps.Size);
        pos += metadata->Vps.Size;

#ifdef DEBUG_NALS
        fprintf(stderr, "Injetado VPS (%d bytes) antes de IRAP frame\n", metadata->Vps.Size);
#endif
    }

    // Injeta SPS antes do frame se necessário
    if (need_inject_sps)
    {
        output->Data[pos++] = 0x00;
        output->Data[pos++] = 0x00;
        output->Data[pos++] = 0x00;
        output->Data[pos++] = 0x01;
        memop_copy_raw(output->Data + pos, metadata->Sps.Data, metadata->Sps.Size);
        pos += metadata->Sps.Size;

#ifdef DEBUG_NALS
        fprintf(stderr, "Injetado SPS (%d bytes) antes de IRAP frame\n", metadata->Sps.Size);
#endif
    }

    // Injeta PPS antes do frame se necessário
    if (need_inject_pps)
    {
        output->Data[pos++] = 0x00;
        output->Data[pos++] = 0x00;
        output->Data[pos++] = 0x00;
        output->Data[pos++] = 0x01;
        memop_copy_raw(output->Data + pos, metadata->Pps.Data, metadata->Pps.Size);
        pos += metadata->Pps.Size;

#ifdef DEBUG_NALS
        fprintf(stderr, "Injetado PPS (%d bytes) antes de IRAP frame\n", metadata->Pps.Size);
#endif
    }

    // Processa NALs do frame
    for (int j = 0; j < frame->Nals.Count; j++)
    {
        NALUIndex* nal = frame->Nals.Items[j];

        // Adiciona start code Annex B: 00 00 00 01
        if (pos + 4 > total_size)
        {
            memop_free_raw(output->Data);
            fprintf(stderr, "Overflow no buffer Annex B (start code).\n");
            return -4;
        }

        output->Data[pos++] = 0x00;
        output->Data[pos++] = 0x00;
        output->Data[pos++] = 0x00;
        output->Data[pos++] = 0x01;

        // nal->Offset já aponta para o início do NAL (sem o prefixo de tamanho)
        if (fseek(f, nal->Offset, SEEK_SET) != 0)
        {
            memop_free_raw(output->Data);
            fprintf(stderr, "Erro no fseek para NAL %d (offset=%llu).\n", j, (unsigned long long)nal->Offset);
            return -5;
        }

        size_t nal_data_size = nal->Size;

        if (pos + nal_data_size > total_size)
        {
            memop_free_raw(output->Data);
            fprintf(stderr, "NAL %d size (%zu) excede buffer alocado (pos=%zu, total=%zu).\n",
                j, nal_data_size, pos, total_size);
            return -6;
        }

        // Lê o NAL unit completo do arquivo
        size_t bytes_read = fread(output->Data + pos, 1, nal_data_size, f);
        if (bytes_read != nal_data_size)
        {
            memop_free_raw(output->Data);
            fprintf(stderr, "Falha ao ler NAL %d: esperado %zu bytes, lido %zu bytes.\n",
                j, nal_data_size, bytes_read);
            return -7;
        }

#ifdef DEBUG_NALS
        fprintf(stderr, "NAL %d: tipo=%d, size=%zu\n", j, nal->Type, nal_data_size);
#endif

        pos += nal_data_size;
    }

    output->Size = pos;

    // Verificação de integridade
    if (pos != total_size)
    {
        fprintf(stderr, "AVISO: Tamanho final (%zu) diferente do esperado (%zu).\n", pos, total_size);
        return -8;
    }

    return 0;
}


// ──────────────────────────────────────────────────────────────────────────────
// Cria fragmento H.265 (múltiplos frames) em formato Annex-B ou MP4
// ──────────────────────────────────────────────────────────────────────────────
int h265_create_fragment(
    FILE* f,
    FrameIndexList* frame_list,
    double timeline_offset,
    double timeline_fragment_duration,
    int frame_offset,
    int frame_length,
    int include_parameter_sets,
    H264FragmentFormat format,  // Reutiliza o enum (H264_FORMAT_ANNEXB / H264_FORMAT_MP4)
    MediaBuffer* output)
{
    // ───────────────────────────────────────────────────────────────
    // VALIDAÇÃO
    // ───────────────────────────────────────────────────────────────

    if (!f || !frame_list || !output)
    {
        fprintf(stderr, "ERRO: Parâmetros NULL\n");
        return -1;
    }

    if (frame_list->Count == 0)
    {
        fprintf(stderr, "ERRO: Lista de frames vazia\n");
        return -2;
    }

    VideoMetadata* metadata = &frame_list->Metadata;

    if (metadata->LengthSize < 1 || metadata->LengthSize > 4)
    {
        fprintf(stderr, "ERRO: LengthSize inválido (%d)\n", metadata->LengthSize);
        return -3;
    }

    // ───────────────────────────────────────────────────────────────
    // DETERMINAR INTERVALO DE FRAMES
    // ───────────────────────────────────────────────────────────────

    int start_frame = 0;
    int end_frame = 0;

    if (frame_offset < 0)
    {
        // MODO TIMELINE
        if (metadata->Fps <= 0)
        {
            fprintf(stderr, "ERRO: FPS inválido (%.2f)\n", metadata->Fps);
            return -4;
        }

        start_frame = (int)(timeline_offset * metadata->Fps);
        end_frame = (int)((timeline_offset + timeline_fragment_duration) * metadata->Fps);

        if (start_frame < 0) start_frame = 0;
        if (end_frame > (int)frame_list->Count) end_frame = frame_list->Count;

        if (start_frame >= (int)frame_list->Count)
        {
            fprintf(stderr, "ERRO: Offset %.2fs → frame %d (total: %d)\n",
                timeline_offset, start_frame, frame_list->Count);
            return -5;
        }
    }
    else
    {
        // MODO FRAME
        start_frame = frame_offset;
        end_frame = frame_offset + frame_length;

        if (start_frame < 0 || start_frame >= (int)frame_list->Count)
        {
            fprintf(stderr, "ERRO: frame_offset %d inválido (0-%d)\n",
                start_frame, frame_list->Count - 1);
            return -6;
        }

        if (end_frame > (int)frame_list->Count)
            end_frame = frame_list->Count;
    }

    int total_frames = end_frame - start_frame;

    if (total_frames <= 0)
    {
        fprintf(stderr, "ERRO: Intervalo inválido (%d-%d)\n", start_frame, end_frame);
        return -7;
    }

    // ───────────────────────────────────────────────────────────────
    // DETECTAR SE TEM IRAP NO FRAGMENTO
    // ───────────────────────────────────────────────────────────────

    int has_irap = 0;

    for (int i = start_frame; i < end_frame; i++)
    {
        FrameIndex* frame = frame_list->Frames[i];
        for (int j = 0; j < frame->Nals.Count; j++)
        {
            if (hevc_is_irap(frame->Nals.Items[j]->Type))
            {
                has_irap = 1;
                break;
            }
        }
        if (has_irap) break;
    }

    // ───────────────────────────────────────────────────────────────
    // CALCULAR TAMANHO DO BUFFER
    // ───────────────────────────────────────────────────────────────

    size_t total_size = 0;
    int prefix_size = (format == H264_FORMAT_ANNEXB) ? 4 : 4;  // Ambos usam 4 bytes

    // VPS/SPS/PPS se solicitado E houver IRAP
    int will_add_vps = 0;
    int will_add_sps = 0;
    int will_add_pps = 0;

    if (include_parameter_sets && has_irap)
    {
        if (metadata->Vps.Data && metadata->Vps.Size > 0)
        {
            total_size += prefix_size + metadata->Vps.Size;
            will_add_vps = 1;
        }
        if (metadata->Sps.Data && metadata->Sps.Size > 0)
        {
            total_size += prefix_size + metadata->Sps.Size;
            will_add_sps = 1;
        }
        if (metadata->Pps.Data && metadata->Pps.Size > 0)
        {
            total_size += prefix_size + metadata->Pps.Size;
            will_add_pps = 1;
        }
    }

    // Somar tamanho de todos os NALs
    for (int i = start_frame; i < end_frame; i++)
    {
        FrameIndex* frame = frame_list->Frames[i];
        for (int j = 0; j < frame->Nals.Count; j++)
        {
            total_size += prefix_size + frame->Nals.Items[j]->Size;
        }
    }

    // ───────────────────────────────────────────────────────────────
    // ALOCAR BUFFER
    // ───────────────────────────────────────────────────────────────

    if (!mbuffer_ensure(output, total_size))   // reusa o buffer do chamador (ver acima)
    {
        fprintf(stderr, "ERRO: Falha ao alocar %zu bytes\n", total_size);
        return -8;
    }

    size_t pos = 0;

    // ───────────────────────────────────────────────────────────────
    // ESCREVER VPS/SPS/PPS (se solicitado)
    // ───────────────────────────────────────────────────────────────

    // VPS
    if (will_add_vps)
    {
        if (format == H264_FORMAT_ANNEXB)
        {
            output->Data[pos++] = 0x00;
            output->Data[pos++] = 0x00;
            output->Data[pos++] = 0x00;
            output->Data[pos++] = 0x01;
        }
        else
        {
            uint32_t size = metadata->Vps.Size;
            output->Data[pos++] = (size >> 24) & 0xFF;
            output->Data[pos++] = (size >> 16) & 0xFF;
            output->Data[pos++] = (size >> 8) & 0xFF;
            output->Data[pos++] = size & 0xFF;
        }

        memop_copy_raw(output->Data + pos, metadata->Vps.Data, metadata->Vps.Size);
        pos += metadata->Vps.Size;
    }

    // SPS
    if (will_add_sps)
    {
        if (format == H264_FORMAT_ANNEXB)
        {
            output->Data[pos++] = 0x00;
            output->Data[pos++] = 0x00;
            output->Data[pos++] = 0x00;
            output->Data[pos++] = 0x01;
        }
        else
        {
            uint32_t size = metadata->Sps.Size;
            output->Data[pos++] = (size >> 24) & 0xFF;
            output->Data[pos++] = (size >> 16) & 0xFF;
            output->Data[pos++] = (size >> 8) & 0xFF;
            output->Data[pos++] = size & 0xFF;
        }

        memop_copy_raw(output->Data + pos, metadata->Sps.Data, metadata->Sps.Size);
        pos += metadata->Sps.Size;
    }

    // PPS
    if (will_add_pps)
    {
        if (format == H264_FORMAT_ANNEXB)
        {
            output->Data[pos++] = 0x00;
            output->Data[pos++] = 0x00;
            output->Data[pos++] = 0x00;
            output->Data[pos++] = 0x01;
        }
        else
        {
            uint32_t size = metadata->Pps.Size;
            output->Data[pos++] = (size >> 24) & 0xFF;
            output->Data[pos++] = (size >> 16) & 0xFF;
            output->Data[pos++] = (size >> 8) & 0xFF;
            output->Data[pos++] = size & 0xFF;
        }

        memop_copy_raw(output->Data + pos, metadata->Pps.Data, metadata->Pps.Size);
        pos += metadata->Pps.Size;
    }

    // ───────────────────────────────────────────────────────────────
    // COPIAR NALs DOS FRAMES
    // ───────────────────────────────────────────────────────────────

    for (int i = start_frame; i < end_frame; i++)
    {
        FrameIndex* frame = frame_list->Frames[i];

        for (int j = 0; j < frame->Nals.Count; j++)
        {
            NALUIndex* nal = frame->Nals.Items[j];

            // Escrever prefixo (Annex-B ou MP4)
            if (format == H264_FORMAT_ANNEXB)
            {
                output->Data[pos++] = 0x00;
                output->Data[pos++] = 0x00;
                output->Data[pos++] = 0x00;
                output->Data[pos++] = 0x01;
            }
            else
            {
                uint32_t size = nal->Size;
                output->Data[pos++] = (size >> 24) & 0xFF;
                output->Data[pos++] = (size >> 16) & 0xFF;
                output->Data[pos++] = (size >> 8) & 0xFF;
                output->Data[pos++] = size & 0xFF;
            }

            // Posicionar e ler NAL do arquivo
            if (fseek(f, nal->Offset, SEEK_SET) != 0)
            {
                memop_free_raw(output->Data);
                fprintf(stderr, "ERRO: fseek falhou (frame %d, NAL %d)\n", i, j);
                return -9;
            }

            size_t nal_size = nal->Size;
            size_t bytes_read = fread(output->Data + pos, 1, nal_size, f);

            if (bytes_read != nal_size)
            {
                memop_free_raw(output->Data);
                fprintf(stderr, "ERRO: Leitura falhou (frame %d, NAL %d)\n", i, j);
                return -10;
            }

            pos += nal_size;
        }
    }

    output->Size = pos;

    // Verificação
    if (pos != total_size)
    {
        fprintf(stderr, "AVISO: Tamanho final %zu != esperado %zu\n", pos, total_size);
    }

    return 0;
}