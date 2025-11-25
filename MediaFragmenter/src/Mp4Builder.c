#include "../include/MediaFragmenter.h"
#include "Mp4Builder.h"
#include "Mp4MetadataUtil.h"
#include "FileUtil.h"
#include "BufferUtil.h"



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

    // Nao pega length_size, mas de onde vem isso? Assumiu que tem este dado. Perguntar como obter este dado
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
    list->Metadata.Pps.Data = meta.Pps.Data; list->Metadata.Pps.Size = meta.Pps.Size;
    list->Metadata.Sps.Data = meta.Sps.Data; list->Metadata.Sps.Size = meta.Sps.Size;
    list->Metadata.LengthSize = meta.LengthSize;
    list->Metadata.Codec = meta.Codec; list->Metadata.Fps = meta.Fps; list->Metadata.Timescale = meta.Timescale;
    list->Metadata.Width = meta.Width; list->Metadata.Height = meta.Height;

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
    buffer_write32(p, 16);
    p += 4;

    // Box type: 'mfhd'
    p[0] = 'm'; p[1] = 'f'; p[2] = 'h'; p[3] = 'd';
    p += 4;

    // Version (1 byte) + Flags (3 bytes)
    buffer_write32(p, 0);
    p += 4;

    // Sequence number
    buffer_write32(p, sequence_number);
}

// Cria box tfhd (Track Fragment Header)
static void create_tfhd_box(uint32_t track_id, MediaBuffer* output)
{
    output->Size = 16;
    output->Data = malloc(output->Size);

    uint8_t* p = output->Data;

    // Box size
    buffer_write32(p, 16);
    p += 4;

    // Box type: 'tfhd'
    p[0] = 't'; p[1] = 'f'; p[2] = 'h'; p[3] = 'd';
    p += 4;

    // Version (0) + Flags (0x020000 = default-base-is-moof)
    buffer_write32(p, 0x00020000);
    p += 4;

    // Track ID
    buffer_write32(p, track_id);
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
    buffer_write32(p, output->Size);
    p += 4;

    // Box type: 'tfdt'
    p[0] = 't'; p[1] = 'f'; p[2] = 'd'; p[3] = 't';
    p += 4;

    // Version + Flags
    buffer_write32(p, use_v1 ? 0x01000000 : 0x00000000);
    p += 4;

    // Base media decode time
    if (use_v1)
    {
        buffer_write64(p, base_media_decode_time);
    }
    else
    {
        buffer_write32(p, (uint32_t)base_media_decode_time);
    }
}

// Cria box trun (Track Fragment Run). Simplificado: assume duração fixa por sample
static void create_trun_box(uint32_t sample_count, uint32_t sample_duration, uint32_t data_offset, MediaBuffer* output)
{
    // Flags corrigidos:
    // 0x000001 = data-offset-present
    // NÃO incluir 0x000004 (first-sample-flags) - Chrome rejeita
    // NÃO incluir 0x000100 (sample-duration) - usaremos default do trex
    uint32_t flags = 0x00000001;  // apenas data-offset

    // Tamanho: header(8) + version/flags(4) + count(4) + data_offset(4) = 20
    output->Size = 20;
    output->Data = malloc(output->Size);

    uint8_t* p = output->Data;

    // Box size
    buffer_write32(p, output->Size);
    p += 4;

    // Box type: 'trun'
    p[0] = 't'; p[1] = 'r'; p[2] = 'u'; p[3] = 'n';
    p += 4;

    // Version (0) + Flags
    buffer_write32(p, flags);
    p += 4;

    // Sample count
    buffer_write32(p, sample_count);
    p += 4;

    // Data offset (offset do mdat relativo ao início do moof)
    buffer_write32(p, data_offset);
}

// Cria box traf (Track Fragment)
static void create_traf_box(uint32_t track_id, uint64_t base_media_decode_time, uint32_t sample_count, uint32_t sample_duration, uint32_t moof_size, MediaBuffer* output)
{
    MediaBuffer tfhd, tfdt, trun;

    create_tfhd_box(track_id, &tfhd);
    create_tfdt_box(base_media_decode_time, &tfdt);

    // data_offset = tamanho do moof + 8 (header do mdat)
    uint32_t data_offset = moof_size + 8;
    create_trun_box(sample_count, sample_duration, data_offset, &trun);

    output->Size = 8 + tfhd.Size + tfdt.Size + trun.Size;
    output->Data = malloc(output->Size);

    uint8_t* p = output->Data;

    buffer_write32(p, output->Size);
    p += 4;

    p[0] = 't'; p[1] = 'r'; p[2] = 'a'; p[3] = 'f';
    p += 4;

    memcpy(p, tfhd.Data, tfhd.Size);
    p += tfhd.Size;

    memcpy(p, tfdt.Data, tfdt.Size);
    p += tfdt.Size;

    memcpy(p, trun.Data, trun.Size);

    free(tfhd.Data);
    free(tfdt.Data);
    free(trun.Data);
}

