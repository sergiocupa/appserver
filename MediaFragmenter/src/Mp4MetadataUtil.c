#include "../include/MediaFragmenter.h"
#include "Mp4Diag.h"
#include "FileUtil.h"


// Tipos de NAL para H.265
#define HEVC_NAL_VPS 32
#define HEVC_NAL_SPS 33
#define HEVC_NAL_PPS 34


static void* memmem(const void* haystack, size_t haystacklen, const void* needle, size_t needlelen)
{
    if (!haystack || !needle || needlelen == 0 || haystacklen < needlelen) return NULL;

    const unsigned char* h = (const unsigned char*)haystack;
    const unsigned char* n = (const unsigned char*)needle;
    size_t i;
    for (i = 0; i <= haystacklen - needlelen; i++)
    {
        if (memcmp(h + i, n, needlelen) == 0)
            return (void*)(h + i);
    }
    return NULL;
}


static int load_vps_sps_pps_hevc(FILE* f, uint8_t** vps, int* vps_len, uint8_t** sps, int* sps_len, uint8_t** pps, int* pps_len, int* length_size)
{
    uint8_t name[5] = { 0 };
    long file_size;

    // Inicializa os ponteiros
    *vps = NULL;
    *sps = NULL;
    *pps = NULL;
    *vps_len = 0;
    *sps_len = 0;
    *pps_len = 0;
    *length_size = 0;

    // Obter tamanho do arquivo
    fseek(f, 0, SEEK_END);
    file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    while (ftell(f) < file_size)
    {
        long box_start = ftell(f);
        uint32_t size = read32(f);
        if (fread(name, 1, 4, f) != 4) break;

        uint64_t box_size = size;
        if (size == 1) {
            box_size = read64(f);
        }
        else if (size == 0) {
            box_size = file_size - box_start;
        }

        if (box_size < 8) {
            fseek(f, box_start + 8, SEEK_SET);
            continue;
        }

        uint64_t next = box_start + box_size;

        // Procurar recursivamente em containers
        if (!memcmp(name, "moov", 4) || !memcmp(name, "trak", 4) ||
            !memcmp(name, "mdia", 4) || !memcmp(name, "minf", 4) ||
            !memcmp(name, "stbl", 4))
        {
            continue;
        }

        // Encontrar stsd
        if (!memcmp(name, "stsd", 4))
        {
            uint8_t version = fgetc(f);
            fseek(f, 3, SEEK_CUR); // flags
            uint32_t entry_count = read32(f);

            for (uint32_t i = 0; i < entry_count; i++)
            {
                long entry_start = ftell(f);
                uint32_t entry_size = read32(f);

                if (entry_size < 8) break;

                uint8_t codec[5] = { 0 };
                if (fread(codec, 1, 4, f) != 4) break;

                // Verificar se é H.265/HEVC
                if (!memcmp(codec, "hvc1", 4) || !memcmp(codec, "hev1", 4))
                {
                    // Pular campos fixos do hvc1/hev1 até achar hvcC
                    // VisualSampleEntry tem 78 bytes de header antes dos sub-boxes
                    fseek(f, entry_start + 8 + 78, SEEK_SET);

                    // Procurar hvcC dentro do entry
                    while (ftell(f) < entry_start + entry_size - 8)
                    {
                        long sub_start = ftell(f);
                        uint32_t sub_size = read32(f);

                        if (sub_size < 8 || sub_size > entry_size) break;

                        uint8_t sub_name[5] = { 0 };
                        if (fread(sub_name, 1, 4, f) != 4) break;

                        if (!memcmp(sub_name, "hvcC", 4))
                        {
                            int hvcc_size = sub_size - 8;
                            if (hvcc_size > 4096 || hvcc_size < 23) {
                                fseek(f, sub_start + sub_size, SEEK_SET);
                                continue;
                            }

                            uint8_t* conf = malloc(hvcc_size);
                            if (!conf) break;

                            if (fread(conf, 1, hvcc_size, f) != hvcc_size) {
                                free(conf);
                                break;
                            }

                            // Parsear hvcC
                            // conf[0] = configurationVersion (deve ser 1)
                            if (conf[0] != 1) {
                                free(conf);
                                fseek(f, sub_start + sub_size, SEEK_SET);
                                continue;
                            }

                            // Offset 21: constantFrameRate(2) + numTemporalLayers(3) + 
                            //            temporalIdNested(1) + lengthSizeMinusOne(2)
                            *length_size = (conf[21] & 0x03) + 1;

                            // Offset 22: numOfArrays
                            int num_arrays = conf[22];
                            int off = 23;

                            // Parsear cada array de NALs
                            for (int arr = 0; arr < num_arrays && off < hvcc_size; arr++)
                            {
                                if (off >= hvcc_size) break;

                                // array_completeness(1) + reserved(1) + NAL_unit_type(6)
                                uint8_t nal_type = conf[off++] & 0x3F;

                                if (off + 2 > hvcc_size) break;

                                // numNalus (2 bytes, big-endian)
                                int num_nalus = (conf[off] << 8) | conf[off + 1];
                                off += 2;

                                for (int n = 0; n < num_nalus && off < hvcc_size; n++)
                                {
                                    if (off + 2 > hvcc_size) break;

                                    // nalUnitLength (2 bytes, big-endian)
                                    int nal_len = (conf[off] << 8) | conf[off + 1];
                                    off += 2;

                                    if (off + nal_len > hvcc_size) break;

                                    // Copiar NAL baseado no tipo
                                    uint8_t** dest = NULL;
                                    int* dest_len = NULL;

                                    if (nal_type == HEVC_NAL_VPS) {
                                        dest = vps;
                                        dest_len = vps_len;
                                    }
                                    else if (nal_type == HEVC_NAL_SPS) {
                                        dest = sps;
                                        dest_len = sps_len;
                                    }
                                    else if (nal_type == HEVC_NAL_PPS) {
                                        dest = pps;
                                        dest_len = pps_len;
                                    }

                                    // Apenas copia o primeiro NAL de cada tipo
                                    if (dest && !*dest && nal_len > 0)
                                    {
                                        *dest = malloc(nal_len);
                                        if (*dest)
                                        {
                                            memcpy(*dest, &conf[off], nal_len);
                                            *dest_len = nal_len;
                                        }
                                    }

                                    off += nal_len;
                                }
                            }

                            free(conf);

                            // Verificar se encontramos pelo menos SPS e PPS
                            if (*sps && *sps_len > 0 && *pps && *pps_len > 0)
                            {
                                return 0;  // Sucesso
                            }

                            // Limpar em caso de falha parcial
                            if (*vps) { free(*vps); *vps = NULL; *vps_len = 0; }
                            if (*sps) { free(*sps); *sps = NULL; *sps_len = 0; }
                            if (*pps) { free(*pps); *pps = NULL; *pps_len = 0; }

                            return -3;  // hvcC incompleto
                        }

                        fseek(f, sub_start + sub_size, SEEK_SET);
                    }
                }

                fseek(f, entry_start + entry_size, SEEK_SET);
            }
        }

        if (next > file_size) break;
        fseek(f, next, SEEK_SET);
    }

    return -1;  // hvcC não encontrado
}


