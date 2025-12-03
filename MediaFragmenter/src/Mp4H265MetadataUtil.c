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
#include "FileUtil.h"


// ──────────────────────────────────────────────────────────────────────────────
// Estrutura do hvcC box (HEVCDecoderConfigurationRecord):
// 
// configurationVersion (1 byte)
// general_profile_space (2 bits), general_tier_flag (1 bit), general_profile_idc (5 bits)
// general_profile_compatibility_flags (4 bytes)
// general_constraint_indicator_flags (6 bytes)
// general_level_idc (1 byte)
// reserved (4 bits) + min_spatial_segmentation_idc (12 bits)
// reserved (6 bits) + parallelismType (2 bits)
// reserved (6 bits) + chromaFormat (2 bits)
// reserved (5 bits) + bitDepthLumaMinus8 (3 bits)
// reserved (5 bits) + bitDepthChromaMinus8 (3 bits)
// avgFrameRate (2 bytes)
// constantFrameRate (2 bits) + numTemporalLayers (3 bits) + temporalIdNested (1 bit) + lengthSizeMinusOne (2 bits)
// numOfArrays (1 byte)
// 
// Para cada array:
//   array_completeness (1 bit) + reserved (1 bit) + NAL_unit_type (6 bits)
//   numNalus (2 bytes)
//   Para cada NAL:
//     nalUnitLength (2 bytes)
//     nalUnit (nalUnitLength bytes)
// ──────────────────────────────────────────────────────────────────────────────


// Tipos de NAL para referência
#define HEVC_NAL_VPS 32
#define HEVC_NAL_SPS 33
#define HEVC_NAL_PPS 34


static int load_hvcc_data(FILE* f, uint8_t** vps, int* vps_len, uint8_t** sps, int* sps_len, uint8_t** pps, int* pps_len, int* length_size)
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

                            // Offset 21: constantFrameRate (2) + numTemporalLayers (3) + temporalIdNested (1) + lengthSizeMinusOne (2)
                            *length_size = (conf[21] & 0x03) + 1;

                            // Offset 22: numOfArrays
                            int num_arrays = conf[22];
                            int off = 23;

                            // Parsear cada array de NALs
                            for (int arr = 0; arr < num_arrays && off < hvcc_size; arr++)
                            {
                                if (off >= hvcc_size) break;

                                // array_completeness (1 bit) + reserved (1 bit) + NAL_unit_type (6 bits)
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


// ──────────────────────────────────────────────────────────────────────────────
// Função pública: Carrega metadados H.265 do arquivo MP4
// ──────────────────────────────────────────────────────────────────────────────
int mp4meta_load_h265_metadata(FILE* f, VideoMetadata* meta)
{
    if (!f || !meta) return -1;

    uint8_t* vps = NULL;
    uint8_t* sps = NULL;
    uint8_t* pps = NULL;
    int vps_len = 0;
    int sps_len = 0;
    int pps_len = 0;
    int length_size = 0;

    fseek(f, 0, SEEK_SET);
    int ret = load_hvcc_data(f, &vps, &vps_len, &sps, &sps_len, &pps, &pps_len, &length_size);

    if (ret != 0)
    {
        fprintf(stderr, "Falha ao carregar hvcC (H.265 metadata)\n");
        return ret;
    }

    // Preencher estrutura de metadados
    meta->LengthSize = length_size;
    meta->Vps.Data = vps;
    meta->Vps.Size = vps_len;
    meta->Sps.Data = sps;
    meta->Sps.Size = sps_len;
    meta->Pps.Data = pps;
    meta->Pps.Size = pps_len;
    meta->Codec = 265;

    return 0;
}


// ──────────────────────────────────────────────────────────────────────────────
// Função auxiliar: Extrai tipo de NAL do header H.265 (para parsing de frames)
// H.265 NAL header tem 2 bytes:
//   byte 0: forbidden_zero_bit (1) | nal_unit_type (6) | nuh_layer_id_high (1)
//   byte 1: nuh_layer_id_low (5) | nuh_temporal_id_plus1 (3)
// ──────────────────────────────────────────────────────────────────────────────
uint8_t h265_get_nal_type_from_header(uint8_t nal_header_byte0)
{
    return (nal_header_byte0 >> 1) & 0x3F;
}