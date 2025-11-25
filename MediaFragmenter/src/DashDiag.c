// ════════════════════════════════════════════════════════════════════════════
// ANALYZE_INIT_BUFFER.C - Analisador de init.mp4 usando Buffer
// ════════════════════════════════════════════════════════════════════════════
//
// Compile: gcc -o analyze_init_buffer analyze_init_buffer.c
// Use:     ./analyze_init_buffer init.mp4
//
// Versão que trabalha com buffer em memória ao invés de FILE*
//
// ════════════════════════════════════════════════════════════════════════════

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Estrutura para gerenciar buffer
typedef struct {
    const uint8_t* data;
    size_t size;
    size_t pos;
} BufferReader;

// Inicializar buffer reader
static void buffer_init(BufferReader* br, const uint8_t* data, size_t size) {
    br->data = data;
    br->size = size;
    br->pos = 0;
}

// Verificar se há bytes disponíveis
static int buffer_available(BufferReader* br, size_t n) {
    return (br->pos + n) <= br->size;
}

// Ler uint32 big-endian
static uint32_t buffer_read32(BufferReader* br) {
    if (!buffer_available(br, 4)) return 0;
    uint32_t val = (br->data[br->pos] << 24) |
        (br->data[br->pos + 1] << 16) |
        (br->data[br->pos + 2] << 8) |
        (br->data[br->pos + 3]);
    br->pos += 4;
    return val;
}

// Ler uint16 big-endian
static uint16_t buffer_read16(BufferReader* br) {
    if (!buffer_available(br, 2)) return 0;
    uint16_t val = (br->data[br->pos] << 8) | br->data[br->pos + 1];
    br->pos += 2;
    return val;
}

// Ler uint8
static uint8_t buffer_read8(BufferReader* br) {
    if (!buffer_available(br, 1)) return 0;
    return br->data[br->pos++];
}

// Ler uint64 big-endian
static uint64_t buffer_read64(BufferReader* br) {
    if (!buffer_available(br, 8)) return 0;
    uint64_t val = ((uint64_t)br->data[br->pos] << 56) |
        ((uint64_t)br->data[br->pos + 1] << 48) |
        ((uint64_t)br->data[br->pos + 2] << 40) |
        ((uint64_t)br->data[br->pos + 3] << 32) |
        ((uint64_t)br->data[br->pos + 4] << 24) |
        ((uint64_t)br->data[br->pos + 5] << 16) |
        ((uint64_t)br->data[br->pos + 6] << 8) |
        (br->data[br->pos + 7]);
    br->pos += 8;
    return val;
}

// Ler bytes
static int buffer_read_bytes(BufferReader* br, uint8_t* dest, size_t n) {
    if (!buffer_available(br, n)) return 0;
    memcpy(dest, br->data + br->pos, n);
    br->pos += n;
    return 1;
}

// Pular bytes
static void buffer_skip(BufferReader* br, size_t n) {
    if (br->pos + n <= br->size) {
        br->pos += n;
    }
    else {
        br->pos = br->size;
    }
}

// Obter posição atual
static size_t buffer_tell(BufferReader* br) {
    return br->pos;
}

// Ir para posição específica
static void buffer_seek(BufferReader* br, size_t pos) {
    if (pos <= br->size) {
        br->pos = pos;
    }
}

// Obter ponteiro para posição atual
static const uint8_t* buffer_current(BufferReader* br) {
    return br->data + br->pos;
}

static void print_hex(const uint8_t* data, size_t len, const char* prefix) {
    for (size_t i = 0; i < len; i++) {
        if (i % 16 == 0) printf("%s", prefix);
        printf("%02x ", data[i]);
        if ((i + 1) % 16 == 0) printf("\n");
    }
    if (len % 16 != 0) printf("\n");
}

