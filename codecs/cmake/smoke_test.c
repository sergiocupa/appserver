/* smoke_test.c -- prova que os 4 codecs linkam e inicializam no alvo.
 * Roda nativo (x64) ou sob QEMU (arm64). Sai 0 se tudo ok. */
#include <stdio.h>
#include <string.h>

#include "opus.h"
#define VPX_CODEC_DISABLE_COMPAT 1
#include "vpx/vpx_encoder.h"
#include "vpx/vp8cx.h"
#include "EbSvtAv1Enc.h"
#undef DEFAULT   /* EbSvtAv1Enc.h faz #define DEFAULT -1; colide com enum do x265 */
#include "x265.h"

int main(void)
{
    int fail = 0;
    const char* arch =
#if defined(__aarch64__)
        "arm64";
#elif defined(__x86_64__)
        "x64";
#else
        "?";
#endif
    printf("codecs smoke: arch=%s ptr=%zu bytes\n", arch, sizeof(void*));

    /* Opus */
    int err = 0;
    OpusEncoder* oe = opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &err);
    if (oe && err == OPUS_OK) { printf("  opus  OK (%s)\n", opus_get_version_string()); opus_encoder_destroy(oe); }
    else { printf("  opus  FALHOU (%d)\n", err); fail = 1; }

    /* VP9 (libvpx) -- aqui e onde NEON vs SSE2 se prova por arquitetura */
    vpx_codec_ctx_t ctx; memset(&ctx, 0, sizeof(ctx));
    vpx_codec_enc_cfg_t cfg; memset(&cfg, 0, sizeof(cfg));
    const vpx_codec_iface_t* iface = vpx_codec_vp9_cx();
    if (vpx_codec_enc_config_default(iface, &cfg, 0) == VPX_CODEC_OK) {
        cfg.g_w = 320; cfg.g_h = 240; cfg.g_timebase.num = 1; cfg.g_timebase.den = 30;
        if (vpx_codec_enc_init(&ctx, iface, &cfg, 0) == VPX_CODEC_OK) {
            printf("  vp9   OK (%s)\n", vpx_codec_version_str());
            vpx_codec_destroy(&ctx);
        } else { printf("  vp9   FALHOU (init)\n"); fail = 1; }
    } else { printf("  vp9   FALHOU (cfg)\n"); fail = 1; }

    /* SVT-AV1 */
    EbComponentType* av1 = NULL;
    EbSvtAv1EncConfiguration av1cfg; memset(&av1cfg, 0, sizeof(av1cfg));
    if (svt_av1_enc_init_handle(&av1, NULL, &av1cfg) == EB_ErrorNone && av1) {
        printf("  av1   OK\n");
        svt_av1_enc_deinit_handle(av1);
    } else { printf("  av1   FALHOU\n"); fail = 1; }

    /* x265 (HEVC) */
    const x265_api* api = x265_api_get(0);
    if (api) { printf("  x265  OK (build %d)\n", api->api_build_number); }
    else { printf("  x265  FALHOU\n"); fail = 1; }

    printf(fail ? "RESULTADO: FALHOU\n" : "RESULTADO: OK\n");
    return fail;
}