static int read_box_header(FILE* f, uint32_t* size, char type[5], uint64_t* payload_start)
{
    long pos = ftell(f);
    if (pos < 0) return -1;

    *size = read32(f);
    if (fread(type, 1, 4, f) != 4) return -1;
    type[4] = 0;

    uint64_t box_size = *size;

    if (*size == 1)
        box_size = read64(f);

    if (box_size < 8) return -1;

    *payload_start = pos + (box_size == *size ? 8 : 16);
    return 0;
}


static int load_codec_type(FILE* f)
{
    int codec_out = 0;

    long file_size;
    fseek(f, 0, SEEK_END);
    file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char type[5] = { 0 };
    uint32_t size;
    uint64_t payload_start;

    while (ftell(f) < file_size)
    {
        long box_start = ftell(f);
        if (read_box_header(f, &size, type, &payload_start) < 0) break;

        uint64_t box_size = size;
        if (size == 1) {
            box_size = payload_start - box_start;
        }
        else if (size == 0) {
            box_size = file_size - box_start;
        }

        if (box_size < 8) {
            fseek(f, box_start + 8, SEEK_SET);
            continue;
        }

        uint64_t next = box_start + box_size;

        // Containers - continuar navegando dentro
        if (!memcmp(type, "moov", 4) || !memcmp(type, "trak", 4) ||
            !memcmp(type, "mdia", 4) || !memcmp(type, "minf", 4) ||
            !memcmp(type, "stbl", 4))
        {
            continue;
        }

        if (!memcmp(type, "stsd", 4))
        {
            fseek(f, payload_start, SEEK_SET);
            uint8_t version = read8(f);
            fseek(f, 3, SEEK_CUR); // flags

            uint32_t entry_count = read32(f);

            if (entry_count > 0 && entry_count < 256)
            {
                for (uint32_t i = 0; i < entry_count; i++)
                {
                    long entry_start = ftell(f);

                    // Verificar bounds
                    if (entry_start + 8 > next) break;

                    uint32_t entry_size = read32(f);

                    if (entry_size < 8 || entry_start + entry_size > next) {
                        break;
                    }

                    char codec[5] = { 0 };
                    if (fread(codec, 1, 4, f) != 4) break;

                    // H.264
                    if (!memcmp(codec, "avc1", 4) || !memcmp(codec, "avc3", 4))
                    {
                        codec_out = 264;
                        return codec_out; // Encontrou, pode retornar
                    }
                    // H.265
                    else if (!memcmp(codec, "hvc1", 4) || !memcmp(codec, "hev1", 4))
                    {
                        codec_out = 265;
                        return codec_out; // Encontrou, pode retornar
                    }

                    fseek(f, entry_start + entry_size, SEEK_SET);
                }
            }
        }

        if (next > file_size) break;
        fseek(f, next, SEEK_SET);
    }

    return codec_out;
}


