#include "../include/MediaFragmenter.h"
#include "FileUtil.h"
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>


// Função para parsear tables de vídeo
static void parse_video_track_tables(FILE* f, uint64_t end, int is_video, StszData* stsz, StcoData* stco, StscData* stsc, SttsData* stts, uint32_t* timescale, int* codec)
{
    uint8_t name[5] = { 0 };

    while ((uint64_t)ftell(f) < end)
    {
        long box_start = ftell(f);
        uint32_t size = read32(f);
        if (fread(name, 1, 4, f) != 4) break;

        uint64_t box_size = size;
        uint64_t header_size = 8;

        if (size == 1) 
        {
            box_size = read64(f);
            header_size = 16;
        }
        else if (size == 0) 
        {
            box_size = end - box_start;  // CORRIGIDO: usa 'end', não file end
        }

        if (box_size < header_size) 
        {
            fseek(f, box_start + header_size, SEEK_SET);
            continue;
        }

        uint64_t next = box_start + box_size;
        uint64_t payload_start = box_start + header_size;

        // hdlr para confirmar vídeo
        if (!memcmp(name, "hdlr", 4))
        {
            fseek(f, payload_start, SEEK_SET);  // CORRIGIDO: vai para payload_start
            fseek(f, 8, SEEK_CUR);  // version/flags + pre_defined
            uint8_t handler[5] = { 0 };
            if (fread(handler, 1, 4, f) == 4) 
            {
                if (!memcmp(handler, "vide", 4))
                {
                    is_video = 1;
                }
            }
            fseek(f, next, SEEK_SET);
            continue;
        }

        // mdhd para timescale
        if (!memcmp(name, "mdhd", 4) && is_video)
        {
            fseek(f, payload_start, SEEK_SET);  // CORRIGIDO
            uint8_t version = read8(f);
            fseek(f, 3, SEEK_CUR);  // flags

            if (version == 1) 
            {
                fseek(f, 16, SEEK_CUR);  // creation + modification time (64-bit)
                *timescale = read32(f);
            }
            else
            {
                fseek(f, 8, SEEK_CUR);  // creation + modification time (32-bit)
                *timescale = read32(f);
            }
            fseek(f, next, SEEK_SET);
            continue;
        }

        // stsd para codec
        if (!memcmp(name, "stsd", 4) && is_video)
        {
            fseek(f, payload_start, SEEK_SET);  // CORRIGIDO
            uint8_t version = read8(f);
            fseek(f, 3, SEEK_CUR);  // flags
            uint32_t entry_count = read32(f);

            if (entry_count > 0 && entry_count < 256)
            {
                for (uint32_t i = 0; i < entry_count; i++) 
                {
                    long entry_start = ftell(f);
                    uint32_t entry_size = read32(f);

                    if (entry_size < 8 || entry_start + entry_size > next) break;

                    uint8_t entry_name[5] = { 0 };
                    if (fread(entry_name, 1, 4, f) != 4) break;

                    if (!memcmp(entry_name, "avc1", 4) || !memcmp(entry_name, "avc3", 4))
                    {
                        *codec = 264;
                    }
                    else if (!memcmp(entry_name, "hvc1", 4) || !memcmp(entry_name, "hev1", 4)) 
                    {
                        *codec = 265;
                    }

                    fseek(f, entry_start + entry_size, SEEK_SET);
                }
            }
            fseek(f, next, SEEK_SET);
            continue;
        }

        // stts para deltas de tempo
        if (!memcmp(name, "stts", 4) && is_video) 
        {
            fseek(f, payload_start, SEEK_SET);  // CORRIGIDO
            uint8_t version = read8(f);
            fseek(f, 3, SEEK_CUR);  // flags

            stts->count = read32(f);

            if (stts->count > 0 && stts->count < 1000000) 
            {  // CORRIGIDO: validação
                stts->entries = malloc(stts->count * sizeof(struct SttsEntry));
                if (!stts->entries)
                {
                    stts->count = 0;
                    fseek(f, next, SEEK_SET);
                    continue;
                }

                for (uint32_t i = 0; i < stts->count; i++) 
                {
                    if (ftell(f) + 8 > next) {  // CORRIGIDO: verificação de bounds
                        stts->count = i;
                        break;
                    }
                    stts->entries[i].count = read32(f);
                    stts->entries[i].delta = read32(f);
                }
            }
            fseek(f, next, SEEK_SET);
            continue;
        }

        // stsc para mapeamento sample-to-chunk
        if (!memcmp(name, "stsc", 4) && is_video)
        {
            fseek(f, payload_start, SEEK_SET);  // CORRIGIDO
            uint8_t version = read8(f);
            fseek(f, 3, SEEK_CUR);  // flags

            stsc->count = read32(f);

            if (stsc->count > 0 && stsc->count < 1000000) 
            {  // CORRIGIDO: validação
                stsc->entries = malloc(stsc->count * sizeof(struct StscEntry));
                if (!stsc->entries)
                {
                    stsc->count = 0;
                    fseek(f, next, SEEK_SET);
                    continue;
                }

                for (uint32_t i = 0; i < stsc->count; i++) 
                {
                    if (ftell(f) + 12 > next)
                    {  // CORRIGIDO: verificação de bounds
                        stsc->count = i;
                        break;
                    }
                    stsc->entries[i].first_chunk = read32(f);
                    stsc->entries[i].samples_per_chunk = read32(f);
                    stsc->entries[i].sample_desc_id = read32(f);
                }
            }
            fseek(f, next, SEEK_SET);
            continue;
        }

        // stsz para tamanhos de samples
        if (!memcmp(name, "stsz", 4) && is_video) 
        {
            fseek(f, payload_start, SEEK_SET);  // CORRIGIDO
            uint8_t version = read8(f);
            fseek(f, 3, SEEK_CUR);  // flags

            uint32_t sample_size = read32(f);
            stsz->count = read32(f);

            if (stsz->count > 0 && stsz->count < 10000000) 
            {  // CORRIGIDO: validação
                stsz->sizes = malloc(stsz->count * sizeof(uint32_t));
                if (!stsz->sizes) 
                {
                    stsz->count = 0;
                    fseek(f, next, SEEK_SET);
                    continue;
                }

                if (sample_size == 0) {  // Tamanhos variáveis
                    for (uint32_t i = 0; i < stsz->count; i++)
                    {
                        if (ftell(f) + 4 > next) 
                        {  // CORRIGIDO: verificação de bounds
                            stsz->count = i;
                            break;
                        }
                        stsz->sizes[i] = read32(f);
                    }
                }
                else
                {  // Tamanho fixo
                    for (uint32_t i = 0; i < stsz->count; i++)
                    {
                        stsz->sizes[i] = sample_size;
                    }
                }
            }
            fseek(f, next, SEEK_SET);
            continue;
        }

        // stco ou co64 para offsets de chunks
        if ((!memcmp(name, "stco", 4) || !memcmp(name, "co64", 4)) && is_video) 
        {
            stco->is_co64 = !memcmp(name, "co64", 4);

            fseek(f, payload_start, SEEK_SET);  // CORRIGIDO
            uint8_t version = read8(f);
            fseek(f, 3, SEEK_CUR);  // flags

            stco->count = read32(f);

            if (stco->count > 0 && stco->count < 1000000)
            {  // CORRIGIDO: validação
                stco->offsets = malloc(stco->count * sizeof(uint64_t));
                if (!stco->offsets) 
                {
                    stco->count = 0;
                    fseek(f, next, SEEK_SET);
                    continue;
                }

                for (uint32_t i = 0; i < stco->count; i++)
                {
                    int bytes_needed = stco->is_co64 ? 8 : 4;
                    if (ftell(f) + bytes_needed > next)
                    {  // CORRIGIDO: verificação de bounds
                        stco->count = i;
                        break;
                    }
                    stco->offsets[i] = stco->is_co64 ? read64(f) : (uint64_t)read32(f);
                }
            }
            fseek(f, next, SEEK_SET);
            continue;
        }

        // Recursa para containers, propagando is_video
        if (!memcmp(name, "moov", 4) || !memcmp(name, "trak", 4) ||
            !memcmp(name, "mdia", 4) || !memcmp(name, "minf", 4) ||
            !memcmp(name, "stbl", 4) || !memcmp(name, "moof", 4) || !memcmp(name, "edts", 4))
        {
            parse_video_track_tables(f, next, is_video, stsz, stco, stsc, stts, timescale, codec);
        }

        if (next > end) break;  // CORRIGIDO: evita ultrapassar o limite
        fseek(f, next, SEEK_SET);
    }
}



