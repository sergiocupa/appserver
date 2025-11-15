#include "../include/MediaFragmenter.h"
#include "Mp4Builder.h"
#include "Mp4MetadataUtil.h"
#include "FileUtil.h"



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
                    stsc->entries[i].first_chunk       = read32(f);
                    stsc->entries[i].samples_per_chunk = read32(f);
                    stsc->entries[i].sample_desc_id    = read32(f);
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


FrameIndexList* mp4builder_get_frames(const char* path)
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
    if (mp4meta_load_video_metadata(f, &meta) != 0)
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
    list->Metadata.Pps.Data   = meta.Pps.Data; list->Metadata.Pps.Size = meta.Pps.Size;
    list->Metadata.Sps.Data   = meta.Sps.Data; list->Metadata.Sps.Size = meta.Sps.Size;
    list->Metadata.LengthSize = meta.LengthSize;
    list->Metadata.Codec      = meta.Codec; list->Metadata.Fps = meta.Fps;
    list->Metadata.Width      = meta.Width; list->Metadata.Height = meta.Height;

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

            mnalu_list_add(&frame->Nals, offset + pos + length_size, nal_len, nal_type);
            
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





// ──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//                                              Funcoes para cria fragmentos MP4
// ──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────


// Cria box mfhd (Movie Fragment Header)
static void create_mfhd_box(uint32_t sequence_number, MediaBuffer* output)
{
    output->Size = 16;
    output->Data = malloc(output->Size);

    uint8_t* p = output->Data;

    // Box size
    write_u32_be(p, 16);
    p += 4;

    // Box type: 'mfhd'
    p[0] = 'm'; p[1] = 'f'; p[2] = 'h'; p[3] = 'd';
    p += 4;

    // Version (1 byte) + Flags (3 bytes)
    write_u32_be(p, 0);
    p += 4;

    // Sequence number
    write_u32_be(p, sequence_number);
}

// Cria box tfhd (Track Fragment Header)
static void create_tfhd_box(uint32_t track_id, MediaBuffer* output)
{
    output->Size = 16;
    output->Data = malloc(output->Size);

    uint8_t* p = output->Data;

    // Box size
    write_u32_be(p, 16);
    p += 4;

    // Box type: 'tfhd'
    p[0] = 't'; p[1] = 'f'; p[2] = 'h'; p[3] = 'd';
    p += 4;

    // Version (0) + Flags (0x020000 = default-base-is-moof)
    write_u32_be(p, 0x00020000);
    p += 4;

    // Track ID
    write_u32_be(p, track_id);
}

// Cria box tfdt (Track Fragment Decode Time)
static void create_tfdt_box(uint64_t base_media_decode_time, MediaBuffer* output)
{
    // Usar versão 1 (64 bits) se necessário
    int use_v1 = (base_media_decode_time > 0xFFFFFFFF);

    output->Size = use_v1 ? 20 : 16;
    output->Data = malloc(output->Size);

    uint8_t* p = output->Data;

    // Box size
    write_u32_be(p, output->Size);
    p += 4;

    // Box type: 'tfdt'
    p[0] = 't'; p[1] = 'f'; p[2] = 'd'; p[3] = 't';
    p += 4;

    // Version + Flags
    write_u32_be(p, use_v1 ? 0x01000000 : 0x00000000);
    p += 4;

    // Base media decode time
    if (use_v1)
    {
        write_u64_be(p, base_media_decode_time);
    }
    else
    {
        write_u32_be(p, (uint32_t)base_media_decode_time);
    }
}

// Cria box trun (Track Fragment Run). Simplificado: assume duração fixa por sample
static void create_trun_box(uint32_t sample_count, uint32_t sample_duration, uint32_t data_offset, MediaBuffer* output)
{
    // Flags: 0x000001 (data-offset-present)
    //        0x000100 (sample-duration-present)
    uint32_t flags = 0x00000101;

    output->Size = 20;  // Header + data_offset + sample_count entries
    output->Data = malloc(output->Size);

    uint8_t* p = output->Data;

    // Box size
    write_u32_be(p, output->Size);
    p += 4;

    // Box type: 'trun'
    p[0] = 't'; p[1] = 'r'; p[2] = 'u'; p[3] = 'n';
    p += 4;

    // Version (0) + Flags
    write_u32_be(p, flags);
    p += 4;

    // Sample count
    write_u32_be(p, sample_count);
    p += 4;

    // Data offset (offset do mdat relativo ao início do moof)
    write_u32_be(p, data_offset);
}

