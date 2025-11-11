#include "../include/MediaFragmenter.h"


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
    MediaBuffer* pay = mbuffer_create(100);
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
    MediaBuffer* mvhd = mbuffer_create(100);// mvhd (very simplified - version 0)

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
    MediaBuffer* url = mbuffer_create(100);
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



// ----------------- public function: escreve init_segment para file -----------------
MediaBuffer* concod_read_init_segment(const VideoInitData* vid)
{
    MediaBuffer* moov = build_moov_box(vid);
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