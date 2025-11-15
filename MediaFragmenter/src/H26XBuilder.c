#include "../include/MediaFragmenter.h"


// contra próximo start code (00 00 00 01 ou 00 00 01)
static size_t find_next_start_code(uint8_t* data, size_t size, size_t offset)
{
    for (size_t i = offset; i < size - 3; i++)
    {
        // Procurar por 00 00 00 01
        if (data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x00 && data[i + 3] == 0x01)
        {
            return i + 4;  // Retorna posição APÓS o start code
        }

        // Procurar por 00 00 01 (start code curto)
        if (data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x01)
        {
            return i + 3;
        }
    }

    return size;  // Não encontrou
}


uint8_t* h26x_create_annexb(VideoMetadata* meta, int* length)
{
    *length = meta->Pps.Size + meta->Sps.Size + 8;
    uint8_t* annexb = malloc(*length);
    annexb[0] = 0;
    annexb[1] = 0;
    annexb[2] = 0;
    annexb[3] = 1;
    int pos = 4;

    memcpy(annexb + pos, meta->Sps.Data, meta->Sps.Size);
    pos += meta->Sps.Size;

    annexb[pos] = 0;
    annexb[pos + 1] = 0;
    annexb[pos + 2] = 0;
    annexb[pos + 3] = 1;
    pos += 4;

    memcpy(annexb + pos, meta->Pps.Data, meta->Pps.Size);
    return annexb;
}


