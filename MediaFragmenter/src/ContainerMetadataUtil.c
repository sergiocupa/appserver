#include "../include/MediaFragmenter.h"
#include "FileUtil.h"


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


static int find_codec_in_box(FILE* f, uint64_t end, int is_video_track)
{
    uint8_t name[5] = { 0 };

    while ((uint64_t)ftell(f) < end)
    {
        long box_start = ftell(f);
        uint32_t size = read32(f);
        if (fread(name, 1, 4, f) != 4)
            break;

        uint64_t box_size = size;
        uint64_t header_size = 8;
        if (size == 1)
        {
            box_size = read64(f);
            header_size = 16;
        }
        else if (size == 0)
        {
            fseek(f, 0, SEEK_END);
            box_size = ftell(f) - box_start;
            fseek(f, box_start + 8, SEEK_SET);
        }

        if ((size == 1 && box_size < 16) || (size != 1 && size != 0 && box_size < 8))
            break;

        uint64_t next = box_start + box_size;

        // Detecta codecs diretamente se for avcC/hvcC
        if (!memcmp(name, "avcC", 4)) return 264;
        if (!memcmp(name, "hvcC", 4)) return 265;

        // Parsing para hdlr: atualiza flag se for vídeo
        if (!memcmp(name, "hdlr", 4)) {
            fseek(f, 8, SEEK_CUR);  // Pula version/flags + pre_defined
            uint8_t handler_type[5] = { 0 };
            if (fread(handler_type, 1, 4, f) == 4) {
                if (!memcmp(handler_type, "vide", 4)) {
                    is_video_track = 1;
                }
            }
            fseek(f, next, SEEK_SET);
            continue;
        }

        // Parsing para stsd: lê entradas e verifica fourcc se for vídeo
        if (!memcmp(name, "stsd", 4)) {
            uint8_t header[8];
            if (fread(header, 1, 8, f) != 8) break;
            // entry_count dos últimos 4 bytes
            uint32_t entry_count = (header[4] << 24) | (header[5] << 16) | (header[6] << 8) | header[7];
            for (uint32_t i = 0; i < entry_count; i++) {
                uint32_t entry_size = read32(f);
                if (entry_size < 8) break;
                uint8_t entry_name[5] = { 0 };
                if (fread(entry_name, 1, 4, f) != 4) break;

                // Verifica fourcc se track de vídeo
                if (is_video_track) {
                    if (!memcmp(entry_name, "avc1", 4) || !memcmp(entry_name, "avc3", 4)) return 264;
                    if (!memcmp(entry_name, "hev1", 4) || !memcmp(entry_name, "hvc1", 4) || !memcmp(entry_name, "dvhe", 4)) return 265;
                }

                // Pula resto da entrada
                if (fseek(f, entry_size - 8, SEEK_CUR) != 0) break;
            }
            fseek(f, next, SEEK_SET);
            continue;
        }

        // Recursa para containers, passando o is_video_track atualizado
        if (!memcmp(name, "moov", 4) || !memcmp(name, "trak", 4) ||
            !memcmp(name, "mdia", 4) || !memcmp(name, "minf", 4) ||
            !memcmp(name, "stbl", 4) || !memcmp(name, "moof", 4))
        {
            int found = find_codec_in_box(f, next, is_video_track);
            if (found) return found;
        }
        fseek(f, next, SEEK_SET);
    }
    return 0;
}


static int codec_version_file(FILE* f)
{
    uint8_t buf[8];
    if (fread(buf, 1, 8, f) != 8) return 0;
    rewind(f);
    // ---- MP4 / MOV ----
    if (memcmp(buf + 4, "ftyp", 4) == 0)
    {
        fseek(f, 0, SEEK_END);
        uint64_t end = ftell(f);
        rewind(f);
        return find_codec_in_box(f, end, 0);  // Inicia com flag 0
    }
    // ---- MKV ----
    fseek(f, 0, SEEK_SET);
    char data[16384];
    size_t len = fread(data, 1, sizeof(data), f);
    if (memmem(data, len, "V_MPEG4/ISO/AVC", 15)) return 264;
    if (memmem(data, len, "V_MPEGH/ISO/HEVC", 16)) return 265;
    return 0;
}


