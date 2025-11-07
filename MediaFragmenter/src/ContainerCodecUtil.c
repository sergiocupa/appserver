#include "../include/MediaFragmenter.h"
#include "FileUtil.h"
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>


const uint8_t FTYP_BOX[] = 
{
        0x00,0x00,0x00,0x18,                // size
        'f','t','y','p',                    // type
        'i','s','o','6',                    // major brand
        0x00,0x00,0x00,0x01,                // minor version
        'i','s','o','6','m','p','4','1'//  'i','s','o','6','a','v','c','1'      // compatible brands
};


// ----------------- cria avcC (H.264) payload -----------------
static MediaBuffer* build_avcC(const VideoInitData* v)
{
    MediaBuffer* avcc = mbuffer_create(100);

    // avcC payload, sem header size/type (we'll wrap it in a box later)
    // configurationVersion(1)
    mbuffer_append_uint8(avcc, 0x01);

    // profile_idc, profile_compatibility, level_idc -> do SPS: sps[1..3]
    uint8_t profile = (v->sps_size >= 3) ? v->sps[1] : 0;
    uint8_t compat = (v->sps_size >= 3) ? v->sps[2] : 0;
    uint8_t level = (v->sps_size >= 3) ? v->sps[3] : 0;
    mbuffer_append_uint8(avcc, profile);
    mbuffer_append_uint8(avcc, compat);
    mbuffer_append_uint8(avcc, level);

    // lengthSizeMinusOne (2 bits) stored in low bits of next byte -> we set to 3 (meaning lengthSize=4)
    mbuffer_append_uint8(avcc, 0xFF);

    // numOfSequenceParameterSets (5 LSB) with 3 MSB set as 111 -> 0xE1 means 1 SPS
    mbuffer_append_uint8(avcc, 0xE1);

    // SPS length (2 bytes BE) + sps data
    mbuffer_append_uint16(avcc, (uint16_t)v->sps_size);
    mbuffer_append(avcc, v->sps, v->sps_size);

    // numOfPictureParameterSets
    mbuffer_append_uint8(avcc, 0x01);
    mbuffer_append_uint16(avcc, (uint16_t)v->pps_size);
    mbuffer_append(avcc, v->pps, v->pps_size);
}

// ----------------- cria hvcC (H.265) minimal payload -----------------
static MediaBuffer* build_hvcC(const VideoInitData* v)
{
    // HEVCDecoderConfigurationRecord minimal-ish
    // configurationVersion
    MediaBuffer* out = mbuffer_create(100);
    mbuffer_append_uint8(out, 0x01);

    // general_profile_space (2) + general_tier_flag (1) + general_profile_idc (5)
    // vamos preencher com zeros exceto profile_idc tirado do VPS/SPS se possível (simplificado)
    mbuffer_append_uint8(out, 0x00);

    // general_profile_compatibility_flags (4 bytes)
    mbuffer_append_uint32(out, 0);

    // general_constraint_indicator_flags (6 bytes)
    mbuffer_append_uint8(out, 0x00); mbuffer_append_uint8(out, 0x00); mbuffer_append_uint8(out, 0x00);
    mbuffer_append_uint8(out, 0x00); mbuffer_append_uint8(out, 0x00); mbuffer_append_uint8(out, 0x00);

    // general_level_idc
    mbuffer_append_uint8(out, 0x00);

    // reserved (12 bits) + min_spatial_segmentation_idc (12 bits) -> we'll write 2 bytes zero + 2 bytes zero
    mbuffer_append_uint16(out, 0x00);
    mbuffer_append_uint16(out, 0x00);

    // parallelismType, chromaFormat, bitDepthLumaMinus8, bitDepthChromaMinus8
    mbuffer_append_uint8(out, 0xFC | 0); // parallelism type in low bits
    mbuffer_append_uint8(out, 0xFC | 0); // chroma format
    mbuffer_append_uint8(out, 0xF8 | 0); // luma
    mbuffer_append_uint8(out, 0xF8 | 0); // chroma

    // averageFrameRate
    mbuffer_append_uint16(out, 0x00);

    // constantFrameRate(2 bits), numTemporalLayers(3 bits), temporalIdNested(1 bit), lengthSizeMinusOne(2 bits)
    // set lengthSizeMinusOne = 3 (4 bytes)
    mbuffer_append_uint8(out, 0x01 << 6 | 0x03);

    // numOfArrays (we will include arrays for VPS(32), SPS(33), PPS(34) if present)
    uint8_t numArrays = 0;
    if (v->vps && v->vps_size) numArrays++;
    if (v->sps && v->sps_size) numArrays++;
    if (v->pps && v->pps_size) numArrays++;
    mbuffer_append_uint8(out, numArrays);

    // now arrays
    if (v->vps && v->vps_size)
    {
        // array_completeness (1) + reserved(1bit) + NAL_unit_type (6 bits) -> store in one byte (we set completeness=1)

        mbuffer_append_uint8(out, (1 << 7) | 32); // VPS has type 32
        mbuffer_append_uint16(out, 1);            // numNalus
        mbuffer_append_uint16(out, (uint16_t)v->vps_size);
        mbuffer_append(out, v->vps, v->vps_size);
    }
    if (v->sps && v->sps_size)
    {
        mbuffer_append_uint8(out, (1 << 7) | 33); // SPS type 33
        mbuffer_append_uint16(out, 1);
        mbuffer_append_uint16(out, (uint16_t)v->sps_size);
        mbuffer_append(out, v->sps, v->sps_size);
    }
    if (v->pps && v->pps_size)
    {
        mbuffer_append_uint8(out, (1 << 7) | 34); // PPS type 34
        mbuffer_append_uint16(out, 1);
        mbuffer_append_uint16(out, (uint16_t)v->pps_size);
        mbuffer_append(out, v->pps, v->pps_size);
    }
}