// Cria box traf (Track Fragment)
static void create_traf_box(uint32_t track_id, uint64_t base_media_decode_time, uint32_t sample_count, uint32_t sample_duration, uint32_t moof_size, MediaBuffer* output)
{
    // Criar sub-boxes
    MediaBuffer tfhd, tfdt, trun;

    create_tfhd_box(track_id, &tfhd);
    create_tfdt_box(base_media_decode_time, &tfdt);

    // data_offset = tamanho do moof + 8 (header do mdat)
    uint32_t data_offset = moof_size + 8;
    create_trun_box(sample_count, sample_duration, data_offset, &trun);

    // Calcular tamanho total do traf
    output->Size = 8 + tfhd.Size + tfdt.Size + trun.Size;
    output->Data = malloc(output->Size);

    uint8_t* p = output->Data;

    // Box size
    write_u32_be(p, output->Size);
    p += 4;

    // Box type: 'traf'
    p[0] = 't'; p[1] = 'r'; p[2] = 'a'; p[3] = 'f';
    p += 4;

    // Copiar sub-boxes
    memcpy(p, tfhd.Data, tfhd.Size);
    p += tfhd.Size;

    memcpy(p, tfdt.Data, tfdt.Size);
    p += tfdt.Size;

    memcpy(p, trun.Data, trun.Size);

    // Liberar sub-boxes
    free(tfhd.Data);
    free(tfdt.Data);
    free(trun.Data);
}

// Cria box moof (Movie Fragment)
static void create_moof_box(
    uint32_t sequence_number,
    uint32_t track_id,
    uint64_t base_media_decode_time,
    uint32_t sample_count,
    uint32_t sample_duration,
    MediaBuffer* output)
{
    // Criar sub-boxes
    MediaBuffer mfhd;
    create_mfhd_box(sequence_number, &mfhd);

    // Calcular tamanho estimado do moof (para data_offset)
    // moof = 8 (header) + mfhd + traf
    // Precisamos estimar o tamanho do traf primeiro

    MediaBuffer tfhd_temp, tfdt_temp;
    create_tfhd_box(track_id, &tfhd_temp);
    create_tfdt_box(base_media_decode_time, &tfdt_temp);

    uint32_t traf_size = 8 + tfhd_temp.Size + tfdt_temp.Size + 20; // +20 para trun
    free(tfhd_temp.Data);
    free(tfdt_temp.Data);

    uint32_t moof_size = 8 + mfhd.Size + traf_size;

    // Agora criar traf com o tamanho correto
    MediaBuffer traf;
    create_traf_box(track_id, base_media_decode_time, sample_count,
        sample_duration, moof_size, &traf);

    // Recalcular tamanho real do moof
    moof_size = 8 + mfhd.Size + traf.Size;

    // Alocar buffer final
    output->Size = moof_size;
    output->Data = malloc(output->Size);

    uint8_t* p = output->Data;

    // Box size
    write_u32_be(p, moof_size);
    p += 4;

    // Box type: 'moof'
    p[0] = 'm'; p[1] = 'o'; p[2] = 'o'; p[3] = 'f';
    p += 4;

    // Copiar sub-boxes
    memcpy(p, mfhd.Data, mfhd.Size);
    p += mfhd.Size;

    memcpy(p, traf.Data, traf.Size);

    // Liberar sub-boxes
    free(mfhd.Data);
    free(traf.Data);
}

// Cria box mdat (Media Data)
static void create_mdat_box(uint8_t* data, size_t data_size, MediaBuffer* output)
{
    output->Size = 8 + data_size;
    output->Data = malloc(output->Size);

    uint8_t* p = output->Data;

    // Box size
    write_u32_be(p, output->Size);
    p += 4;

    // Box type: 'mdat'
    p[0] = 'm'; p[1] = 'd'; p[2] = 'a'; p[3] = 't';
    p += 4;

    // Copiar dados
    memcpy(p, data, data_size);
}


