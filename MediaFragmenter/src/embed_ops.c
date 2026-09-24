//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  HLS (passthrough fMP4) e Converter (WebM VP9/AV1) embutidos, sem ffmpeg.
//  NAO TESTADO EM RUNTIME. Verificar ownership dos MediaBuffer do MediaFragmenter.

#include "embed_ops.h"
#include "memory_pool.h"   // memop_* (evita declaracao implicita -> ponteiro truncado em x64)
#include "mux_webm.h"
#include "mux_mp4.h"
#include "audio_aac.h"
#include "mp4_timing.h"
#include "MediaFragmenter.h"   // mp4builder_* / h26x_* (ajustar include path no .vcxproj)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef HAVE_LIBYUV
#include "libyuv/scale.h"
#endif

// ---- HLS: passthrough H.264/H.265 -> fMP4 + m3u8 --------------------------
int embed_build_hls(const char* source_mp4, const char* base, int frag_seconds,
                    int* out_width, int* out_height, double* out_duration, int* out_segments)
{
    if (frag_seconds <= 0) frag_seconds = 5;

    FrameIndexList* list = mp4builder_get_frames(source_mp4);
    if (!list || list->Count == 0) { if (list) mframe_list_release(&list); return -1; }
    VideoMetadata* meta = &list->Metadata;

    double   fps       = (meta->Fps > 0.0) ? meta->Fps : 30.0;
    uint32_t timescale = (meta->Timescale > 0) ? meta->Timescale : 90000;
    if (out_width)  *out_width  = meta->Width;
    if (out_height) *out_height = meta->Height;
    if (out_duration) *out_duration = list->Count / fps;

    // init.mp4
    MP4InitConfig cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.FragmentDuration = frag_seconds; cfg.TrackID = 1;
    MediaBuffer init; memset(&init, 0, sizeof(init));
    if (mp4builder_create_init(meta, &cfg, &init) != 0 || !init.Data) { mframe_list_release(&list); return -2; }
    {
        char p[1024]; snprintf(p, sizeof(p), "%s\\init.mp4", base);
        FILE* fi = 0; if (fopen_s(&fi, p, "wb") == 0 && fi) { fwrite(init.Data, 1, init.Size, fi); fclose(fi); }
    }
    memop_free_raw(init.Data);

    int total = (int)list->Count;
    int fpf   = (int)(fps * frag_seconds + 0.5); if (fpf < 1) fpf = 1;
    int nfrag = (total + fpf - 1) / fpf;

    FILE* fsrc = 0;
    if (fopen_s(&fsrc, source_mp4, "rb") != 0 || !fsrc) { mframe_list_release(&list); return -3; }

    // playlist.m3u8 (montado em memoria e gravado no fim)
    char* m3u8 = (char*)memop_alloc_raw(256 + (size_t)nfrag * 48);
    int   mlen = 0;
    mlen += sprintf_s(m3u8 + mlen, 256,
        "#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-PLAYLIST-TYPE:VOD\n"
        "#EXT-X-TARGETDURATION:%d\n#EXT-X-MEDIA-SEQUENCE:0\n#EXT-X-MAP:URI=\"init.mp4\"\n",
        frag_seconds);

    for (int i = 0; i < nfrag; i++)
    {
        int start = i * fpf;
        int count = (total - start < fpf) ? (total - start) : fpf;
        if (count <= 0) break;

        MP4FragmentInfo fi; memset(&fi, 0, sizeof(fi));
        fi.SequenceNumber      = i + 1;
        fi.TrackID             = 1;
        fi.Timescale           = timescale;
        fi.BaseMediaDecodeTime = (uint64_t)((double)start * timescale / fps);

        MediaBuffer frag; memset(&frag, 0, sizeof(frag));
        if (mp4builder_create_fragment(fsrc, list, -1, -1, start, count, &fi, &frag) == 0 && frag.Data)
        {
            char p[1024]; snprintf(p, sizeof(p), "%s\\segment_%05d.m4s", base, i);
            FILE* fo = 0; if (fopen_s(&fo, p, "wb") == 0 && fo) { fwrite(frag.Data, 1, frag.Size, fo); fclose(fo); }
            memop_free_raw(frag.Data);
        }
        mlen += sprintf_s(m3u8 + mlen, 48, "#EXTINF:%.3f,\nsegment_%05d.m4s\n", count / fps, i);
    }
    mlen += sprintf_s(m3u8 + mlen, 16, "#EXT-X-ENDLIST\n");

    {
        char p[1024]; snprintf(p, sizeof(p), "%s\\playlist.m3u8", base);
        FILE* fp = 0; if (fopen_s(&fp, p, "wb") == 0 && fp) { fwrite(m3u8, 1, mlen, fp); fclose(fp); }
    }
    memop_free_raw(m3u8);
    fclose(fsrc);
    mframe_list_release(&list);

    if (out_segments) *out_segments = nfrag;
    return 0;
}

// ---- H.264 (OpenH264) -> MKV: helpers de NAL/avcC -------------------------

// Itera NALs Annex-B em [buf,len): retorna ponteiro (sem start code), *nal_len e avanca *pos.
static const uint8_t* next_nal(const uint8_t* buf, int len, int* pos, int* nal_len)
{
    int p = *pos;
    while (p + 2 < len && !(buf[p] == 0 && buf[p + 1] == 0 && buf[p + 2] == 1)) p++;
    if (p + 2 >= len) { *pos = len; return 0; }
    p += 3;                       // pula 00 00 01 (o 00 extra do 00 00 00 01 fica fora do NAL)
    int start = p, q = p;
    while (q + 2 < len && !(buf[q] == 0 && buf[q + 1] == 0 && buf[q + 2] == 1)) q++;
    int end = (q + 2 >= len) ? len : q;
    // remove um 00 de trailer (caso 00 00 00 01 do proximo)
    if (end > start && end < len && buf[end - 1] == 0) end--;
    *nal_len = end - start;
    *pos = end;
    return buf + start;
}