// Cria box moof (Movie Fragment)
static void create_moof_box(uint32_t sequence_number, uint32_t track_id, uint64_t base_media_decode_time, uint32_t sample_count, uint32_t sample_duration, MediaBuffer* output)
{
    MediaBuffer mfhd;
    create_mfhd_box(sequence_number, &mfhd);

    // Estimar tamanho do moof para calcular data_offset
    MediaBuffer tfhd_temp, tfdt_temp;
    create_tfhd_box(track_id, &tfhd_temp);
    create_tfdt_box(base_media_decode_time, &tfdt_temp);

    // trun tem 20 bytes (sem first-sample-flags)
    uint32_t trun_size = 20;
    uint32_t traf_size = 8 + tfhd_temp.Size + tfdt_temp.Size + trun_size;
    free(tfhd_temp.Data);
    free(tfdt_temp.Data);

    uint32_t moof_size = 8 + mfhd.Size + traf_size;

    // Criar traf com tamanho correto
    MediaBuffer traf;
    create_traf_box(track_id, base_media_decode_time, sample_count, sample_duration, moof_size, &traf);

    // Recalcular tamanho real
    moof_size = 8 + mfhd.Size + traf.Size;

    output->Size = moof_size;
    output->Data = malloc(output->Size);

    uint8_t* p = output->Data;

    buffer_write32(p, moof_size);
    p += 4;

    p[0] = 'm'; p[1] = 'o'; p[2] = 'o'; p[3] = 'f';
    p += 4;

    memcpy(p, mfhd.Data, mfhd.Size);
    p += mfhd.Size;

    memcpy(p, traf.Data, traf.Size);

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
    buffer_write32(p, output->Size);
    p += 4;

    // Box type: 'mdat'
    p[0] = 'm'; p[1] = 'd'; p[2] = 'a'; p[3] = 't';
    p += 4;

    // Copiar dados
    memcpy(p, data, data_size);
}





// CRIAÇÃO DE BOXES: ftyp. Cria box ftyp (File Type Box). Define o tipo do arquivo MP4
static void create_ftyp_box(MediaBuffer* output)
{
    // ftyp para fragmentos DASH:
    // - major_brand: 'iso5' (ISO Base Media File Format v5)
    // - minor_version: 512
    // - compatible_brands: 'iso5', 'iso6', 'mp41'

    output->Size = 24;
    output->Data = malloc(output->Size);

    uint8_t* p = output->Data;

    // Box size
    buffer_write32(p, 24);
    p += 4;

    // Box type: 'ftyp'
    write_fourcc(p, "ftyp");
    p += 4;

    // Major brand: 'iso5'
    write_fourcc(p, "iso5");
    p += 4;

    // Minor version
    buffer_write32(p, 512);
    p += 4;

    // Compatible brands
    write_fourcc(p, "iso5");
    p += 4;
    write_fourcc(p, "mp41");
    p += 4;
}

// CRIAÇÃO DE BOXES: moov > mvhd Cria box mvhd (Movie Header Box)
static void create_mvhd_box(uint32_t timescale, uint32_t next_track_id, MediaBuffer* output)
{
    output->Size = 108;
    output->Data = malloc(output->Size);

    uint8_t* p = output->Data;

    // Box size
    buffer_write32(p, 108);
    p += 4;

    // Box type: 'mvhd'
    write_fourcc(p, "mvhd");
    p += 4;

    // Version (1 byte) + Flags (3 bytes)
    buffer_write32(p, 0);
    p += 4;

    // Creation time
    buffer_write32(p, 0);
    p += 4;

    // Modification time
    buffer_write32(p, 0);
    p += 4;

    // Timescale
    buffer_write32(p, timescale);
    p += 4;

    // Duration (0 para fragmentos)
    buffer_write32(p, 0);
    p += 4;

    // Rate (1.0 = 0x00010000)
    buffer_write32(p, 0x00010000);
    p += 4;

    // Volume (1.0 = 0x0100)
    buffer_write16(p, 0x0100);
    p += 2;

    // Reserved
    memset(p, 0, 10);
    p += 10;

    // Matrix (identity matrix)
    uint32_t matrix[9] = {
        0x00010000, 0, 0,
        0, 0x00010000, 0,
        0, 0, 0x40000000
    };
    for (int i = 0; i < 9; i++)
    {
        buffer_write32(p, matrix[i]);
        p += 4;
    }

    // Pre-defined
    memset(p, 0, 24);
    p += 24;

    // Next track ID
    buffer_write32(p, next_track_id);
}