static int load_width_height(FILE* f, int* width_out, int* height_out)
{
    *width_out = *height_out = 0;

    long file_size;
    fseek(f, 0, SEEK_END);
    file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char type[5] = { 0 };
    uint32_t size;
    uint64_t payload_start;

    while (ftell(f) < file_size)
    {
        long box_start = ftell(f);
        if (read_box_header(f, &size, type, &payload_start) < 0) break;

        uint64_t box_size = size;
        if (size == 1) {
            box_size = payload_start - box_start;
        }
        else if (size == 0) {
            box_size = file_size - box_start;
        }

        if (box_size < 8) {
            fseek(f, box_start + 8, SEEK_SET);
            continue;
        }

        uint64_t next = box_start + box_size;

        // Containers - continuar navegando dentro
        if (!memcmp(type, "moov", 4) || !memcmp(type, "trak", 4) ||
            !memcmp(type, "mdia", 4) || !memcmp(type, "minf", 4) ||
            !memcmp(type, "stbl", 4))
        {
            continue;
        }

        if (!memcmp(type, "stsd", 4))
        {
            fseek(f, payload_start, SEEK_SET);
            uint8_t version = read8(f);
            fseek(f, 3, SEEK_CUR); // flags

            uint32_t entry_count = read32(f);

            if (entry_count > 0 && entry_count < 256)
            {
                for (uint32_t i = 0; i < entry_count; i++)
                {
                    long entry_start = ftell(f);

                    // Verificar bounds
                    if (entry_start + 8 > next) break;

                    uint32_t entry_size = read32(f);

                    if (entry_size < 8 || entry_start + entry_size > next) {
                        break;
                    }

                    char codec[5] = { 0 };
                    if (fread(codec, 1, 4, f) != 4) break;

                    // H.264 ou H.265
                    if (!memcmp(codec, "avc1", 4) || !memcmp(codec, "avc3", 4) ||
                        !memcmp(codec, "hvc1", 4) || !memcmp(codec, "hev1", 4))
                    {
                        // Estrutura VisualSampleEntry:
                        // 4 bytes: size (já lido)
                        // 4 bytes: codec (já lido)
                        // 6 bytes: reserved (0)
                        // 2 bytes: data_reference_index
                        // 2 bytes: pre_defined (0)
                        // 2 bytes: reserved (0)
                        // 12 bytes: pre_defined[3] (0)
                        // 2 bytes: width
                        // 2 bytes: height

                        // Total offset = 8 + 6 + 2 + 2 + 2 + 12 = 32 bytes do início da entry
                        fseek(f, entry_start + 32, SEEK_SET);

                        // Verificar se ainda estamos dentro do entry
                        if (ftell(f) + 4 > entry_start + entry_size) {
                            fseek(f, entry_start + entry_size, SEEK_SET);
                            continue;
                        }

                        *width_out = read16(f);
                        *height_out = read16(f);

                        // Validar dimensões razoáveis
                        if (*width_out > 0 && *width_out < 16384 &&
                            *height_out > 0 && *height_out < 16384)
                        {
                            return 0;
                        }
                    }

                    fseek(f, entry_start + entry_size, SEEK_SET);
                }
            }
        }

        if (next > file_size) break;
        fseek(f, next, SEEK_SET);
    }

    return -1;
}


