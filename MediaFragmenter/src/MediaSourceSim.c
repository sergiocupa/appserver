#include "../include/MediaFragmenter.h"
#include "MediaSourceSim.h"
#include "MediaFragmenterType.h"



void* event_loop_thread(void* arg)
{
    VideoOutput* v = (VideoOutput*)arg;
    v->win = SDL_CreateWindow("DASH Player Simulator", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, v->Width, v->Height, 0);
    v->ren = SDL_CreateRenderer(v->win, -1, 0);
    v->tex = SDL_CreateTexture(v->ren, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, v->Width, v->Height);
    //v->tex = SDL_CreateTexture(v->ren, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STATIC, v->Width, v->Height);

    SetEvent(v->WaitShow);

    while (v->Running)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event)) 
        {
            if (event.type == SDL_QUIT) 
            {
                v->Running = 0;
                if (v->Quit) v->Quit();
                
                if(v->Wait) SetEvent(v->Wait);
            }
            else if (event.type == SDL_KEYDOWN) 
            {
                if (v->KeyDown) v->KeyDown(event.key.keysym.sym);
            }
        }
        SDL_Delay(10);  // Evita 100% CPU
    }
}

static VideoOutput* video_output_create(int w, int h)
{
    SDL_Init(SDL_INIT_VIDEO);
    VideoOutput* v = calloc(1, sizeof(VideoOutput));
    v->Width  = w;
    v->Height = h;

    v->WaitShow = CreateEvent(NULL, TRUE, FALSE, NULL);

    v->Running     = true;
    v->EventThread = _beginthread(event_loop_thread, 0, (void*)v);

    WaitForSingleObject(v->WaitShow, INFINITE);// espera mostrar tela
    return v;
}

static void video_output_show(VideoOutput* v, unsigned char** planes, SBufferInfo* info)
{
    SDL_UpdateYUVTexture
    (v->tex, NULL,
        planes[0], info->UsrData.sSystemBuffer.iStride[0],
        planes[1], info->UsrData.sSystemBuffer.iStride[1],
        planes[2], info->UsrData.sSystemBuffer.iStride[2]
    );
    SDL_RenderClear(v->ren);
    SDL_RenderCopy(v->ren, v->tex, NULL, NULL);
    SDL_RenderPresent(v->ren);
    SDL_Delay(33);
}

static void video_output_destroy(VideoOutput* v)
{
    SDL_DestroyTexture(v->tex);
    SDL_DestroyRenderer(v->ren);
    SDL_DestroyWindow(v->win);
    SDL_Quit();
    free(v);
}



static ISVCDecoder* h264_decoder_create()
{
    ISVCDecoder* decoder = NULL;

    int rv = WelsCreateDecoder(&decoder);
    if (rv != 0 || !decoder)
    {
        free(decoder);
        return NULL;
    }

    SDecodingParam param;
    memset(&param, 0, sizeof(SDecodingParam));
    param.uiTargetDqLayer = 0xFF;      // Todas as camadas
    param.eEcActiveIdc = ERROR_CON_DISABLE;
    param.bParseOnly = false;
    param.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;

    int rsv = (*decoder)->Initialize(decoder,&param);
    if (rsv != 0) 
    {
        (*decoder)->Uninitialize(decoder);
        WelsDestroyDecoder(decoder);
        free(decoder);
        return NULL;
    }

    return decoder;
}


static void h264_decoder_feed(ISVCDecoder* decoder, unsigned char* data, int len, VideoOutput* out)
{
    unsigned char* planes[3] = { NULL, NULL, NULL };
    SBufferInfo info;
    memset(&info, 0, sizeof(info));

    DECODING_STATE state = (*decoder)->DecodeFrame2(decoder, data, len, planes, &info);
    if (state == 0 && info.iBufferStatus == 1) 
    {
        video_output_show(out, planes, &info);
    }
}

static void h264_decoder_destroy(ISVCDecoder* decoder)
{
    (*decoder)->Uninitialize(decoder);
    WelsDestroyDecoder(decoder);
    free(decoder);
}


MediaSourceSession* media_sim_create(int width, int height)
{
    MediaSourceSession* source = malloc(sizeof(MediaSourceSession));
    source->Decoder = h264_decoder_create();
    source->Output  = video_output_create(width, height);
    return source;
}