static void put_be32(uint8_t* p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

// ---- Converter: reencode -> WebM unico (VP9/AV1) + Opus -------------------
int embed_transcode_webm(const char* source_mp4, const char* out_path, MediaCodec vcodec)
{
    FrameIndexList* fl = mp4builder_get_frames(source_mp4);
    if (!fl || fl->Count == 0) { if (fl) mframe_list_release(&fl); return -1; }
    VideoMetadata* meta = &fl->Metadata;

    double vfps = (meta->Fps > 0.0) ? meta->Fps : 30.0;
    int    ifps = (int)(vfps + 0.5);
    int    w = meta->Width, h = meta->Height;

    int64_t* vpts = 0; uint32_t vpts_n = 0;
    mp4_video_pts_ms(source_mp4, &vpts, &vpts_n);
    #define VPTS(i) ((vpts && (uint32_t)(i) < vpts_n) ? vpts[i] : (int64_t)((i) * 1000.0 / vfps + 0.5))

    DecoderInstance* dec = h26x_decoder_create(meta->Codec);
    if (!dec) { mframe_list_release(&fl); memop_free_raw(vpts); return -2; }

    // audio opcional
    int has_audio = 0, arate = 48000, achannels = 2; void* aud = 0; MediaEncoder* aenc = 0;
    uint8_t opus_head[19]; int opus_head_len = 0;
    if (aac_open(source_mp4, &arate, &achannels, &aud) == 0 && aud)
    {
        opus_head_len = webm_build_opus_head(achannels, arate, opus_head);
        MediaEncoderParams ap; memset(&ap, 0, sizeof(ap));
        ap.Codec = MEDIA_CODEC_OPUS; ap.SampleRate = arate; ap.Channels = achannels; ap.BitrateBps = 128000;
        aenc = media_encoder_open(&ap);
        has_audio = (aenc != 0);
    }

    const char* mux_codec = (vcodec == MEDIA_CODEC_AV1) ? "V_AV1" : "V_VP9";
    WebmMux* mux = webm_open(out_path, mux_codec, w, h, ifps,
                             has_audio, arate, achannels, has_audio ? opus_head : 0, opus_head_len);

    MediaEncoderParams vp; memset(&vp, 0, sizeof(vp));
    vp.Codec = vcodec; vp.Width = w; vp.Height = h; vp.Fps = ifps;
    vp.BitrateBps = (meta->Width >= 1920) ? 5000000 : 3000000; vp.SpeedPreset = 8;
    MediaEncoder* venc = media_encoder_open(&vp);
    if (!venc || !mux) { if (venc) venc->Close(venc); if (mux) webm_close(mux); if (aenc) aenc->Close(aenc);
                         if (aud) aac_close(aud); h26x_decoder_release(&dec); mframe_list_release(&fl); memop_free_raw(vpts); return -3; }

    FILE* fsrc = 0; fopen_s(&fsrc, source_mp4, "rb");
    MediaBuffer annexb; mbuffer_init(&annexb);
    int64_t frame_no = 0, last_kf = -1000000;

    for (uint64_t i = 0; fsrc && i < fl->Count; i++)
    {
        annexb.Size = 0;
        h26x_put_single_frame(fsrc, fl->Frames[i], meta, &annexb);
        ImagePlaneList* imgs = imagep_list_new(4);
        h26x_decode_frames(dec, &annexb, imgs);

        for (int k = 0; k < imgs->Count; k++)
        {
            ImagePlane* img = imgs->Items[k];
            int64_t ts = VPTS(frame_no);
            int is_key = (frame_no == 0) || (ts - last_kf >= 2000);
            if (is_key) last_kf = ts;

            MediaFrame mf; memset(&mf, 0, sizeof(mf));
            mf.Kind = MEDIA_KIND_VIDEO; mf.Width = w; mf.Height = h;
            mf.Y = img->Planes[0]; mf.StrideY = img->Strides[0];
            mf.U = img->Planes[1]; mf.StrideU = img->Strides[1];
            mf.V = img->Planes[2]; mf.StrideV = img->Strides[2];
            mf.Pts = frame_no; mf.KeyFrame = is_key;

            venc->SendFrame(venc, &mf);
            MediaPacket pkt;
            while (venc->ReceivePacket(venc, &pkt) == 1)
                webm_write_video(mux, VPTS(pkt.Pts), pkt.KeyFrame, pkt.Data, pkt.Size);

            if (has_audio && aenc)
            {
                int16_t* pcm; int ns; int64_t ats;
                while (aac_read(aud, &pcm, &ns, &ats) == 1)
                {
                    MediaFrame af; memset(&af, 0, sizeof(af));
                    af.Kind = MEDIA_KIND_AUDIO; af.SampleRate = arate; af.Channels = achannels; af.Pcm = pcm; af.PcmSamples = ns;
                    aenc->SendFrame(aenc, &af);
                    MediaPacket apk;
                    while (aenc->ReceivePacket(aenc, &apk) == 1) webm_write_audio(mux, apk.Pts, apk.Data, apk.Size);
                    if (ats >= ts) break;
                }
            }
            frame_no++;
        }
        imagep_list_release(&imgs, 1);
    }

    venc->SendFrame(venc, 0);
    MediaPacket pkt;
    while (venc->ReceivePacket(venc, &pkt) == 1) webm_write_video(mux, VPTS(pkt.Pts), pkt.KeyFrame, pkt.Data, pkt.Size);
    venc->Close(venc);
    if (aenc) { aenc->SendFrame(aenc, 0); MediaPacket apk; while (aenc->ReceivePacket(aenc, &apk) == 1) webm_write_audio(mux, apk.Pts, apk.Data, apk.Size); aenc->Close(aenc); }
    webm_close(mux);

    if (annexb.Data) memop_free_raw(annexb.Data);
    if (fsrc) fclose(fsrc);
    if (aud) aac_close(aud);
    h26x_decoder_release(&dec);
    mframe_list_release(&fl);
    memop_free_raw(vpts);
    #undef VPTS
    return 0;
}

// ---- H.264/AVCC helpers (compartilhados por convert MP4 e HLS multi-res) ---
// Monta o avcC a partir de SPS/PPS ja capturados (buffer 'a' com folga, >=600B).
static int build_avcc(const uint8_t* sps, int sps_len, const uint8_t* pps, int pps_len, uint8_t* a)
{
    int n = 0;
    a[n++] = 1; a[n++] = sps[1]; a[n++] = sps[2]; a[n++] = sps[3];   // configVersion + AVCProfileIndication/compat/level
    a[n++] = 0xFF;                                                    // 6 bits reserved + lengthSizeMinusOne=3
    a[n++] = 0xE1;                                                    // 3 bits reserved + numOfSPS=1
    a[n++] = (sps_len >> 8) & 0xFF; a[n++] = sps_len & 0xFF; memcpy(a + n, sps, sps_len); n += sps_len;
    a[n++] = 1;                                                       // numOfPPS=1
    a[n++] = (pps_len >> 8) & 0xFF; a[n++] = pps_len & 0xFF; memcpy(a + n, pps, pps_len); n += pps_len;
    return n;
}

// Extrai SPS/PPS (uma vez) e concatena os NALs VCL num frame AVCC (prefixo de 4 bytes).
// Retorna o tamanho AVCC (0 se o pacote nao tinha VCL).
static int h264_to_avcc(const MediaPacket* pkt, uint8_t sps[256], int* sps_len, uint8_t pps[256], int* pps_len,
                        uint8_t** avccframe, int* avcccap)
{
    int fpos = 0, nlen, alen = 0; const uint8_t* nal;
    while ((nal = next_nal(pkt->Data, pkt->Size, &fpos, &nlen)))
    {
        int type = nal[0] & 0x1F;
        if      (type == 7 && *sps_len == 0 && nlen <= 256) { memcpy(sps, nal, nlen); *sps_len = nlen; }
        else if (type == 8 && *pps_len == 0 && nlen <= 256) { memcpy(pps, nal, nlen); *pps_len = nlen; }
        else if (type == 1 || type == 5)
        {
            int need = alen + 4 + nlen;
            if (need > *avcccap) { *avccframe = (uint8_t*)memop_realloc_raw(*avccframe, need); *avcccap = need; }
            put_be32(*avccframe + alen, (uint32_t)nlen);
            memcpy(*avccframe + alen + 4, nal, nlen);
            alen += 4 + nlen;
        }
    }
    return alen;
}

// ---- Converter: reencode H.264 (OpenH264) -> MP4 single-file (video-only) --
int embed_transcode_h264(const char* source_mp4, const char* out_mp4_path)
{
    FrameIndexList* fl = mp4builder_get_frames(source_mp4);
    if (!fl || fl->Count == 0) { if (fl) mframe_list_release(&fl); return -1; }
    VideoMetadata* meta = &fl->Metadata;
    double vfps = (meta->Fps > 0.0) ? meta->Fps : 30.0;
    int ifps = (int)(vfps + 0.5), w = meta->Width, h = meta->Height;

    int64_t* vpts = 0; uint32_t vpts_n = 0;
    mp4_video_pts_ms(source_mp4, &vpts, &vpts_n);
    #define VP2(i) ((vpts && (uint32_t)(i) < vpts_n) ? vpts[i] : (int64_t)((i) * 1000.0 / vfps + 0.5))

    DecoderInstance* dec = h26x_decoder_create(meta->Codec);
    MediaEncoderParams vp; memset(&vp, 0, sizeof(vp));
    vp.Codec = MEDIA_CODEC_H264; vp.Width = w; vp.Height = h; vp.Fps = ifps;
    vp.BitrateBps = (w >= 1920) ? 5000000 : 3000000; vp.SpeedPreset = 8;
    MediaEncoder* venc = media_encoder_open(&vp);
    if (!dec || !venc) { if (venc) venc->Close(venc); if (dec) h26x_decoder_release(&dec); mframe_list_release(&fl); memop_free_raw(vpts); return -2; }

    // Audio: AAC passthrough (sem re-encode) -> faixa mp4a no MP4.
    AacRawInfo ainfo; void* actx = 0;
    int has_audio = (aac_open_raw(source_mp4, &ainfo, &actx) == 0 && actx);

    Mp4Mux* mux = 0;
    uint8_t sps[256], pps[256]; int sps_len = 0, pps_len = 0;
    uint8_t* avccframe = 0; int avcccap = 0;
    #define OPEN_MUX(a,an) mp4_open2(out_mp4_path, w, h, (a), (an), \
                                     has_audio?ainfo.asc:0, has_audio?ainfo.asc_len:0, \
                                     has_audio?ainfo.rate:0, has_audio?ainfo.channels:0)

    FILE* fsrc = 0; fopen_s(&fsrc, source_mp4, "rb");
    MediaBuffer annexb; mbuffer_init(&annexb);
    int64_t frame_no = 0;

    for (uint64_t i = 0; fsrc && i < fl->Count; i++)
    {
        annexb.Size = 0; h26x_put_single_frame(fsrc, fl->Frames[i], meta, &annexb);
        ImagePlaneList* imgs = imagep_list_new(4);
        h26x_decode_frames(dec, &annexb, imgs);
        for (int k = 0; k < imgs->Count; k++)
        {
            ImagePlane* img = imgs->Items[k];
            MediaFrame mf; memset(&mf, 0, sizeof(mf));
            mf.Kind = MEDIA_KIND_VIDEO; mf.Width = w; mf.Height = h;
            mf.Y = img->Planes[0]; mf.StrideY = img->Strides[0];
            mf.U = img->Planes[1]; mf.StrideU = img->Strides[1];
            mf.V = img->Planes[2]; mf.StrideV = img->Strides[2];
            mf.Pts = frame_no;
            venc->SendFrame(venc, &mf);
            MediaPacket pkt;
            while (venc->ReceivePacket(venc, &pkt) == 1)
            {
                int alen = h264_to_avcc(&pkt, sps, &sps_len, pps, &pps_len, &avccframe, &avcccap);
                if (!mux && sps_len > 0 && pps_len > 0)
                { uint8_t a[600]; int an = build_avcc(sps, sps_len, pps, pps_len, a); mux = OPEN_MUX(a, an); }
                if (mux && alen > 0) mp4_write_video(mux, VP2(pkt.Pts), pkt.KeyFrame, avccframe, alen);
            }
            frame_no++;
        }
        imagep_list_release(&imgs, 1);
    }

    venc->SendFrame(venc, 0);
    MediaPacket pkt;
    while (venc->ReceivePacket(venc, &pkt) == 1)
    {
        int alen = h264_to_avcc(&pkt, sps, &sps_len, pps, &pps_len, &avccframe, &avcccap);
        if (!mux && sps_len > 0 && pps_len > 0)
        { uint8_t a[600]; int an = build_avcc(sps, sps_len, pps, pps_len, a); mux = mp4_open(out_mp4_path, w, h, a, an); }
        if (mux && alen > 0) mp4_write_video(mux, VP2(pkt.Pts), pkt.KeyFrame, avccframe, alen);
    }
    venc->Close(venc);

    // Faixa de audio: copia todos os samples AAC codificados (passthrough).
    if (mux && has_audio)
    {
        const uint8_t* ad; int as, adur; int64_t adts;
        while (aac_read_raw(actx, &ad, &as, &adts, &adur) == 1) mp4_write_audio(mux, adur, ad, as);
    }
    if (actx) aac_close(actx);
    if (mux) mp4_close(mux);

    if (annexb.Data) memop_free_raw(annexb.Data);
    if (fsrc) fclose(fsrc);
    h26x_decoder_release(&dec);
    mframe_list_release(&fl);
    memop_free_raw(vpts); memop_free_raw(avccframe);
    #undef VP2
    #undef OPEN_MUX
    return mux ? 0 : -3;
}