int concod_codec_version(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) return -1;

    int hdot = codec_version_file(f);

    fclose(f);
    return hdot;
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

    fseek(f, 0, SEEK_SET);

    char type[5];
    uint32_t size;
    uint64_t payload;

    while (!feof(f))
    {
        long box_start = ftell(f);

        uint64_t payload_start = 0;
        if (read_box_header(f, &size, type, &payload_start) < 0) break;
        uint64_t next = box_start + size;

        if (!memcmp(type, "stsd", 4))
        {
            fseek(f, payload_start + 4, SEEK_SET); // skip version/flags + entry_count
            uint32_t entry_count = read32(f);

            for (uint32_t i = 0; i < entry_count; i++)
            {
                long entry_start = ftell(f);
                uint32_t entry_size = read32(f);

                char codec[5];
                fread(codec, 1, 4, f);
                codec[4] = 0;

                if (!memcmp(codec, "avc1", 4) || !memcmp(codec, "avc3", 4))
                    codec_out = 264;

                else if (!memcmp(codec, "hvc1", 4) || !memcmp(codec, "hev1", 4))
                    codec_out = 265;

                fseek(f, entry_start + entry_size, SEEK_SET);
            }
        }

        fseek(f, next, SEEK_SET);
    }

    return codec_out;
}


static int load_width_height(FILE* f, int* width_out, int* height_out)
{
    *width_out = *height_out = 0;

    fseek(f, 0, SEEK_SET);

    char type[5];
    uint32_t size;
    uint64_t payload_start;

    while (!feof(f))
    {
        long box_start = ftell(f);
        if (read_box_header(f, &size, type, &payload_start) < 0) break;
        uint64_t next = box_start + size;

        if (!memcmp(type, "stsd", 4))
        {
            fseek(f, payload_start, SEEK_SET);
            fseek(f, 4, SEEK_CUR); // version/flags
            uint32_t entry_count = read32(f);

            for (uint32_t i = 0; i < entry_count; i++)
            {
                long entry_start = ftell(f);
                uint32_t entry_size = read32(f);

                char codec[5];
                fread(codec, 1, 4, f);
                codec[4] = 0;

                if (!memcmp(codec, "avc1", 4) || !memcmp(codec, "avc3", 4))
                {
                    // width e height ficam em offset fixo dentro de avc1
                    fseek(f, entry_start + 8 + 6 + 2 + 16, SEEK_SET);
                    *width_out  = read16(f);
                    *height_out = read16(f);
                    return 0;
                }

                fseek(f, entry_start + entry_size, SEEK_SET);
            }
        }

        fseek(f, next, SEEK_SET);
    }

    return -1;
}


static int load_timescale_and_fps(FILE* f, uint32_t* timescale_out, double* fps_out)
{
    *timescale_out = 0;
    *fps_out = 0.0;

    fseek(f, 0, SEEK_SET);

    char type[5];
    uint32_t size;
    uint64_t payload_start;

    uint32_t timescale = 0;
    double total_ticks = 0;
    uint32_t total_samples = 0;

    while (!feof(f))
    {
        long box_start = ftell(f);
        if (read_box_header(f, &size, type, &payload_start) < 0) break;
        uint64_t next = box_start + size;

        /* mdhd -> timescale */
        if (!memcmp(type, "mdhd", 4))
        {
            fseek(f, payload_start, SEEK_SET);
            uint8_t version = read8(f);

            if (version == 1)
            {
                fseek(f, 8, SEEK_CUR); // creation + modification time (64-bit)
                timescale = read32(f);
            }
            else
            {
                fseek(f, 8, SEEK_CUR); // creation + modification time (32-bit)
                timescale = read32(f);
            }
        }

        /* stts -> sample timing */
        if (!memcmp(type, "stts", 4))
        {
            fseek(f, payload_start + 4, SEEK_SET); // skip version/flags
            uint32_t count = read32(f);

            for (uint32_t i = 0; i < count; i++)
            {
                uint32_t sample_count = read32(f);
                uint32_t delta = read32(f);
                total_samples += sample_count;
                total_ticks += (double)sample_count * (double)delta;
            }
        }

        fseek(f, next, SEEK_SET);
    }

    if (timescale == 0 || total_ticks == 0 || total_samples == 0)
        return -1;

    *timescale_out = timescale;
    *fps_out = (double)total_samples / (total_ticks / timescale);

    return 0;
}



