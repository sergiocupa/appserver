#include "../include/dashstream.h"
#include <stdlib.h>
#include <memory.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>


void dashstr_buffer_init(DashDataBuffer* buffer)
{
	buffer->MaxLength = 100;
	buffer->Length    = 0;
	buffer->Data      = malloc(buffer->MaxLength * sizeof(char));
}
void dashstr_buffer_prepare(DashDataBuffer* buffer, uint64_t size)
{
    buffer->MaxLength = size;
    buffer->Length    = 0;
    buffer->Data      = malloc(buffer->MaxLength * sizeof(char));
}
DashDataBuffer* dashstr_buffer_new()
{
    DashDataBuffer* db = malloc(sizeof(DashDataBuffer));
    db->MaxLength = 100;
    db->Length    = 0;
    db->Data      = malloc(db->MaxLength * sizeof(char));
    return db;
}
DashDataBuffer* dashstr_buffer_create(int size)
{
    DashDataBuffer* db = malloc(sizeof(DashDataBuffer));
    db->MaxLength = size;
    db->Length    = 0;
    db->Data      = malloc(db->MaxLength * sizeof(char));
    return db;
}
void dashstr_buffer_reserve(DashDataBuffer* buffer, int length)
{
    buffer->MaxLength = (buffer->MaxLength + length) * 2;
    char* content = (char*)realloc(buffer->Data, buffer->MaxLength * sizeof(char));
    if (!content) assert(0);
    buffer->Data = content;
}
void dashstr_buffer_append(DashDataBuffer* buffer, const uint_fast8_t* data, const int length)
{
	if ((buffer->Length + length +1) >= buffer->MaxLength)
	{
        dashstr_buffer_reserve(buffer, length);
	}

    memcpy(buffer->Data, data, length);
    buffer->Length += length;
    buffer->Data[buffer->Length] = 0;
}
void dashstr_buffer_append_string(DashDataBuffer* buffer, const char* data)
{
    int size = strlen(data);

    if ((buffer->Length + size + 1) >= buffer->MaxLength)
    {
        dashstr_buffer_reserve(buffer, size);
    }

    memcpy(buffer->Data, data, size);
    buffer->Length += size;
    buffer->Data[buffer->Length] = 0;
}
void dashstr_buffer_append_uint8(DashDataBuffer* buffer, uint32_t value)
{
    if ((buffer->Length + 2) >= buffer->MaxLength)
    {
        dashstr_buffer_reserve(buffer, 1);
    }

    buffer->Data[buffer->Length]     = value;
    buffer->Data[buffer->Length + 1] = 0;
}
void dashstr_buffer_append_uint16(DashDataBuffer* buffer, uint32_t value)
{
    if ((buffer->Length + 3) >= buffer->MaxLength)
    {
        dashstr_buffer_reserve(buffer, 2);
    }

    buffer->Data[buffer->Length]   = value >> 8;
    buffer->Data[buffer->Length+1] = value & 0xFF;
    buffer->Data[buffer->Length+2] = 0;
}
void dashstr_buffer_append_uint32(DashDataBuffer* buffer, uint32_t value)
{
    if ((buffer->Length + 5) >= buffer->MaxLength)
    {
        dashstr_buffer_reserve(buffer, 4);
    }

    buffer->Data[buffer->Length]   = value >> 24;
    buffer->Data[buffer->Length+1] = value >> 16;
    buffer->Data[buffer->Length+2] = value >> 8;
    buffer->Data[buffer->Length+3] = value & 0xFF;
    buffer->Data[buffer->Length+4] = 0;
}
void dashstr_buffer_append_uint64(DashDataBuffer* buffer, uint32_t value)
{
    if ((buffer->Length + 9) >= buffer->MaxLength)
    {
        dashstr_buffer_reserve(buffer, 8);
    }

    buffer->Data[buffer->Length]   = value >> 56;
    buffer->Data[buffer->Length+1] = value >> 48;
    buffer->Data[buffer->Length+2] = value >> 40;
    buffer->Data[buffer->Length+3] = value >> 32;
    buffer->Data[buffer->Length+4] = value >> 24;
    buffer->Data[buffer->Length+5] = value >> 16;
    buffer->Data[buffer->Length+6] = value >> 8;
    buffer->Data[buffer->Length+7] = value & 0xFF;
    buffer->Data[buffer->Length+8] = 0;
}
void dashstr_buffer_release(DashDataBuffer** buffer)
{
	if (*buffer)
	{
		free((*buffer)->Data);
		free(*buffer);
		*buffer = 0;
	}
}