// ---- H.265/HEVC helpers (hvcC + HVCC) -------------------------------------
// Monta o HEVCDecoderConfigurationRecord (hvcC) a partir de VPS/SPS/PPS. 'o' com folga.
static int build_hvcc(const uint8_t* vps, int vl, const uint8_t* sps, int sl, const uint8_t* pps, int pl, uint8_t* o)
{
    int n = 0;
    o[n++] = 1;                                   // configurationVersion
    // profile_tier_level geral (12 bytes): copiado do SPS (apos NAL header[2] + 1 byte de ids).
    if (sl >= 15) { memcpy(o + n, sps + 3, 12); } else { memset(o + n, 0, 12); }
    n += 12;
    o[n++] = 0xF0; o[n++] = 0x00;                 // reserved(1111) + min_spatial_segmentation_idc=0
    o[n++] = 0xFC;                                // reserved(111111) + parallelismType=0
    o[n++] = 0xFD;                                // reserved(111111) + chromaFormat=1 (4:2:0)
    o[n++] = 0xF8;                                // reserved + bitDepthLumaMinus8=0
    o[n++] = 0xF8;                                // reserved + bitDepthChromaMinus8=0
    o[n++] = 0x00; o[n++] = 0x00;                 // avgFrameRate=0
    o[n++] = 0x0B;                                // cfr=0 + numTempLayers=1 + tempIdNested=0 + lengthSizeMinusOne=3
    o[n++] = 3;                                   // numOfArrays: VPS, SPS, PPS
    o[n++] = 32; o[n++] = 0; o[n++] = 1; o[n++] = (vl >> 8) & 0xFF; o[n++] = vl & 0xFF; memcpy(o + n, vps, vl); n += vl;
    o[n++] = 33; o[n++] = 0; o[n++] = 1; o[n++] = (sl >> 8) & 0xFF; o[n++] = sl & 0xFF; memcpy(o + n, sps, sl); n += sl;
    o[n++] = 34; o[n++] = 0; o[n++] = 1; o[n++] = (pl >> 8) & 0xFF; o[n++] = pl & 0xFF; memcpy(o + n, pps, pl); n += pl;
    return n;
}

