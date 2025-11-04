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




int __rgb_to_yuv(uint8_t* rgb, int w, int h,
    uint8_t** y, uint8_t** u, uint8_t** v,
    int* stride_y, int* stride_u, int* stride_v)
{
    if (!rgb || !y || !u || !v || w <= 0 || h <= 0)
        return -1;

    int strideY = w;
    int strideU = (w + 1) / 2;
    int strideV = (w + 1) / 2;
    int h_uv = (h + 1) / 2;

    *y = (uint8_t*)malloc(strideY * h);
    *u = (uint8_t*)malloc(strideU * h_uv);
    *v = (uint8_t*)malloc(strideV * h_uv);

    if (!*y || !*u || !*v) {
        free(*y); free(*u); free(*v);
        return -2;
    }

    *stride_y = strideY;
    *stride_u = strideU;
    *stride_v = strideV;

    // Converter RGB → YUV (ITU-R BT.601)
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            int idx = (j * w + i) * 3;
            uint8_t r = rgb[idx + 0];
            uint8_t g = rgb[idx + 1];
            uint8_t b = rgb[idx + 2];

            int Y = (66 * r + 129 * g + 25 * b + 128) >> 8;
            int U = (-38 * r - 74 * g + 112 * b + 128) >> 8;
            int V = (112 * r - 94 * g - 18 * b + 128) >> 8;

            Y = Y + 16;
            U = U + 128;
            V = V + 128;

            if (Y < 0) Y = 0; else if (Y > 255) Y = 255;
            if (U < 0) U = 0; else if (U > 255) U = 255;
            if (V < 0) V = 0; else if (V > 255) V = 255;

            (*y)[j * strideY + i] = (uint8_t)Y;
        }
    }

    // Subamostrar U e V corretamente (média 2x2)
    for (int j = 0; j < h_uv; j++) {
        for (int i = 0; i < strideU; i++) {
            int sumU = 0, sumV = 0, count = 0;

            for (int dy = 0; dy < 2; dy++) {
                for (int dx = 0; dx < 2; dx++) {
                    int x = i * 2 + dx;
                    int y = j * 2 + dy;
                    if (x < w && y < h) {
                        int idx = (y * w + x) * 3;
                        uint8_t r = rgb[idx + 0];
                        uint8_t g = rgb[idx + 1];
                        uint8_t b = rgb[idx + 2];

                        int U = (-38 * r - 74 * g + 112 * b + 128) >> 8;
                        int V = (112 * r - 94 * g - 18 * b + 128) >> 8;
                        U += 128; V += 128;

                        sumU += U;
                        sumV += V;
                        count++;
                    }
                }
            }

            (*u)[j * strideU + i] = (uint8_t)(sumU / count);
            (*v)[j * strideV + i] = (uint8_t)(sumV / count);
        }
    }

    return 0;
}



// esta gerando pequenas sombras em borda de branco para azul. 
int rgb_to_yuv(uint8_t* rgb, int w, int h, uint8_t** y, uint8_t** u, uint8_t** v, int* stride_y, int* stride_u, int* stride_v)
{
    if (!rgb || w <= 0 || h <= 0 || !y || !u || !v)
        return -1;

    // Strides (1 byte por pixel em cada plano)
    *stride_y = w;
    *stride_u = w / 2;
    *stride_v = w / 2;

    int y_size = w * h;
    int uv_size = (w / 2) * (h / 2);

    // Aloca planos
    *y = (uint8_t*)malloc(y_size);
    *u = (uint8_t*)malloc(uv_size);
    *v = (uint8_t*)malloc(uv_size);
    if (!*y || !*u || !*v)
        return -1;

    memset(*y, 0, y_size);
    memset(*u, 0, uv_size);
    memset(*v, 0, uv_size);

    // Conversão RGB → YUV (BT.601)
    for (int j = 0; j < h; j++)
    {
        for (int i = 0; i < w; i++)
        {
            int idx_rgb = (j * w + i) * 3;

            uint8_t r = rgb[idx_rgb + 0];
            uint8_t g = rgb[idx_rgb + 1];
            uint8_t b = rgb[idx_rgb + 2];

            // Cálculo YUV (BT.601)
            float Yf = 0.299f * r + 0.587f * g + 0.114f * b;
            float Uf = -0.168736f * r - 0.331264f * g + 0.5f * b + 128.0f;
            float Vf = 0.5f * r - 0.418688f * g - 0.081312f * b + 128.0f;

            (*y)[j * (*stride_y) + i] = (uint8_t)(
                Yf < 0 ? 0 : (Yf > 255 ? 255 : Yf)
                );

            // Subamostragem 4:2:0 — U e V a cada bloco 2x2
            if ((j % 2 == 0) && (i % 2 == 0))
            {
                int uv_index = (j / 2) * (*stride_u) + (i / 2);
                (*u)[uv_index] = (uint8_t)(Uf < 0 ? 0 : (Uf > 255 ? 255 : Uf)
                    );
                (*v)[uv_index] = (uint8_t)(Vf < 0 ? 0 : (Vf > 255 ? 255 : Vf));
            }
        }
    }

    return 0;
}