void dashstr_buffer_append_box_from_buf(DashDataBuffer* db, const char* type, DashDataBuffer* content)
{
    uint32_t size = (uint32_t)(8 + content->Length);
    dashstr_buffer_append_uint32(db, size);
    dashstr_buffer_append_string(db, type);

    if (content->Length)
    {
        dashstr_buffer_append(db, content->Data, content->Length);
    }
}


static void write_ftyp(FILE* f)
{
    // 24 bytes
    uint8_t ftyp_box[] = {
        0x00,0x00,0x00,0x18,                 // size
        'f','t','y','p',                     // type
        'i','s','o','6',                     // major brand
        0x00,0x00,0x00,0x01,                 // minor version
        'i','s','o','6','m','p','4','1'//  'i','s','o','6','a','v','c','1'      // compatible brands
    };
    fwrite(ftyp_box, 1, sizeof(ftyp_box), f);
}


// ----------------- cria avcC (H.264) payload -----------------
DashDataBuffer* build_avcC(const VideoInitData* v)
{
    DashDataBuffer* avcc = dashstr_buffer_create(100);

    // avcC payload, sem header size/type (we'll wrap it in a box later)
    // configurationVersion(1)
    dashstr_buffer_append_uint8(avcc, 0x01);

    // profile_idc, profile_compatibility, level_idc -> do SPS: sps[1..3]
    uint8_t profile = (v->sps_size >= 3) ? v->sps[1] : 0;
    uint8_t compat  = (v->sps_size >= 3) ? v->sps[2] : 0;
    uint8_t level   = (v->sps_size >= 3) ? v->sps[3] : 0;
    dashstr_buffer_append_uint8(avcc, profile);
    dashstr_buffer_append_uint8(avcc, compat);
    dashstr_buffer_append_uint8(avcc, level);

    // lengthSizeMinusOne (2 bits) stored in low bits of next byte -> we set to 3 (meaning lengthSize=4)
    dashstr_buffer_append_uint8(avcc, 0xFF);

    // numOfSequenceParameterSets (5 LSB) with 3 MSB set as 111 -> 0xE1 means 1 SPS
    dashstr_buffer_append_uint8(avcc, 0xE1);

    // SPS length (2 bytes BE) + sps data
    dashstr_buffer_append_uint16(avcc, (uint16_t)v->sps_size);
    dashstr_buffer_append(avcc, v->sps, v->sps_size);

    // numOfPictureParameterSets
    dashstr_buffer_append_uint8(avcc, 0x01);
    dashstr_buffer_append_uint16(avcc, (uint16_t)v->pps_size);
    dashstr_buffer_append(avcc, v->pps, v->pps_size);
}


