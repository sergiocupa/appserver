#include "MediaFragmenterType.h"


int load_bmp_manual(const char* path, uint8_t** pixels, int* w, int* h)
{
    FILE* f = fopen(path, "rb");
    if (!f) {
        printf("Erro ao abrir BMP\n");
        return -1;
    }

    char sig[2];
    fread(sig, 1, 2, f);
    if (sig[0] != 'B' || sig[1] != 'M') {
        printf("Não é BMP válido\n");
        fclose(f);
        return -1;
    }

    BmpHeader header;
    fread(&header.file_size, 4, 1, f);
    fread(&header.reserved, 4, 1, f);
    fread(&header.data_offset, 4, 1, f);
    fread(&header.header_size, 4, 1, f);
    fread(&header.width, 4, 1, f);
    fread(&header.height, 4, 1, f);
    fread(&header.planes, 2, 1, f);
    fread(&header.bpp, 2, 1, f);

    if (header.bpp != 24) {
        printf("Apenas BMP 24-bit suportado\n");
        fclose(f);
        return -1;
    }

    *w = header.width;
    *h = abs(header.height);

    *pixels = (uint8_t*)malloc((*w) * (*h) * 3);
    if (!*pixels) {
        fclose(f);
        return -1;
    }

    fseek(f, header.data_offset, SEEK_SET);

    int row_bytes = (*w) * 3;
    int padding = (4 - (row_bytes % 4)) % 4;

    uint8_t* row_buf = (uint8_t*)malloc(row_bytes + padding);
    if (!row_buf) {
        fclose(f);
        free(*pixels);
        return -1;
    }

    int bottom_up = (header.height > 0);
    for (int y = 0; y < *h; y++) {
        fread(row_buf, 1, row_bytes + padding, f);

        // destino correto da linha
        int dest_y = bottom_up ? (*h - 1 - y) : y;
        uint8_t* dst = *pixels + dest_y * row_bytes;

        // copia apenas pixels válidos
        for (int i = 0; i < row_bytes; i += 3) {
            // BGR → RGB aqui
            dst[i + 0] = row_buf[i + 2];
            dst[i + 1] = row_buf[i + 1];
            dst[i + 2] = row_buf[i + 0];
        }
    }

    free(row_buf);
    fclose(f);

    return 0;
}


int rgb_to_yuv(uint8_t* rgb, int w, int h, uint8_t** y, uint8_t** u, uint8_t** v, int* stride_y, int* stride_u, int* stride_v)
{
    if (!rgb || w <= 0 || h <= 0 || !y || !u || !v)
        return -1;

    // Strides (1 byte por pixel em cada plano)
    *stride_y = w;
    *stride_u = (w + 1) / 2;  // Teto para larguras ímpares
    *stride_v = (w + 1) / 2;

    int y_size = w * h;
    int uv_size = (*stride_u) * ((h + 1) / 2);  // Teto para alturas ímpares

    // Aloca planos
    *y = (uint8_t*)malloc(y_size);
    *u = (uint8_t*)malloc(uv_size);
    *v = (uint8_t*)malloc(uv_size);
    if (!*y || !*u || !*v)
        return -1;

    // Conversão RGB → YUV (BT.601) com subamostragem média no croma
    for (int j = 0; j < h; j += 2)
    {
        for (int i = 0; i < w; i += 2)
        {
            float sum_U = 0.0f;
            float sum_V = 0.0f;
            int count = 0;

            // Processa o bloco 2x2 (ou menos nos limites)
            for (int dj = 0; dj < 2; dj++)
            {
                int jj = j + dj;
                if (jj >= h) continue;

                for (int di = 0; di < 2; di++)
                {
                    int ii = i + di;
                    if (ii >= w) continue;

                    int idx_rgb = (jj * w + ii) * 3;
                    uint8_t r = rgb[idx_rgb + 0];
                    uint8_t g = rgb[idx_rgb + 1];
                    uint8_t b = rgb[idx_rgb + 2];

                    // Calcula Y (resolução completa)
                    float Yf = 0.299f * r + 0.587f * g + 0.114f * b;
                    int Y_clamped = (int)(Yf + 0.5f);  // Arredonda para o mais próximo
                    (*y)[jj * (*stride_y) + ii] = (uint8_t)(Y_clamped < 0 ? 0 : (Y_clamped > 255 ? 255 : Y_clamped));

                    // Acumula U e V para média
                    float Uf = -0.168736f * r - 0.331264f * g + 0.5f * b + 128.0f;
                    float Vf = 0.5f * r - 0.418688f * g - 0.081312f * b + 128.0f;
                    sum_U += Uf;
                    sum_V += Vf;
                    count++;
                }
            }

            // Média e armazena U/V se o bloco tiver pixels
            if (count > 0)
            {
                float avg_U = sum_U / count;
                float avg_V = sum_V / count;
                int U_clamped = (int)(avg_U + 0.5f);  // Arredonda para o mais próximo
                int V_clamped = (int)(avg_V + 0.5f);
                int uv_index = (j / 2) * (*stride_u) + (i / 2);
                (*u)[uv_index] = (uint8_t)(U_clamped < 0 ? 0 : (U_clamped > 255 ? 255 : U_clamped));
                (*v)[uv_index] = (uint8_t)(V_clamped < 0 ? 0 : (V_clamped > 255 ? 255 : V_clamped));
            }
        }
    }

    return 0;  // Sucesso
}