// CRIAÇÃO DE BOXES: moov > mvex > mehd. Cria box mehd (Movie Extends Header Box)
static void create_mehd_box(uint64_t fragment_duration, MediaBuffer* output)
{
    // Usar versão 1 (64 bits) se necessário
    int use_v1 = (fragment_duration > 0xFFFFFFFF);

    output->Size = use_v1 ? 20 : 16;
    output->Data = malloc(output->Size);

    uint8_t* p = output->Data;

    // Box size
    buffer_write32(p, output->Size);
    p += 4;

    // Box type: 'mehd'
    write_fourcc(p, "mehd");
    p += 4;

    // Version + Flags
    buffer_write32(p, use_v1 ? 0x01000000 : 0x00000000);
    p += 4;

    // Fragment duration
    if (use_v1)
    {
        buffer_write64(p, fragment_duration);
    }
    else
    {
        buffer_write32(p, (uint32_t)fragment_duration);
    }
}

// CRIAÇÃO DE BOXES: moov > mvex > trex. Cria box trex (Track Extends Box). Define valores padrão para fragmentos
static void create_trex_box(uint32_t track_id, uint32_t default_sample_duration, MediaBuffer* output)
{
    output->Size = 32;
    output->Data = malloc(output->Size);

    uint8_t* p = output->Data;

    // Box size
    buffer_write32(p, 32);
    p += 4;

    // Box type: 'trex'
    p[0] = 't'; p[1] = 'r'; p[2] = 'e'; p[3] = 'x';
    p += 4;

    // Version + Flags
    buffer_write32(p, 0);
    p += 4;

    // Track ID
    buffer_write32(p, track_id);
    p += 4;

    // Default sample description index
    buffer_write32(p, 1);
    p += 4;

    // Default sample duration - AGORA COM VALOR CORRETO!
    buffer_write32(p, default_sample_duration);
    p += 4;

    // Default sample size (0 = variável, cada sample terá tamanho diferente)
    buffer_write32(p, 0);
    p += 4;

    // Default sample flags
    // 0x00010000 = sample_depends_on=1 (depende de outro)
    // 0x01000000 = sample_is_non_sync_sample (não é keyframe por padrão)
    buffer_write32(p, 0x01010000);
    p += 4;
}

// CRIAÇÃO DE BOXES: moov > mvex. Cria box mvex (Movie Extends Box). Indica que o arquivo usa fragmentação
static void create_mvex_box(uint32_t track_id, uint64_t fragment_duration, uint32_t default_sample_duration, MediaBuffer* output)
{
    MediaBuffer mehd, trex;

    create_mehd_box(fragment_duration, &mehd);
    create_trex_box(track_id, default_sample_duration, &trex);  // ← Agora passa duração!

    output->Size = 8 + mehd.Size + trex.Size;
    output->Data = malloc(output->Size);

    uint8_t* p = output->Data;

    buffer_write32(p, output->Size);
    p += 4;

    p[0] = 'm'; p[1] = 'v'; p[2] = 'e'; p[3] = 'x';
    p += 4;

    memcpy(p, mehd.Data, mehd.Size);
    p += mehd.Size;

    memcpy(p, trex.Data, trex.Size);

    free(mehd.Data);
    free(trex.Data);
}