static MediaBuffer* build_stsd_box(const VideoInitData* v)
{
    MediaBuffer* pay          = mbuffer_create(100);
    MediaBuffer* sample_entry = mbuffer_create(100);

    // version (1 byte) + flags (3 bytes)
    mbuffer_append_uint32(pay, 0);

    // entry_count = 1
    mbuffer_append_uint32(pay, 1);

    // build sample entry (avc1 or hev1) into a DynBuf sample_entry
    if (v->hdot_version == HDOT_H264)
    {
        // avc1 sample entry (payload only, without outer box header)
        // fields: reserved(6) + data_reference_index(2)
        uint8_t reserved6[6] = { 0,0,0,0,0,0 };
        mbuffer_append(sample_entry, reserved6, 6);
        mbuffer_append_uint16(sample_entry, 1);

        // pre-defined + reserved + pre-defined (just zeros) 16 bytes
        uint8_t zeros16[16] = { 0 };
        mbuffer_append(sample_entry, zeros16, 16);

        mbuffer_append_uint16(sample_entry, (uint16_t)v->width);
        mbuffer_append_uint16(sample_entry, (uint16_t)v->height);
        mbuffer_append_uint32(sample_entry, 0x00480000);// horiz resolution 72dpi (0x0048 << 16)
        mbuffer_append_uint32(sample_entry, 0x00480000);// vert resolution
        mbuffer_append_uint32(sample_entry, 0); // reserved
        mbuffer_append_uint16(sample_entry, 1);

        // compressorname 32 bytes (fill zeros)
        uint8_t compname[32] = { 0 };
        mbuffer_append(sample_entry, compname, 32);
        mbuffer_append_uint16(sample_entry, 0x0018); // depth
        mbuffer_append_uint16(sample_entry, 0xFFFF); // pre_defined

        MediaBuffer* avcc = build_avcC(v);

        // wrap avcC as box
        mbuffer_append_uint32(sample_entry, (uint32_t)(8 + avcc->Length));
        mbuffer_append_string(sample_entry, "avcC");

        //db_write_u32be(&sample_entry, (uint32_t)(8 + avcc.len)); // avcC size
        //db_write_fourcc(&sample_entry, "avcC");
        if (avcc->Length)
        {
            mbuffer_append(sample_entry, avcc->Data, avcc->Length);
        }
        mbuffer_release(&avcc);
    }
    else if (v->hdot_version == HDOT_H265)
    {
        // hev1 sample entry - similar fields to avc1
        uint8_t reserved6[6] = { 0,0,0,0,0,0 };
        mbuffer_append(sample_entry, reserved6, 6);
        mbuffer_append_uint16(sample_entry, 1); // data_reference_index

        uint8_t zeros16[16] = { 0 };
        mbuffer_append(sample_entry, zeros16, 16);
        mbuffer_append_uint16(sample_entry, (uint16_t)v->width);
        mbuffer_append_uint16(sample_entry, (uint16_t)v->height);
        mbuffer_append_uint32(sample_entry, 0x00480000);
        mbuffer_append_uint32(sample_entry, 0x00480000);
        mbuffer_append_uint32(sample_entry, 0);
        mbuffer_append_uint16(sample_entry, 1);

        uint8_t compname[32] = { 0 };
        mbuffer_append(sample_entry, compname, 32);
        mbuffer_append_uint16(sample_entry, 0x0018);
        mbuffer_append_uint16(sample_entry, 0xFFFF);

        MediaBuffer* hvcc = build_hvcC(v);

        mbuffer_append_uint32(sample_entry, (uint32_t)(8 + hvcc->Length));
        mbuffer_append_string(sample_entry, "hvcC");

        if (hvcc->Length)
        {
            mbuffer_append(sample_entry, hvcc->Data, hvcc->Length);
        }
        mbuffer_release(&hvcc);
    }

    MediaBuffer* sample_entry_box = mbuffer_create(100);
    const char* type = (v->hdot_version == HDOT_H264) ? "avc1" : "hev1";

    // write size and type manually into sample_entry_box
    mbuffer_append_uint32(sample_entry_box, (uint32_t)(8 + sample_entry->Length));
    mbuffer_append_string(sample_entry_box, type);
    mbuffer_append(sample_entry_box, sample_entry->Data, sample_entry->Length);
    mbuffer_release(&sample_entry);

    // agora adiciona sample_entry_box aos stsd_payload
    mbuffer_append(pay, sample_entry_box->Data, sample_entry_box->Length);
    mbuffer_release(&sample_entry_box);

    // agora embrulha stsd_payload em uma box "stsd"

    MediaBuffer* out = mbuffer_create(100);
    mbuffer_box_from_buf(out, "stsd", pay);
    mbuffer_release(&pay);
    return out;
}