static int load_timescale_and_fps(FILE* f, uint32_t* timescale_out, double* fps_out)
{
    *timescale_out = 0;
    *fps_out = 0.0;

    long file_size;
    fseek(f, 0, SEEK_END);
    file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char type[5] = { 0 };
    uint32_t size;
    uint64_t payload_start;
    uint32_t timescale = 0;
    double total_ticks = 0;
    uint32_t total_samples = 0;
    int found_timescale = 0;
    int found_stts = 0;

    while (ftell(f) < file_size)
    {
        long box_start = ftell(f);
        if (read_box_header(f, &size, type, &payload_start) < 0) break;

        uint64_t box_size = size;
        if (size == 1) {
            // Size estendido já foi tratado em read_box_header
            box_size = payload_start - box_start;
        }
        else if (size == 0) {
            box_size = file_size - box_start;
        }

        if (box_size < 8) {
            fseek(f, box_start + 8, SEEK_SET);
            continue;
        }

        uint64_t next = box_start + box_size;

        // Containers - continuar navegando dentro
        if (!memcmp(type, "moov", 4) || !memcmp(type, "trak", 4) ||
            !memcmp(type, "mdia", 4) || !memcmp(type, "minf", 4) ||
            !memcmp(type, "stbl", 4))
        {
            continue;
        }

        /* mdhd -> timescale */
        if (!memcmp(type, "mdhd", 4))
        {
            fseek(f, payload_start, SEEK_SET);
            uint8_t version = read8(f);
            fseek(f, 3, SEEK_CUR); // flags

            if (version == 1)
            {
                fseek(f, 16, SEEK_CUR); // creation + modification time (64-bit cada)
                timescale = read32(f);
            }
            else // version 0
            {
                fseek(f, 8, SEEK_CUR); // creation + modification time (32-bit cada)
                timescale = read32(f);
            }

            found_timescale = 1;

            // Se já temos stts, podemos retornar
            if (found_stts && timescale > 0 && total_samples > 0)
            {
                *timescale_out = timescale;
                *fps_out = (double)total_samples / (total_ticks / timescale);
                return 0;
            }
        }

        /* stts -> sample timing */
        if (!memcmp(type, "stts", 4))
        {
            fseek(f, payload_start, SEEK_SET);
            uint8_t version = read8(f);
            fseek(f, 3, SEEK_CUR); // flags

            uint32_t entry_count = read32(f);

            // Verificar se temos dados suficientes
            if (entry_count > 0 && entry_count < 100000)
            {
                for (uint32_t i = 0; i < entry_count; i++)
                {
                    // Verificar se não vamos além do box
                    if (ftell(f) + 8 > next) break;

                    uint32_t sample_count = read32(f);
                    uint32_t sample_delta = read32(f);

                    total_samples += sample_count;
                    total_ticks += (double)sample_count * (double)sample_delta;
                }
            }

            found_stts = 1;

            // Se já temos timescale, podemos retornar
            if (found_timescale && timescale > 0 && total_samples > 0 && total_ticks > 0)
            {
                *timescale_out = timescale;
                *fps_out = (double)total_samples / (total_ticks / timescale);
                return 0;
            }
        }

        if (next > file_size) break;
        fseek(f, next, SEEK_SET);
    }

    // Verificação final
    if (timescale == 0 || total_ticks == 0.0 || total_samples == 0)
        return -1;

    *timescale_out = timescale;
    *fps_out = (double)total_samples / (total_ticks / (double)timescale);
    return 0;
}


