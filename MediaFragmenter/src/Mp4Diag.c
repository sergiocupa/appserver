#include "Mp4Diag.h"
#include "MediaFragmenterType.h"
#include "BufferUtil.h"



void mp4diag_display_frame_index(FrameIndexList* frames)
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



int mp4diag_validate_sps_pps(const MediaBuffer* sps, const MediaBuffer* pps)
{
    int ret = 0;

    if (!sps || !pps)
    {
        fprintf(stderr, "SPS ou PPS NULL!\n");
        ret |= 0b0001;
        return ret;
    }

    uint8_t sps_byte0 = sps->Data[0];
    uint8_t sps_type  = sps_byte0 & 0x1F;

    if (sps_type != 7)
    {
        printf("ERRO! Esperado 7 (SPS)\n");
        printf("\n     PROBLEMA IDENTIFICADO: SPS não tem NAL type correto!\n");
        printf("    SOLUÇÃO: O avcC pode estar no formato sem NAL header.\n");
        printf("              Você precisa adicionar 0x67 no início do SPS.\n");
        ret |= 0b0010;
    }

    uint8_t pps_byte0 = pps->Data[0];
    uint8_t pps_type  = pps_byte0 & 0x1F;

    if (pps_type != 8)
    {
        printf(" ERRO! Esperado 8 (PPS)\n");
        printf("\n     PROBLEMA IDENTIFICADO: PPS não tem NAL type correto!\n");
        printf("    SOLUÇÃO: O avcC pode estar no formato sem NAL header.\n");
        printf("              Você precisa adicionar 0x68 no início do PPS.\n");
        ret |= 0b0100;
    }
    return ret;
}