// CRIAÇÃO DE BOXES: moov > trak > tkhd. Cria box tkhd (Track Header Box)
static void create_tkhd_box(uint32_t track_id, uint32_t width, uint32_t height, MediaBuffer* output)
{
    output->Size = 92;
    output->Data = malloc(output->Size);

    uint8_t* p = output->Data;

    // Box size
    buffer_write32(p, 92);
    p += 4;

    // Box type: 'tkhd'
    write_fourcc(p, "tkhd");
    p += 4;

    // Version (0) + Flags (track enabled + in movie + in preview)
    buffer_write32(p, 0x00000007);
    p += 4;

    // Creation time
    buffer_write32(p, 0);
    p += 4;

    // Modification time
    buffer_write32(p, 0);
    p += 4;

    // Track ID
    buffer_write32(p, track_id);
    p += 4;

    // Reserved
    buffer_write32(p, 0);
    p += 4;

    // Duration (0 para fragmentos)
    buffer_write32(p, 0);
    p += 4;

    // Reserved
    memset(p, 0, 8);
    p += 8;

    // Layer
    buffer_write16(p, 0);
    p += 2;

    // Alternate group
    buffer_write16(p, 0);
    p += 2;

    // Volume (0 para vídeo)
    buffer_write16(p, 0);
    p += 2;

    // Reserved
    buffer_write16(p, 0);
    p += 2;

    // Matrix (identity)
    uint32_t matrix[9] = { 0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000 };

    for (int i = 0; i < 9; i++)
    {
        buffer_write32(p, matrix[i]);
        p += 4;
    }

    // Width (16.16 fixed point)
    buffer_write32(p, width << 16);
    p += 4;

    // Height (16.16 fixed point)
    buffer_write32(p, height << 16);
}

// CRIAÇÃO DE BOXES: moov > trak > mdia > mdhd. Cria box mdhd (Media Header Box)
static void create_mdhd_box(uint32_t timescale, MediaBuffer* output)
{
    output->Size = 32;
    output->Data = malloc(output->Size);

    uint8_t* p = output->Data;

    // Box size
    buffer_write32(p, 32);
    p += 4;

    // Box type: 'mdhd'
    write_fourcc(p, "mdhd");
    p += 4;

    // Version + Flags
    buffer_write32(p, 0);
    p += 4;

    // Creation time
    buffer_write32(p, 0);
    p += 4;

    // Modification time
    buffer_write32(p, 0);
    p += 4;

    // Timescale
    buffer_write32(p, timescale);
    p += 4;

    // Duration (0 para fragmentos)
    buffer_write32(p, 0);
    p += 4;

    // Language (undetermined = 0x55C4)
    buffer_write16(p, 0x55C4);
    p += 2;

    // Pre-defined
    buffer_write16(p, 0);
}

// CRIAÇÃO DE BOXES: moov > trak > mdia > hdlr. Cria box hdlr (Handler Reference Box)
static void create_hdlr_box(MediaBuffer* output)
{
    const char* handler_name = "VideoHandler";
    size_t name_len = strlen(handler_name);

    output->Size = 32 + name_len + 1;
    output->Data = malloc(output->Size);

    uint8_t* p = output->Data;

    // Box size
    buffer_write32(p, output->Size);
    p += 4;

    // Box type: 'hdlr'
    write_fourcc(p, "hdlr");
    p += 4;

    // Version + Flags
    buffer_write32(p, 0);
    p += 4;

    // Pre-defined
    buffer_write32(p, 0);
    p += 4;

    // Handler type: 'vide' (video)
    write_fourcc(p, "vide");
    p += 4;

    // Reserved
    memset(p, 0, 12);
    p += 12;

    // Handler name
    strcpy((char*)p, handler_name);
}

// CRIAÇÃO DE BOXES: moov > trak > mdia > minf > vmhd. Cria box vmhd (Video Media Header Box)
static void create_vmhd_box(MediaBuffer* output)
{
    output->Size = 20;
    output->Data = malloc(output->Size);

    uint8_t* p = output->Data;

    // Box size
    buffer_write32(p, 20);
    p += 4;

    // Box type: 'vmhd'
    write_fourcc(p, "vmhd");
    p += 4;

    // Version (0) + Flags (1)
    buffer_write32(p, 0x00000001);
    p += 4;

    // Graphics mode
    buffer_write16(p, 0);
    p += 2;

    // Opcolor (R, G, B)
    buffer_write16(p, 0);
    p += 2;
    buffer_write16(p, 0);
    p += 2;
    buffer_write16(p, 0);
}

// CRIAÇÃO DE BOXES: moov > trak > mdia > minf > dinf > dref. Cria box dref (Data Reference Box)
static void create_dref_box(MediaBuffer* output)
{
    output->Size = 28;
    output->Data = malloc(output->Size);

    uint8_t* p = output->Data;

    // Box size
    buffer_write32(p, 28);
    p += 4;

    // Box type: 'dref'
    write_fourcc(p, "dref");
    p += 4;

    // Version + Flags
    buffer_write32(p, 0);
    p += 4;

    // Entry count
    buffer_write32(p, 1);
    p += 4;

    // --- url box ---
    // Size
    buffer_write32(p, 12);
    p += 4;

    // Type: 'url '
    write_fourcc(p, "url ");
    p += 4;

    // Version + Flags (0x000001 = data is in this file)
    buffer_write32(p, 0x00000001);
}

