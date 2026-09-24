// VideoCodecWrapper.c — camada de CONTAINER/orquestracao: annexb + fragmentos H26x.
// (O DECODE H26x + tipos de imagem foram para codecs/media/codec_video.c.)
#include "H264Builder.h"
#include "H265Builder.h"
#include "../include/MediaFragmenter.h"

int h26x_create_annexb(VideoMetadata* meta, MediaBuffer* output)
{
    if (!meta || !output) return -1;

    switch (meta->Codec)
    {
    case 264:
    {
        output->Data = h264_create_annexb(meta, &output->Size);
    }
    break;
    case 265:
    {
        output->Data = h265_create_annexb(meta, &output->Size);
    }
    break;
    default:
        return -2;
    }
    return 0;
}

int h26x_put_single_frame(FILE* f, FrameIndex* frame, VideoMetadata* meta, MediaBuffer* output)
{
    if (!meta || !output) return -1;

    switch (meta->Codec)
    {
    case 264:
    {
        return h264_create_single_frame(f, frame, meta, output);
    }
    case 265:
    {
        return h265_create_single_frame(f, frame, meta, output);
    }
    default:
        return -2;
    }
    return -1;
}

int h26x_put_fragment(FILE* f, FrameIndexList* frame_list, double timeline_offset, double timeline_fragment_duration, int frame_offset, int frame_length, int include_sps_pps, H264FragmentFormat format, MediaBuffer* output)
{
    if (!frame_list || !output) return -1;

    switch (frame_list->Metadata.Codec)
    {
    case 264:
    {
        return h264_create_fragment(f, frame_list, timeline_offset, timeline_fragment_duration, frame_offset, frame_length, include_sps_pps, format, output);
    }
    case 265:
    {
        return h265_create_fragment(f, frame_list, timeline_offset, timeline_fragment_duration, frame_offset, frame_length, include_sps_pps, format, output);
    }
    default:
        return -2;
    }
    return -1;
}