// ----------------- cria hvcC (H.265) minimal payload -----------------
DashDataBuffer* build_hvcC(const VideoInitData* v) 
{
    // HEVCDecoderConfigurationRecord minimal-ish
    // configurationVersion
    DashDataBuffer* out = dashstr_buffer_create(100);
    dashstr_buffer_append_uint8(out, 0x01);

    // general_profile_space (2) + general_tier_flag (1) + general_profile_idc (5)
    // vamos preencher com zeros exceto profile_idc tirado do VPS/SPS se possível (simplificado)
    dashstr_buffer_append_uint8(out, 0x00);

    // general_profile_compatibility_flags (4 bytes)
    dashstr_buffer_append_uint32(out, 0);

    // general_constraint_indicator_flags (6 bytes)
    dashstr_buffer_append_uint8(out, 0x00); dashstr_buffer_append_uint8(out, 0x00); dashstr_buffer_append_uint8(out, 0x00);
    dashstr_buffer_append_uint8(out, 0x00); dashstr_buffer_append_uint8(out, 0x00); dashstr_buffer_append_uint8(out, 0x00);

    // general_level_idc
    dashstr_buffer_append_uint8(out, 0x00);

    // reserved (12 bits) + min_spatial_segmentation_idc (12 bits) -> we'll write 2 bytes zero + 2 bytes zero
    dashstr_buffer_append_uint16(out, 0x00);
    dashstr_buffer_append_uint16(out, 0x00);

    // parallelismType, chromaFormat, bitDepthLumaMinus8, bitDepthChromaMinus8
    dashstr_buffer_append_uint8(out, 0xFC | 0); // parallelism type in low bits
    dashstr_buffer_append_uint8(out, 0xFC | 0); // chroma format
    dashstr_buffer_append_uint8(out, 0xF8 | 0); // luma
    dashstr_buffer_append_uint8(out, 0xF8 | 0); // chroma

    // averageFrameRate
    dashstr_buffer_append_uint16(out, 0x00);

    // constantFrameRate(2 bits), numTemporalLayers(3 bits), temporalIdNested(1 bit), lengthSizeMinusOne(2 bits)
    // set lengthSizeMinusOne = 3 (4 bytes)
    dashstr_buffer_append_uint8(out, 0x01 << 6 | 0x03);

    // numOfArrays (we will include arrays for VPS(32), SPS(33), PPS(34) if present)
    uint8_t numArrays = 0;
    if (v->vps && v->vps_size) numArrays++;
    if (v->sps && v->sps_size) numArrays++;
    if (v->pps && v->pps_size) numArrays++;
    dashstr_buffer_append_uint8(out, numArrays);

    // now arrays
    if (v->vps && v->vps_size) 
    {
        // array_completeness (1) + reserved(1bit) + NAL_unit_type (6 bits) -> store in one byte (we set completeness=1)

        dashstr_buffer_append_uint8(out, (1 << 7) | 32); // VPS has type 32
        dashstr_buffer_append_uint16(out, 1);            // numNalus
        dashstr_buffer_append_uint16(out, (uint16_t)v->vps_size);
        dashstr_buffer_append(out, v->vps, v->vps_size);
    }
    if (v->sps && v->sps_size)
    {
        dashstr_buffer_append_uint8(out, (1 << 7) | 33); // SPS type 33
        dashstr_buffer_append_uint16(out, 1);
        dashstr_buffer_append_uint16(out, (uint16_t)v->sps_size);
        dashstr_buffer_append(out, v->sps, v->sps_size);
    }
    if (v->pps && v->pps_size) 
    {
        dashstr_buffer_append_uint8(out, (1 << 7) | 34); // PPS type 34
        dashstr_buffer_append_uint16(out, 1);
        dashstr_buffer_append_uint16(out, (uint16_t)v->pps_size);
        dashstr_buffer_append(out, v->pps, v->pps_size);
    }
}