void mp4diag_video_metadata(const VideoMetadata* meta)
{
    if (!meta) {
        fprintf(stderr, "[ERROR] VideoMetadata is NULL!\n");
        return;
    }

    printf("\n");
    printf("========================================================================\n");
    printf("                                                                        \n");
    printf("                    VIDEO METADATA - DETAILED VIEW                     \n");
    printf("                                                                        \n");
    printf("========================================================================\n");
    printf("\n");

    // ========================================================================
    // BASIC INFORMATION
    // ========================================================================

    printf("+-------------------------------------------------------------------+\n");
    printf("| BASIC INFORMATION                                                 |\n");
    printf("+-------------------------------------------------------------------+\n\n");

    // Codec
    printf("  Codec: H.%d", meta->Codec);
    if (meta->Codec == 264) {
        printf(" (H.264/AVC)\n");
    }
    else if (meta->Codec == 265) {
        printf(" (H.265/HEVC)\n");
    }
    else {
        printf(" (Unknown)\n");
    }

    // Resolution
    printf("  Resolution: %d x %d pixels\n", meta->Width, meta->Height);

    // Aspect ratio
    double aspect_ratio = (double)meta->Width / (double)meta->Height;
    printf("  Aspect Ratio: %.2f:1", aspect_ratio);
    if (aspect_ratio > 1.76 && aspect_ratio < 1.79) {
        printf(" (16:9)\n");
    }
    else if (aspect_ratio > 1.32 && aspect_ratio < 1.34) {
        printf(" (4:3)\n");
    }
    else if (aspect_ratio > 2.38 && aspect_ratio < 2.40) {
        printf(" (Cinemascope)\n");
    }
    else {
        printf("\n");
    }

    // FPS
    printf("  Frame Rate: %.2f fps\n", meta->Fps);

    // Length Size
    printf("  NAL Length Size: %d bytes", meta->LengthSize);
    if (meta->LengthSize == 4) {
        printf(" (standard)\n");
    }
    else if (meta->LengthSize == 2) {
        printf(" (compact)\n");
    }
    else {
        printf("\n");
    }

    printf("\n");

    // ========================================================================
    // SPS (SEQUENCE PARAMETER SET)
    // ========================================================================

    printf("+-------------------------------------------------------------------+\n");
    printf("| SPS (SEQUENCE PARAMETER SET)                                      |\n");
    printf("+-------------------------------------------------------------------+\n\n");

    if (!meta->Sps.Data || meta->Sps.Size <= 0) {
        printf("  [ERROR] SPS not available\n\n");
    }
    else {
        printf("  Size: %d bytes\n\n", meta->Sps.Size);

        // Hex dump of SPS
        printf("  Hex Dump:\n");
        printf("  +--------------------------------------------------------------+\n");
        for (int i = 0; i < meta->Sps.Size; i++) {
            if (i % 16 == 0) {
                if (i > 0) printf(" |\n");
                printf("  | %04X: ", i);
            }
            printf("%02X ", meta->Sps.Data[i]);

            // Fill spaces if last line
            if (i == meta->Sps.Size - 1) {
                int remaining = 16 - ((i + 1) % 16);
                if (remaining != 16) {
                    for (int j = 0; j < remaining; j++) {
                        printf("   ");
                    }
                }
                printf(" |\n");
            }
        }
        printf("  +--------------------------------------------------------------+\n\n");

        // SPS Analysis
        if (meta->Sps.Size >= 4) {
            printf("  Analysis:\n");

            uint8_t nal_header = meta->Sps.Data[0];
            uint8_t forbidden_zero = (nal_header >> 7) & 0x01;
            uint8_t nal_ref_idc = (nal_header >> 5) & 0x03;
            uint8_t nal_type = nal_header & 0x1F;

            printf("    * NAL Header: 0x%02X\n", nal_header);
            printf("      +- forbidden_zero_bit: %d %s\n",
                forbidden_zero, forbidden_zero == 0 ? "[OK]" : "[ERROR]");
            printf("      +- nal_ref_idc: %d\n", nal_ref_idc);
            printf("      +- nal_unit_type: %d ", nal_type);

            if (nal_type == 7) {
                printf("[OK] (SPS)\n\n");

                // Profile
                uint8_t profile_idc = meta->Sps.Data[1];
                printf("    * Profile IDC: 0x%02X (", profile_idc);
                switch (profile_idc) {
                case 0x42: printf("Baseline"); break;
                case 0x4D: printf("Main"); break;
                case 0x58: printf("Extended"); break;
                case 0x64: printf("High"); break;
                case 0x6E: printf("High 10"); break;
                case 0x7A: printf("High 4:2:2"); break;
                case 0xF4: printf("High 4:4:4 Predictive"); break;
                default: printf("Unknown"); break;
                }
                printf(")\n");

                // Constraint flags
                uint8_t constraints = meta->Sps.Data[2];
                printf("    * Constraint Flags: 0x%02X\n", constraints);
                printf("      +- constraint_set0_flag: %d\n", (constraints >> 7) & 0x01);
                printf("      +- constraint_set1_flag: %d\n", (constraints >> 6) & 0x01);
                printf("      +- constraint_set2_flag: %d\n", (constraints >> 5) & 0x01);
                printf("      +- constraint_set3_flag: %d\n", (constraints >> 4) & 0x01);
                printf("      +- constraint_set4_flag: %d\n", (constraints >> 3) & 0x01);
                printf("      +- constraint_set5_flag: %d\n", (constraints >> 2) & 0x01);

                // Level
                uint8_t level_idc = meta->Sps.Data[3];
                printf("    * Level IDC: %d (Level %.1f)\n", level_idc, level_idc / 10.0);

            }
            else {
                printf("[ERROR] (Expected 7)\n");
                printf("    [WARNING] This is not a valid SPS!\n\n");
            }
        }
    }

    // ========================================================================
    // PPS (PICTURE PARAMETER SET)
    // ========================================================================

    printf("+-------------------------------------------------------------------+\n");
    printf("| PPS (PICTURE PARAMETER SET)                                       |\n");
    printf("+-------------------------------------------------------------------+\n\n");

    if (!meta->Pps.Data || meta->Pps.Size <= 0)
    {
        printf("  [ERROR] PPS not available\n\n");
    }
    else {
        printf("  Size: %d bytes\n\n", meta->Pps.Size);

        // Hex dump of PPS
        printf("  Hex Dump:\n");
        printf("  +--------------------------------------------------------------+\n");
        for (int i = 0; i < meta->Pps.Size; i++) {
            if (i % 16 == 0) {
                if (i > 0) printf(" |\n");
                printf("  | %04X: ", i);
            }
            printf("%02X ", meta->Pps.Data[i]);

            if (i == meta->Pps.Size - 1) 
            {
                int remaining = 16 - ((i + 1) % 16);
                if (remaining != 16) 
                {
                    for (int j = 0; j < remaining; j++) 
                    {
                        printf("   ");
                    }
                }
                printf(" |\n");
            }
        }
        printf("  +--------------------------------------------------------------+\n\n");

        // PPS Analysis
        if (meta->Pps.Size >= 1)
        {
            printf("  Analysis:\n");

            uint8_t nal_header = meta->Pps.Data[0];
            uint8_t forbidden_zero = (nal_header >> 7) & 0x01;
            uint8_t nal_ref_idc = (nal_header >> 5) & 0x03;
            uint8_t nal_type = nal_header & 0x1F;

            printf("    * NAL Header: 0x%02X\n", nal_header);
            printf("      +- forbidden_zero_bit: %d %s\n",
                forbidden_zero, forbidden_zero == 0 ? "[OK]" : "[ERROR]");
            printf("      +- nal_ref_idc: %d\n", nal_ref_idc);
            printf("      +- nal_unit_type: %d ", nal_type);

            if (nal_type == 8) 
            {
                printf("[OK] (PPS)\n\n");
            }
            else {
                printf("[ERROR] (Expected 8)\n");
                printf("    [WARNING] This is not a valid PPS!\n\n");
            }
        }
    }

    // ========================================================================
    // VALIDATION AND COMPATIBILITY
    // ========================================================================

    printf("+-------------------------------------------------------------------+\n");
    printf("| VALIDATION AND COMPATIBILITY                                      |\n");
    printf("+-------------------------------------------------------------------+\n\n");

    int validation_ok = 1;

    // Validate SPS
    if (!meta->Sps.Data || meta->Sps.Size <= 0) 
    {
        printf("  [ERROR] SPS missing or invalid\n");
        validation_ok = 0;
    }
    else if ((meta->Sps.Data[0] & 0x1F) != 7)
    {
        printf("  [ERROR] SPS does not have NAL type 7 (has %d)\n", meta->Sps.Data[0] & 0x1F);
        validation_ok = 0;
    }
    else
    {
        printf("  [OK] SPS valid (NAL type 7)\n");
    }

    // Validate PPS
    if (!meta->Pps.Data || meta->Pps.Size <= 0) 
    {
        printf("  [ERROR] PPS missing or invalid\n");
        validation_ok = 0;
    }
    else if ((meta->Pps.Data[0] & 0x1F) != 8)
    {
        printf("  [ERROR] PPS does not have NAL type 8 (has %d)\n", meta->Pps.Data[0] & 0x1F);
        validation_ok = 0;
    }
    else
    {
        printf("  [OK] PPS valid (NAL type 8)\n");
    }

    // OpenH264 compatibility
    if (meta->Sps.Data && meta->Sps.Size >= 2)
    {
        uint8_t profile = meta->Sps.Data[1];
        printf("  ");

        if (profile == 0x42 || profile == 0x4D || profile == 0x64) 
        {
            printf("[OK] Profile compatible with OpenH264\n");
        }
        else if (profile == 0x6E || profile == 0x7A || profile == 0xF4)
        {
            printf("[WARNING] Profile may NOT be supported by OpenH264\n");
            printf("    (High 10, High 4:2:2, High 4:4:4 may fail)\n");
            validation_ok = 0;
        }
        else {
            printf("[WARNING] Unknown profile (0x%02X)\n", profile);
        }
    }

    // Validate resolution
    if (meta->Width <= 0 || meta->Height <= 0) 
    {
        printf("  [ERROR] Invalid resolution\n");
        validation_ok = 0;
    }
    else if (meta->Width > 7680 || meta->Height > 4320) 
    {
        printf("  [WARNING] Very high resolution (>8K), may have performance issues\n");
    }
    else 
    {
        printf("  [OK] Valid resolution\n");
    }

    // Validate FPS
    if (meta->Fps <= 0 || meta->Fps > 240)
    {
        printf("  [WARNING] Unusual frame rate: %.2f fps\n", meta->Fps);
    }
    else 
    {
        printf("  [OK] Valid frame rate\n");
    }

    printf("\n");

    // ========================================================================
    // FINAL SUMMARY
    // ========================================================================

    printf("+-------------------------------------------------------------------+\n");
    printf("| SUMMARY                                                           |\n");
    printf("+-------------------------------------------------------------------+\n\n");

    if (validation_ok) 
    {
        printf("  [SUCCESS] ALL DATA IS VALID\n");
        printf("            Ready for decoding!\n");
    }
    else {
        printf("  [WARNING] ISSUES FOUND\n");
        printf("            Check the problems above before decoding.\n");
    }

    printf("\n");
    printf("========================================================================\n");
    printf("                           END OF REPORT                             \n");
    printf("========================================================================\n");
    printf("\n");
}