int h26x_create_single_frame(FILE* f, FrameIndex* frame, VideoMetadata* metadata, MediaBuffer* output)
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

    // Verifica se frame contém IDR (tipo 5 para H.264)
    int has_idr = 0;
    int has_sps = 0;
    int has_pps = 0;

    for (int i = 0; i < frame->Nals.Count; i++)
    {
        uint8_t nal_type = frame->Nals.Items[i]->Type;
        if (nal_type == 5) has_idr = 1;      // IDR slice
        if (nal_type == 7) has_sps = 1;      // SPS
        if (nal_type == 8) has_pps = 1;      // PPS
    }

    // Se tem IDR mas não tem SPS/PPS, precisamos injetar
    int need_inject_sps = (has_idr && !has_sps && metadata->Sps.Data && metadata->Sps.Size > 0);
    int need_inject_pps = (has_idr && !has_pps && metadata->Pps.Data && metadata->Pps.Size > 0);

    // Calcula tamanho necessário para Annex B:
    // Para cada NAL: 4 bytes (start code) + tamanho do NAL
    // + SPS/PPS se necessário
    size_t total_size = 0;

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

    output->Data = (uint8_t*)malloc(total_size);
    if (!output->Data)
    {
        fprintf(stderr, "Falha na alocação de memória (%zu bytes).\n", total_size);
        return -3;
    }

    size_t pos = 0;

    // Injeta SPS antes do frame se necessário
    if (need_inject_sps)
    {
        output->Data[pos++] = 0x00;
        output->Data[pos++] = 0x00;
        output->Data[pos++] = 0x00;
        output->Data[pos++] = 0x01;
        memcpy(output->Data + pos, metadata->Sps.Data, metadata->Sps.Size);
        pos += metadata->Sps.Size;

#ifdef DEBUG_NALS
        fprintf(stderr, "Injetado SPS (%d bytes) antes de IDR frame\n", metadata->sps_len);
#endif
    }

    // Injeta PPS antes do frame se necessário
    if (need_inject_pps)
    {
        output->Data[pos++] = 0x00;
        output->Data[pos++] = 0x00;
        output->Data[pos++] = 0x00;
        output->Data[pos++] = 0x01;
        memcpy(output->Data + pos, metadata->Pps.Data, metadata->Pps.Size);
        pos += metadata->Pps.Size;

#ifdef DEBUG_NALS
        fprintf(stderr, "Injetado PPS (%d bytes) antes de IDR frame\n", metadata->pps_len);
#endif
    }

    // Processa NALs do frame
    for (int j = 0; j < frame->Nals.Count; j++)
    {
        NALUIndex* nal = frame->Nals.Items[j];

        // Adiciona start code Annex B: 00 00 00 01
        if (pos + 4 > total_size)
        {
            free(output->Data);
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
            free(output->Data);
            fprintf(stderr, "Erro no fseek para NAL %d (offset=%llu).\n", j, nal->Offset);
            return -5;
        }

        // Tamanho do NAL (header + payload)
        size_t nal_data_size = nal->Size;

        if (pos + nal_data_size > total_size)
        {
            free(output->Data);
            fprintf(stderr, "NAL %d size (%zu) excede buffer alocado (pos=%zu, total=%zu).\n", j, nal_data_size, pos, total_size);
            return -6;
        }

        // Lê o NAL unit completo do arquivo
        size_t bytes_read = fread(output->Data + pos, 1, nal_data_size, f);
        if (bytes_read != nal_data_size)
        {
            free(output->Data);
            fprintf(stderr, "Falha ao ler NAL %d: esperado %zu bytes, lido %zu bytes.\n", j, nal_data_size, bytes_read);
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






// include_parameter_sets - Se true, injeta SPS/PPS no início
int h264_create_fragment(FILE* f, FrameIndexList* frame_list, double timeline_offset, double timeline_fragment_duration, int frame_offset, int frame_length, int include_sps_pps, H264FragmentFormat format, MediaBuffer* output)
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
        if (end_frame > frame_list->Count) end_frame = frame_list->Count;

        if (start_frame >= frame_list->Count)
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

        if (start_frame < 0 || start_frame >= frame_list->Count)
        {
            fprintf(stderr, "ERRO: frame_offset %d inválido (0-%d)\n",
                start_frame, frame_list->Count - 1);
            return -6;
        }

        if (end_frame > frame_list->Count)
            end_frame = frame_list->Count;
    }

    int total_frames = end_frame - start_frame;

    if (total_frames <= 0)
    {
        fprintf(stderr, "ERRO: Intervalo inválido (%d-%d)\n", start_frame, end_frame);
        return -7;
    }

    // ───────────────────────────────────────────────────────────────
    // DETECTAR SE TEM IDR NO FRAGMENTO
    // ───────────────────────────────────────────────────────────────

    int has_idr = 0;

    for (int i = start_frame; i < end_frame; i++)
    {
        FrameIndex* frame = frame_list->Frames[i];
        for (int j = 0; j < frame->Nals.Count; j++)
        {
            if (frame->Nals.Items[j]->Type == 5)
            {
                has_idr = 1;
                break;
            }
        }
        if (has_idr) break;
    }

    // ───────────────────────────────────────────────────────────────
    // CALCULAR TAMANHO DO BUFFER
    // ───────────────────────────────────────────────────────────────

    size_t total_size = 0;
    int prefix_size = (format == H264_FORMAT_ANNEXB) ? 4 : 4;  // Ambos usam 4 bytes

    // SPS/PPS se solicitado E houver IDR
    int will_add_sps = 0;
    int will_add_pps = 0;

    if (include_sps_pps && has_idr)
    {
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

    output->Data = (uint8_t*)malloc(total_size);
    if (!output->Data)
    {
        fprintf(stderr, "ERRO: Falha ao alocar %zu bytes\n", total_size);
        return -8;
    }

    size_t pos = 0;

    // ───────────────────────────────────────────────────────────────
    // ESCREVER SPS/PPS (se solicitado)
    // ───────────────────────────────────────────────────────────────

    if (will_add_sps)
    {
        if (format == H264_FORMAT_ANNEXB)
        {
            // Annex-B: [00 00 00 01]
            output->Data[pos++] = 0x00;
            output->Data[pos++] = 0x00;
            output->Data[pos++] = 0x00;
            output->Data[pos++] = 0x01;
        }
        else
        {
            // MP4: [size] (4 bytes big-endian)
            uint32_t size = metadata->Sps.Size;
            output->Data[pos++] = (size >> 24) & 0xFF;
            output->Data[pos++] = (size >> 16) & 0xFF;
            output->Data[pos++] = (size >> 8) & 0xFF;
            output->Data[pos++] = size & 0xFF;
        }

        memcpy(output->Data + pos, metadata->Sps.Data, metadata->Sps.Size);
        pos += metadata->Sps.Size;
    }

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

        memcpy(output->Data + pos, metadata->Pps.Data, metadata->Pps.Size);
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
                // Start code: [00 00 00 01]
                output->Data[pos++] = 0x00;
                output->Data[pos++] = 0x00;
                output->Data[pos++] = 0x00;
                output->Data[pos++] = 0x01;
            }
            else
            {
                // Size prefix: [size] (4 bytes big-endian)
                uint32_t size = nal->Size;
                output->Data[pos++] = (size >> 24) & 0xFF;
                output->Data[pos++] = (size >> 16) & 0xFF;
                output->Data[pos++] = (size >> 8) & 0xFF;
                output->Data[pos++] = size & 0xFF;
            }

            // Posicionar e ler NAL do arquivo
            if (fseek(f, nal->Offset, SEEK_SET) != 0)
            {
                free(output->Data);
                fprintf(stderr, "ERRO: fseek falhou (frame %d, NAL %d)\n", i, j);
                return -9;
            }

            size_t nal_size = nal->Size;
            size_t bytes_read = fread(output->Data + pos, 1, nal_size, f);

            if (bytes_read != nal_size)
            {
                free(output->Data);
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




//
//NALList* h26x_parse_annexb_stream(uint8_t* data, size_t size)
//{
//    NALList* list = nal_list_new(100);
//
//    size_t offset = 0;
//
//    // Pular possível start code inicial
//    if (size >= 4 && data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x00 && data[3] == 0x01)
//    {
//        offset = 4;
//    }
//    else if (size >= 3 && data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01)
//    {
//        offset = 3;
//    }
//
//    while (offset < size)
//    {
//        // Início do NAL atual
//        size_t nal_start = offset;
//
//        // Encontrar próximo start code
//        size_t next_start = find_next_start_code(data, size, offset + 1);
//
//        // Tamanho do NAL (até próximo start code ou fim)
//        size_t nal_size;
//        if (next_start < size)
//        {
//            // Voltar 4 ou 3 bytes para pegar apenas o NAL (sem o próximo start code)
//            if (next_start >= 4 && data[next_start - 4] == 0x00) nal_size = next_start - nal_start - 4;
//                                                            else nal_size = next_start - nal_start - 3;
//        }
//        else
//        {
//            // Último NAL
//            nal_size = size - nal_start;
//        }
//
//        if (nal_size > 0)
//        {
//            // Copiar NAL
//            uint8_t* nal = malloc(nal_size);
//            memcpy(nal, data + nal_start, nal_size);
//
//            // Tipo do NAL (5 bits menos significativos do primeiro byte)
//            uint8_t nal_type = nal[0] & 0x1F;
//
//            nal_list_add(list, nal, nal_size, nal_type);
//        }
//
//        offset = next_start;
//    }
//
//    return list;
//}
//
//
//static void build_frame(NALList* nals, FrameList* frames, int index, int* current_frame_start)
//{
//    // Calcular tamanho total do frame
//    size_t total_size = 0;
//    int has_idr = 0;
//
//    for (int j = (*current_frame_start); j < index; j++)
//    {
//        NAL* nalu = nals->Items[j];
//        total_size += 4 + nalu->Size;// start code + NAL
//        if (nalu->Type == 5) has_idr = 1;
//    }
//
//    // Montar frame em Annex-B
//    int_fast8_t* adata = malloc(total_size);
//    int size = 0;
//
//    for (int j = (*current_frame_start); j < index; j++)
//    {
//        NAL* naj = nals->Items[j];
//
//        // Start code
//        adata[size++] = 0x00;
//        adata[size++] = 0x00;
//        adata[size++] = 0x00;
//        adata[size++] = 0x01;
//
//        memcpy(adata + size, naj->Data, naj->Size);// NAL data
//        size += naj->Size;
//    }
//
//    frame_list_add(frames, adata, size, has_idr);
//}
//
//
//// Agrupa NALs em frames completos. Um frame pode ter vários NALs (slice, SEI, etc)
//FrameList* h26x_group_nals_into_frames(NALList* nals)
//{
//    FrameList* frames = frame_list_new((int)((double)nals->Count * 1.5));
//
//    int current_frame_start = 0;
//
//    for (int i = 0; i < nals->Count; i++)
//    {
//        NAL* nal = nals->Items[i];
//
//        // NAL types que começam um novo frame: 1 = Non-IDR slice; 5 = IDR slice
//        int is_slice = (nal->Type == 1 || nal->Type == 5);
//
//        // Se encontrou slice E não é o primeiro NAL, finalizar frame anterior
//        if (is_slice && i > 0 && i > current_frame_start)
//        {
//            build_frame(nals, frames, i, &current_frame_start);
//            current_frame_start = i;
//        }
//    }
//
//    // Frame final
//    if (current_frame_start < nals->Count)
//    {
//        build_frame(nals, frames, nals->Count, &current_frame_start);
//    }
//
//    return frames;
//}
//
//
//
//void h26x_create_frames()
//{
//    // refazer h26x_create_fragment, para diretamente crie os frames 
//    // Perguntar a Claude, o que é mais eficiente, para depois incorporar os frames no MP4:
//    //     Como são dispostos os NAL no MP4?
//    //     No Mp4 são dispostos os NAL ou frames?
//    //     Como são dispostos os NAL no H264? São dispostos NAL, ou Frames. Se são só NAL, entao os frames são só referenciados por index?
//    //     Preocupação é com eficencia nestas conversoes
//    ...
//}