DashDataBuffer* build_stsd_box(DashDataBuffer* out, const VideoInitData* v) 
{
    DashDataBuffer* pay          = dashstr_buffer_create(100);
    DashDataBuffer* sample_entry = dashstr_buffer_create(100);

    // version (1 byte) + flags (3 bytes)
    dashstr_buffer_append_uint32(pay, 0);
  
    // entry_count = 1
    dashstr_buffer_append_uint32(pay, 1);

    // build sample entry (avc1 or hev1) into a DynBuf sample_entry
    if (v->hdot_version == HDOT_H264) 
    {
        // avc1 sample entry (payload only, without outer box header)
        // fields: reserved(6) + data_reference_index(2)
        uint8_t reserved6[6] = { 0,0,0,0,0,0 };
        dashstr_buffer_append(sample_entry, reserved6, 6);
        dashstr_buffer_append_uint16(sample_entry,1);

        // pre-defined + reserved + pre-defined (just zeros) 16 bytes
        uint8_t zeros16[16] = { 0 };
        dashstr_buffer_append(sample_entry, zeros16, 16);

        dashstr_buffer_append_uint16(sample_entry, (uint16_t)v->width);
        dashstr_buffer_append_uint16(sample_entry, (uint16_t)v->height);
        dashstr_buffer_append_uint32(sample_entry, 0x00480000);// horiz resolution 72dpi (0x0048 << 16)
        dashstr_buffer_append_uint32(sample_entry, 0x00480000);// vert resolution
        dashstr_buffer_append_uint32(sample_entry, 0); // reserved
        dashstr_buffer_append_uint16(sample_entry, 1);

        // compressorname 32 bytes (fill zeros)
        uint8_t compname[32] = { 0 };
        dashstr_buffer_append(sample_entry, compname, 32);
        dashstr_buffer_append_uint16(sample_entry, 0x0018); // depth
        dashstr_buffer_append_uint16(sample_entry, 0xFFFF); // pre_defined

        DashDataBuffer* avcc = build_avcC(v);

        // wrap avcC as box
        dashstr_buffer_append_uint32(sample_entry, (uint32_t)(8 + avcc->Length));
        dashstr_buffer_append_string(sample_entry,"avcC");

        //db_write_u32be(&sample_entry, (uint32_t)(8 + avcc.len)); // avcC size
        //db_write_fourcc(&sample_entry, "avcC");
        if (avcc->Length)
        {
            dashstr_buffer_append(sample_entry, avcc->Data, avcc->Length);
        }
        dashstr_buffer_release(&avcc);
    }
    else if (v->hdot_version == HDOT_H265) 
    {
        // hev1 sample entry - similar fields to avc1
        uint8_t reserved6[6] = { 0,0,0,0,0,0 };
        dashstr_buffer_append(sample_entry, reserved6, 6);
        dashstr_buffer_append_uint16(sample_entry, 1); // data_reference_index

        uint8_t zeros16[16] = { 0 };
        dashstr_buffer_append(sample_entry, zeros16, 16);
        dashstr_buffer_append_uint16(sample_entry, (uint16_t)v->width);
        dashstr_buffer_append_uint16(sample_entry, (uint16_t)v->height);
        dashstr_buffer_append_uint32(sample_entry, 0x00480000);
        dashstr_buffer_append_uint32(sample_entry, 0x00480000);
        dashstr_buffer_append_uint32(sample_entry, 0);
        dashstr_buffer_append_uint16(sample_entry, 1);

        uint8_t compname[32] = { 0 };
        dashstr_buffer_append(sample_entry, compname, 32);
        dashstr_buffer_append_uint16(sample_entry, 0x0018);
        dashstr_buffer_append_uint16(sample_entry, 0xFFFF);

        DashDataBuffer* hvcc = build_hvcC(v);

        dashstr_buffer_append_uint32(sample_entry, (uint32_t)(8 + hvcc->Length));
        dashstr_buffer_append_string(sample_entry, "hvcC");

        if (hvcc->Length)
        {
            dashstr_buffer_append(sample_entry, hvcc->Data, hvcc->Length);
        }
        dashstr_buffer_release(&hvcc);
    }

    DashDataBuffer* sample_entry_box = dashstr_buffer_create(100);
    const char* type = (v->hdot_version == HDOT_H264) ? "avc1" : "hev1";

    // write size and type manually into sample_entry_box
    dashstr_buffer_append_uint32(sample_entry_box, (uint32_t)(8 + sample_entry->Length));
    dashstr_buffer_append_string(sample_entry_box, type);
    dashstr_buffer_append(sample_entry_box, sample_entry->Data, sample_entry->Length);
    dashstr_buffer_release(&sample_entry);

    // agora adiciona sample_entry_box aos stsd_payload
    dashstr_buffer_append(pay, sample_entry_box->Data, sample_entry_box->Length);
    dashstr_buffer_release(&sample_entry_box);

    // agora embrulha stsd_payload em uma box "stsd"
    dashstr_buffer_append_box_from_buf(out, "stsd", pay);
    dashstr_buffer_release(&pay);
}