void analyze_trex(BufferReader* br, size_t offset) {
    printf("\n");
    printf("═══════════════════════════════════════════════════════════════════════════\n");
    printf("ANALISANDO TREX BOX\n");
    printf("═══════════════════════════════════════════════════════════════════════════\n");

    buffer_seek(br, offset);

    uint32_t size = buffer_read32(br);
    char type[5] = { 0 };
    buffer_read_bytes(br, (uint8_t*)type, 4);

    printf("\nPosição: offset 0x%zX (%zu)\n", offset, offset);
    printf("Tamanho: %u bytes\n", size);
    printf("Tipo: %s\n", type);

    if (strcmp(type, "trex") != 0) {
        printf("❌ ERRO: Não é um trex box!\n");
        return;
    }

    // Ler conteúdo do trex
    uint32_t version_flags = buffer_read32(br);
    uint32_t track_id = buffer_read32(br);
    uint32_t desc_index = buffer_read32(br);
    uint32_t duration = buffer_read32(br);
    uint32_t sample_size = buffer_read32(br);
    uint32_t sample_flags = buffer_read32(br);

    printf("\n────────────────────────────────────────────────────────────────────────────\n");
    printf("CONTEÚDO DO TREX:\n");
    printf("────────────────────────────────────────────────────────────────────────────\n");
    printf("  version/flags:                     0x%08X\n", version_flags);
    printf("  track_id:                          %u\n", track_id);
    printf("  default_sample_description_index:  %u\n", desc_index);
    printf("  default_sample_duration:           %u ", duration);

    int has_problems = 0;

    if (duration == 0) {
        printf("❌ CRÍTICO!\n");
        has_problems = 1;
    }
    else {
        printf("✓\n");
    }

    printf("  default_sample_size:               %u\n", sample_size);
    printf("  default_sample_flags:              0x%08X ", sample_flags);

    if (sample_flags == 0x01010000) {
        printf("⚠️\n");
        has_problems = 1;
    }
    else {
        printf("✓\n");
    }

    // Decodificar flags
    printf("\n────────────────────────────────────────────────────────────────────────────\n");
    printf("DECODIFICAÇÃO DE default_sample_flags (0x%08X):\n", sample_flags);
    printf("────────────────────────────────────────────────────────────────────────────\n");

    int is_leading = (sample_flags >> 26) & 0x3;
    int depends_on = (sample_flags >> 24) & 0x3;
    int is_depended_on = (sample_flags >> 22) & 0x3;
    int has_redundancy = (sample_flags >> 20) & 0x3;
    int is_non_sync = (sample_flags >> 16) & 0x1;

    printf("  Bits 26-27 (is_leading):           %d\n", is_leading);
    printf("  Bits 24-25 (sample_depends_on):    %d ", depends_on);

    if (depends_on == 0) printf("(unknown)\n");
    else if (depends_on == 1) printf("(depends on others = P/B frames)\n");
    else if (depends_on == 2) printf("(independent = I/IDR frames)\n");
    else printf("(reserved)\n");

    printf("  Bits 22-23 (sample_is_depended_on): %d\n", is_depended_on);
    printf("  Bits 20-21 (sample_has_redundancy):  %d\n", has_redundancy);
    printf("  Bit 16 (sample_is_non_sync_sample):  %d ", is_non_sync);

    if (is_non_sync == 0) printf("(is keyframe)\n");
    else printf("(NOT keyframe)\n");

    // Diagnóstico
    printf("\n");
    printf("═══════════════════════════════════════════════════════════════════════════\n");
    printf("DIAGNÓSTICO DO TREX:\n");
    printf("═══════════════════════════════════════════════════════════════════════════\n");

    if (duration == 0) {
        printf("\n❌ PROBLEMA CRÍTICO: default_sample_duration = 0\n\n");
        printf("   Chrome não consegue determinar a duração dos frames!\n");
        printf("   Isso causa CHUNK_DEMUXER_ERROR_APPEND_FAILED.\n\n");
        printf("   SOLUÇÃO:\n");
        printf("   ────────\n");
        printf("   Calcule: default_sample_duration = timescale / fps\n");
        printf("   \n");
        printf("   Exemplos:\n");
        printf("     25 fps @ timescale 12800: 512\n");
        printf("     25 fps @ timescale 90000: 3600\n");
        printf("     30 fps @ timescale 90000: 3000\n\n");
    }

    if (sample_flags == 0x01010000) {
        printf("\n⚠️  PROBLEMA: default_sample_flags = 0x01010000\n\n");
        printf("   Indica que TODOS os samples são dependentes E non-sync.\n");
        printf("   Chrome pode rejeitar fragmentos sem keyframes!\n\n");
        printf("   SOLUÇÃO:\n");
        printf("   ────────\n");
        printf("   Use: 0x00010000 (dependentes por padrão)\n\n");
    }

    if (!has_problems) {
        printf("\n✓ TREX parece correto!\n");
    }
}