FrameIndexList* concod_get_frames(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f)
    {
        perror("Erro ao abrir arquivo");
        return NULL;
    }
    // Adicionado: Extrai metadata cedo para obter length_size do avcC
    //   Fazer uma funcao só para esta info
    //   Por fim validar tamanho do frame, com tamanho total dos NALs.
    
    VideoMetadata meta = { 0 };
    if (concod_load_video_metadata(f, &meta) != 0) 
    {
        printf("Falha ao carregar metadata para length_size.\n");
        fclose(f);
        return NULL;
    }
    fseek(f, 0, SEEK_SET);

    // Nao pega length_size, mas de onde vem isso? O Grok assumiu que tem este dado. Perguntar como obter este dado
    int length_size = meta.LengthSize;  // Adicionado: Usa length_size extraído (geralmente 4, mas dinâmico)
    if (length_size < 1 || length_size > 4) 
    {
        printf("length_size inválido: %d\n", length_size);
        fclose(f);
        return NULL;
    }
    // Libera SPS/PPS se não necessários aqui (metadata será recarregada depois)
    if (meta.sps) free(meta.sps);
    if (meta.pps) free(meta.pps);

    // Parse tables
    StszData stsz = { 0 };
    StcoData stco = { 0 };
    StscData stsc = { 0 };
    SttsData stts = { 0 };
    uint32_t timescale = 0;
    rewind(f);
    fseek(f, 0, SEEK_END);
    uint64_t file_end = ftell(f);
    rewind(f);
    parse_video_track_tables(f, file_end, 0, &stsz, &stco, &stsc, &stts, &timescale, &meta.Codec);
    if (stsz.count == 0 || meta.Codec == 0)
    {
        printf("Falha ao parsear tables de vídeo.\n");
        fclose(f);
        return NULL;
    }

    // Cria lista ordenada
    FrameIndexList* list = mframe_list_new(stsz.count);
    list->Metadata.pps = meta.pps; list->Metadata.pps_len = meta.pps_len; 
    list->Metadata.sps = meta.sps; list->Metadata.sps_len = meta.pps_len;
    list->Metadata.LengthSize = meta.LengthSize; 
    list->Metadata.Codec = meta.Codec; list->Metadata.Fps = meta.Fps; 
    list->Metadata.width = meta.width; list->Metadata.height = meta.height;

    // Calcula offsets absolutos para cada sample usando stsc
    uint64_t* sample_offsets = malloc(stsz.count * sizeof(uint64_t));
    uint32_t sample_idx = 0;
    uint32_t chunk_idx = 0;
    uint32_t spc = stsc.count > 0 ? stsc.entries[0].samples_per_chunk : 1;
    uint32_t entry_idx = 0;
    for (uint32_t i = 0; i < stsz.count; i++)
    {
        if (sample_idx >= spc)
        {
            chunk_idx++;
            sample_idx = 0;
            if (entry_idx + 1 < stsc.count && chunk_idx + 1 == stsc.entries[entry_idx + 1].first_chunk)
            {
                entry_idx++;
            }
            spc = stsc.entries[entry_idx].samples_per_chunk;
        }
        if (chunk_idx >= stco.count) break;
        uint64_t chunk_offset = stco.offsets[chunk_idx];
        uint64_t local_offset = 0;
        for (uint32_t j = 0; j < sample_idx; j++)
        {
            local_offset += stsz.sizes[i - (sample_idx - j)];
        }
        sample_offsets[i] = chunk_offset + local_offset;
        sample_idx++;
    }
    // Para cada sample (frame)
    for (uint32_t i = 0; i < stsz.count; i++)
    {
        uint64_t offset = sample_offsets[i];
        uint32_t size = stsz.sizes[i];
        FrameIndex* frame = mframe_new(offset);
        frame->Size = size;
        // Parse NALs dentro do sample (formato AVCC: length-prefixed, focado em H.264)
        fseek(f, offset, SEEK_SET);
        uint64_t pos = 0;
        while (pos < size)
        {
            // Corrigido: Lê nal_len dinamicamente com base em length_size (em vez de fixo read32)
            uint8_t nal_len_buf[4] = { 0 };
            if (fread(nal_len_buf, 1, length_size, f) != (size_t)length_size) break;
            uint32_t nal_len = read_n(nal_len_buf, length_size);
            if (nal_len == 0 || pos + nal_len + length_size > size) break;  // Corrigido: Usa length_size no check de overflow
            uint8_t nal_header = read8(f);
            uint8_t nal_type = nal_header & 0x1F; // Para H.264
            mnalu_list_add(&frame->Nals, offset + pos, nal_len, nal_type);  // Corrigido: Removido +1 incorreto no nal_len (agora usa nal_len puro como tamanho do NAL)
            fseek(f, nal_len - 1, SEEK_CUR); // Pula resto (header lido)
            pos += nal_len + length_size;  // Corrigido: Atualiza pos com length_size dinâmico (em vez de fixo +4)
        }
        // Adicionado: Verificação de depuração para garantir que todo o sample foi parseado
        if (pos != size)
        {
            fprintf(stderr, "Aviso: Sample %u parseado incompleto (pos=%llu, size=%u)\n", i, pos, size);
        }
        mframe_list_add(list, frame);
    }
    // Libera recursos
    free(sample_offsets);
    free(stsz.sizes);
    free(stco.offsets);
    free(stsc.entries);
    free(stts.entries);

    fclose(f);

    return list;
}