// Extrai VPS/SPS/PPS (uma vez) e concatena os NALs VCL em HVCC (prefixo de 4 bytes).
static int h265_to_hvcc(const MediaPacket* pkt, uint8_t vps[256], int* vl, uint8_t sps[256], int* sl,
                        uint8_t pps[256], int* pl, uint8_t** frame, int* cap)
{
    int fpos = 0, nlen, alen = 0; const uint8_t* nal;
    while ((nal = next_nal(pkt->Data, pkt->Size, &fpos, &nlen)))
    {
        int type = (nal[0] >> 1) & 0x3F;   // HEVC NAL unit type
        if      (type == 32 && *vl == 0 && nlen <= 256) { memcpy(vps, nal, nlen); *vl = nlen; }
        else if (type == 33 && *sl == 0 && nlen <= 256) { memcpy(sps, nal, nlen); *sl = nlen; }
        else if (type == 34 && *pl == 0 && nlen <= 256) { memcpy(pps, nal, nlen); *pl = nlen; }
        else if (type <= 31)               // VCL
        {
            int need = alen + 4 + nlen;
            if (need > *cap) { *frame = (uint8_t*)memop_realloc_raw(*frame, need); *cap = need; }
            put_be32(*frame + alen, (uint32_t)nlen);
            memcpy(*frame + alen + 4, nal, nlen);
            alen += 4 + nlen;
        }
    }
    return alen;
}