void analyze_avcc(BufferReader* br, size_t offset, uint32_t size)
{
    printf("\n");
    printf("═══════════════════════════════════════════════════════════════════════════\n");
    printf("ANALISANDO avcC BOX (Decoder Configuration)\n");
    printf("═══════════════════════════════════════════════════════════════════════════\n");

    buffer_seek(br, offset + 8); // Pular size + 'avcC'

    printf("\nPosição: offset 0x%zX (%zu)\n", offset, offset);
    printf("Tamanho: %u bytes\n", size);

    uint8_t config_version = buffer_read8(br);
    uint8_t avc_profile = buffer_read8(br);
    uint8_t profile_compat = buffer_read8(br);
    uint8_t avc_level = buffer_read8(br);
    uint8_t length_size_minus_one = buffer_read8(br);

    int nal_unit_length_size = (length_size_minus_one & 0x3) + 1;

    printf("\n────────────────────────────────────────────────────────────────────────────\n");
    printf("CONFIGURAÇÃO:\n");
    printf("────────────────────────────────────────────────────────────────────────────\n");
    printf("  configurationVersion:  %u\n", config_version);
    printf("  AVCProfileIndication:  %u (0x%02X)\n", avc_profile, avc_profile);
    printf("  profile_compatibility: %u (0x%02X)\n", profile_compat, profile_compat);
    printf("  AVCLevelIndication:    %u (0x%02X)\n", avc_level, avc_level);
    printf("  lengthSizeMinusOne:    %u → NAL unit length size = %d bytes ",
        length_size_minus_one, nal_unit_length_size);

    int has_problems = 0;

    if (nal_unit_length_size != 4) {
        printf("⚠️\n");
        has_problems = 1;
    }
    else {
        printf("✓\n");
    }

    // SPS
    uint8_t num_sps = buffer_read8(br) & 0x1F;
    printf("\n  Número de SPS:         %u\n", num_sps);

    if (num_sps == 0) {
        printf("    ❌ CRÍTICO: Nenhum SPS encontrado!\n");
        has_problems = 1;
    }

    for (int i = 0; i < num_sps; i++) {
        uint16_t sps_length = buffer_read16(br);
        printf("    SPS #%d: %u bytes\n", i, sps_length);

        const uint8_t* sps_data = buffer_current(br);
        buffer_skip(br, sps_length);

        printf("      Primeiros bytes: ");
        for (int j = 0; j < (sps_length < 8 ? sps_length : 8); j++) {
            printf("%02x ", sps_data[j]);
        }
        printf("\n");

        // Verificar NAL unit header
        uint8_t nal_type = sps_data[0] & 0x1F;
        printf("      NAL unit type: %u ", nal_type);

        if (nal_type == 7) {
            printf("(SPS) ✓\n");
        }
        else {
            printf("(ERRO: deveria ser 7 para SPS!) ❌\n");
            has_problems = 1;
        }
    }

    // PPS
    uint8_t num_pps = buffer_read8(br);
    printf("\n  Número de PPS:         %u\n", num_pps);

    if (num_pps == 0) {
        printf("    ❌ CRÍTICO: Nenhum PPS encontrado!\n");
        has_problems = 1;
    }

    for (int i = 0; i < num_pps; i++) {
        uint16_t pps_length = buffer_read16(br);
        printf("    PPS #%d: %u bytes\n", i, pps_length);

        const uint8_t* pps_data = buffer_current(br);
        buffer_skip(br, pps_length);

        printf("      Primeiros bytes: ");
        for (int j = 0; j < (pps_length < 8 ? pps_length : 8); j++) {
            printf("%02x ", pps_data[j]);
        }
        printf("\n");

        // Verificar NAL unit header
        uint8_t nal_type = pps_data[0] & 0x1F;
        printf("      NAL unit type: %u ", nal_type);

        if (nal_type == 8) {
            printf("(PPS) ✓\n");
        }
        else {
            printf("(ERRO: deveria ser 8 para PPS!) ❌\n");
            has_problems = 1;
        }
    }

    printf("\n");
    printf("═══════════════════════════════════════════════════════════════════════════\n");
    printf("DIAGNÓSTICO DO avcC:\n");
    printf("═══════════════════════════════════════════════════════════════════════════\n");

    if (nal_unit_length_size != 4) {
        printf("\n⚠️  NAL unit length size = %d (esperado: 4)\n", nal_unit_length_size);
        printf("   MP4 fragmentado geralmente usa 4 bytes para length prefix.\n\n");
    }

    if (num_sps == 0 || num_pps == 0) {
        printf("\n❌ CRÍTICO: SPS/PPS ausentes!\n");
        printf("   Chrome não consegue inicializar o decoder sem SPS/PPS.\n\n");
    }

    if (!has_problems) {
        printf("\n✓ avcC parece correto!\n");
    }
}