// CRIAÇÃO DE BOXES: moov > trak > mdia > minf > dinf. Cria box dinf (Data Information Box)
static void create_dinf_box(MediaBuffer* output)
{
    MediaBuffer dref;
    create_dref_box(&dref);

    output->Size = 8 + dref.Size;
    output->Data = malloc(output->Size);

    uint8_t* p = output->Data;

    // Box size
    buffer_write32(p, output->Size);
    p += 4;

    // Box type: 'dinf'
    write_fourcc(p, "dinf");
    p += 4;

    // Copiar dref
    memcpy(p, dref.Data, dref.Size);

    free(dref.Data);
}

// CRIAÇÃO DE BOXES: avcC (AVC Configuration). Cria box avcC (AVC Decoder Configuration Record). Contém SPS e PPS
static void create_avcc_box(VideoMetadata* metadata, MediaBuffer* output)
{
    if (!metadata->Sps.Data || metadata->Sps.Size == 0)
    {
        fprintf(stderr, "ERRO: SPS não encontrado\n");
        output->Size = 0;
        output->Data = NULL;
        return;
    }

    if (!metadata->Pps.Data || metadata->Pps.Size == 0)
    {
        fprintf(stderr, "ERRO: PPS não encontrado\n");
        output->Size = 0;
        output->Data = NULL;
        return;
    }

    // Tamanho: header(8) + config(7) + sps_array(3+2+size) + pps_array(3+2+size)
    output->Size = 8 + 7 + 5 + metadata->Sps.Size + 3 + metadata->Pps.Size;
    output->Data = malloc(output->Size);

    uint8_t* p = output->Data;

    // Box size
    buffer_write32(p, output->Size);
    p += 4;

    // Box type: 'avcC'
    write_fourcc(p, "avcC");
    p += 4;

    // --- AVC Configuration ---

    // Configuration version
    *p++ = 1;

    // Profile (do SPS)
    *p++ = metadata->Sps.Data[1];

    // Profile compatibility (do SPS)
    *p++ = metadata->Sps.Data[2];

    // Level (do SPS)
    *p++ = metadata->Sps.Data[3];

    // Length size minus one (3 = 4 bytes)
    *p++ = 0xFF;  // 6 bits reserved + 2 bits length = 0b11111111

    // --- SPS Array ---

    // Number of SPS (1 SPS, com reserved bits)
    *p++ = 0xE1;  // 3 bits reserved + 5 bits count = 0b11100001

    // SPS length
    buffer_write16(p, metadata->Sps.Size);
    p += 2;

    // SPS data
    memcpy(p, metadata->Sps.Data, metadata->Sps.Size);
    p += metadata->Sps.Size;

    // --- PPS Array ---

    // Number of PPS
    *p++ = 1;

    // PPS length
    buffer_write16(p, metadata->Pps.Size);
    p += 2;

    // PPS data
    memcpy(p, metadata->Pps.Data, metadata->Pps.Size);
}

// CRIAÇÃO DE BOXES: moov > trak > mdia > minf > stbl > stsd. Cria box stsd (Sample Description Box). Contém descrição do codec (avc1 + avcC)
static void create_stsd_box(VideoMetadata* metadata, MediaBuffer* output)
{
    // Criar avcC
    MediaBuffer avcc;
    create_avcc_box(metadata, &avcc);

    if (!avcc.Data)
    {
        output->Size = 0;
        output->Data = NULL;
        return;
    }

    // stsd: 16 bytes + avc1 entry (86 bytes base + avcC)
    output->Size = 16 + 86 + avcc.Size;
    output->Data = malloc(output->Size);

    uint8_t* p = output->Data;

    // Box size
    buffer_write32(p, output->Size);
    p += 4;

    // Box type: 'stsd'
    write_fourcc(p, "stsd");
    p += 4;

    // Version + Flags
    buffer_write32(p, 0);
    p += 4;

    // Entry count
    buffer_write32(p, 1);
    p += 4;

    // --- avc1 entry ---

    // Entry size
    buffer_write32(p, 86 + avcc.Size);
    p += 4;

    // Format: 'avc1'
    write_fourcc(p, "avc1");
    p += 4;

    // Reserved (6 bytes)
    memset(p, 0, 6);
    p += 6;

    // Data reference index
    buffer_write16(p, 1);
    p += 2;

    // Pre-defined + Reserved (16 bytes)
    memset(p, 0, 16);
    p += 16;

    // Width
    buffer_write16(p, metadata->Width);
    p += 2;

    // Height
    buffer_write16(p, metadata->Height);
    p += 2;

    // Horizontal resolution (72 dpi = 0x00480000)
    buffer_write32(p, 0x00480000);
    p += 4;

    // Vertical resolution (72 dpi = 0x00480000)
    buffer_write32(p, 0x00480000);
    p += 4;

    // Reserved
    buffer_write32(p, 0);
    p += 4;

    // Frame count (1)
    buffer_write16(p, 1);
    p += 2;

    // Compressor name (32 bytes, primeiro byte = length)
    memset(p, 0, 32);
    p += 32;

    // Depth (0x0018 = 24-bit color)
    buffer_write16(p, 0x0018);
    p += 2;

    // Pre-defined
    buffer_write16(p, 0xFFFF);
    p += 2;

    // Copiar avcC
    memcpy(p, avcc.Data, avcc.Size);

    free(avcc.Data);
}