// ----------------- monta um moov simples (mvhd + one trak with stsd) -------------
DashDataBuffer* build_moov_box(const VideoInitData* v)
{
    DashDataBuffer* moov_content = dashstr_buffer_create(100);
    DashDataBuffer* mvhd         = dashstr_buffer_create(100);// mvhd (very simplified - version 0)

    // version + flags
    dashstr_buffer_append_uint8(mvhd, 0x00); 
    dashstr_buffer_append_uint8(mvhd, 0x00); 
    dashstr_buffer_append_uint8(mvhd, 0x00); 
    dashstr_buffer_append_uint8(mvhd, 0x00);

    dashstr_buffer_append_uint32(mvhd, 0);//            creation_time
    dashstr_buffer_append_uint32(mvhd, 0);//            modification_time
    dashstr_buffer_append_uint32(mvhd, v->timescale);// timescale
    dashstr_buffer_append_uint32(mvhd, 0);//            duration
    dashstr_buffer_append_uint32(mvhd, 0x00010000);//   rate 1.0
    dashstr_buffer_append_uint16(mvhd, 0x0100);//       volume
    dashstr_buffer_append_uint16(mvhd, 0); //           reserved

    // reserved 2*4
    dashstr_buffer_append_uint32(mvhd, 0); dashstr_buffer_append_uint32(mvhd, 0);

    // matrix (9 * 4 bytes)
    uint32_t matrix[9] = { 0x00010000,0,0,0,0x00010000,0,0,0,0x40000000 };
    for (int i = 0; i < 9; i++)
    {
        dashstr_buffer_append_uint32(mvhd, matrix[i]);
    }

    // pre_defined 6 * 4
    for (int i = 0; i < 6; i++)
    {
        dashstr_buffer_append_uint32(mvhd, 0);
    }

    // next_track_ID
    dashstr_buffer_append_uint32(mvhd, 2);

    // write mvhd box
    dashstr_buffer_append_box_from_buf(moov_content, "mvhd", mvhd);
    dashstr_buffer_release(&mvhd);

    // ---- trak (we will build tkhd + mdia with stsd inside) ----
    DashDataBuffer* trak = dashstr_buffer_create(100);
    DashDataBuffer* tkhd = dashstr_buffer_create(100);// tkhd (minimal)

    // flags: enabled
    dashstr_buffer_append_uint8(tkhd, 0x00);
    dashstr_buffer_append_uint8(tkhd, 0x00);
    dashstr_buffer_append_uint8(tkhd, 0x00);
    dashstr_buffer_append_uint8(tkhd, 0x07);

    dashstr_buffer_append_uint32(tkhd, 0);// creation time
    dashstr_buffer_append_uint32(tkhd, 0);// modification
    dashstr_buffer_append_uint32(tkhd, 1);// track_ID
    dashstr_buffer_append_uint32(tkhd, 0);// reserved
    dashstr_buffer_append_uint32(tkhd, 0);// duration
    dashstr_buffer_append_uint32(tkhd, 0);// reserved
    dashstr_buffer_append_uint32(tkhd, 0);// reserved
    dashstr_buffer_append_uint16(tkhd, 0);// layer
    dashstr_buffer_append_uint16(tkhd, 0);// alternate_group
    dashstr_buffer_append_uint16(tkhd, 0x0100);// volume
    dashstr_buffer_append_uint16(tkhd, 0);// reserved

    // matrix
    for (int i = 0; i < 9; i++)
    {
        dashstr_buffer_append_uint32(tkhd, matrix[i]);
    }
    // width/height fixed 16.16
    dashstr_buffer_append_uint32(tkhd, (uint32_t)(v->width << 16));
    dashstr_buffer_append_uint32(tkhd, (uint32_t)(v->height << 16));
    dashstr_buffer_append_box_from_buf(trak, "tkhd", tkhd);
    dashstr_buffer_release(&tkhd);

    DashDataBuffer* mdia = dashstr_buffer_create(100);// mdia
    DashDataBuffer* mdhd = dashstr_buffer_create(100); // mdhd

    dashstr_buffer_append_uint8(mdhd, 0x00);
    dashstr_buffer_append_uint8(mdhd, 0x00);
    dashstr_buffer_append_uint8(mdhd, 0x00);
    dashstr_buffer_append_uint8(mdhd, 0x00);

    dashstr_buffer_append_uint32(mdhd, 0);
    dashstr_buffer_append_uint32(mdhd, 0);
    dashstr_buffer_append_uint32(mdhd, v->timescale);
    dashstr_buffer_append_uint32(mdhd, 0);
    // language(2) + pre_defined
    dashstr_buffer_append_uint16(mdhd, 0x55C4);
    dashstr_buffer_append_uint16(mdhd, 0);
    dashstr_buffer_append_box_from_buf(mdia, "mdhd", mdhd);
    dashstr_buffer_release(&mdhd);


    DashDataBuffer* hdlr = dashstr_buffer_create(100);// hdlr
    dashstr_buffer_append_uint8(hdlr, 0x00); 
    dashstr_buffer_append_uint8(hdlr, 0x00); 
    dashstr_buffer_append_uint8(hdlr, 0x00); 
    dashstr_buffer_append_uint8(hdlr, 0x00);

    dashstr_buffer_append_uint32(hdlr, 0); 
    dashstr_buffer_append_uint32(hdlr, 0);
    dashstr_buffer_append_string(hdlr, "vide"); // handler_type

    // reserved 3 * 4
    dashstr_buffer_append_uint32(hdlr, 0); 
    dashstr_buffer_append_uint32(hdlr, 0); 
    dashstr_buffer_append_uint32(hdlr, 0);

    // name (null terminated)
    const char name[] = "VideoHandler";
    dashstr_buffer_append(hdlr, name, sizeof(name));
    dashstr_buffer_append_box_from_buf(mdia, "hdlr", hdlr);
    dashstr_buffer_release(&hdlr);


    DashDataBuffer* minf = dashstr_buffer_create(100);// minf
    DashDataBuffer* vmhd = dashstr_buffer_create(100);// vmhd (video media header)
    // version/flags 1
    dashstr_buffer_append_uint8(vmhd, 0x00);
    dashstr_buffer_append_uint8(vmhd, 0x00);
    dashstr_buffer_append_uint8(vmhd, 0x00);
    dashstr_buffer_append_uint8(vmhd, 0x01);
    // graphicsmode + opcolor
    dashstr_buffer_append_uint16(vmhd, 0);
    dashstr_buffer_append_uint16(vmhd, 0);
    dashstr_buffer_append_uint16(vmhd, 0);
    dashstr_buffer_append_box_from_buf(minf, "vmhd", vmhd);
    dashstr_buffer_release(&vmhd);


    DashDataBuffer* dinf = dashstr_buffer_create(100);// dinf (data info) -> dref
    DashDataBuffer* dref = dashstr_buffer_create(100);
    // version/flags
    dashstr_buffer_append_uint8(dref, 0x00);
    dashstr_buffer_append_uint8(dref, 0x00);
    dashstr_buffer_append_uint8(dref, 0x00);
    dashstr_buffer_append_uint8(dref, 0x00);
    dashstr_buffer_append_uint32(dref, 1);// entry_count
    // url box (self-contained)
    DashDataBuffer* url    = dashstr_buffer_create(100);
    DashDataBuffer* urlbox = dashstr_buffer_create(100);
    // flags=1 (self-contained)
    dashstr_buffer_append_uint8(url, 0x00);
    dashstr_buffer_append_uint8(url, 0x00);
    dashstr_buffer_append_uint8(url, 0x00);
    dashstr_buffer_append_uint8(url, 0x01);
    // wrap url in box
    dashstr_buffer_append_box_from_buf(urlbox, "url ", url);
    dashstr_buffer_append(dref, urlbox->Data, urlbox->Length);
    dashstr_buffer_append_box_from_buf(dinf, "dref", dref);
    dashstr_buffer_append_box_from_buf(minf, "dinf", dinf);
    dashstr_buffer_release(&url);
    dashstr_buffer_release(&urlbox);
    dashstr_buffer_release(&dref);
    dashstr_buffer_release(&dinf);


    // stsd (we will build stsd with avc1/hev1)
    DashDataBuffer* stbl = build_stsd_box(&stbl, v);

    // stts, stsc, stsz, stco minimal placeholders (empty but present)
    DashDataBuffer* tmp = dashstr_buffer_create(100);
    dashstr_buffer_append_uint8(tmp, 0); dashstr_buffer_append_uint8(tmp, 0); dashstr_buffer_append_uint8(tmp, 0); dashstr_buffer_append_uint8(tmp, 0); 
    dashstr_buffer_append_uint32(tmp, 0); // entry_count 0
    dashstr_buffer_append_box_from_buf(stbl, "stts", &tmp);

    tmp->Length  = 0;
    tmp->Data[0] = 0;
    dashstr_buffer_append_uint8(tmp, 0); dashstr_buffer_append_uint8(tmp, 0); dashstr_buffer_append_uint8(tmp, 0); dashstr_buffer_append_uint8(tmp, 0);
    dashstr_buffer_append_uint32(tmp, 0); 
    dashstr_buffer_append_box_from_buf(stbl, "stsc", &tmp);

    dashstr_buffer_append_uint8(tmp, 0); dashstr_buffer_append_uint8(tmp, 0); dashstr_buffer_append_uint8(tmp, 0); dashstr_buffer_append_uint8(tmp, 0);
    dashstr_buffer_append_uint32(tmp, 0);
    dashstr_buffer_append_box_from_buf(stbl, "stsz", &tmp);

    dashstr_buffer_append_uint8(tmp, 0); dashstr_buffer_append_uint8(tmp, 0); dashstr_buffer_append_uint8(tmp, 0); dashstr_buffer_append_uint8(tmp, 0);
    dashstr_buffer_append_uint32(tmp, 0);
    dashstr_buffer_append_box_from_buf(stbl, "stso", &tmp);
    dashstr_buffer_release(&tmp);

    DashDataBuffer* out = dashstr_buffer_create(100);

    dashstr_buffer_append_box_from_buf(minf, "stbl", &stbl); // wrap stbl into minf->stbl
    dashstr_buffer_append_box_from_buf(mdia, "minf", &minf);// wrap minf into mdia
    dashstr_buffer_append_box_from_buf(trak, "mdia", &mdia);// finally add mdia into trak
    dashstr_buffer_append_box_from_buf(moov_content, "trak", &trak);  // wrap trak into moov
    dashstr_buffer_append_box_from_buf(out, "moov", &moov_content);  // write moov box to out buffer

    dashstr_buffer_release(stbl);
    dashstr_buffer_release(minf);
    dashstr_buffer_release(mdia);
    dashstr_buffer_release(trak);
    dashstr_buffer_release(moov_content);

    return out;
}