static int load_sps_pps(FILE* f, uint8_t** sps, int* sps_len, uint8_t** pps, int* pps_len, int* length_size)
{
    uint8_t name[5] = { 0 };

    while (!feof(f))
    {
        long box_start = ftell(f);
        uint32_t size = read32(f);
        if (fread(name, 1, 4, f) != 4) break;

        uint64_t box_size = size;
        if (size == 1) box_size = read64(f);
        if (box_size < 8) return -5;

        uint64_t next = box_start + box_size;

        // Encontrar stsd
        if (!memcmp(name, "stsd", 4))
        {
            fseek(f, 4, SEEK_CUR); // version/flags
            uint32_t entry_count = read32(f);

            for (uint32_t i = 0; i < entry_count; i++)
            {
                long entry_start = ftell(f);
                uint32_t entry_size = read32(f);

                uint8_t codec[5] = { 0 };
                fread(codec, 1, 4, f);

                if (!memcmp(codec, "avc1", 4) || !memcmp(codec, "avc3", 4))
                {
                    // pular campos fixos do avc1 até achar avcC
                    fseek(f, entry_start + 86, SEEK_SET);

                    while (ftell(f) < entry_start + entry_size)
                    {
                        long sub_start = ftell(f);
                        uint32_t sub_size = read32(f);
                        uint8_t sub_name[5] = { 0 };
                        fread(sub_name, 1, 4, f);

                        if (!memcmp(sub_name, "avcC", 4))
                        {
                            uint8_t conf[1024];
                            fread(conf, 1, sub_size - 8, f);

                            *length_size = (conf[4] & 3) + 1;

                            int off = 5;
                            int sps_count = conf[off++] & 0x1F;

                            if (sps_count > 0)
                            {
                                *sps_len = (conf[off] << 8) | conf[off + 1];
                                off += 2;
                                (*sps) = malloc(sps_len);
                                memcpy((*sps), &conf[off], sps_len);
                                off += *sps_len;
                            }

                            int pps_count = conf[off++];
                            if (pps_count > 0)
                            {
                                *pps_len = (conf[off] << 8) | conf[off + 1];
                                off += 2;
                                (*pps) = malloc(pps_len);
                                memcpy((*pps), &conf[off], pps_len);
                            }

                            return 0;
                        }

                        fseek(f, sub_start + sub_size, SEEK_SET);
                    }
                }

                fseek(f, entry_start + entry_size, SEEK_SET);
            }
        }

        fseek(f, next, SEEK_SET);
    }
    return -1;
}


// nenhuma funcao interna encontrou metadados
...
int concod_load_video_metadata(FILE* f, VideoMetadata* meta)
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
    uint8_t* sps, pps; int sps_len = 0, pps_len = 0, length_size = 0;
    ret = load_sps_pps(f, &sps, &sps_len, &pps, &pps_len, &length_size);
    if (ret != 0)
    {
        printf("SPS/PPS not found.\n");
        return -4;
    }

    meta->LengthSize = length_size;
    meta->sps = sps;
    meta->sps_len = sps_len;
    meta->pps = pps;
    meta->pps_len = pps_len;
    meta->Codec = codec;
    meta->Fps = fps_out;
    meta->width = w;
    meta->height = h;


    /*int codec = codec_version_file(f);
    if (codec != 264)
    {
        printf("Codec não é H.264.\n");
        return -11;
    }*/
    return 0; // avcC não encontrado
}