// CRIAÇÃO DE BOXES: moov > trak > mdia > minf > stbl (outros). Cria box stts (Time to Sample Box) - vazio para fragmentos
static void create_stts_box(MediaBuffer* output)
{
    output->Size = 16;
    output->Data = malloc(output->Size);

    uint8_t* p = output->Data;

    buffer_write32(p, 16);
    p += 4;
    write_fourcc(p, "stts");
    p += 4;
    buffer_write32(p, 0);  // Version + Flags
    p += 4;
    buffer_write32(p, 0);  // Entry count = 0
}

// Cria box stsc (Sample to Chunk Box) - vazio para fragmentos
static void create_stsc_box(MediaBuffer* output)
{
    output->Size = 16;
    output->Data = malloc(output->Size);

    uint8_t* p = output->Data;

    buffer_write32(p, 16);
    p += 4;
    write_fourcc(p, "stsc");
    p += 4;
    buffer_write32(p, 0);
    p += 4;
    buffer_write32(p, 0);
}

// Cria box stsz (Sample Size Box) - vazio para fragmentos
static void create_stsz_box(MediaBuffer* output)
{
    output->Size = 20;
    output->Data = malloc(output->Size);

    uint8_t* p = output->Data;

    buffer_write32(p, 20);
    p += 4;
    write_fourcc(p, "stsz");
    p += 4;
    buffer_write32(p, 0);  // Version + Flags
    p += 4;
    buffer_write32(p, 0);  // Sample size = 0 (variable)
    p += 4;
    buffer_write32(p, 0);  // Sample count = 0
}

// Cria box stco (Chunk Offset Box) - vazio para fragmentos
static void create_stco_box(MediaBuffer* output)
{
    output->Size = 16;
    output->Data = malloc(output->Size);

    uint8_t* p = output->Data;

    buffer_write32(p, 16);
    p += 4;
    write_fourcc(p, "stco");
    p += 4;
    buffer_write32(p, 0);
    p += 4;
    buffer_write32(p, 0);
}

// CRIAÇÃO DE BOXES: moov > trak > mdia > minf > stbl. Cria box stbl (Sample Table Box)
static void create_stbl_box(VideoMetadata* metadata, MediaBuffer* output)
{
    // Criar sub-boxes
    MediaBuffer stsd, stts, stsc, stsz, stco;

    create_stsd_box(metadata, &stsd);
    if (!stsd.Data)
    {
        output->Size = 0;
        output->Data = NULL;
        return;
    }

    create_stts_box(&stts);
    create_stsc_box(&stsc);
    create_stsz_box(&stsz);
    create_stco_box(&stco);

    // Calcular tamanho total
    output->Size = 8 + stsd.Size + stts.Size + stsc.Size + stsz.Size + stco.Size;
    output->Data = malloc(output->Size);

    uint8_t* p = output->Data;

    // Box size
    buffer_write32(p, output->Size);
    p += 4;

    // Box type: 'stbl'
    write_fourcc(p, "stbl");
    p += 4;

    // Copiar sub-boxes
    memcpy(p, stsd.Data, stsd.Size);
    p += stsd.Size;

    memcpy(p, stts.Data, stts.Size);
    p += stts.Size;

    memcpy(p, stsc.Data, stsc.Size);
    p += stsc.Size;

    memcpy(p, stsz.Data, stsz.Size);
    p += stsz.Size;

    memcpy(p, stco.Data, stco.Size);

    // Liberar sub-boxes
    free(stsd.Data);
    free(stts.Data);
    free(stsc.Data);
    free(stsz.Data);
    free(stco.Data);
}