void concod_display_frame_index(FrameIndexList* frames)
{
    for (int i = 0; i < frames->Count; i++)
    {
        FrameIndex* f = frames->Frames[i];
        printf("Frame %d | Offset %llu | Size %llu\n", i, f->Offset, f->Size);
        for (int j = 0; j < f->Nals.Count; j++)
        {
            NALUIndex* ni = f->Nals.Items[j];
            printf("  NAL %d | Offset %llu | Size %llu | Type %d\n", j, ni->Offset, ni->Size, ni->Type);
        }
    }
}



// Constrói header Annex B com SPS e PPS
int build_initial_header_from_meta(const VideoMetadata* meta, uint8_t* header, int* length)
{
    if (!meta || !header || !length) {
        fprintf(stderr, "Parâmetros inválidos em build_initial_header_from_meta\n");
        return -1;
    }

    if (!meta->sps || meta->sps_len <= 0) {
        fprintf(stderr, "SPS inválido: %p, len=%d\n", (void*)meta->sps, meta->sps_len);
        return -1;
    }

    if (!meta->pps || meta->pps_len <= 0) {
        fprintf(stderr, "PPS inválido: %p, len=%d\n", (void*)meta->pps, meta->pps_len);
        return -1;
    }

    int pos = 0;

    // Start code + SPS
    header[pos++] = 0x00;
    header[pos++] = 0x00;
    header[pos++] = 0x00;
    header[pos++] = 0x01;

    memcpy(header + pos, meta->sps, meta->sps_len);
    pos += meta->sps_len;

    // Start code + PPS
    header[pos++] = 0x00;
    header[pos++] = 0x00;
    header[pos++] = 0x00;
    header[pos++] = 0x01;

    memcpy(header + pos, meta->pps, meta->pps_len);
    pos += meta->pps_len;

    *length = pos;

    printf("DEBUG: Header construído com %d bytes (SPS=%d, PPS=%d)\n",
        pos, meta->sps_len, meta->pps_len);

    // Debug: Mostra primeiros bytes
    printf("DEBUG: Header bytes: ");
    for (int i = 0; i < (pos < 20 ? pos : 20); i++) {
        printf("%02X ", header[i]);
    }
    printf("\n");

    return 0;
}