// ════════════════════════════════════════════════════════════════════════════
// ANÁLISE DE BOXES ESPECÍFICOS
// ════════════════════════════════════════════════════════════════════════════

void analyze_ftyp(BufferReader* br, size_t start, uint32_t size, int depth) {
    char indent[100] = { 0 };
    for (int i = 0; i < depth; i++) strcat(indent, "  ");

    buffer_seek(br, start + 8);

    char major_brand[5] = { 0 };
    memcpy(major_brand, buffer_current(br), 4);
    buffer_skip(br, 4);

    uint32_t minor_version = buffer_read32(br);

    printf("%s  major_brand: %s\n", indent, major_brand);
    printf("%s  minor_version: %u\n", indent, minor_version);
    printf("%s  compatible_brands: ", indent);

    int count = 0;
    while (buffer_tell(br) < start + size) {
        char brand[5] = { 0 };
        memcpy(brand, buffer_current(br), 4);
        buffer_skip(br, 4);
        if (count > 0) printf(", ");
        printf("%s", brand);
        count++;
    }
    printf("\n");
}

void analyze_mvhd(BufferReader* br, size_t start, uint32_t size, int depth) {
    char indent[100] = { 0 };
    for (int i = 0; i < depth; i++) strcat(indent, "  ");

    buffer_seek(br, start + 8);

    uint8_t version = buffer_read8(br);
    buffer_skip(br, 3); // flags

    printf("%s  version: %u\n", indent, version);

    if (version == 1) {
        buffer_skip(br, 16); // creation + modification time
        uint32_t timescale = buffer_read32(br);
        uint64_t duration = buffer_read64(br);
        printf("%s  timescale: %u\n", indent, timescale);
        printf("%s  duration: %llu (%.2f seconds)\n", indent, duration,
            (double)duration / timescale);
    }
    else {
        buffer_skip(br, 8); // creation + modification time
        uint32_t timescale = buffer_read32(br);
        uint32_t duration = buffer_read32(br);
        printf("%s  timescale: %u\n", indent, timescale);
        printf("%s  duration: %u (%.2f seconds)\n", indent, duration,
            (double)duration / timescale);
    }
}