// ----------------- monta um moov simples (mvhd + one trak with stsd) -------------
static MediaBuffer* build_moov_box(const VideoInitData* v) // uint32_t timescale
{
    MediaBuffer* moov_content = mbuffer_create(100);
    MediaBuffer* mvhd         = mbuffer_create(100);// mvhd (very simplified - version 0)

    // version + flags
    mbuffer_append_uint8(mvhd, 0x00);
    mbuffer_append_uint8(mvhd, 0x00);
    mbuffer_append_uint8(mvhd, 0x00);
    mbuffer_append_uint8(mvhd, 0x00);

    mbuffer_append_uint32(mvhd, 0);//            creation_time
    mbuffer_append_uint32(mvhd, 0);//            modification_time
    mbuffer_append_uint32(mvhd, v->timescale);//       timescale
    mbuffer_append_uint32(mvhd, 0);//            duration
    mbuffer_append_uint32(mvhd, 0x00010000);//   rate 1.0
    mbuffer_append_uint16(mvhd, 0x0100);//       volume
    mbuffer_append_uint16(mvhd, 0); //           reserved

    // reserved 2*4
    mbuffer_append_uint32(mvhd, 0); mbuffer_append_uint32(mvhd, 0);

    // matrix (9 * 4 bytes)
    uint32_t matrix[9] = { 0x00010000,0,0,0,0x00010000,0,0,0,0x40000000 };
    for (int i = 0; i < 9; i++)
    {
        mbuffer_append_uint32(mvhd, matrix[i]);
    }

    // pre_defined 6 * 4
    for (int i = 0; i < 6; i++)
    {
        mbuffer_append_uint32(mvhd, 0);
    }

    // next_track_ID
    mbuffer_append_uint32(mvhd, 2);

    // write mvhd box
    mbuffer_box_from_buf(moov_content, "mvhd", mvhd);
    mbuffer_release(&mvhd);

    // ---- trak (we will build tkhd + mdia with stsd inside) ----
    MediaBuffer* trak = mbuffer_create(100);
    MediaBuffer* tkhd = mbuffer_create(100);// tkhd (minimal)

    // flags: enabled
    mbuffer_append_uint8(tkhd, 0x00);
    mbuffer_append_uint8(tkhd, 0x00);
    mbuffer_append_uint8(tkhd, 0x00);
    mbuffer_append_uint8(tkhd, 0x07);

    mbuffer_append_uint32(tkhd, 0);// creation time
    mbuffer_append_uint32(tkhd, 0);// modification
    mbuffer_append_uint32(tkhd, 1);// track_ID
    mbuffer_append_uint32(tkhd, 0);// reserved
    mbuffer_append_uint32(tkhd, 0);// duration
    mbuffer_append_uint32(tkhd, 0);// reserved
    mbuffer_append_uint32(tkhd, 0);// reserved
    mbuffer_append_uint16(tkhd, 0);// layer
    mbuffer_append_uint16(tkhd, 0);// alternate_group
    mbuffer_append_uint16(tkhd, 0x0100);// volume
    mbuffer_append_uint16(tkhd, 0);// reserved

    // matrix
    for (int i = 0; i < 9; i++)
    {
        mbuffer_append_uint32(tkhd, matrix[i]);
    }
    // width/height fixed 16.16
    mbuffer_append_uint32(tkhd, (uint32_t)(v->width << 16));
    mbuffer_append_uint32(tkhd, (uint32_t)(v->height << 16));
    mbuffer_box_from_buf(trak, "tkhd", tkhd);
    mbuffer_release(&tkhd);

    MediaBuffer* mdia = mbuffer_create(100);// mdia
    MediaBuffer* mdhd = mbuffer_create(100); // mdhd

    mbuffer_append_uint8(mdhd, 0x00);
    mbuffer_append_uint8(mdhd, 0x00);
    mbuffer_append_uint8(mdhd, 0x00);
    mbuffer_append_uint8(mdhd, 0x00);

    mbuffer_append_uint32(mdhd, 0);
    mbuffer_append_uint32(mdhd, 0);
    mbuffer_append_uint32(mdhd, v->timescale);
    mbuffer_append_uint32(mdhd, 0);
    // language(2) + pre_defined
    mbuffer_append_uint16(mdhd, 0x55C4);
    mbuffer_append_uint16(mdhd, 0);
    mbuffer_box_from_buf(mdia, "mdhd", mdhd);
    mbuffer_release(&mdhd);


    MediaBuffer* hdlr = mbuffer_create(100);// hdlr
    mbuffer_append_uint8(hdlr, 0x00);
    mbuffer_append_uint8(hdlr, 0x00);
    mbuffer_append_uint8(hdlr, 0x00);
    mbuffer_append_uint8(hdlr, 0x00);

    mbuffer_append_uint32(hdlr, 0);
    mbuffer_append_uint32(hdlr, 0);
    mbuffer_append_string(hdlr, "vide"); // handler_type

    // reserved 3 * 4
    mbuffer_append_uint32(hdlr, 0);
    mbuffer_append_uint32(hdlr, 0);
    mbuffer_append_uint32(hdlr, 0);

    // name (null terminated)
    const uint_fast8_t name[] = "VideoHandler";
    mbuffer_append(hdlr, name, sizeof(name));
    mbuffer_box_from_buf(mdia, "hdlr", hdlr);
    mbuffer_release(&hdlr);


    MediaBuffer* minf = mbuffer_create(100);// minf
    MediaBuffer* vmhd = mbuffer_create(100);// vmhd (video media header)
    // version/flags 1
    mbuffer_append_uint8(vmhd, 0x00);
    mbuffer_append_uint8(vmhd, 0x00);
    mbuffer_append_uint8(vmhd, 0x00);
    mbuffer_append_uint8(vmhd, 0x01);
    // graphicsmode + opcolor
    mbuffer_append_uint16(vmhd, 0);
    mbuffer_append_uint16(vmhd, 0);
    mbuffer_append_uint16(vmhd, 0);
    mbuffer_box_from_buf(minf, "vmhd", vmhd);
    mbuffer_release(&vmhd);


    MediaBuffer* dinf = mbuffer_create(100);// dinf (data info) -> dref
    MediaBuffer* dref = mbuffer_create(100);
    // version/flags
    mbuffer_append_uint8(dref, 0x00);
    mbuffer_append_uint8(dref, 0x00);
    mbuffer_append_uint8(dref, 0x00);
    mbuffer_append_uint8(dref, 0x00);
    mbuffer_append_uint32(dref, 1);// entry_count
    // url box (self-contained)
    MediaBuffer* url    = mbuffer_create(100);
    MediaBuffer* urlbox = mbuffer_create(100);
    // flags=1 (self-contained)
    mbuffer_append_uint8(url, 0x00);
    mbuffer_append_uint8(url, 0x00);
    mbuffer_append_uint8(url, 0x00);
    mbuffer_append_uint8(url, 0x01);
    // wrap url in box
    mbuffer_box_from_buf(urlbox, "url ", url);
    mbuffer_append(dref, urlbox->Data, urlbox->Length);
    mbuffer_box_from_buf(dinf, "dref", dref);
    mbuffer_box_from_buf(minf, "dinf", dinf);
    mbuffer_release(&url);
    mbuffer_release(&urlbox);
    mbuffer_release(&dref);
    mbuffer_release(&dinf);


    // stsd (we will build stsd with avc1/hev1)
    MediaBuffer* stbl = build_stsd_box(v);

    // stts, stsc, stsz, stco minimal placeholders (empty but present)
    MediaBuffer* tmp = mbuffer_create(100);
    mbuffer_append_uint8(tmp, 0); mbuffer_append_uint8(tmp, 0); mbuffer_append_uint8(tmp, 0); mbuffer_append_uint8(tmp, 0);
    mbuffer_append_uint32(tmp, 0); // entry_count 0
    mbuffer_box_from_buf(stbl, "stts", tmp);

    tmp->Length = 0;
    tmp->Data[0] = 0;
    mbuffer_append_uint8(tmp, 0); mbuffer_append_uint8(tmp, 0); mbuffer_append_uint8(tmp, 0); mbuffer_append_uint8(tmp, 0);
    mbuffer_append_uint32(tmp, 0);
    mbuffer_box_from_buf(stbl, "stsc", tmp);

    mbuffer_append_uint8(tmp, 0); mbuffer_append_uint8(tmp, 0); mbuffer_append_uint8(tmp, 0); mbuffer_append_uint8(tmp, 0);
    mbuffer_append_uint32(tmp, 0);
    mbuffer_box_from_buf(stbl, "stsz", tmp);

    mbuffer_append_uint8(tmp, 0); mbuffer_append_uint8(tmp, 0); mbuffer_append_uint8(tmp, 0); mbuffer_append_uint8(tmp, 0);
    mbuffer_append_uint32(tmp, 0);
    mbuffer_box_from_buf(stbl, "stso", tmp);
    mbuffer_release(&tmp);

    MediaBuffer* out = mbuffer_create(100);

    mbuffer_box_from_buf(minf, "stbl", stbl); // wrap stbl into minf->stbl
    mbuffer_box_from_buf(mdia, "minf", minf);// wrap minf into mdia
    mbuffer_box_from_buf(trak, "mdia", mdia);// finally add mdia into trak
    mbuffer_box_from_buf(moov_content, "trak", trak);  // wrap trak into moov
    mbuffer_box_from_buf(out, "moov", moov_content);  // write moov box to out buffer

    mbuffer_release(&stbl);
    mbuffer_release(&minf);
    mbuffer_release(&mdia);
    mbuffer_release(&trak);
    mbuffer_release(&moov_content);

    return out;
}