// Envia SPS/PPS para o decodificador
int concod_send_initial_header_from_meta(ISVCDecoder* decoder, const VideoMetadata* meta)
{
    if (!decoder || !meta) {
        fprintf(stderr, "Decoder ou metadata NULL\n");
        return -1;
    }

    uint8_t header[1024];
    int header_len = 0;

    // Constrói header com SPS/PPS
    if (build_initial_header_from_meta(meta, header, &header_len) != 0) 
    {
        fprintf(stderr, "Falha ao construir header\n");
        return -1;
    }

    if (header_len <= 0 || header_len > 1024) {
        fprintf(stderr, "Tamanho de header inválido: %d\n", header_len);
        return -1;
    }

    printf("DEBUG: Enviando header de %d bytes para decodificador\n", header_len);

    uint8_t* planes[3] = { NULL, NULL, NULL };
    SBufferInfo info;
    memset(&info, 0, sizeof(SBufferInfo));

    // Envia header para o decodificador
    DECODING_STATE state = (*decoder)->DecodeFrame2(decoder, header, header_len, planes, &info);

    printf("DEBUG: Estado após DecodeFrame2: %d\n", state);

    // Códigos de erro OpenH264:
    // dsErrorFree = 0: Sucesso
    // dsFramePending = 1: Frame pendente (normal)
    // dsRefLost = 2: Referência perdida
    // dsBitstreamError = 4: Erro no bitstream
    // dsDepLayerLost = 8: Camada dependente perdida
    // dsNoParamSets = 16: SPS/PPS não encontrados ou inválidos

    if (state == 16 || state == dsNoParamSets) {
        fprintf(stderr, "ERRO 16: SPS/PPS não foram aceitos pelo decodificador!\n");
        fprintf(stderr, "Possíveis causas:\n");
        fprintf(stderr, "  - SPS/PPS corrompidos\n");
        fprintf(stderr, "  - Formato Annex B incorreto\n");
        fprintf(stderr, "  - Decodificador não inicializado corretamente\n");
        return -2;
    }

    if (state != dsErrorFree && state != dsFramePending) {
        fprintf(stderr, "Erro ao enviar cabeçalho SPS/PPS: %d\n", state);
        return -2;
    }

    printf("DEBUG: Header SPS/PPS enviado com sucesso\n");
    return 0;
}