void analyze_tkhd(BufferReader* br, size_t start, uint32_t size, int depth) {
    char indent[100] = { 0 };
    for (int i = 0; i < depth; i++) strcat(indent, "  ");

    buffer_seek(br, start + 8);

    uint8_t version = buffer_read8(br);
    uint32_t flags = buffer_read32(br) & 0xFFFFFF;

    printf("%s  version: %u\n", indent, version);
    printf("%s  flags: 0x%06X ", indent, flags);
    if (flags & 0x000001) printf("(enabled) ");
    if (flags & 0x000002) printf("(in-movie) ");
    if (flags & 0x000004) printf("(in-preview) ");
    printf("\n");

    if (version == 1) {
        buffer_skip(br, 16); // creation + modification
        uint32_t track_id = buffer_read32(br);
        printf("%s  track_id: %u\n", indent, track_id);
    }
    else {
        buffer_skip(br, 8); // creation + modification
        uint32_t track_id = buffer_read32(br);
        printf("%s  track_id: %u\n", indent, track_id);
    }
}

void analyze_mdhd(BufferReader* br, size_t start, uint32_t size, int depth) {
    char indent[100] = { 0 };
    for (int i = 0; i < depth; i++) strcat(indent, "  ");

    buffer_seek(br, start + 8);

    uint8_t version = buffer_read8(br);
    buffer_skip(br, 3); // flags

    printf("%s  version: %u\n", indent, version);

    if (version == 1) {
        buffer_skip(br, 16); // creation + modification
        uint32_t timescale = buffer_read32(br);
        uint64_t duration = buffer_read64(br);
        printf("%s  timescale: %u ← CRÍTICO!\n", indent, timescale);
        printf("%s  duration: %llu\n", indent, duration);
    }
    else {
        buffer_skip(br, 8); // creation + modification
        uint32_t timescale = buffer_read32(br);
        uint32_t duration = buffer_read32(br);
        printf("%s  timescale: %u ← CRÍTICO!\n", indent, timescale);
        printf("%s  duration: %u\n", indent, duration);
    }
}