int _01_concod_load_video_metadata(FILE* f, VideoMetadata* meta)
{
    // Inicialize meta
    meta->sps = NULL;
    meta->pps = NULL;
    meta->sps_len = 0;
    meta->pps_len = 0;

    int length_size = 0; // Declarado aqui para ser usado em mdat se necessário

    // Reset para início (conforme sua correção)
    fseek(f, 0, SEEK_SET);

    // Procurar o marcador 'avcC' no arquivo
    uint8_t buffer[1024];
    size_t read;
    long pos = 0;
    int found = 0;
    while ((read = fread(buffer, 1, sizeof(buffer), f)) > 0) 
    {
        for (size_t i = 0; i + 3 < read; i++) {
            if (buffer[i] == 'a' && buffer[i + 1] == 'v' && buffer[i + 2] == 'c' && buffer[i + 3] == 'C') 
            {
                found = 1;
                long type_pos = pos + i;
                // Volta para ler o tamanho do box (4 bytes antes do type)
                fseek(f, type_pos - 4, SEEK_SET);
                uint32_t box_size = read32(f);
                uint64_t avcc_size;
                if (box_size == 1) { // largesize
                    avcc_size = read64(f);
                    if (avcc_size < 16) {
                        fclose(f);
                        fprintf(stderr, "Box avcC largesize inválido.\n");
                        return -3;
                    }
                    avcc_size -= 16; // Ajuste para conteúdo
                    fseek(f, type_pos + 4, SEEK_SET); // Após 'avcC'
                }
                else if (box_size < 8) {
                    fclose(f);
                    fprintf(stderr, "Box avcC inválido (tamanho pequeno).\n");
                    return -3;
                }
                else {
                    avcc_size = box_size - 8;
                    fseek(f, type_pos + 4, SEEK_SET); // Após 'avcC'
                }
                // Limite razoável para avcC (geralmente < 256 bytes)
                if (avcc_size > 1024) {
                    fclose(f);
                    fprintf(stderr, "Box avcC grande demais (%llu bytes).\n", avcc_size);
                    return -3;
                }
                uint8_t* config = (uint8_t*)malloc(avcc_size);
                size_t config_read = fread(config, 1, avcc_size, f);
                if (config_read != avcc_size) {
                    free(config);
                    fclose(f);
                    fprintf(stderr, "Falha ao ler conteúdo de avcC.\n");
                    return -3;
                }

                // Leitura básica do avcC content
                if (config_read <= 8) {
                    free(config);
                    fclose(f);
                    fprintf(stderr, "avcC inválido ou curto demais.\n");
                    return -3;
                }
                length_size = (config[4] & 0x03) + 1; // Extrai lengthSizeMinusOne aqui
                int offset = 5; // Após version (1), profile, compat, level, reserved+lengthSizeMinusOne
                int sps_count = config[offset++] & 0x1F;
                if (sps_count > 0) {
                    if (offset + 2 > config_read) {
                        free(config);
                        fclose(f);
                        fprintf(stderr, "Dados insuficientes para sps_len.\n");
                        return -4;
                    }
                    int sps_len = (config[offset] << 8) | config[offset + 1];
                    offset += 2;
                    if (sps_len > 512 || offset + sps_len > config_read) { // Limite razoável + check overflow
                        free(config);
                        fclose(f);
                        fprintf(stderr, "SPS len inválido (%d) ou excede dados (%zu).\n", sps_len, config_read);
                        return -5;
                    }
                    meta->sps = (uint8_t*)malloc(sps_len);
                    memcpy(meta->sps, &config[offset], sps_len);
                    meta->sps_len = sps_len;
                    offset += sps_len;
                }
                if (offset >= config_read) {
                    free(config);
                    fclose(f);
                    fprintf(stderr, "Dados insuficientes para pps_count.\n");
                    return -4;
                }
                int pps_count = config[offset++];
                if (pps_count > 0) {
                    if (offset + 2 > config_read) {
                        free(config);
                        fclose(f);
                        fprintf(stderr, "Dados insuficientes para pps_len.\n");
                        return -4;
                    }
                    int pps_len = (config[offset] << 8) | config[offset + 1];
                    offset += 2;
                    if (pps_len > 512 || offset + pps_len > config_read) {
                        free(config);
                        fclose(f);
                        fprintf(stderr, "PPS len inválido (%d) ou excede dados (%zu).\n", pps_len, config_read);
                        return -5;
                    }
                    meta->pps = (uint8_t*)malloc(pps_len);
                    memcpy(meta->pps, &config[offset], pps_len);
                    meta->pps_len = pps_len;
                }
                free(config);
                break; // Sai do loop de busca
            }
        }
        if (found) break;
        pos += read - 3;
        fseek(f, pos, SEEK_SET); // Overlap
    }
    if (!found) {
        fclose(f);
        fprintf(stderr, "Marcador 'avcC' não encontrado.\n");
        return -2;
    }

    // Complemento: Se não encontrou SPS/PPS no avcC, procure no mdat (in-band)
    if (meta->sps == NULL || meta->pps == NULL) {
        if (length_size == 0) {
            fclose(f);
            fprintf(stderr, "length_size não definido (avcC inválido).\n");
            return -6;
        }
        // Reset para buscar 'mdat' do início
        fseek(f, 0, SEEK_SET);
        pos = 0;
        found = 0;
        while ((read = fread(buffer, 1, sizeof(buffer), f)) > 0) {
            for (size_t i = 0; i + 3 < read; i++) {
                if (buffer[i] == 'm' && buffer[i + 1] == 'd' && buffer[i + 2] == 'a' && buffer[i + 3] == 't') {
                    found = 1;
                    long type_pos = pos + i;
                    fseek(f, type_pos - 4, SEEK_SET);
                    uint32_t box_size = read32(f);
                    uint64_t mdat_size;
                    if (box_size == 1) { // largesize
                        mdat_size = read64(f) - 16;
                    }
                    else if (box_size == 0) { // Até EOF
                        long current = ftell(f);
                        fseek(f, 0, SEEK_END);
                        long end = ftell(f);
                        fseek(f, current, SEEK_SET);
                        mdat_size = end - (type_pos + 4);
                    }
                    else {
                        mdat_size = box_size - 8;
                    }
                    if (mdat_size < 0) {
                        fclose(f);
                        fprintf(stderr, "Box mdat tamanho inválido.\n");
                        return -4;
                    }
                    long data_pos = type_pos + 4; // Após 'mdat'
                    fseek(f, data_pos, SEEK_SET);

                    // Parse NAL units no mdat até encontrar SPS e PPS (limite aumentado para 10MB)
                    uint8_t nal_len_buf[4];
                    uint64_t parsed_bytes = 0;
                    int found_sps = (meta->sps != NULL);
                    int found_pps = (meta->pps != NULL);
                    while (parsed_bytes < 1024 * 1024 * 10 && !feof(f) && !(found_sps && found_pps))
                    {
                        if (fread(nal_len_buf, 1, length_size, f) < (size_t)length_size) 
                        {
                            fprintf(stderr, "Falha ao ler nal_len (EOF prematuro).\n");
                            break;
                        }
                        uint32_t nal_len = read_n(nal_len_buf, length_size);
                        if (nal_len == 0 || nal_len > 1024 * 1024) 
                        {
                            fprintf(stderr, "nal_len inválido (%u).\n", nal_len);
                            break;
                        }
                        uint8_t* nal = (uint8_t*)malloc(nal_len);
                        if (fread(nal, 1, nal_len, f) < nal_len)
                        {
                            free(nal);
                            fprintf(stderr, "Falha ao ler NAL data.\n");
                            break;
                        }
                        parsed_bytes += length_size + nal_len;
                        if (parsed_bytes > mdat_size)
                        {
                            free(nal);
                            fprintf(stderr, "Excedeu mdat_size (%llu > %llu).\n", parsed_bytes, mdat_size);
                            break;
                        }
                        uint8_t nal_type = nal[0] & 0x1F;
                        if (nal_type == 7 && !found_sps) { // SPS
                            meta->sps = nal;
                            meta->sps_len = nal_len;
                            found_sps = 1;
                            fprintf(stderr, "SPS encontrado no mdat (len=%d).\n", nal_len); // Depuração
                        }
                        else if (nal_type == 8 && !found_pps) { // PPS
                            meta->pps = nal;
                            meta->pps_len = nal_len;
                            found_pps = 1;
                            fprintf(stderr, "PPS encontrado no mdat (len=%d).\n", nal_len); // Depuração
                        }
                        else {
                            free(nal);
                            fprintf(stderr, "NAL type %d ignorado.\n", nal_type); // Depuração para ver se parseia
                        }
                    }
                    if (!found_sps || !found_pps) {
                        fclose(f);
                        fprintf(stderr, "SPS/PPS não encontrados no mdat após %llu bytes.\n", parsed_bytes);
                        return -5;
                    }
                    break; // Sai do loop de busca mdat
                }
            }
            if (found) break;
            pos += read - 3;
            fseek(f, pos, SEEK_SET); // Overlap
        }
        if (!found) {
            fclose(f);
            fprintf(stderr, "Marcador 'mdat' não encontrado.\n");
            return -4;
        }
    }

    fclose(f);
    return 0;
}