// Função de conversão AVCC para Annex B com injeção automática de SPS/PPS para IDR
uint8_t* concod_convert_avcc_to_annexb(FILE* f, FrameIndex* frame, VideoMetadata* metadata, size_t* annexb_size)
{
    if (!frame || frame->Nals.Count == 0)
    {
        fprintf(stderr, "Frame inválido ou sem NALs.\n");
        return NULL;
    }

    if (!metadata || metadata->LengthSize < 1 || metadata->LengthSize > 4) {
        fprintf(stderr, "Metadata ou length_size inválido.\n");
        return NULL;
    }

    // Verifica se frame contém IDR (tipo 5 para H.264)
    int has_idr = 0;
    int has_sps = 0;
    int has_pps = 0;

    for (int i = 0; i < frame->Nals.Count; i++) {
        uint8_t nal_type = frame->Nals.Items[i]->Type;
        if (nal_type == 5) has_idr = 1;      // IDR slice
        if (nal_type == 7) has_sps = 1;      // SPS
        if (nal_type == 8) has_pps = 1;      // PPS
    }

    // Se tem IDR mas não tem SPS/PPS, precisamos injetar
    int need_inject_sps = (has_idr && !has_sps && metadata->sps && metadata->sps_len > 0);
    int need_inject_pps = (has_idr && !has_pps && metadata->pps && metadata->pps_len > 0);

    // Calcula tamanho necessário para Annex B:
    // Para cada NAL: 4 bytes (start code) + tamanho do NAL
    // + SPS/PPS se necessário
    size_t total_size = 0;

    if (need_inject_sps) {
        total_size += 4 + metadata->sps_len;  // start code + SPS
    }
    if (need_inject_pps) {
        total_size += 4 + metadata->pps_len;  // start code + PPS
    }

    for (int i = 0; i < frame->Nals.Count; i++) {
        total_size += 4 + frame->Nals.Items[i]->Size;
    }

    uint8_t* annexb = (uint8_t*)malloc(total_size);
    if (!annexb) {
        fprintf(stderr, "Falha na alocação de memória (%zu bytes).\n", total_size);
        return NULL;
    }

    size_t pos = 0;

    // Injeta SPS antes do frame se necessário
    if (need_inject_sps) {
        annexb[pos++] = 0x00;
        annexb[pos++] = 0x00;
        annexb[pos++] = 0x00;
        annexb[pos++] = 0x01;
        memcpy(annexb + pos, metadata->sps, metadata->sps_len);
        pos += metadata->sps_len;

#ifdef DEBUG_NALS
        fprintf(stderr, "Injetado SPS (%d bytes) antes de IDR frame\n", metadata->sps_len);
#endif
    }

    // Injeta PPS antes do frame se necessário
    if (need_inject_pps) {
        annexb[pos++] = 0x00;
        annexb[pos++] = 0x00;
        annexb[pos++] = 0x00;
        annexb[pos++] = 0x01;
        memcpy(annexb + pos, metadata->pps, metadata->pps_len);
        pos += metadata->pps_len;

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
            free(annexb);
            fprintf(stderr, "Overflow no buffer Annex B (start code).\n");
            return NULL;
        }

        annexb[pos++] = 0x00;
        annexb[pos++] = 0x00;
        annexb[pos++] = 0x00;
        annexb[pos++] = 0x01;

        // nal->Offset já aponta para o início do NAL (sem o prefixo de tamanho)
        if (fseek(f, nal->Offset, SEEK_SET) != 0)
        {
            free(annexb);
            fprintf(stderr, "Erro no fseek para NAL %d (offset=%llu).\n", j, nal->Offset);
            return NULL;
        }

        // Tamanho do NAL (header + payload)
        size_t nal_data_size = nal->Size;

        if (pos + nal_data_size > total_size)
        {
            free(annexb);
            fprintf(stderr, "NAL %d size (%zu) excede buffer alocado (pos=%zu, total=%zu).\n",
                j, nal_data_size, pos, total_size);
            return NULL;
        }

        // Lê o NAL unit completo do arquivo
        size_t bytes_read = fread(annexb + pos, 1, nal_data_size, f);
        if (bytes_read != nal_data_size)
        {
            free(annexb);
            fprintf(stderr, "Falha ao ler NAL %d: esperado %zu bytes, lido %zu bytes.\n",
                j, nal_data_size, bytes_read);
            return NULL;
        }

#ifdef DEBUG_NALS
        fprintf(stderr, "NAL %d: tipo=%d, size=%zu\n", j, nal->Type, nal_data_size);
#endif

        pos += nal_data_size;
    }

    *annexb_size = pos;

    // Verificação de integridade
    if (pos != total_size) {
        fprintf(stderr, "AVISO: Tamanho final (%zu) diferente do esperado (%zu).\n", pos, total_size);
    }

    return annexb;
}