// CRIAÇÃO DE BOXES: moov > trak > mdia > minf. Cria box minf (Media Information Box)
static void create_minf_box(VideoMetadata* metadata, MediaBuffer* output)
{
    // Criar sub-boxes
    MediaBuffer vmhd, dinf, stbl;

    create_vmhd_box(&vmhd);
    create_dinf_box(&dinf);
    create_stbl_box(metadata, &stbl);

    if (!stbl.Data)
    {
        output->Size = 0;
        output->Data = NULL;
        free(vmhd.Data);
        free(dinf.Data);
        return;
    }

    // Calcular tamanho total
    output->Size = 8 + vmhd.Size + dinf.Size + stbl.Size;
    output->Data = malloc(output->Size);

    uint8_t* p = output->Data;

    // Box size
    buffer_write32(p, output->Size);
    p += 4;

    // Box type: 'minf'
    write_fourcc(p, "minf");
    p += 4;

    // Copiar sub-boxes
    memcpy(p, vmhd.Data, vmhd.Size);
    p += vmhd.Size;

    memcpy(p, dinf.Data, dinf.Size);
    p += dinf.Size;

    memcpy(p, stbl.Data, stbl.Size);

    // Liberar sub-boxes
    free(vmhd.Data);
    free(dinf.Data);
    free(stbl.Data);
}

// CRIAÇÃO DE BOXES: moov > trak > mdia. Cria box mdia (Media Box)
static void create_mdia_box(VideoMetadata* metadata, uint32_t timescale, MediaBuffer* output)
{
    // Criar sub-boxes
    MediaBuffer mdhd, hdlr, minf;

    create_mdhd_box(timescale, &mdhd);
    create_hdlr_box(&hdlr);
    create_minf_box(metadata, &minf);

    if (!minf.Data)
    {
        output->Size = 0;
        output->Data = NULL;
        free(mdhd.Data);
        free(hdlr.Data);
        return;
    }

    // Calcular tamanho total
    output->Size = 8 + mdhd.Size + hdlr.Size + minf.Size;
    output->Data = malloc(output->Size);

    uint8_t* p = output->Data;

    // Box size
    buffer_write32(p, output->Size);
    p += 4;

    // Box type: 'mdia'
    write_fourcc(p, "mdia");
    p += 4;

    // Copiar sub-boxes
    memcpy(p, mdhd.Data, mdhd.Size);
    p += mdhd.Size;

    memcpy(p, hdlr.Data, hdlr.Size);
    p += hdlr.Size;

    memcpy(p, minf.Data, minf.Size);

    // Liberar sub-boxes
    free(mdhd.Data);
    free(hdlr.Data);
    free(minf.Data);
}

// CRIAÇÃO DE BOXES: moov > trak. Cria box trak (Track Box)
static void create_trak_box(VideoMetadata* metadata, uint32_t track_id, uint32_t timescale, MediaBuffer* output)
{
    // Criar sub-boxes
    MediaBuffer tkhd, mdia;

    create_tkhd_box(track_id, metadata->Width, metadata->Height, &tkhd);
    create_mdia_box(metadata, timescale, &mdia);

    if (!mdia.Data)
    {
        output->Size = 0;
        output->Data = NULL;
        free(tkhd.Data);
        return;
    }

    // Calcular tamanho total
    output->Size = 8 + tkhd.Size + mdia.Size;
    output->Data = malloc(output->Size);

    uint8_t* p = output->Data;

    // Box size
    buffer_write32(p, output->Size);
    p += 4;

    // Box type: 'trak'
    write_fourcc(p, "trak");
    p += 4;

    // Copiar sub-boxes
    memcpy(p, tkhd.Data, tkhd.Size);
    p += tkhd.Size;

    memcpy(p, mdia.Data, mdia.Size);

    // Liberar sub-boxes
    free(tkhd.Data);
    free(mdia.Data);
}