static int load_sps_pps(FILE* f, uint8_t** sps, int* sps_len, uint8_t** pps, int* pps_len, int* length_size)
{
    uint8_t name[5] = { 0 };
    long file_size;

    // Inicializa os ponteiros
    *sps = NULL;
    *pps = NULL;
    *sps_len = 0;
    *pps_len = 0;
    *length_size = 0;

    // Obter tamanho do arquivo
    fseek(f, 0, SEEK_END);
    file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    while (ftell(f) < file_size)
    {
        long box_start = ftell(f);
        uint32_t size = read32(f);
        if (fread(name, 1, 4, f) != 4) break;

        uint64_t box_size = size;
        if (size == 1) {
            box_size = read64(f);
        }
        else if (size == 0) {
            box_size = file_size - box_start;
        }

        if (box_size < 8) {
            fseek(f, box_start + 8, SEEK_SET);
            continue;
        }

        uint64_t next = box_start + box_size;

        // Procurar recursivamente em containers
        if (!memcmp(name, "moov", 4) || !memcmp(name, "trak", 4) ||
            !memcmp(name, "mdia", 4) || !memcmp(name, "minf", 4) ||
            !memcmp(name, "stbl", 4))
        {
            continue;
        }

        // Encontrar stsd
        if (!memcmp(name, "stsd", 4))
        {
            uint8_t version = fgetc(f);
            fseek(f, 3, SEEK_CUR); // flags
            uint32_t entry_count = read32(f);

            for (uint32_t i = 0; i < entry_count; i++)
            {
                long entry_start = ftell(f);
                uint32_t entry_size = read32(f);

                if (entry_size < 8) break;

                uint8_t codec[5] = { 0 };
                if (fread(codec, 1, 4, f) != 4) break;

                if (!memcmp(codec, "avc1", 4) || !memcmp(codec, "avc3", 4))
                {
                    // Pular campos fixos do avc1 até achar avcC
                    fseek(f, entry_start + 8 + 78, SEEK_SET);

                    // Procurar avcC dentro do avc1
                    while (ftell(f) < entry_start + entry_size - 8)
                    {
                        long sub_start = ftell(f);
                        uint32_t sub_size = read32(f);

                        if (sub_size < 8 || sub_size > entry_size) break;

                        uint8_t sub_name[5] = { 0 };
                        if (fread(sub_name, 1, 4, f) != 4) break;

                        if (!memcmp(sub_name, "avcC", 4))
                        {
                            int avcc_size = sub_size - 8;
                            if (avcc_size > 1024 || avcc_size < 7) {
                                fseek(f, sub_start + sub_size, SEEK_SET);
                                continue;
                            }

                            uint8_t conf[1024];
                            if (fread(conf, 1, avcc_size, f) != avcc_size) break;

                            // conf[0] = configurationVersion (deve ser 1)
                            if (conf[0] != 1) {
                                fseek(f, sub_start + sub_size, SEEK_SET);
                                continue;
                            }

                            // conf[4] bits 0-1 = lengthSizeMinusOne
                            *length_size = (conf[4] & 3) + 1;

                            // conf[5] bits 0-4 = numOfSequenceParameterSets
                            int off = 5;
                            int sps_count = conf[off++] & 0x1F;

                            if (sps_count > 0)
                            {
                                *sps_len = (conf[off] << 8) | conf[off + 1];
                                off += 2;

                                if (*sps_len > 0 && *sps_len < avcc_size - off)
                                {
                                    *sps = malloc(*sps_len);
                                    memcpy(*sps, &conf[off], *sps_len);
                                    off += *sps_len;
                                }
                                else
                                {
                                    *sps_len = 0;
                                }
                            }

                            // numOfPictureParameterSets
                            if (off < avcc_size)
                            {
                                int pps_count = conf[off++];
                                if (pps_count > 0 && off + 2 <= avcc_size)
                                {
                                    *pps_len = (conf[off] << 8) | conf[off + 1];
                                    off += 2;

                                    if (*pps_len > 0 && *pps_len <= avcc_size - off)
                                    {
                                        *pps = malloc(*pps_len);
                                        if (*pps == NULL)
                                        {
                                            if (*sps) free(*sps);
                                            *sps = NULL;
                                            *sps_len = 0;
                                            return -2;
                                        }

                                        memcpy(*pps, &conf[off], *pps_len);
                                    }
                                    else
                                    {
                                        *pps_len = 0;
                                    }
                                }
                            }

                            if (*sps && *sps_len > 0 && *pps && *pps_len > 0)
                            {
                                return 0;  // Sucesso
                            }

                            // Se chegou aqui, algo deu errado
                            if (*sps)
                            {
                                free(*sps);
                                *sps = NULL;
                            }
                            if (*pps)
                            {
                                free(*pps);
                                *pps = NULL;
                            }
                            return -3;
                        }

                        fseek(f, sub_start + sub_size, SEEK_SET);
                    }
                }

                fseek(f, entry_start + entry_size, SEEK_SET);
            }
        }

        if (next > file_size) break;
        fseek(f, next, SEEK_SET);
    }

    return -1;
}