void analyze_hdlr(BufferReader* br, size_t start, uint32_t size, int depth) {
    char indent[100] = { 0 };
    for (int i = 0; i < depth; i++) strcat(indent, "  ");

    buffer_seek(br, start + 8);

    buffer_skip(br, 4); // version + flags
    buffer_skip(br, 4); // pre_defined

    char handler[5] = { 0 };
    memcpy(handler, buffer_current(br), 4);
    buffer_skip(br, 4);

    printf("%s  handler_type: %s\n", indent, handler);
}

void analyze_trex(BufferReader* br, size_t start, uint32_t size, int depth) {
    char indent[100] = { 0 };
    for (int i = 0; i < depth; i++) strcat(indent, "  ");

    buffer_seek(br, start + 8);

    buffer_skip(br, 4); // version + flags
    uint32_t track_id = buffer_read32(br);
    uint32_t desc_index = buffer_read32(br);
    uint32_t duration = buffer_read32(br);
    uint32_t sample_size = buffer_read32(br);
    uint32_t sample_flags = buffer_read32(br);

    printf("%s  track_id: %u\n", indent, track_id);
    printf("%s  default_sample_description_index: %u\n", indent, desc_index);
    printf("%s  default_sample_duration: %u ", indent, duration);
    if (duration == 0) {
        printf("← ❌ CRÍTICO! DEVE SER > 0\n");
    }
    else {
        printf("← ✓\n");
    }
    printf("%s  default_sample_size: %u\n", indent, sample_size);
    printf("%s  default_sample_flags: 0x%08X ", indent, sample_flags);
    if (sample_flags == 0x01010000) {
        printf("← ⚠️  PROBLEMA!\n");
    }
    else {
        printf("← ✓\n");
    }
}

void analyze_mehd(BufferReader* br, size_t start, uint32_t size, int depth) {
    char indent[100] = { 0 };
    for (int i = 0; i < depth; i++) strcat(indent, "  ");

    buffer_seek(br, start + 8);

    uint8_t version = buffer_read8(br);
    buffer_skip(br, 3); // flags

    if (version == 1) {
        uint64_t duration = buffer_read64(br);
        printf("%s  fragment_duration: %llu\n", indent, duration);
    }
    else {
        uint32_t duration = buffer_read32(br);
        printf("%s  fragment_duration: %u\n", indent, duration);
    }
}

void analyze_avcc(BufferReader* br, size_t start, uint32_t size, int depth) {
    char indent[100] = { 0 };
    for (int i = 0; i < depth; i++) strcat(indent, "  ");

    buffer_seek(br, start + 8);

    uint8_t config_version = buffer_read8(br);
    uint8_t profile = buffer_read8(br);
    uint8_t compat = buffer_read8(br);
    uint8_t level = buffer_read8(br);
    uint8_t length_size_minus_one = buffer_read8(br);

    int nal_size = (length_size_minus_one & 0x3) + 1;

    printf("%s  configurationVersion: %u\n", indent, config_version);
    printf("%s  AVCProfileIndication: %u (0x%02X)\n", indent, profile, profile);
    printf("%s  profile_compatibility: %u (0x%02X)\n", indent, compat, compat);
    printf("%s  AVCLevelIndication: %u (0x%02X)\n", indent, level, level);
    printf("%s  lengthSizeMinusOne: %u → NAL length = %d bytes ",
        indent, length_size_minus_one, nal_size);
    if (nal_size != 4) {
        printf("← ⚠️  Deveria ser 4!\n");
    }
    else {
        printf("← ✓\n");
    }

    uint8_t num_sps = buffer_read8(br) & 0x1F;
    printf("%s  numOfSequenceParameterSets: %u\n", indent, num_sps);

    for (int i = 0; i < num_sps; i++) {
        uint16_t sps_len = buffer_read16(br);
        printf("%s    SPS[%d]: %u bytes\n", indent, i, sps_len);
        buffer_skip(br, sps_len);
    }

    uint8_t num_pps = buffer_read8(br);
    printf("%s  numOfPictureParameterSets: %u\n", indent, num_pps);

    for (int i = 0; i < num_pps; i++) {
        uint16_t pps_len = buffer_read16(br);
        printf("%s    PPS[%d]: %u bytes\n", indent, i, pps_len);
        buffer_skip(br, pps_len);
    }
}

// ════════════════════════════════════════════════════════════════════════════
// ANALISADOR RECURSIVO
// ════════════════════════════════════════════════════════════════════════════

void analyze_box(BufferReader* br, size_t end, int depth);