//void write_init_segment(const char* filename, VideoInitData* v)
//{
//    FILE* f = fopen(filename, "wb");
//    if (!f) {
//        perror("Erro ao abrir arquivo");
//        return;
//    }
//
//    // === [ftyp] box ===
//    write_ftyp(f);
//
//    // === [moov] box ===
//    write_moov(f, v);
//
//    fclose(f);
//    printf("Init segment gerado: %s\n", filename);
//}


// ----------------- public function: escreve init_segment para file -----------------
void write_init_segment(const char* filename, const VideoInitData* v) 
{
    FILE* f = fopen(filename, "wb");
    if (!f) { perror("open out"); return; }

    // ftyp
    write_ftyp(f);

    DashDataBuffer* moov = build_moov_box(v);

    // write moov to file
    if (moov->Length) fwrite(moov->Data, 1, moov->Length, f);
    db_free(&moov);

    fclose(f);
    printf("Init segment gravado: %s\n", filename);
}





//
//
//DashDataBuffer* dash_create_init_segment()
//{
//    DashDataBuffer* db = dashstr_buffer_create(48);
//
//    // --- ftyp box ---
//    uint8_t ftyp[] =
//    {
//        0x00,0x00,0x00,0x18,  // size
//        'f','t','y','p',
//        'i','s','o','6',
//        0x00,0x00,0x00,0x00,
//        'i','s','o','6',
//        'm','p','4','1'
//    };
//
//    uint8_t moov[] =
//    {
//        0x00,0x00,0x00,0x10,
//        'm','o','o','v',
//        0x00,0x00,0x00,0x08,
//        'm','v','h','d',
//        0x00,0x00,0x00,0x00, // versão/flags
//        0x00,0x00,0x00,0x01  // timescale
//    };
//
//    memcpy(db->Data, ftyp, sizeof(ftyp));
//    memcpy(db->Data + sizeof(ftyp), moov, sizeof(moov));
//    return db;
//}
//
//
//DashDataBuffer* dash_create_fragment(uint8_t* frame, size_t size)
//{
//    size_t total = size + 20 + 8;
//    DashDataBuffer* db = dashstr_buffer_create(total);
//
//    // --- moof box ---
//    uint8_t moof_header[20] = 
//    {
//        0x00,0x00,0x00,0x10, 'm','o','o','f',
//        0x00,0x00,0x00,0x08, 'm','f','h','d',
//        0x00,0x00,0x00,0x00 // dummy flags
//    };
//
//    uint32_t mdat_size = (uint32_t)(size + 8);
//    uint8_t header[8] = 
//    {
//        (mdat_size >> 24) & 0xFF,
//        (mdat_size >> 16) & 0xFF,
//        (mdat_size >> 8) & 0xFF,
//        (mdat_size) & 0xFF,
//        'm','d','a','t'
//    };
//
//    memcpy(db->Data, moof_header, sizeof(moof_header));
//    memcpy(db->Data+ 20, header, sizeof(header));
//    memcpy(db->Data +28, frame, size);
//    return db;
//}


void dash_extract_frames(char* path_file)
{
    //H264Source* src = open_h264_source("video_init.h264");


}