int embed_transcode_h265(const char* source_mp4, const char* out_mp4_path)
{
    FrameIndexList* fl = mp4builder_get_frames(source_mp4);
    if (!fl || fl->Count == 0) { if (fl) mframe_list_release(&fl); return -1; }
    VideoMetadata* meta = &fl->Metadata;
    double vfps = (meta->Fps > 0.0) ? meta->Fps : 30.0;
    int ifps = (int)(vfps + 0.5), w = meta->Width, h = meta->Height;

    int64_t* vpts = 0; uint32_t vpts_n = 0;
    mp4_video_pts_ms(source_mp4, &vpts, &vpts_n);
    #define VP2(i) ((vpts && (uint32_t)(i) < vpts_n) ? vpts[i] : (int64_t)((i) * 1000.0 / vfps + 0.5))

    DecoderInstance* dec = h26x_decoder_create(meta->Codec);
    MediaEncoderParams vp; memset(&vp, 0, sizeof(vp));
    vp.Codec = MEDIA_CODEC_H265; vp.Width = w; vp.Height = h; vp.Fps = ifps;
    vp.BitrateBps = (w >= 1920) ? 4000000 : 2500000; vp.SpeedPreset = 8;   // HEVC ~30% menor que H.264
    MediaEncoder* venc = media_encoder_open(&vp);
    if (!dec || !venc) { if (venc) venc->Close(venc); if (dec) h26x_decoder_release(&dec); mframe_list_release(&fl); memop_free_raw(vpts); return -2; }

    AacRawInfo ainfo; void* actx = 0;
    int has_audio = (aac_open_raw(source_mp4, &ainfo, &actx) == 0 && actx);

    Mp4Mux* mux = 0;
    uint8_t vps[256], sps[256], pps[256]; int vl = 0, sl = 0, pl = 0;
    uint8_t* hvccframe = 0; int hvcccap = 0;
    #define OPEN_HEVC(c,cl) mp4_open_hevc(out_mp4_path, w, h, (c), (cl), \
                                         has_audio?ainfo.asc:0, has_audio?ainfo.asc_len:0, \
                                         has_audio?ainfo.rate:0, has_audio?ainfo.channels:0)

    FILE* fsrc = 0; fopen_s(&fsrc, source_mp4, "rb");
    MediaBuffer annexb; mbuffer_init(&annexb);
    int64_t frame_no = 0;

    for (uint64_t i = 0; fsrc && i < fl->Count; i++)
    {
        annexb.Size = 0; h26x_put_single_frame(fsrc, fl->Frames[i], meta, &annexb);
        ImagePlaneList* imgs = imagep_list_new(4);
        h26x_decode_frames(dec, &annexb, imgs);
        for (int k = 0; k < imgs->Count; k++)
        {
            ImagePlane* img = imgs->Items[k];
            MediaFrame mf; memset(&mf, 0, sizeof(mf));
            mf.Kind = MEDIA_KIND_VIDEO; mf.Width = w; mf.Height = h;
            mf.Y = img->Planes[0]; mf.StrideY = img->Strides[0];
            mf.U = img->Planes[1]; mf.StrideU = img->Strides[1];
            mf.V = img->Planes[2]; mf.StrideV = img->Strides[2];
            mf.Pts = frame_no;
            venc->SendFrame(venc, &mf);
            MediaPacket pkt;
            while (venc->ReceivePacket(venc, &pkt) == 1)
            {
                int alen = h265_to_hvcc(&pkt, vps, &vl, sps, &sl, pps, &pl, &hvccframe, &hvcccap);
                if (!mux && vl > 0 && sl > 0 && pl > 0)
                { uint8_t c[2048]; int cl = build_hvcc(vps, vl, sps, sl, pps, pl, c); mux = OPEN_HEVC(c, cl); }
                if (mux && alen > 0) mp4_write_video(mux, VP2(pkt.Pts), pkt.KeyFrame, hvccframe, alen);
            }
            frame_no++;
        }
        imagep_list_release(&imgs, 1);
    }

    venc->SendFrame(venc, 0);
    MediaPacket pkt;
    while (venc->ReceivePacket(venc, &pkt) == 1)
    {
        int alen = h265_to_hvcc(&pkt, vps, &vl, sps, &sl, pps, &pl, &hvccframe, &hvcccap);
        if (!mux && vl > 0 && sl > 0 && pl > 0)
        { uint8_t c[2048]; int cl = build_hvcc(vps, vl, sps, sl, pps, pl, c); mux = OPEN_HEVC(c, cl); }
        if (mux && alen > 0) mp4_write_video(mux, VP2(pkt.Pts), pkt.KeyFrame, hvccframe, alen);
    }
    venc->Close(venc);

    if (mux && has_audio)
    {
        const uint8_t* ad; int as, adur; int64_t adts;
        while (aac_read_raw(actx, &ad, &as, &adts, &adur) == 1) mp4_write_audio(mux, adur, ad, as);
    }
    if (actx) aac_close(actx);
    if (mux) mp4_close(mux);

    if (annexb.Data) memop_free_raw(annexb.Data);
    if (fsrc) fclose(fsrc);
    h26x_decoder_release(&dec);
    mframe_list_release(&fl);
    memop_free_raw(vpts); memop_free_raw(hvccframe);
    #undef VP2
    #undef OPEN_HEVC
    return mux ? 0 : -3;
}