int is_container(const char* type) {
    const char* containers[] = {
        "moov", "trak", "mdia", "minf", "stbl", "mvex",
        "traf", "moof", "stsd", "dinf", NULL
    };

    for (int i = 0; containers[i]; i++) {
        if (strcmp(type, containers[i]) == 0) return 1;
    }
    return 0;
}

void analyze_box(BufferReader* br, size_t end, int depth) {
    char indent[100] = { 0 };
    for (int i = 0; i < depth; i++) strcat(indent, "  ");

    while (buffer_tell(br) < end && !buffer_eof(br)) {
        size_t box_start = buffer_tell(br);

        if (!buffer_available(br, 8)) break;

        uint32_t size32 = buffer_read32(br);
        char type[5] = { 0 };
        memcpy(type, buffer_current(br), 4);
        buffer_skip(br, 4);

        uint64_t size = size32;
        size_t header_size = 8;

        if (size32 == 1) {
            if (!buffer_available(br, 8)) break;
            size = buffer_read64(br);
            header_size = 16;
        }
        else if (size32 == 0) {
            size = end - box_start;
        }

        if (size < header_size) break;

        size_t box_end = box_start + size;
        if (box_end > end) box_end = end;

        // Mostrar box
        printf("%s[%s] size=%llu offset=0x%zX", indent, type, size, box_start);

        // Análise específica
        if (strcmp(type, "ftyp") == 0) {
            printf("\n");
            analyze_ftyp(br, box_start, size, depth);
        }
        else if (strcmp(type, "mvhd") == 0) {
            printf("\n");
            analyze_mvhd(br, box_start, size, depth);
        }
        else if (strcmp(type, "tkhd") == 0) {
            printf("\n");
            analyze_tkhd(br, box_start, size, depth);
        }
        else if (strcmp(type, "mdhd") == 0) {
            printf(" ← TIMESCALE AQUI!\n");
            analyze_mdhd(br, box_start, size, depth);
        }
        else if (strcmp(type, "hdlr") == 0) {
            printf("\n");
            analyze_hdlr(br, box_start, size, depth);
        }
        else if (strcmp(type, "trex") == 0) {
            printf(" ← CRÍTICO!\n");
            analyze_trex(br, box_start, size, depth);
        }
        else if (strcmp(type, "mehd") == 0) {
            printf("\n");
            analyze_mehd(br, box_start, size, depth);
        }
        else if (strcmp(type, "avcC") == 0) {
            printf(" ← CODEC CONFIG!\n");
            analyze_avcc(br, box_start, size, depth);
        }
        else if (strcmp(type, "avc1") == 0) {
            printf(" ← VIDEO SAMPLE ENTRY\n");
        }
        else {
            printf("\n");
        }

        // Recursão para containers
        if (is_container(type)) {
            size_t payload_start = box_start + header_size;
            buffer_seek(br, payload_start);
            analyze_box(br, box_end, depth + 1);
        }

        buffer_seek(br, box_end);
    }
}

void mp4diag_analyze_init(const uint8_t* data, size_t size)
{
    printf("════════════════════════════════════════════════════════════════════════════\n");
    printf("ANÁLISE COMPLETA DO INIT.MP4 - TODOS OS BOXES\n");
    printf("════════════════════════════════════════════════════════════════════════════\n");
    printf("\nTamanho total: %zu bytes\n\n", size);

    BufferReader br;
    buffer_init(&br, data, size);

    analyze_box(&br, size, 0);

    printf("\n════════════════════════════════════════════════════════════════════════════\n");
    printf("FIM DA ANÁLISE\n");
    printf("════════════════════════════════════════════════════════════════════════════\n");
}






// ════════════════════════════════════════════════════════════════════════════
// ANÁLISE DE BOXES ESPECÍFICOS DE FRAGMENTOS
// ════════════════════════════════════════════════════════════════════════════

void analyze_mfhd(BufferReader* br, size_t start, uint32_t size, int depth) {
    char indent[100] = { 0 };
    for (int i = 0; i < depth; i++) strcat(indent, "  ");

    buffer_seek(br, start + 8);

    buffer_skip(br, 4); // version + flags
    uint32_t sequence = buffer_read32(br);

    printf("%s  sequence_number: %u\n", indent, sequence);
}