void media_sim_init_segment(MediaSourceSession* source, MediaBuffer* data)
{
    h264_decoder_feed(source->Decoder, data->Data, data->Length, source->Output);
}

void media_sim_feed(MediaSourceSession* source, MediaBuffer* data)
{
    h264_decoder_feed(source->Decoder, data->Data, data->Length, source->Output);
}

void media_sim_release(MediaSourceSession** source)
{
    if (*source)
    {
        h264_decoder_destroy((*source)->Decoder);
        video_output_destroy((*source)->Output);
        free(*source);
        *source = 0;
    }
}

void medias_waiting(VideoOutput* v)
{
    v->Wait = CreateEvent(NULL, TRUE, FALSE, NULL);
    // Em outra thread: SetEvent(hEvent);
    WaitForSingleObject(v->Wait, INFINITE);  // Na main
    CloseHandle(v->Wait);


}








int concod_send_initial_header_from_meta_02(ISVCDecoder* decoder, const VideoMetadata* meta)
{
    if (!decoder || !meta) {
        fprintf(stderr, "ERRO: Decoder ou metadata NULL\n");
        return -1;
    }

    if (!meta->sps || meta->sps_len <= 0 || !meta->pps || meta->pps_len <= 0) {
        fprintf(stderr, "ERRO: SPS ou PPS inválido\n");
        return -1;
    }

    printf("\n=== ENVIANDO SPS/PPS AO DECODER ===\n");
    printf("SPS: %d bytes\n", meta->sps_len);
    printf("PPS: %d bytes\n", meta->pps_len);

    // ========================================================================
    // MÉTODO 1: Enviar SPS e PPS JUNTOS em um único buffer Annex-B
    // ========================================================================
    // Este é o método mais compatível com OpenH264

    size_t total_size = 8 + meta->sps_len + meta->pps_len; // 2x start codes + dados
    uint8_t* buffer = malloc(total_size);
    if (!buffer) {
        fprintf(stderr, "ERRO: Falha ao alocar buffer\n");
        return -1;
    }

    int pos = 0;

    // Start code + SPS
    buffer[pos++] = 0x00;
    buffer[pos++] = 0x00;
    buffer[pos++] = 0x00;
    buffer[pos++] = 0x01;
    memcpy(buffer + pos, meta->sps, meta->sps_len);
    pos += meta->sps_len;

    // Start code + PPS
    buffer[pos++] = 0x00;
    buffer[pos++] = 0x00;
    buffer[pos++] = 0x00;
    buffer[pos++] = 0x01;
    memcpy(buffer + pos, meta->pps, meta->pps_len);
    pos += meta->pps_len;

    printf("Buffer total: %d bytes\n", pos);
    printf("Primeiros bytes: ");
    for (int i = 0; i < (pos < 20 ? pos : 20); i++) {
        printf("%02X ", buffer[i]);
    }
    printf("...\n");

    // ========================================================================
    // CRÍTICO: Usar NULL nos planes para indicar que é apenas configuração
    // ========================================================================
    uint8_t* planes[3] = { NULL, NULL, NULL };
    SBufferInfo info;
    memset(&info, 0, sizeof(SBufferInfo));

    // Primeira tentativa: enviar buffer completo
    DECODING_STATE state = (*decoder)->DecodeFrame2(decoder, buffer, pos, planes, &info);

    printf("Estado após DecodeFrame2: %d (bufferStatus=%d)\n", state, info.iBufferStatus);

    free(buffer);

    // ========================================================================
    // Análise do resultado
    // ========================================================================
    // Estados possíveis do OpenH264:
    // 0 (dsErrorFree) = Sucesso
    // 1 (dsFramePending) = Frame pendente (também é sucesso para param sets)
    // 16 (dsNoParamSets) = ERRO que queremos evitar

    if (state == 16 || state == dsNoParamSets) {
        fprintf(stderr, "\n❌ ERRO 16: Decoder rejeitou SPS/PPS!\n\n");

        // ====================================================================
        // FALLBACK: Tentar método alternativo enviando separadamente
        // ====================================================================
        fprintf(stderr, "Tentando método alternativo (envio separado)...\n");

        // Enviar SPS sozinho
        uint8_t sps_buf[512];
        int sps_pos = 0;
        sps_buf[sps_pos++] = 0x00;
        sps_buf[sps_pos++] = 0x00;
        sps_buf[sps_pos++] = 0x00;
        sps_buf[sps_pos++] = 0x01;
        memcpy(sps_buf + sps_pos, meta->sps, meta->sps_len);
        sps_pos += meta->sps_len;

        memset(&info, 0, sizeof(SBufferInfo));
        state = (*decoder)->DecodeFrame2(decoder, sps_buf, sps_pos, planes, &info);
        printf("  SPS: estado = %d\n", state);

        if (state == 16) {
            fprintf(stderr, "❌ SPS também foi rejeitado!\n");
            fprintf(stderr, "\nPOSSÍVEIS CAUSAS:\n");
            fprintf(stderr, "1. Decoder não foi inicializado corretamente\n");
            fprintf(stderr, "2. Versão do OpenH264 incompatível\n");
            fprintf(stderr, "3. SPS tem erro de sintaxe (improvável, pois NAL type está correto)\n");
            fprintf(stderr, "4. Falta configurar opções do decoder antes\n");
            return -2;
        }

        // Enviar PPS sozinho
        uint8_t pps_buf[256];
        int pps_pos = 0;
        pps_buf[pps_pos++] = 0x00;
        pps_buf[pps_pos++] = 0x00;
        pps_buf[pps_pos++] = 0x00;
        pps_buf[pps_pos++] = 0x01;
        memcpy(pps_buf + pps_pos, meta->pps, meta->pps_len);
        pps_pos += meta->pps_len;

        memset(&info, 0, sizeof(SBufferInfo));
        state = (*decoder)->DecodeFrame2(decoder, pps_buf, pps_pos, planes, &info);
        printf("  PPS: estado = %d\n", state);

        if (state == 16) {
            fprintf(stderr, "❌ PPS também foi rejeitado!\n");
            return -2;
        }

        printf("✓ Método alternativo funcionou!\n");
    }

    // Estados 0 ou 1 são sucesso para param sets
    if (state != 0 && state != 1) {
        fprintf(stderr, "⚠️  Estado inesperado: %d (continuando...)\n", state);
    }

    printf("✓ SPS/PPS configurados com sucesso\n\n");
    return 0;
}







