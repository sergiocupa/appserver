//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Pipeline embutido: MP4 -> VP9/WebM por resolucao + MPD (DASH). Sem ffmpeg.
//  NAO TESTADO EM RUNTIME. Alguns pontos de ownership do MediaFragmenter estao
//  marcados com TODO(verificar) - confirmar contra a implementacao real.

#include "pipeline.h"
#include "memory_pool.h"   // memop_* (evita declaracao implicita -> ponteiro truncado em x64)
#include "media_codec.h"
#include "mux_webm.h"
#include "audio_aac.h"         // demux+decode AAC do MP4 (self-reporta: sem fdk-aac -> video-only)
#include "mp4_timing.h"        // PTS exato por frame do video (afinacao)

#include "MediaFragmenter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_LIBYUV
#include "libyuv/scale.h"
#endif

// MPD segmentado (SegmentTemplate): init-<id>.webm + chunk-<id>-$Number$.webm por Representation.
static void write_dash_mpd(const char* out_dir, const PipeTrack* tracks, int n, double dur, int seg_ms,
                           int has_audio, const char* vcodec_str)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s\\manifest.mpd", out_dir);
    FILE* f = 0;
    if (fopen_s(&f, path, "wb") != 0 || !f) return;

    char codecs[64];
    if (has_audio) snprintf(codecs, sizeof(codecs), "%s,opus", vcodec_str);   // reps A+V muxadas por pista
    else           snprintf(codecs, sizeof(codecs), "%s", vcodec_str);

    fprintf(f,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<MPD xmlns=\"urn:mpeg:dash:schema:mpd:2011\" type=\"static\" "
        "mediaPresentationDuration=\"PT%.3fS\" minBufferTime=\"PT2S\" "
        "profiles=\"urn:mpeg:dash:profile:isoff-live:2011\">\n"
        "  <Period>\n"
        "    <AdaptationSet mimeType=\"video/webm\" codecs=\"%s\" segmentAlignment=\"true\" startWithSAP=\"1\">\n"
        "      <SegmentTemplate timescale=\"1000\" duration=\"%d\" startNumber=\"1\" "
        "initialization=\"init-$RepresentationID$.webm\" media=\"chunk-$RepresentationID$-$Number$.webm\"/>\n",
        dur, codecs, seg_ms);

    for (int i = 0; i < n; i++)
        fprintf(f,
            "      <Representation id=\"%s\" bandwidth=\"%d\" width=\"%d\" height=\"%d\"/>\n",
            tracks[i].Name, tracks[i].BitrateBps, tracks[i].Width, tracks[i].Height);

    fprintf(f, "    </AdaptationSet>\n  </Period>\n</MPD>\n");
    fclose(f);
}