void analyze_tfhd(BufferReader* br, size_t start, uint32_t size, int depth) {
    char indent[100] = { 0 };
    for (int i = 0; i < depth; i++) strcat(indent, "  ");

    buffer_seek(br, start + 8);

    uint32_t version_flags = buffer_read32(br);
    uint32_t flags = version_flags & 0xFFFFFF;
    uint32_t track_id = buffer_read32(br);

    printf("%s  flags: 0x%06X\n", indent, flags);
    printf("%s  track_id: %u\n", indent, track_id);

    if (flags & 0x000001) {
        uint64_t base_offset = buffer_read64(br);
        printf("%s  base_data_offset: %llu\n", indent, base_offset);
    }
    if (flags & 0x000002) {
        uint32_t desc_index = buffer_read32(br);
        printf("%s  sample_description_index: %u\n", indent, desc_index);
    }
    if (flags & 0x000008) {
        uint32_t duration = buffer_read32(br);
        printf("%s  default_sample_duration: %u\n", indent, duration);
    }
    if (flags & 0x000010) {
        uint32_t size = buffer_read32(br);
        printf("%s  default_sample_size: %u\n", indent, size);
    }
    if (flags & 0x000020) {
        uint32_t sample_flags = buffer_read32(br);
        printf("%s  default_sample_flags: 0x%08X\n", indent, sample_flags);
    }
}

void analyze_tfdt(BufferReader* br, size_t start, uint32_t size, int depth) {
    char indent[100] = { 0 };
    for (int i = 0; i < depth; i++) strcat(indent, "  ");

    buffer_seek(br, start + 8);

    uint8_t version = buffer_read8(br);
    buffer_skip(br, 3); // flags

    printf("%s  version: %u\n", indent, version);

    if (version == 1) {
        uint64_t time = buffer_read64(br);
        printf("%s  baseMediaDecodeTime: %llu\n", indent, time);
    }
    else {
        uint32_t time = buffer_read32(br);
        printf("%s  baseMediaDecodeTime: %u\n", indent, time);
    }
}

void analyze_trun(BufferReader* br, size_t start, uint32_t size, int depth) {
    char indent[100] = { 0 };
    for (int i = 0; i < depth; i++) strcat(indent, "  ");

    buffer_seek(br, start + 8);

    uint32_t version_flags = buffer_read32(br);
    uint8_t version = (version_flags >> 24) & 0xFF;
    uint32_t flags = version_flags & 0xFFFFFF;
    uint32_t sample_count = buffer_read32(br);

    printf("%s  version: %u\n", indent, version);
    printf("%s  flags: 0x%06X\n", indent, flags);
    printf("%s  sample_count: %u\n", indent, sample_count);

    printf("%s  flags decoded:\n", indent);
    printf("%s    data-offset:           %s\n", indent, (flags & 0x000001) ? "YES" : "NO");
    printf("%s    first-sample-flags:    %s", indent, (flags & 0x000004) ? "YES" : "NO");
    if (flags & 0x000004) {
        printf(" ← ⚠️  Pode causar problemas!\n");
    }
    else {
        printf(" ← ✓\n");
    }
    printf("%s    sample-duration:       %s\n", indent, (flags & 0x000100) ? "YES" : "NO");
    printf("%s    sample-size:           %s\n", indent, (flags & 0x000200) ? "YES" : "NO");
    printf("%s    sample-flags:          %s\n", indent, (flags & 0x000400) ? "YES" : "NO");
    printf("%s    composition-offset:    %s\n", indent, (flags & 0x000800) ? "YES" : "NO");

    if (flags & 0x000001) {
        uint32_t data_offset = buffer_read32(br);
        printf("%s  data_offset: %u (0x%X)\n", indent, data_offset, data_offset);
    }

    if (flags & 0x000004) {
        uint32_t first_flags = buffer_read32(br);
        printf("%s  first_sample_flags: 0x%08X\n", indent, first_flags);
    }
}