// CRIAÇÃO DE BOXES: moov. Cria box moov (Movie Box)
static void create_moov_box(VideoMetadata* metadata, uint32_t timescale, uint32_t track_id, uint64_t fragment_duration, float fps, MediaBuffer* output)
{
    MediaBuffer mvhd, trak, mvex;

    create_mvhd_box(timescale, track_id + 1, &mvhd);
    create_trak_box(metadata, track_id, timescale, &trak);

    if (!trak.Data)
    {
        output->Size = 0;
        output->Data = NULL;
        free(mvhd.Data);
        return;
    }

    // NOVO: Calcular duração padrão do sample
    uint32_t default_sample_duration = 0;
    if (fps > 0)
    {
        default_sample_duration = (uint32_t)(timescale / fps);
    }
    else
    {
        // Fallback: assume 30fps
        default_sample_duration = timescale / 30;
    }

    create_mvex_box(track_id, fragment_duration, default_sample_duration, &mvex);

    output->Size = 8 + mvhd.Size + trak.Size + mvex.Size;
    output->Data = malloc(output->Size);

    uint8_t* p = output->Data;

    buffer_write32(p, output->Size);
    p += 4;

    p[0] = 'm'; p[1] = 'o'; p[2] = 'o'; p[3] = 'v';
    p += 4;

    memcpy(p, mvhd.Data, mvhd.Size);
    p += mvhd.Size;

    memcpy(p, trak.Data, trak.Size);
    p += trak.Size;

    memcpy(p, mvex.Data, mvex.Size);

    free(mvhd.Data);
    free(trak.Data);
    free(mvex.Data);
}





// FUNÇÃO PRINCIPAL: CRIAR INITIALIZATION SEGMENT MP4. (ftyp + moov)
int mp4builder_create_init(VideoMetadata* metadata, MP4InitConfig* config, MediaBuffer* output)
{
    if (!metadata || !config || !output)
    {
        fprintf(stderr, "ERRO: Parâmetros NULL\n");
        return -1;
    }

    // Validar SPS/PPS
    if (!metadata->Sps.Data || metadata->Sps.Size == 0)
    {
        fprintf(stderr, "ERRO: SPS não encontrado\n");
        return -2;
    }

    if (!metadata->Pps.Data || metadata->Pps.Size == 0)
    {
        fprintf(stderr, "ERRO: PPS não encontrado\n");
        return -3;
    }

    // Validar configuração
    if (config->Timescale == 0)
    {
        fprintf(stderr, "ERRO: Timescale inválido\n");
        return -4;
    }

    if (config->TrackID == 0)
    {
        fprintf(stderr, "ERRO: TrackID inválido\n");
        return -5;
    }

    // ───────────────────────────────────────────────────────────────
    // CRIAR BOX ftyp
    // ───────────────────────────────────────────────────────────────

    MediaBuffer ftyp;
    create_ftyp_box(&ftyp);

    // ───────────────────────────────────────────────────────────────
    // CRIAR BOX moov
    // ───────────────────────────────────────────────────────────────

    MediaBuffer moov;
    create_moov_box(metadata, config->Timescale, config->TrackID, config->FragmentDuration, metadata->Fps, &moov);

    if (!moov.Data)
    {
        fprintf(stderr, "ERRO: Falha ao criar moov box\n");
        free(ftyp.Data);
        return -6;
    }

    // ───────────────────────────────────────────────────────────────
    // JUNTAR ftyp + moov
    // ───────────────────────────────────────────────────────────────

    output->Size = ftyp.Size + moov.Size;
    output->Data = malloc(output->Size);

    if (!output->Data)
    {
        fprintf(stderr, "ERRO: Falha ao alocar %zu bytes\n", output->Size);
        free(ftyp.Data);
        free(moov.Data);
        return -7;
    }

    memcpy(output->Data, ftyp.Data, ftyp.Size);
    memcpy(output->Data + ftyp.Size, moov.Data, moov.Size);

    /*  printf("Initialization segment criado:\n");
      printf("   Resolução: %dx%d\n", metadata->Width, metadata->Height);
      printf("   Timescale: %u\n", config->Timescale);
      printf("   Track ID: %u\n", config->TrackID);
      printf("   ftyp: %zu bytes\n", ftyp.Size);
      printf("   moov: %zu bytes\n", moov.Size);
      printf("   Total: %zu bytes\n", output->Size);*/

      // Liberar buffers temporários
    free(ftyp.Data);
    free(moov.Data);

    return 0;
}


int mp4builder_create_fragment(FILE* f, FrameIndexList* frame_list, double timeline_offset, double timeline_fragment_duration, int frame_offset, int frame_length, MP4FragmentInfo* frag_info, MediaBuffer* output)
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

    create_moof_box(frag_info->SequenceNumber, frag_info->TrackID, frag_info->BaseMediaDecodeTime, sample_count, sample_duration, &moof);

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