int pipeline_mp4_to_webm_dash(const char* source_mp4, const char* out_dir,
                              MediaCodec video_codec,
                              const PipeTrack* tracks, int track_count,
                              int fps, double duration_sec)
{
    if (!source_mp4 || !out_dir || !tracks || track_count <= 0) return -1;
    if (fps <= 0) fps = 30;

    // Codec de video de saida (ambos em WebM): VP9 (default) ou AV1.
    const char* mux_codec = (video_codec == MEDIA_CODEC_AV1) ? "V_AV1" : "V_VP9"; // CodecID Matroska
    const char* mpd_codec = (video_codec == MEDIA_CODEC_AV1) ? "av01.0.08M.08" : "vp9";
    if (video_codec != MEDIA_CODEC_AV1) video_codec = MEDIA_CODEC_VP9;

    // 1) Demux: indice de frames + metadados (SPS/PPS/VPS, codec 264/265, dimensoes).
    FrameIndexList* fl = mp4builder_get_frames(source_mp4);
    if (!fl || fl->Count == 0) { if (fl) mframe_list_release(&fl); return -2; }
    VideoMetadata* meta = &fl->Metadata;

    // Afinacao: fps REAL vem do demux (suporta fracionario, ex. 29.97). O param 'fps' e
    // so fallback. vfps (double) governa os timestamps; ifps (int) os encoders/mux.
    double vfps = (meta->Fps > 0.0) ? meta->Fps : (fps > 0 ? (double)fps : 30.0);
    int    ifps = (int)(vfps + 0.5);

    // PTS exato por frame via stts (suporta fps fracionario / VFR). Fallback: vfps constante.
    int64_t* vpts = 0; uint32_t vpts_n = 0;
    mp4_video_pts_ms(source_mp4, &vpts, &vpts_n);
    #define VPTS(i) ((vpts && (uint32_t)(i) < vpts_n) ? vpts[i] : (int64_t)((i) * 1000.0 / vfps + 0.5))
    int64_t last_kf_ms = -1000000;   // keyframe/segmento por TEMPO (~2s), robusto a VFR

    // 2) Decoder H.26x da entrada.
    DecoderInstance* dec = h26x_decoder_create(meta->Codec);
    if (!dec) { mframe_list_release(&fl); return -3; }

    FILE* fp = 0;
    if (fopen_s(&fp, source_mp4, "rb") != 0 || !fp) { h26x_decoder_release(&dec); mframe_list_release(&fl); return -4; }

    // --- Audio (Opus) opcional: exige um decoder AAC da track de audio do MP4 (HAVE_AAC).
    // Sem HAVE_AAC (e/ou sem HAVE_LIBOPUS), segue video-only automaticamente.
    int           has_audio = 0, arate = 48000, achannels = 2;
    uint8_t       opus_head[19]; int opus_head_len = 0;
    void*         aud = 0;          // contexto do decoder AAC de entrada
    MediaEncoder* aenc = 0;
    if (aac_open(source_mp4, &arate, &achannels, &aud) == 0 && aud) has_audio = 1;
    if (has_audio)
    {
        opus_head_len = webm_build_opus_head(achannels, arate, opus_head);
        MediaEncoderParams ap; memset(&ap, 0, sizeof(ap));
        ap.Codec = MEDIA_CODEC_OPUS; ap.SampleRate = arate; ap.Channels = achannels; ap.BitrateBps = 128000;
        aenc = media_encoder_open(&ap);
        if (!aenc) has_audio = 0;   // libopus nao compilada -> video-only
    }

    // 3) Um encoder VP9 + um mux WebM (A+V) por pista de saida.
    MediaEncoder** enc = (MediaEncoder**)memop_calloc_raw(track_count, sizeof(MediaEncoder*));
    WebmMux**      mux = (WebmMux**)memop_calloc_raw(track_count, sizeof(WebmMux*));
    int ok = (enc && mux);
    for (int t = 0; ok && t < track_count; t++)
    {
        MediaEncoderParams p;
        memset(&p, 0, sizeof(p));
        p.Codec = video_codec;             // VP9 ou AV1
        p.Width = tracks[t].Width; p.Height = tracks[t].Height;
        p.BitrateBps = tracks[t].BitrateBps; p.Fps = ifps; p.SpeedPreset = 8;
        enc[t] = media_encoder_open(&p);   // NULL se a lib do codec nao compilada

        // Modo segmentado (DASH): init-<name>.webm + chunk-<name>-N.webm, segmentos de ~2s.
        // Declara a track Opus (has_audio + OpusHead) quando ha audio -> reps A+V.
        mux[t] = webm_open_segmented(out_dir, tracks[t].Name, mux_codec,
                                     tracks[t].Width, tracks[t].Height, ifps, 2000,
                                     has_audio, arate, achannels,
                                     has_audio ? opus_head : 0, opus_head_len);
    }

    // 4) Loop de frames: demux 1 frame -> Annex-B -> decode -> YUV -> (por pista) scale -> encode -> mux.
    MediaBuffer annexb; mbuffer_init(&annexb);
    int64_t frame_no = 0;

    for (uint64_t i = 0; ok && i < fl->Count; i++)
    {
        annexb.Size = 0;
        h26x_put_single_frame(fp, fl->Frames[i], meta, &annexb);

        ImagePlaneList* images = imagep_list_new(4);
        h26x_decode_frames(dec, &annexb, images);   // 0..N frames YUV decodificados

        for (int k = 0; k < images->Count; k++)
        {
            ImagePlane* img = images->Items[k];
            int64_t ts_ms = VPTS(frame_no);                              // PTS exato do frame atual
            int is_key = (frame_no == 0) || (ts_ms - last_kf_ms >= 2000);// keyframe/segmento por tempo
            if (is_key) last_kf_ms = ts_ms;

            for (int t = 0; t < track_count; t++)
            {
                if (!enc[t] || !mux[t]) continue;

                int dw = tracks[t].Width, dh = tracks[t].Height;
                int cw = (dw + 1) / 2, ch = (dh + 1) / 2;
                uint8_t* dy = (uint8_t*)memop_alloc_raw((size_t)dw * dh);
                uint8_t* du = (uint8_t*)memop_alloc_raw((size_t)cw * ch);
                uint8_t* dv = (uint8_t*)memop_alloc_raw((size_t)cw * ch);
                if (!dy || !du || !dv) { memop_free_raw(dy); memop_free_raw(du); memop_free_raw(dv); continue; }

#ifdef HAVE_LIBYUV
                I420Scale(img->Planes[0], img->Strides[0],
                          img->Planes[1], img->Strides[1],
                          img->Planes[2], img->Strides[2],
                          img->Width, img->Height,
                          dy, dw, du, cw, dv, cw,
                          dw, dh, kFilterBilinear);
#else
                // Sem libyuv: so funciona se a fonte ja tiver o tamanho da pista (copia direta).
                if (img->Width == dw && img->Height == dh)
                {
                    for (int y = 0; y < dh; y++) memcpy(dy + (size_t)y * dw, img->Planes[0] + (size_t)y * img->Strides[0], dw);
                    for (int y = 0; y < ch; y++) { memcpy(du + (size_t)y * cw, img->Planes[1] + (size_t)y * img->Strides[1], cw);
                                                   memcpy(dv + (size_t)y * cw, img->Planes[2] + (size_t)y * img->Strides[2], cw); }
                }
                else { memop_free_raw(dy); memop_free_raw(du); memop_free_raw(dv); continue; } // precisa de libyuv p/ redimensionar
#endif
                MediaFrame mf; memset(&mf, 0, sizeof(mf));
                mf.Kind = MEDIA_KIND_VIDEO;
                mf.Width = dw; mf.Height = dh;
                mf.Y = dy; mf.StrideY = dw;
                mf.U = du; mf.StrideU = cw;
                mf.V = dv; mf.StrideV = cw;
                mf.Pts = frame_no; mf.KeyFrame = is_key;

                enc[t]->SendFrame(enc[t], &mf);
                MediaPacket pkt;
                while (enc[t]->ReceivePacket(enc[t], &pkt) == 1)
                {
                    int64_t vms = VPTS(pkt.Pts);   // PTS exato do pacote (stts) — afinacao
                    webm_write_video(mux[t], vms, pkt.KeyFrame, pkt.Data, pkt.Size);
                }

                memop_free_raw(dy); memop_free_raw(du); memop_free_raw(dv);
            }

            // Interleave: grava o audio (Opus) ate o ts do video atual, em todas as pistas.
            if (has_audio && aenc)
            {
                int16_t* pcm; int nsamp; int64_t ats;
                while (aac_read(aud, &pcm, &nsamp, &ats) == 1)
                {
                    MediaFrame af; memset(&af, 0, sizeof(af));
                    af.Kind = MEDIA_KIND_AUDIO; af.SampleRate = arate; af.Channels = achannels;
                    af.Pcm = pcm; af.PcmSamples = nsamp;
                    aenc->SendFrame(aenc, &af);
                    MediaPacket ap;
                    while (aenc->ReceivePacket(aenc, &ap) == 1)
                        for (int t = 0; t < track_count; t++) if (mux[t]) webm_write_audio(mux[t], ap.Pts, ap.Data, ap.Size);
                    if (ats >= ts_ms) break;
                }
            }
            frame_no++;
        }

        imagep_list_release(&images, 1); // TODO(verificar): is_release_items=1 libera os planos?
    }

    // 5) Drain: video (por pista) + audio (compartilhado), depois fecha os muxes.
    //    Cada pacote drenado tambem carimba pelo seu proprio PTS (afinacao).
    for (int t = 0; t < track_count; t++)
    {
        if (enc[t])
        {
            enc[t]->SendFrame(enc[t], 0);   // flush
            MediaPacket pkt;
            while (enc[t]->ReceivePacket(enc[t], &pkt) == 1)
                if (mux[t]) webm_write_video(mux[t], VPTS(pkt.Pts), pkt.KeyFrame, pkt.Data, pkt.Size);
            enc[t]->Close(enc[t]);
        }
    }
    if (aenc)
    {
        aenc->SendFrame(aenc, 0);   // flush audio
        MediaPacket ap;
        while (aenc->ReceivePacket(aenc, &ap) == 1)
            for (int t = 0; t < track_count; t++) if (mux[t]) webm_write_audio(mux[t], ap.Pts, ap.Data, ap.Size);
        aenc->Close(aenc);
    }
    if (aud) aac_close(aud);
    for (int t = 0; t < track_count; t++) if (mux[t]) webm_close(mux[t]);

    if (annexb.Data) memop_free_raw(annexb.Data);  // MediaBuffer de pilha: libera o Data (aloc. via mbuffer_*)
    fclose(fp);
    h26x_decoder_release(&dec);
    mframe_list_release(&fl);
    memop_free_raw(enc); memop_free_raw(mux); memop_free_raw(vpts);
    #undef VPTS

    // 6) MPD (DASH) com SegmentTemplate (init + chunks de ~2s por pista); codecs <video>[,opus].
    write_dash_mpd(out_dir, tracks, track_count, duration_sec, 2000, has_audio, mpd_codec);

    return ok ? 0 : -5;
}