int mp4builder_create_fragment( FILE* f, FrameIndexList* frame_list, double timeline_offset, double timeline_fragment_duration, int frame_offset, int frame_length, MP4FragmentInfo* frag_info, MediaBuffer* output)
{
    if (!f || !frame_list || !frag_info || !output)
    {
        fprintf(stderr, "ERRO: Parâmetros NULL\n");
        return -1;
    }

    // ───────────────────────────────────────────────────────────────
    // PASSO 1: Usar Função 01 para obter dados H.264 (size-prefixed)
    // ───────────────────────────────────────────────────────────────

    MediaBuffer h264_data;

    int result = h264_create_fragment(
        f,
        frame_list,
        timeline_offset,
        timeline_fragment_duration,
        frame_offset,
        frame_length,
        0,  // NÃO incluir SPS/PPS (vão no init segment)
        H264_FORMAT_MP4,  // Formato size-prefixed
        &h264_data
    );

    if (result != 0)
    {
        fprintf(stderr, "ERRO: Falha ao criar dados H.264 (%d)\n", result);
        return result;
    }

    // ───────────────────────────────────────────────────────────────
    // PASSO 2: Calcular número de samples (frames)
    // ───────────────────────────────────────────────────────────────

    int start_frame, end_frame;

    if (frame_offset < 0)
    {
        // Timeline
        float fps = frame_list->Metadata.Fps;
        if (fps <= 0) fps = 30.0f;

        start_frame = (int)(timeline_offset * fps);
        end_frame = (int)((timeline_offset + timeline_fragment_duration) * fps);

        if (end_frame > frame_list->Count)
            end_frame = frame_list->Count;
    }
    else
    {
        // Frames
        start_frame = frame_offset;
        end_frame = frame_offset + frame_length;

        if (end_frame > frame_list->Count)
            end_frame = frame_list->Count;
    }

    uint32_t sample_count = end_frame - start_frame;

    // Calcular duração por sample (assumindo FPS constante)
    uint32_t sample_duration = frag_info->Timescale / (frame_list->Metadata.Fps > 0 ? frame_list->Metadata.Fps : 30.0f);

    // ───────────────────────────────────────────────────────────────
    // PASSO 3: Criar box moof
    // ───────────────────────────────────────────────────────────────

    MediaBuffer moof;

    create_moof_box( frag_info->SequenceNumber, frag_info->TrackID, frag_info->BaseMediaDecodeTime, sample_count, sample_duration, &moof);

    // ───────────────────────────────────────────────────────────────
    // PASSO 4: Criar box mdat
    // ───────────────────────────────────────────────────────────────

    MediaBuffer mdat;
    create_mdat_box(h264_data.Data, h264_data.Size, &mdat);

    // ───────────────────────────────────────────────────────────────
    // PASSO 5: Juntar moof + mdat
    // ───────────────────────────────────────────────────────────────

    output->Size = moof.Size + mdat.Size;
    output->Data = malloc(output->Size);

    if (!output->Data)
    {
        fprintf(stderr, "ERRO: Falha ao alocar %zu bytes\n", output->Size);
        free(h264_data.Data);
        free(moof.Data);
        free(mdat.Data);
        return -2;
    }

    memcpy(output->Data, moof.Data, moof.Size);
    memcpy(output->Data + moof.Size, mdat.Data, mdat.Size);

 /*   printf("Fragmento MP4 criado:\n");
    printf("   Sequence: %u\n", frag_info->SequenceNumber);
    printf("   Samples: %u\n", sample_count);
    printf("   moof: %zu bytes\n", moof.Size);
    printf("   mdat: %zu bytes\n", mdat.Size);
    printf("   Total: %zu bytes\n", output->Size);*/

    // Liberar buffers temporários
    free(h264_data.Data);
    free(moof.Data);
    free(mdat.Data);

    return 0;
}