// ---- HLS multi-resolucao H.264 (decode->scale->OpenH264->fMP4) -------------
typedef struct { uint8_t* data; int size; int64_t ms; int key; } EncSample;

// Fecha o segmento atual: calcula duracoes, escreve <name>-N.m4s (moof+mdat),
// emite EXTINF na playlist e reseta o acumulador.
static void flush_hls_seg(const char* base, const char* name, int* seg_index, FILE* pl,
                          EncSample* seg, int* seg_n, int ifps)
{
    if (*seg_n <= 0) return;
    int n = *seg_n, last_dur = (ifps > 0) ? (1000 / ifps) : 33;
    Mp4Sample* ms = (Mp4Sample*)memop_alloc_raw((size_t)n * sizeof(Mp4Sample));
    double dur_s = 0;
    for (int i = 0; i < n; i++)
    {
        int dur = (i + 1 < n) ? (int)(seg[i + 1].ms - seg[i].ms) : last_dur;
        if (dur <= 0) dur = last_dur;
        ms[i].data = seg[i].data; ms[i].size = seg[i].size; ms[i].duration_ms = dur; ms[i].keyframe = seg[i].key;
        dur_s += dur / 1000.0;
    }
    char path[1024]; snprintf(path, sizeof(path), "%s\\%s-%d.m4s", base, name, *seg_index);
    mp4_write_segment(path, (uint32_t)(*seg_index + 1), seg[0].ms, ms, n);
    if (pl) fprintf(pl, "#EXTINF:%.3f,\n%s-%d.m4s\n", dur_s, name, *seg_index);
    for (int i = 0; i < n; i++) memop_free_raw(seg[i].data);
    memop_free_raw(ms);
    *seg_n = 0; (*seg_index)++;
}

