#include "MediaFragmenterType.h"



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