// Função para parsear tables de vídeo (agora void, preenche structs se video track)
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
        if (size == 1) {
            box_size = read64(f);
            header_size = 16;
        }
        else if (size == 0) {
            fseek(f, 0, SEEK_END);
            box_size = ftell(f) - box_start;
            fseek(f, box_start + 8, SEEK_SET);
        }

        if ((size == 1 && box_size < 16) || (size != 1 && size != 0 && box_size < 8)) break;

        uint64_t next = box_start + box_size;

        // hdlr para confirmar vídeo
        if (!memcmp(name, "hdlr", 4)) {
            fseek(f, 8, SEEK_CUR);  // version/flags + pre_defined
            uint8_t handler[5] = { 0 };
            if (fread(handler, 1, 4, f) == 4) {
                if (!memcmp(handler, "vide", 4)) is_video = 1;
            }
            fseek(f, next, SEEK_SET);
            continue;
        }

        // mdhd para timescale
        if (!memcmp(name, "mdhd", 4) && is_video) {
            fseek(f, 12, SEEK_CUR);  // version/flags + datas
            *timescale = read32(f);
            fseek(f, next, SEEK_SET);
            continue;
        }

        // stsd para codec (focado em H.264)
        if (!memcmp(name, "stsd", 4) && is_video) {
            uint8_t header[8];
            if (fread(header, 1, 8, f) != 8) break;
            uint32_t entry_count = (header[4] << 24) | (header[5] << 16) | (header[6] << 8) | header[7];
            for (uint32_t i = 0; i < entry_count; i++) {
                uint32_t entry_size = read32(f);
                uint8_t entry_name[5] = { 0 };
                if (fread(entry_name, 1, 4, f) != 4) break;
                if (!memcmp(entry_name, "avc1", 4) || !memcmp(entry_name, "avc3", 4)) *codec = 264;
                fseek(f, entry_size - 8, SEEK_CUR);
            }
            fseek(f, next, SEEK_SET);
            continue;
        }

        // stts para deltas de tempo
        if (!memcmp(name, "stts", 4) && is_video) {
            fseek(f, 4, SEEK_CUR);  // version/flags
            stts->count = read32(f);
            stts->entries = malloc(stts->count * sizeof(struct SttsEntry));
            for (uint32_t i = 0; i < stts->count; i++) {
                stts->entries[i].count = read32(f);
                stts->entries[i].delta = read32(f);
            }
            fseek(f, next, SEEK_SET);
            continue;
        }

        // stsc para mapeamento sample-to-chunk
        if (!memcmp(name, "stsc", 4) && is_video) {
            fseek(f, 4, SEEK_CUR);  // version/flags
            stsc->count = read32(f);
            stsc->entries = malloc(stsc->count * sizeof(struct StscEntry));
            for (uint32_t i = 0; i < stsc->count; i++) {
                stsc->entries[i].first_chunk = read32(f);
                stsc->entries[i].samples_per_chunk = read32(f);
                stsc->entries[i].sample_desc_id = read32(f);
            }
            fseek(f, next, SEEK_SET);
            continue;
        }

        // stsz para tamanhos de samples
        if (!memcmp(name, "stsz", 4) && is_video) {
            fseek(f, 4, SEEK_CUR);  // version/flags
            uint32_t sample_size = read32(f);
            stsz->count = read32(f);
            stsz->sizes = malloc(stsz->count * sizeof(uint32_t));
            if (sample_size == 0) {  // Variáveis
                for (uint32_t i = 0; i < stsz->count; i++) {
                    stsz->sizes[i] = read32(f);
                }
            }
            else {  // Fixo
                for (uint32_t i = 0; i < stsz->count; i++) {
                    stsz->sizes[i] = sample_size;
                }
            }
            fseek(f, next, SEEK_SET);
            continue;
        }

        // stco ou co64 para offsets de chunks
        if ((!memcmp(name, "stco", 4) || !memcmp(name, "co64", 4)) && is_video) {
            stco->is_co64 = !memcmp(name, "co64", 4);
            fseek(f, 4, SEEK_CUR);  // version/flags
            stco->count = read32(f);
            stco->offsets = malloc(stco->count * sizeof(uint64_t));
            for (uint32_t i = 0; i < stco->count; i++) {
                stco->offsets[i] = stco->is_co64 ? read64(f) : (uint64_t)read32(f);
            }
            fseek(f, next, SEEK_SET);
            continue;
        }

        // Recursa para containers, propagando is_video
        if (!memcmp(name, "moov", 4) || !memcmp(name, "trak", 4) ||
            !memcmp(name, "mdia", 4) || !memcmp(name, "minf", 4) ||
            !memcmp(name, "stbl", 4) || !memcmp(name, "moof", 4) || !memcmp(name, "edts", 4)) {
            parse_video_track_tables(f, next, is_video, stsz, stco, stsc, stts, timescale, codec);
        }
        fseek(f, next, SEEK_SET);
    }
}