int mp4meta_load_video_metadata(FILE* f, VideoMetadata* meta)
{
    if (!f || !meta) return -1;

    memset(meta, 0, sizeof(*meta));

    fseek(f, 0, SEEK_SET);
    int codec = load_codec_type(f);
    if (codec == 0)
    {
        printf("Invalid codec.\n");
        return -1;
    }

    fseek(f, 0, SEEK_SET);
    int w = 0, h = 0;
    int ret = load_width_height(f, &w, &h);
    if (ret != 0)
    {
        printf("Width or height not found.\n");
        return -2;
    }

    fseek(f, 0, SEEK_SET);
    uint32_t timescale = 0; double fps_out = 0;
    ret = load_timescale_and_fps(f, &timescale, &fps_out);
    if (ret != 0)
    {
        printf("Width or height not found.\n");
        return -3;
    }

    fseek(f, 0, SEEK_SET);


    if (codec == 264)
    {
        // H.264: Carregar SPS/PPS do avcC
        uint8_t* sps = NULL;
        uint8_t* pps = NULL;
        int sps_len = 0, pps_len = 0, length_size = 0;

        ret = load_sps_pps(f, &sps, &sps_len, &pps, &pps_len, &length_size);
        if (ret != 0)
        {
            printf("SPS/PPS not found for H.264.\n");
            return ret;
        }

        meta->LengthSize = length_size;
        meta->Sps.Size = sps_len;
        meta->Sps.Data = sps;
        meta->Pps.Size = pps_len;
        meta->Pps.Data = pps;
        meta->Vps.Size = 0;
        meta->Vps.Data = NULL;

        // Validar SPS/PPS para H.264
        ret = mp4diag_validate_sps_pps(&meta->Sps, &meta->Pps);
        if (ret != 0)
        {
            return ret;
        }
    }
    else if (codec == 265)
    {
        // H.265: Carregar VPS/SPS/PPS do hvcC
        uint8_t* vps = NULL;
        uint8_t* sps = NULL;
        uint8_t* pps = NULL;
        int vps_len = 0, sps_len = 0, pps_len = 0, length_size = 0;

        ret = load_vps_sps_pps_hevc(f, &vps, &vps_len, &sps, &sps_len, &pps, &pps_len, &length_size);
        if (ret != 0)
        {
            printf("VPS/SPS/PPS not found for H.265.\n");
            return ret;
        }

        meta->LengthSize = length_size;
        meta->Vps.Size = vps_len;
        meta->Vps.Data = vps;
        meta->Sps.Size = sps_len;
        meta->Sps.Data = sps;
        meta->Pps.Size = pps_len;
        meta->Pps.Data = pps;

        // Nota: Criar mp4diag_validate_vps_sps_pps() se necessário
        // Por enquanto, apenas verificar se existem
        if (!meta->Sps.Data || meta->Sps.Size == 0 ||
            !meta->Pps.Data || meta->Pps.Size == 0)
        {
            printf("H.265 SPS or PPS is empty.\n");
            return -4;
        }
    }
    else
    {
        printf("Unsupported codec: %d\n", codec);
        return -5;
    }


    meta->Codec      = codec;
    meta->Fps        = fps_out;
    meta->Timescale  = timescale;
    meta->Width      = w;
    meta->Height     = h;

    /*ret = mp4diag_validate_sps_pps(&meta->Sps, &meta->Pps);
    if (ret != 0)
    {
        return ret;
    }*/

    return 0; // avcC não encontrado
}