void analyze_mdat(BufferReader* br, size_t start, uint32_t size, int depth) {
    char indent[100] = { 0 };
    for (int i = 0; i < depth; i++) strcat(indent, "  ");

    buffer_seek(br, start + 8);

    uint32_t data_size = size - 8;
    printf("%s  data_size: %u bytes\n", indent, data_size);

    // Analisar primeiros bytes
    const uint8_t* first = buffer_current(br);
    size_t len = (data_size < 16) ? data_size : 16;

    printf("%s  first 16 bytes: ", indent);
    for (size_t i = 0; i < len; i++) {
        printf("%02x ", first[i]);
    }
    printf("\n");

    // Detectar formato
    if (len >= 4) {
        if ((first[0] == 0x00 && first[1] == 0x00 &&
            first[2] == 0x00 && first[3] == 0x01) ||
            (first[0] == 0x00 && first[1] == 0x00 && first[2] == 0x01)) {
            printf("%s  ❌ CRÍTICO: Formato Annex-B detectado!\n", indent);
            printf("%s     MP4 deve usar AVCC (length prefix)!\n", indent);
        }
        else {
            uint32_t nal_len = (first[0] << 24) | (first[1] << 16) |
                (first[2] << 8) | first[3];
            if (nal_len > 0 && nal_len < data_size) {
                printf("%s  ✓ Formato AVCC detectado\n", indent);
                printf("%s  first NAL length: %u\n", indent, nal_len);

                if (len >= 5) {
                    uint8_t nal_type = first[4] & 0x1F;
                    printf("%s  first NAL type: %u ", indent, nal_type);
                    if (nal_type == 5) printf("(IDR/keyframe) ✓\n");
                    else if (nal_type == 1) printf("(Non-IDR) ⚠️\n");
                    else if (nal_type == 6) printf("(SEI)\n");
                    else if (nal_type == 7) printf("(SPS)\n");
                    else if (nal_type == 8) printf("(PPS)\n");
                    else printf("(type %u)\n", nal_type);
                }
            }
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
// ANALISADOR RECURSIVO
// ════════════════════════════════════════════════════════════════════════════


int is_frag_container(const char* type) 
{
    const char* containers[] = {
        "moof", "traf", NULL
    };

    for (int i = 0; containers[i]; i++) {
        if (strcmp(type, containers[i]) == 0) return 1;
    }
    return 0;
}

void analyze_frag_box(BufferReader* br, size_t end, int depth) 
{
    char indent[100] = { 0 };
    for (int i = 0; i < depth; i++) strcat(indent, "  ");

    while (buffer_tell(br) < end && !buffer_eof(br)) {
        size_t box_start = buffer_tell(br);

        if (!buffer_available(br, 8)) break;

        uint32_t size32 = buffer_read32(br);
        char type[5] = { 0 };
        memcpy(type, buffer_current(br), 4);
        buffer_skip(br, 4);

        uint64_t size = size32;
        size_t header_size = 8;

        if (size32 == 1) {
            if (!buffer_available(br, 8)) break;
            size = buffer_read64(br);
            header_size = 16;
        }
        else if (size32 == 0) {
            size = end - box_start;
        }

        if (size < header_size) break;

        size_t box_end = box_start + size;
        if (box_end > end) box_end = end;

        // Mostrar box
        printf("%s[%s] size=%llu offset=0x%zX", indent, type, size, box_start);

        // Análise específica
        if (strcmp(type, "mfhd") == 0) {
            printf("\n");
            analyze_mfhd(br, box_start, size, depth);
        }
        else if (strcmp(type, "tfhd") == 0) {
            printf("\n");
            analyze_tfhd(br, box_start, size, depth);
        }
        else if (strcmp(type, "tfdt") == 0) {
            printf("\n");
            analyze_tfdt(br, box_start, size, depth);
        }
        else if (strcmp(type, "trun") == 0) {
            printf(" ← CRÍTICO!\n");
            analyze_trun(br, box_start, size, depth);
        }
        else if (strcmp(type, "mdat") == 0) {
            printf(" ← DADOS DE VÍDEO\n");
            analyze_mdat(br, box_start, size, depth);
        }
        else {
            printf("\n");
        }

        // Recursão para containers
        if (is_frag_container(type)) {
            size_t payload_start = box_start + header_size;
            buffer_seek(br, payload_start);
            analyze_box(br, box_end, depth + 1);
        }

        buffer_seek(br, box_end);
    }
}

void mp4diag_analyze_fragment(const uint8_t* data, size_t size) {
    printf("════════════════════════════════════════════════════════════════════════════\n");
    printf("ANÁLISE COMPLETA DO FRAGMENT - TODOS OS BOXES\n");
    printf("════════════════════════════════════════════════════════════════════════════\n");
    printf("\nTamanho total: %zu bytes\n\n", size);

    BufferReader br;
    buffer_init(&br, data, size);

    analyze_frag_box(&br, size, 0);

    printf("\n════════════════════════════════════════════════════════════════════════════\n");
    printf("FIM DA ANÁLISE\n");
    printf("════════════════════════════════════════════════════════════════════════════\n");
}