FrameIndexList* concod_index_frames(const char* path)
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
    if (concod_load_video_metadata(f, &meta) != 0) {
        printf("Falha ao carregar metadata para length_size.\n");
        fclose(f);
        return NULL;
    }
    fseek(f, 0, SEEK_SET);

    // Nao pega length_size, mas de onde vem isso? O Grok assumiu que tem este dado. Perguntar como obter este dado
    int length_size = meta.LengthSize;  // Adicionado: Usa length_size extraído (geralmente 4, mas dinâmico)
    if (length_size < 1 || length_size > 4) {
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

    // Calcula FPS de stts e timescale
    double total_ticks = 0.0;
    uint32_t total_samples = 0;
    for (uint32_t i = 0; i < stts.count; i++) {
        total_samples += stts.entries[i].count;
        total_ticks += (double)stts.entries[i].count * stts.entries[i].delta;
    }
    double duration_sec = total_ticks / timescale;
    meta.Fps = (duration_sec > 0) ? (double)total_samples / duration_sec : 30.0;

    // Cria lista ordenada
    FrameIndexList* list = mframe_list_new(stsz.count);
    list->Metadata.pps = meta.pps; list->Metadata.pps_len = meta.pps_len; 
    list->Metadata.sps = meta.sps; list->Metadata.sps_len = meta.pps_len;
    list->Metadata.LengthSize = meta.LengthSize;

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
        for (uint32_t j = 0; j < sample_idx; j++) {
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
        if (pos != size) {
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



// Função principal (focada em H.264)
FrameIndexList* __concod_index_frames(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) 
    {
        perror("Erro ao abrir arquivo");
        return NULL;
    }

    /*int codec = codec_version_file(f);
    if (codec != 264) 
    {
        printf("Codec não é H.264.\n");
        fclose(f);
        return NULL;
    }*/
    int codec = 0;

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
    parse_video_track_tables(f, file_end, 0, &stsz, &stco, &stsc, &stts, &timescale, &codec);
    if (stsz.count == 0 || codec == 0) 
    {
        printf("Falha ao parsear tables de vídeo.\n");
        fclose(f);
        return NULL;
    }

    // Calcula FPS de stts e timescale
    double total_ticks = 0.0;
    uint32_t total_samples = 0;
    for (uint32_t i = 0; i < stts.count; i++) {
        total_samples += stts.entries[i].count;
        total_ticks += (double)stts.entries[i].count * stts.entries[i].delta;
    }
    double duration_sec = total_ticks / timescale;
    double fps = (duration_sec > 0) ? (double)total_samples / duration_sec : 30.0;

    // Cria lista ordenada
    FrameIndexList* list = mframe_list_new(stsz.count);

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
        for (uint32_t j = 0; j < sample_idx; j++) {
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
            uint32_t nal_len = read32(f);
            if (nal_len == 0 || pos + nal_len + 4 > size) break;
            uint8_t nal_header = read8(f);
            uint8_t nal_type = nal_header & 0x1F;  // Para H.264
            mnalu_list_add(&frame->Nals, offset + pos, nal_len + 1, nal_type);  // +1 para header
            fseek(f, nal_len - 1, SEEK_CUR);  // Pula resto (header lido)
            pos += nal_len + 4;
        }

        mframe_list_add(list, frame);
    }

    // Libera recursos
    free(sample_offsets);
    free(stsz.sizes);
    free(stco.offsets);
    free(stsc.entries);
    free(stts.entries);

    concod_load_video_metadata(f, &list->Metadata);
    list->Metadata.Codec = codec;
    list->Metadata.Fps = fps;

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


// ----------------- public function: escreve init_segment para file -----------------
MediaBuffer* concod_read_init_segment(const VideoInitData* vid)
{
    MediaBuffer* moov   = build_moov_box(vid);
    MediaBuffer* output = mbuffer_create(moov->Length + sizeof(FTYP_BOX));
    mbuffer_append(output, FTYP_BOX, sizeof(FTYP_BOX));
    mbuffer_append(output, moov->Data, moov->Length);
    mbuffer_release(&moov);
    return output;
}


// saida como buffer...
MediaBuffer* concod_read_frame(const FrameIndex* frame, FILE* src)
{
    MediaBuffer* output = mbuffer_create(frame->Size + 50);

    // --- moof ---
    mbuffer_append_string(output, "[moof - frame offset ");
    mbuffer_append_uint64_string(output, frame->Offset);
    mbuffer_append_string(output, " size ");
    mbuffer_append_uint64_string(output, frame->Size);
    mbuffer_append_string(output, "]\n");

    // --- mdat ---
    mbuffer_append_by_file(output, src, frame->Offset, frame->Size);

    return output;
}


uint_fast8_t* concod_convert_avcc_to_annexb_01(FILE* f, FrameIndex* frame, size_t* annexb_size)
{
    size_t total_size = frame->Size + frame->Nals.Count * 4;  // Margem para start codes
    uint_fast8_t* annexb = malloc(total_size);
    if (!annexb) return NULL;

    size_t pos = 0;
    for (int j = 0; j < frame->Nals.Count; j++) 
    {
        NALUIndex* nal = frame->Nals.Items[j];
        // Adiciona start code 00 00 00 01
        annexb[pos++] = 0;
        annexb[pos++] = 0;
        annexb[pos++] = 0;
        annexb[pos++] = 1;

        if (fseek(f, nal->Offset + 4, SEEK_SET) != 0) 
        {
            free(annexb);
            return NULL;
        }

        size_t nal_data_size = nal->Size - 1; /* Tamanho sem o header já considerado? Ajuste se nal->Size = nal_len */
        if (fread(annexb + pos, 1, nal_data_size, f) != nal_data_size) 
        {
            free(annexb);
            return NULL;
        }

        pos += nal_data_size;
    }
    *annexb_size = pos;
    return annexb;
}



void build_initial_header_from_meta(const  VideoMetadata* meta, uint8_t* header, int* length)
{
    if (!meta || !meta->sps || !meta->pps) return -1;

    int pos = 0;

    memcpy(header + pos, "\x00\x00\x00\x01", 4);
    pos += 4;
    memcpy(header + pos, meta->sps, meta->sps_len);
    pos += meta->sps_len;

    memcpy(header + pos, "\x00\x00\x00\x01", 4);
    pos += 4;
    memcpy(header + pos, meta->pps, meta->pps_len);
    pos += meta->pps_len;

    (*length) = pos;
}


int concod_send_initial_header_from_meta(ISVCDecoder* decoder, const VideoMetadata* meta)
{
    uint8_t* planes[3] = { NULL, NULL, NULL };
    SBufferInfo info;
    memset(&info, 0, sizeof(info));
    uint8_t header[1024];
    int pos = 0;
    build_initial_header_from_meta(meta, header, &pos);

    DECODING_STATE state = (*decoder)->DecodeFrame2(decoder, header, pos, planes, &info);

    if (state != dsErrorFree && state != dsFramePending) 
    {
        printf("Erro ao enviar cabeçalho SPS/PPS: %d\n", state);
        return -2;
    }

    return 0;
}


// Função de conversão AVCC para Annex B
uint8_t* concod_convert_avcc_to_annexb(FILE* f, FrameIndex* frame, size_t* annexb_size) 
{
    if (!frame || frame->Nals.Count == 0) {
        fprintf(stderr, "Frame inválido ou sem NALs.\n");
        return NULL;
    }

    // Aloca buffer suficiente: tamanho total do frame é sum(4 + nal_size), 
    // que é igual ao sum(4 + nal_size) após substituir lengths por start codes (00 00 00 01)
    size_t total_size = frame->Size;
    uint8_t* annexb = (uint8_t*)malloc(total_size);
    if (!annexb) {
        fprintf(stderr, "Falha na alocação de memória.\n");
        return NULL;
    }

    int im = 0;
    int ix = 0;
    while (ix < frame->Nals.Count)
    {
        im += frame->Nals.Items[ix]->Size;
        ix++;
    }

    size_t pos = 0;
    for (int j = 0; j < frame->Nals.Count; j++) 
    {
        NALUIndex* nal = frame->Nals.Items[j];

        // Adiciona start code Annex B: 00 00 00 01
        if (pos + 4 > total_size) {
            free(annexb);
            fprintf(stderr, "Overflow no buffer Annex B.\n");
            return NULL;
        }
        annexb[pos++] = 0x00;
        annexb[pos++] = 0x00;
        annexb[pos++] = 0x00;
        annexb[pos++] = 0x01;

        // Posiciona no início do NAL unit (após o length field de 4 bytes no AVCC)
        if (fseek(f, nal->Offset + 4, SEEK_SET) != 0) {
            free(annexb);
            fprintf(stderr, "Erro no fseek para NAL offset.\n");
            return NULL;
        }

        // Tamanho do NAL data (header + body, que é nal->Size)
        size_t nal_data_size = nal->Size;
        if (pos + nal_data_size > total_size) {
            free(annexb);
            fprintf(stderr, "NAL size excede buffer alocado.\n");
            return NULL;
        }

        // Lê o NAL unit diretamente do arquivo para o buffer
        if (fread(annexb + pos, 1, nal_data_size, f) != nal_data_size) {
            free(annexb);
            fprintf(stderr, "Falha ao ler NAL data do arquivo.\n");
            return NULL;
        }
        pos += nal_data_size;
    }

    *annexb_size = pos;  // Tamanho real utilizado (deve ser igual a frame->Size)
    return annexb;
}


uint_fast8_t* __concod_convert_avcc_to_annexb(FILE* f, FrameIndex* frame, size_t* annexb_size)
{
    // Calcula tamanho total: soma dos tamanhos dos NALs + 4 bytes por start code
    size_t total_size = frame->Size + frame->Nals.Count * 4;
    uint_fast8_t* annexb = malloc(total_size);
    if (!annexb) return NULL;

    size_t pos = 0;

    for (int j = 0; j < frame->Nals.Count; j++)
    {
        NALUIndex* nal = frame->Nals.Items[j];

        // Adiciona start code 00 00 00 01 (Annex B)
        annexb[pos++] = 0x00;
        annexb[pos++] = 0x00;
        annexb[pos++] = 0x00;
        annexb[pos++] = 0x01;

        // Posiciona no offset do NAL no arquivo
        // nal->Offset aponta para o length field (4 bytes) do formato AVCC
        // Pulamos esses 4 bytes para pegar o NAL unit direto
        if (fseek(f, nal->Offset + 4, SEEK_SET) != 0)
        {
            free(annexb);
            return NULL;
        }

        // Tamanho do NAL unit (sem o length field de 4 bytes do AVCC)
        // nal->Size já deve incluir o tamanho total do NAL no formato AVCC
        size_t nal_data_size = nal->Size - 4;  // Subtrai os 4 bytes do length field

        // Lê o NAL unit do arquivo diretamente para o buffer annexb
        if (fread(annexb + pos, 1, nal_data_size, f) != nal_data_size)
        {
            free(annexb);
            return NULL;
        }

        pos += nal_data_size;
    }

    // Retorna o tamanho real utilizado
    *annexb_size = pos;

    return annexb;
}