void find_and_analyze_boxes(BufferReader* br) 
{
    printf("Tamanho do buffer: %zu bytes\n\n", br->size);

    int found_trex = 0;
    int found_avcc = 0;

    while (buffer_tell(br) < br->size) {
        size_t box_start = buffer_tell(br);

        if (!buffer_available(br, 8)) break;

        uint32_t size = buffer_read32(br);
        char type[5] = { 0 };
        buffer_read_bytes(br, (uint8_t*)type, 4);

        uint64_t box_size = size;
        if (size == 1) {
            box_size = buffer_read64(br);
        }
        else if (size == 0) {
            box_size = br->size - box_start;
        }

        if (box_size < 8) break;

        // Procurar trex
        if (strcmp(type, "trex") == 0) {
            found_trex = 1;
            analyze_trex(br, box_start);
        }

        // Procurar avcC
        if (strcmp(type, "avcC") == 0) {
            found_avcc = 1;
            analyze_avcc(br, box_start, size);
        }

        size_t next = box_start + box_size;
        if (next >= br->size) break;
        buffer_seek(br, next);
    }

    printf("\n");
    printf("═══════════════════════════════════════════════════════════════════════════\n");
    printf("RESUMO DA ANÁLISE:\n");
    printf("═══════════════════════════════════════════════════════════════════════════\n");

    if (!found_trex) {
        printf("\n❌ CRÍTICO: trex box não encontrado!\n");
        printf("   Este arquivo não é um MP4 fragmentado válido.\n");
    }

    if (!found_avcc) {
        printf("\n❌ CRÍTICO: avcC box não encontrado!\n");
        printf("   Sem configuração do decoder H.264.\n");
    }

    if (found_trex && found_avcc) {
        printf("\n✓ Estrutura básica do init.mp4 parece correta.\n");
        printf("  Se o vídeo ainda não reproduz, verifique os valores\n");
        printf("  específicos do trex e avcC acima.\n");
    }

    printf("\n");
}

// Função pública para analisar buffer
void analyze_init_buffer(const uint8_t* data, size_t size) 
{
    BufferReader br;
    buffer_init(&br, data, size);
    find_and_analyze_boxes(&br);
}