// Função auxiliar: remove prefixo de tamanho se existir
static int strip_length_prefix(const uint8_t* input, int input_len,
    uint8_t* output, int* output_len,
    int length_size)
{
    if (!input || !output || input_len < 1) {
        return -1;
    }

    // Se length_size for 0, assumir que não há prefixo
    if (length_size == 0) {
        memcpy(output, input, input_len);
        *output_len = input_len;
        return 0;
    }

    // Verificar se o primeiro byte parece ser um NAL type válido
    uint8_t first_byte = input[0];
    uint8_t nal_type = first_byte & 0x1F;

    // NAL types válidos para SPS/PPS: 7, 8
    if (nal_type == 7 || nal_type == 8) {
        // Já é um NAL direto, sem prefixo de tamanho
        memcpy(output, input, input_len);
        *output_len = input_len;
        return 0;
    }

    // Caso contrário, pode ter prefixo de tamanho
    // Tentar pular os primeiros 'length_size' bytes
    if (input_len <= length_size) {
        fprintf(stderr, "AVISO: Input muito curto para ter prefixo\n");
        memcpy(output, input, input_len);
        *output_len = input_len;
        return 0;
    }

    // Ler o tamanho do prefixo
    int nal_length = 0;
    for (int i = 0; i < length_size; i++) {
        nal_length = (nal_length << 8) | input[i];
    }

    // Verificar se o tamanho faz sentido
    if (nal_length + length_size != input_len) {
        fprintf(stderr, "AVISO: Tamanho no prefixo (%d) não corresponde ao esperado\n", nal_length);
        // Usar dados originais mesmo assim
        memcpy(output, input, input_len);
        *output_len = input_len;
        return 0;
    }

    // Copiar apenas os dados do NAL (pulando o prefixo)
    memcpy(output, input + length_size, nal_length);
    *output_len = nal_length;

    printf("  → Removido prefixo de tamanho (%d bytes)\n", length_size);
    return 0;
}



// ============================================================================
// VERSÃO FINAL - SPS/PPS ESTÃO CORRETOS!
// ============================================================================
//
// Os dados extraídos estão perfeitos:
// SPS: 67 64 00 28... (NAL type 7) ✓
// PPS: 68 EB E2 4B... (NAL type 8) ✓
//
// O erro 4 deve estar no ENVIO ao decoder. Esta versão testa
// TODOS os métodos possíveis até encontrar o que funciona.
//
// ============================================================================