// Uma rendition: decode da fonte -> scale I420 (libyuv) -> OpenH264 -> init-<name>.mp4
// + <name>-N.m4s + <name>.m3u8. Retorna 0 em sucesso.
static int build_one_hls_h264(const char* source, const char* base, const char* name,
                              int ow, int oh, int bitrate, int frag_seconds)
{
    FrameIndexList* fl = mp4builder_get_frames(source);
    if (!fl || fl->Count == 0) { if (fl) mframe_list_release(&fl); return -1; }
    VideoMetadata* meta = &fl->Metadata;
    double vfps = (meta->Fps > 0.0) ? meta->Fps : 30.0; int ifps = (int)(vfps + 0.5);

    int64_t* vpts = 0; uint32_t vpts_n = 0; mp4_video_pts_ms(source, &vpts, &vpts_n);
    #define MS(i) ((vpts && (uint32_t)(i) < vpts_n) ? vpts[i] : (int64_t)((i) * 1000.0 / vfps + 0.5))

    DecoderInstance* dec = h26x_decoder_create(meta->Codec);
    MediaEncoderParams vp; memset(&vp, 0, sizeof(vp));
    vp.Codec = MEDIA_CODEC_H264; vp.Width = ow; vp.Height = oh; vp.Fps = ifps;
    vp.BitrateBps = (bitrate > 0) ? bitrate : 2000000; vp.SpeedPreset = 8;
    MediaEncoder* venc = media_encoder_open(&vp);
    if (!dec || !venc) { if (venc) venc->Close(venc); if (dec) h26x_decoder_release(&dec); mframe_list_release(&fl); memop_free_raw(vpts); return -2; }

    char pinit[1024]; snprintf(pinit, sizeof(pinit), "%s\\init-%s.mp4", base, name);
    FILE* pl = 0; { char pp[1024]; snprintf(pp, sizeof(pp), "%s\\%s.m3u8", base, name); fopen_s(&pl, pp, "wb"); }
    if (pl) fprintf(pl, "#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-PLAYLIST-TYPE:VOD\n"
                        "#EXT-X-TARGETDURATION:%d\n#EXT-X-MEDIA-SEQUENCE:0\n"
                        "#EXT-X-MAP:URI=\"init-%s.mp4\"\n", frag_seconds + 1, name);

    int init_written = 0, seg_index = 0;
    uint8_t sps[256], pps[256]; int sps_len = 0, pps_len = 0;
    uint8_t* avccframe = 0; int avcccap = 0;
    EncSample* seg = 0; int seg_n = 0, seg_cap = 0; int64_t seg_start = -1;

    FILE* fsrc = 0; fopen_s(&fsrc, source, "rb");
    MediaBuffer annexb; mbuffer_init(&annexb);
    int cw = (ow + 1) / 2, ch = (oh + 1) / 2;
    int64_t frame_no = 0;

    for (uint64_t i = 0; fsrc && i < fl->Count; i++)
    {
        annexb.Size = 0; h26x_put_single_frame(fsrc, fl->Frames[i], meta, &annexb);
        ImagePlaneList* imgs = imagep_list_new(4);
        h26x_decode_frames(dec, &annexb, imgs);
        for (int k = 0; k < imgs->Count; k++)
        {
            ImagePlane* img = imgs->Items[k];
            uint8_t* dy = (uint8_t*)memop_alloc_raw((size_t)ow * oh);
            uint8_t* du = (uint8_t*)memop_alloc_raw((size_t)cw * ch);
            uint8_t* dv = (uint8_t*)memop_alloc_raw((size_t)cw * ch);
            if (!dy || !du || !dv) { memop_free_raw(dy); memop_free_raw(du); memop_free_raw(dv); continue; }
#ifdef HAVE_LIBYUV
            I420Scale(img->Planes[0], img->Strides[0], img->Planes[1], img->Strides[1], img->Planes[2], img->Strides[2],
                      img->Width, img->Height, dy, ow, du, cw, dv, cw, ow, oh, kFilterBilinear);
#else
            if (img->Width == ow && img->Height == oh)
            {
                for (int y = 0; y < oh; y++) memcpy(dy + (size_t)y * ow, img->Planes[0] + (size_t)y * img->Strides[0], ow);
                for (int y = 0; y < ch; y++) { memcpy(du + (size_t)y * cw, img->Planes[1] + (size_t)y * img->Strides[1], cw);
                                               memcpy(dv + (size_t)y * cw, img->Planes[2] + (size_t)y * img->Strides[2], cw); }
            }
            else { memop_free_raw(dy); memop_free_raw(du); memop_free_raw(dv); continue; }   // sem libyuv nao ha scale
#endif
            MediaFrame mf; memset(&mf, 0, sizeof(mf));
            mf.Kind = MEDIA_KIND_VIDEO; mf.Width = ow; mf.Height = oh;
            mf.Y = dy; mf.StrideY = ow; mf.U = du; mf.StrideU = cw; mf.V = dv; mf.StrideV = cw; mf.Pts = frame_no;
            venc->SendFrame(venc, &mf);
            MediaPacket pkt;
            while (venc->ReceivePacket(venc, &pkt) == 1)
            {
                int alen = h264_to_avcc(&pkt, sps, &sps_len, pps, &pps_len, &avccframe, &avcccap);
                if (!init_written && sps_len > 0 && pps_len > 0)
                { uint8_t a[600]; int an = build_avcc(sps, sps_len, pps, pps_len, a); mp4_write_init(pinit, ow, oh, a, an); init_written = 1; }
                if (!init_written || alen <= 0) continue;
                int64_t ms = MS(pkt.Pts);
                if (pkt.KeyFrame && seg_n > 0 && (ms - seg_start) >= (int64_t)frag_seconds * 1000)
                    flush_hls_seg(base, name, &seg_index, pl, seg, &seg_n, ifps);
                if (seg_n == 0) seg_start = ms;
                if (seg_n == seg_cap) { seg_cap = seg_cap ? seg_cap * 2 : 64; seg = (EncSample*)memop_realloc_raw(seg, (size_t)seg_cap * sizeof(EncSample)); }
                uint8_t* cp = (uint8_t*)memop_alloc_raw(alen); memcpy(cp, avccframe, alen);
                seg[seg_n].data = cp; seg[seg_n].size = alen; seg[seg_n].ms = ms; seg[seg_n].key = pkt.KeyFrame; seg_n++;
            }
            memop_free_raw(dy); memop_free_raw(du); memop_free_raw(dv);
            frame_no++;
        }
        imagep_list_release(&imgs, 1);
    }

    venc->SendFrame(venc, 0);
    { MediaPacket pkt;
      while (venc->ReceivePacket(venc, &pkt) == 1)
      {
          int alen = h264_to_avcc(&pkt, sps, &sps_len, pps, &pps_len, &avccframe, &avcccap);
          if (!init_written || alen <= 0) continue;
          int64_t ms = MS(pkt.Pts);
          if (pkt.KeyFrame && seg_n > 0 && (ms - seg_start) >= (int64_t)frag_seconds * 1000)
              flush_hls_seg(base, name, &seg_index, pl, seg, &seg_n, ifps);
          if (seg_n == 0) seg_start = ms;
          if (seg_n == seg_cap) { seg_cap = seg_cap ? seg_cap * 2 : 64; seg = (EncSample*)memop_realloc_raw(seg, (size_t)seg_cap * sizeof(EncSample)); }
          uint8_t* cp = (uint8_t*)memop_alloc_raw(alen); memcpy(cp, avccframe, alen);
          seg[seg_n].data = cp; seg[seg_n].size = alen; seg[seg_n].ms = ms; seg[seg_n].key = pkt.KeyFrame; seg_n++;
      } }
    flush_hls_seg(base, name, &seg_index, pl, seg, &seg_n, ifps);   // ultimo segmento

    venc->Close(venc);
    if (pl) { fprintf(pl, "#EXT-X-ENDLIST\n"); fclose(pl); }
    if (annexb.Data) memop_free_raw(annexb.Data);
    if (fsrc) fclose(fsrc);
    h26x_decoder_release(&dec);
    mframe_list_release(&fl);
    memop_free_raw(vpts); memop_free_raw(avccframe); memop_free_raw(seg);
    #undef MS
    return init_written ? 0 : -3;
}

