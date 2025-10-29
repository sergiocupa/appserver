#include "../include/MediaFragmenter.h"
#include "Mp4Util.h"
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


//// Fragmentador 
//static uint8_t* mp4_write_ftyp(FILE* f)
//{
//    // 24 bytes
//    uint8_t ftyp_box[] = {
//        0x00,0x00,0x00,0x18,                 // size
//        'f','t','y','p',                     // type
//        'i','s','o','6',                     // major brand
//        0x00,0x00,0x00,0x01,                 // minor version
//        'i','s','o','6','m','p','4','1'//  'i','s','o','6','a','v','c','1'      // compatible brands
//    };
//
//    return ftyp_box;
//    fwrite(ftyp_box, 1, sizeof(ftyp_box), f);
//}

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

static uint32_t read32(FILE* f)
{
    uint8_t b[4];
    if (fread(b, 1, 4, f) != 4)
        return 0;
    return (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];
}

static uint64_t read64(FILE* f)
{
    uint8_t b[8];
    if (fread(b, 1, 8, f) != 8)
        return 0;
    uint64_t val = 0;
    for (int i = 0; i < 8; i++)
        val = (val << 8) | b[i];
    return val;
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


int mmp4_codec_version(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) return -1;

    int hdot = codec_version_file(f);

    fclose(f);
    return hdot;
}


static int is_decodable_h264(int nalType) { return (nalType == 1 || nalType == 5); }

static int is_decodable_h265(int nalType) { return ((nalType >= 0 && nalType <= 21) && !(nalType >= 32 && nalType <= 34)); }

static char guess_frame_type_h264(int nalType) { return (nalType == 5) ? 'I' : 'P'; }

static char guess_frame_type_h265(int nalType) 
{
    if (nalType >= 19 && nalType <= 21) return 'I';
    if (nalType >= 1 && nalType <= 9) return 'P';
    return 'B';
}




FrameIndexList* mmp4_index_frames(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) {
        perror("Erro ao abrir arquivo");
        return NULL;
    }

    FrameIndexList* list = mframe_list_new();
    list->Codec = codec_version_file(f);
    list->Fps   = 30.0; // valor padrão estimado, pode ser extraído do moov posteriormente

    uint8_t b;
    uint8_t prev_bytes[3] = { 0 };
    uint64_t offset = 0;
    uint64_t start = 0;
    uint64_t size = 0;
    int prev_nalType = -1;
    double frame_time = 1.0 / list->Fps;
    double pts = 0.0;

    while (fread(&b, 1, 1, f) == 1) 
    {
        if (prev_bytes[0] == 0x00 && prev_bytes[1] == 0x00 && b == 0x01) 
        {
            // Achou novo start code
            if (size > 0 && prev_nalType >= 0)
            {
                int decodable = (list->Codec == 264) ? is_decodable_h264(prev_nalType) : is_decodable_h265(prev_nalType);

                if (decodable) 
                {
                    char ftype = (list->Codec == 264) ? guess_frame_type_h264(prev_nalType) : guess_frame_type_h265(prev_nalType);
                    mframe_list_add(list, start, size, prev_nalType, ftype, pts);
                    pts += frame_time;
                }
            }

            start = offset - 2;
            uint8_t nal_header;
            fread(&nal_header, 1, 1, f);
            offset += 4;

            prev_nalType = (list->Codec == 264) ? (nal_header & 0x1F) : ((nal_header >> 1) & 0x3F);

            size = 4;
        }
        else {
            size++;
        }

        prev_bytes[0] = prev_bytes[1];
        prev_bytes[1] = b;
        offset++;
    }

    // último frame
    if (size > 0 && prev_nalType >= 0) 
    {
        int decodable = (list->Codec == 264) ? is_decodable_h264(prev_nalType) : is_decodable_h265(prev_nalType);

        if (decodable) 
        {
            char ftype = (list->Codec == 264) ? guess_frame_type_h264(prev_nalType) : guess_frame_type_h265(prev_nalType);
            mframe_list_add(list, start, size, prev_nalType, ftype, pts);
        }
    }

    fclose(f);
    return list;
}


// ----------------- public function: escreve init_segment para file -----------------
MediaBuffer* mmp4_read_init_segment(const VideoInitData* vid)
{
    MediaBuffer* moov   = build_moov_box(vid);
    MediaBuffer* output = mbuffer_create(moov->Length + sizeof(FTYP_BOX));
    mbuffer_append(output, FTYP_BOX, sizeof(FTYP_BOX));
    mbuffer_append(output, moov->Data, moov->Length);
    mbuffer_release(&moov);
    return output;
}




// saida como buffer...
MediaBuffer* mmp4_read_frame(const FrameIndex* frame, FILE* src)
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