int find_nal_unit(const uint8_t* data, int len, const uint8_t** nal_data, int* nal_len)
{
    if (!data || len < 1 || !nal_data || !nal_len) {
        return 0;
    }

    *nal_data = data;
    *nal_len = len;

    // Verifica 00 00 00 01
    if (len >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) {
        *nal_data = data + 4;
        *nal_len = len - 4;
        printf("DEBUG: Start code encontrado: 00 00 00 01 (4 bytes)\n");
        return 1;
    }

    // Verifica 00 00 01
    if (len >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1) {
        *nal_data = data + 3;
        *nal_len = len - 3;
        printf("DEBUG: Start code encontrado: 00 00 01 (3 bytes)\n");
        return 1;
    }

    // Nenhum start code: assume que é o NAL puro (comum em AVCC)
    printf("DEBUG: Nenhum start code encontrado. NAL puro (sem prefixo).\n");
    return 1; // ainda válido, só não tem start code
}



int concod_send_initial_header_from_meta(ISVCDecoder* decoder, const VideoMetadata* meta)
{
    if (!decoder || !meta) {
        fprintf(stderr, "ERRO: Decoder ou metadata NULL\n");
        return -1;
    }

    if (!meta->sps || meta->sps_len <= 0 || !meta->pps || meta->pps_len <= 0) {
        fprintf(stderr, "ERRO: SPS ou PPS inválido\n");
        return -1;
    }


    printf("\n╔═══════════════════════════════════════╗\n");
    printf("║  CONFIGURANDO DECODER                ║\n");
    printf("╚═══════════════════════════════════════╝\n");
    printf("SPS: %d bytes (NAL type %d)\n", meta->sps_len, meta->sps[0] & 0x1F);
    printf("PPS: %d bytes (NAL type %d)\n\n", meta->pps_len, meta->pps[0] & 0x1F);

    uint8_t* planes[3] = { NULL, NULL, NULL };
    SBufferInfo info;
    DECODING_STATE state;
    int method_worked = 0;

    // ════════════════════════════════════════════════════════════════════════
    // MÉTODO 1: SPS e PPS SEPARADOS com start code 4-byte (00 00 00 01)
    // ════════════════════════════════════════════════════════════════════════
    printf("▶ Método 1: SPS e PPS separados (4-byte start code)...\n");

    //// Enviar SPS
    uint8_t sps_buf[512];
    int sps_pos = 0;
    //sps_buf[sps_pos++] = 0x00;
    //sps_buf[sps_pos++] = 0x00;
    //sps_buf[sps_pos++] = 0x00;
    //sps_buf[sps_pos++] = 0x01;
    //memcpy(sps_buf + sps_pos, meta->sps, meta->sps_len);
    //sps_pos += meta->sps_len;

    //memset(&info, 0, sizeof(SBufferInfo));
    //state = (*decoder)->DecodeFrame2(decoder, sps_buf, sps_pos, planes, &info);
    //printf("  SPS: estado = %d\n", state);

    //if (state == 0 || state == 1) {
    //    // Enviar PPS
    //    uint8_t pps_buf[256];
    //    int pps_pos = 0;
    //    pps_buf[pps_pos++] = 0x00;
    //    pps_buf[pps_pos++] = 0x00;
    //    pps_buf[pps_pos++] = 0x00;
    //    pps_buf[pps_pos++] = 0x01;
    //    memcpy(pps_buf + pps_pos, meta->pps, meta->pps_len);
    //    pps_pos += meta->pps_len;

    //    memset(&info, 0, sizeof(SBufferInfo));
    //    state = (*decoder)->DecodeFrame2(decoder, pps_buf, pps_pos, planes, &info);
    //    printf("  PPS: estado = %d\n", state);

    //    if (state == 0 || state == 1) {
    //        printf("  ✓ SUCESSO!\n\n");
    //        return 0;
    //    }
    //}
    //printf("  ✗ Falhou (SPS=%d)\n\n", state);

    //// ════════════════════════════════════════════════════════════════════════
    //// MÉTODO 2: SPS e PPS separados com start code 3-byte (00 00 01)
    //// ════════════════════════════════════════════════════════════════════════
    //printf("▶ Método 2: SPS e PPS separados (3-byte start code)...\n");

    //// Enviar SPS com start code curto
    //sps_pos = 0;
    //sps_buf[sps_pos++] = 0x00;
    //sps_buf[sps_pos++] = 0x00;
    //sps_buf[sps_pos++] = 0x01;
    //memcpy(sps_buf + sps_pos, meta->sps, meta->sps_len);
    //sps_pos += meta->sps_len;

    //memset(&info, 0, sizeof(SBufferInfo));
    //state = (*decoder)->DecodeFrame2(decoder, sps_buf, sps_pos, planes, &info);
    //printf("  SPS: estado = %d\n", state);

    //if (state == 0 || state == 1) {
    //    // Enviar PPS
    //    uint8_t pps_buf[256];
    //    int pps_pos = 0;
    //    pps_buf[pps_pos++] = 0x00;
    //    pps_buf[pps_pos++] = 0x00;
    //    pps_buf[pps_pos++] = 0x01;
    //    memcpy(pps_buf + pps_pos, meta->pps, meta->pps_len);
    //    pps_pos += meta->pps_len;

    //    memset(&info, 0, sizeof(SBufferInfo));
    //    state = (*decoder)->DecodeFrame2(decoder, pps_buf, pps_pos, planes, &info);
    //    printf("  PPS: estado = %d\n", state);

    //    if (state == 0 || state == 1) {
    //        printf("  ✓ SUCESSO!\n\n");
    //        return 0;
    //    }
    //}
    //printf("  ✗ Falhou\n\n");


    // Processa SPS
    const uint8_t* sps_nal;
    int sps_nal_len;
    if (!find_nal_unit(meta->sps, meta->sps_len, &sps_nal, &sps_nal_len)) {
        fprintf(stderr, "SPS inválido\n");
        return -1;
    }

    // Processa PPS
    const uint8_t* pps_nal;
    int pps_nal_len;
    if (!find_nal_unit(meta->pps, meta->pps_len, &pps_nal, &pps_nal_len)) {
        fprintf(stderr, "PPS inválido\n");
        return -1;
    }



    // ════════════════════════════════════════════════════════════════════════
    // MÉTODO 3: SPS+PPS juntos com start code 4-byte
    // ════════════════════════════════════════════════════════════════════════
    printf("▶ Método 3: SPS+PPS juntos (4-byte start code)...\n");

    size_t buf_size = 8 + meta->sps_len + meta->pps_len;
    uint8_t* buffer = malloc(buf_size);
    if (!buffer) return -1;

    int pos = 0;
    buffer[pos++] = 0x00;
    buffer[pos++] = 0x00;
    buffer[pos++] = 0x00;
    buffer[pos++] = 0x01;
    memcpy(buffer + pos, sps_nal, sps_nal_len);
    pos += sps_nal_len;

    buffer[pos++] = 0x00;
    buffer[pos++] = 0x00;
    buffer[pos++] = 0x00;
    buffer[pos++] = 0x01;
    memcpy(buffer + pos, pps_nal, pps_nal_len);
    pos += pps_nal_len;

    memset(&info, 0, sizeof(SBufferInfo));
    state = (*decoder)->DecodeFrame2(decoder, buffer, pos, planes, &info);
    printf("  Estado: %d\n", state);
    free(buffer);

    if (state == 0 || state == 1) {
        printf("  ✓ SUCESSO!\n\n");
        return 0;
    }
    printf("  ✗ Falhou\n\n");

    // ════════════════════════════════════════════════════════════════════════
    // MÉTODO 4: SPS+PPS juntos com start code 3-byte
    // ════════════════════════════════════════════════════════════════════════
    printf("▶ Método 4: SPS+PPS juntos (3-byte start code)...\n");

    buffer = malloc(buf_size - 2);
    if (!buffer) return -1;

    pos = 0;
    buffer[pos++] = 0x00;
    buffer[pos++] = 0x00;
    buffer[pos++] = 0x01;
    memcpy(buffer + pos, meta->sps, meta->sps_len);
    pos += meta->sps_len;

    buffer[pos++] = 0x00;
    buffer[pos++] = 0x00;
    buffer[pos++] = 0x01;
    memcpy(buffer + pos, meta->pps, meta->pps_len);
    pos += meta->pps_len;

    memset(&info, 0, sizeof(SBufferInfo));
    state = (*decoder)->DecodeFrame2(decoder, buffer, pos, planes, &info);
    printf("  Estado: %d\n", state);
    free(buffer);

    if (state == 0 || state == 1) {
        printf("  ✓ SUCESSO!\n\n");
        return 0;
    }
    printf("  ✗ Falhou\n\n");

    // ════════════════════════════════════════════════════════════════════════
    // MÉTODO 5: Apenas SPS primeiro, aguardar, depois PPS
    // ════════════════════════════════════════════════════════════════════════
    printf("▶ Método 5: SPS sozinho, depois PPS...\n");

    // SPS com flush
    sps_pos = 0;
    sps_buf[sps_pos++] = 0x00;
    sps_buf[sps_pos++] = 0x00;
    sps_buf[sps_pos++] = 0x00;
    sps_buf[sps_pos++] = 0x01;
    memcpy(sps_buf + sps_pos, meta->sps, meta->sps_len);
    sps_pos += meta->sps_len;

    memset(&info, 0, sizeof(SBufferInfo));
    state = (*decoder)->DecodeFrame2(decoder, sps_buf, sps_pos, planes, &info);
    printf("  SPS: estado = %d\n", state);

    // Tentar flush (enviar NULL para processar)
    memset(&info, 0, sizeof(SBufferInfo));
    (*decoder)->DecodeFrame2(decoder, NULL, 0, planes, &info);

    // Agora PPS
    uint8_t pps_buf[256];
    int pps_pos = 0;
    pps_buf[pps_pos++] = 0x00;
    pps_buf[pps_pos++] = 0x00;
    pps_buf[pps_pos++] = 0x00;
    pps_buf[pps_pos++] = 0x01;
    memcpy(pps_buf + pps_pos, meta->pps, meta->pps_len);
    pps_pos += meta->pps_len;

    memset(&info, 0, sizeof(SBufferInfo));
    state = (*decoder)->DecodeFrame2(decoder, pps_buf, pps_pos, planes, &info);
    printf("  PPS: estado = %d\n", state);

    if (state == 0 || state == 1) {
        printf("  ✓ SUCESSO!\n\n");
        return 0;
    }
    printf("  ✗ Falhou\n\n");

    // ════════════════════════════════════════════════════════════════════════
    // TODOS OS MÉTODOS FALHARAM
    // ════════════════════════════════════════════════════════════════════════

    printf("╔═══════════════════════════════════════╗\n");
    printf("║  ❌ TODOS OS 5 MÉTODOS FALHARAM     ║\n");
    printf("╚═══════════════════════════════════════╝\n\n");

    printf("Último estado: %d\n", state);
    printf("Códigos:\n");
    printf("  0 = dsErrorFree (sucesso)\n");
    printf("  1 = dsFramePending\n");
    printf("  2 = dsRefLost\n");
    printf("  4 = dsBitstreamError\n");
    printf("  16 = dsNoParamSets\n\n");

    if (state == 4) {
        printf("ERRO 4 (dsBitstreamError) persistiu!\n\n");
        printf("Isso indica problema de COMPATIBILIDADE:\n");
        printf("1. Profile do vídeo:\n");
        printf("   Profile IDC: 0x%02X ", meta->sps[1]);
        switch (meta->sps[1]) {
        case 0x42: printf("(Baseline - OK)\n"); break;
        case 0x4D: printf("(Main - OK)\n"); break;
        case 0x64: printf("(High - OK)\n"); break;
        case 0x6E: printf("(High 10 - ❌ NÃO SUPORTADO!)\n"); break;
        case 0x7A: printf("(High 4:2:2 - ❌ NÃO SUPORTADO!)\n"); break;
        default: printf("(Desconhecido)\n"); break;
        }
        printf("   Level IDC: %d (Level %.1f)\n", meta->sps[3], meta->sps[3] / 10.0);

        printf("\n2. Versão do OpenH264:\n");
        printf("   Execute: pkg-config --modversion openh264\n");
        printf("   Recomendado: >= 2.0.0\n");

        printf("\n3. SOLUÇÃO RECOMENDADA:\n");
        printf("   Recodifique o vídeo:\n");
        printf("   $ ffmpeg -i seu_video.mp4 -c:v libx264 \\\n");
        printf("            -profile:v baseline -level 3.1 \\\n");
        printf("            -pix_fmt yuv420p saida.mp4\n\n");
    }

    return -2;
}