// Rendition de AUDIO (AAC passthrough): init-audio.mp4 + audio-N.m4s + audio.m3u8.
// Uma so faixa de audio, compartilhada por todas as variantes de video. Retorna 0 se houver audio.
static int build_audio_hls_h264(const char* source, const char* base, int frag_seconds)
{
    AacRawInfo info; void* actx = 0;
    if (aac_open_raw(source, &info, &actx) != 0 || !actx) return -1;

    char pinit[1024]; snprintf(pinit, sizeof(pinit), "%s\\init-audio.mp4", base);
    if (mp4_write_init_audio(pinit, info.asc, info.asc_len, info.timescale, info.channels) != 0) { aac_close(actx); return -2; }

    FILE* pl = 0; { char pp[1024]; snprintf(pp, sizeof(pp), "%s\\audio.m3u8", base); fopen_s(&pl, pp, "wb"); }
    if (pl) fprintf(pl, "#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-PLAYLIST-TYPE:VOD\n"
                        "#EXT-X-TARGETDURATION:%d\n#EXT-X-MEDIA-SEQUENCE:0\n#EXT-X-MAP:URI=\"init-audio.mp4\"\n", frag_seconds + 1);

    Mp4Sample* seg = 0; int seg_n = 0, seg_cap = 0, seg_index = 0;
    int64_t seg_base = 0, seg_dur = 0;
    const uint8_t* ad; int as, adur; int64_t adts;
    while (aac_read_raw(actx, &ad, &as, &adts, &adur) == 1)
    {
        if (seg_n > 0 && seg_dur >= (int64_t)frag_seconds * info.timescale)
        {
            char path[1024]; snprintf(path, sizeof(path), "%s\\audio-%d.m4s", base, seg_index);
            mp4_write_segment(path, (uint32_t)(seg_index + 1), seg_base, seg, seg_n);
            if (pl) fprintf(pl, "#EXTINF:%.3f,\naudio-%d.m4s\n", (double)seg_dur / info.timescale, seg_index);
            for (int i = 0; i < seg_n; i++) memop_free_raw((void*)seg[i].data);
            seg_n = 0; seg_dur = 0; seg_index++;
        }
        if (seg_n == 0) seg_base = adts;
        if (seg_n == seg_cap) { seg_cap = seg_cap ? seg_cap * 2 : 128; seg = (Mp4Sample*)memop_realloc_raw(seg, (size_t)seg_cap * sizeof(Mp4Sample)); }
        uint8_t* cp = (uint8_t*)memop_alloc_raw(as); memcpy(cp, ad, as);
        seg[seg_n].data = cp; seg[seg_n].size = as; seg[seg_n].duration_ms = adur; seg[seg_n].keyframe = 1;
        seg_n++; seg_dur += adur;
    }
    if (seg_n > 0)   // ultimo segmento
    {
        char path[1024]; snprintf(path, sizeof(path), "%s\\audio-%d.m4s", base, seg_index);
        mp4_write_segment(path, (uint32_t)(seg_index + 1), seg_base, seg, seg_n);
        if (pl) fprintf(pl, "#EXTINF:%.3f,\naudio-%d.m4s\n", (double)seg_dur / info.timescale, seg_index);
        for (int i = 0; i < seg_n; i++) memop_free_raw((void*)seg[i].data);
    }
    if (pl) { fprintf(pl, "#EXT-X-ENDLIST\n"); fclose(pl); }
    memop_free_raw(seg);
    aac_close(actx);
    return 0;
}

int embed_build_hls_h264(const char* source_mp4, const char* base,
                         const PipeTrack* tracks, int track_count, int frag_seconds)
{
    if (frag_seconds <= 0) frag_seconds = 2;   // HLS reencode: segmentos de ~2s (fronteira em keyframe)

    int has_audio = (build_audio_hls_h264(source_mp4, base, frag_seconds) == 0);
    const char* vcodecs = has_audio ? "avc1.640028,mp4a.40.2" : "avc1.640028";

    FILE* master = 0; { char p[1024]; snprintf(p, sizeof(p), "%s\\master.m3u8", base); fopen_s(&master, p, "wb"); }
    if (master)
    {
        fprintf(master, "#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-INDEPENDENT-SEGMENTS\n");
        if (has_audio)
            fprintf(master, "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",NAME=\"audio\",DEFAULT=YES,AUTOSELECT=YES,URI=\"audio.m3u8\"\n");
    }

    int ok = 0;
    for (int t = 0; t < track_count; t++)
    {
        int rc = build_one_hls_h264(source_mp4, base, tracks[t].Name,
                                    tracks[t].Width, tracks[t].Height, tracks[t].BitrateBps, frag_seconds);
        if (rc == 0)
        {
            ok++;
            if (master)
            {
                fprintf(master, "#EXT-X-STREAM-INF:BANDWIDTH=%d,RESOLUTION=%dx%d,CODECS=\"%s\"%s\n%s.m3u8\n",
                        tracks[t].BitrateBps > 0 ? tracks[t].BitrateBps : 2000000,
                        tracks[t].Width, tracks[t].Height, vcodecs,
                        has_audio ? ",AUDIO=\"aud\"" : "", tracks[t].Name);
            }
        }
    }
    if (master) fclose(master);
    return ok > 0 ? 0 : -1;
}
