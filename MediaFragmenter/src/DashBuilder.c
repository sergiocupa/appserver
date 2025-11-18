#include "DashBuilder.h"


static double dash_calculate_duration(FrameIndexList* frames, double fps)
{
    if (!frames || frames->Count == 0 || fps <= 0) 
    {
        return 0.0;
    }
    return (double)frames->Count / fps;
}

static uint64_t dash_calculate_bitrate(FrameIndexList* frames, double duration)
{
    if (!frames || duration <= 0)
    {
        return 1000000; // 1 Mbps default
    }

    uint64_t total_bytes = 0;
    for (int i = 0; i < frames->Count; i++)
    {
        total_bytes += frames->Frames[i]->Size;
    }

    // Bitrate em bits por segundo
    return (uint64_t)((total_bytes * 8.0) / duration);
}

static char* dash_generate_codec_string(VideoMetadata* meta)
{
    char* codec_str = (char*)malloc(64);

    if (meta->Codec == 264) 
    {
        // H.264 AVC
        if (meta->Sps.Size >= 4) {
            uint8_t profile = meta->Sps.Data[1];
            uint8_t constraints = meta->Sps.Data[2];
            uint8_t level = meta->Sps.Data[3];

            snprintf(codec_str, 64, "avc1.%02X%02X%02X",
                profile, constraints, level);
        }
        else {
            // Fallback para codec genérico
            strcpy(codec_str, "avc1.64001f"); // High Profile, Level 3.1
        }
    }
    else if (meta->Codec == 265)
    {
        // H.265 HEVC
        if (meta->Sps.Size >= 13) 
        {
            // Parsing simplificado do SPS H.265
            uint8_t profile = (meta->Sps.Data[1] >> 6) & 0x03;
            uint8_t tier = (meta->Sps.Data[1] >> 5) & 0x01;
            uint8_t level = meta->Sps.Data[12];

            snprintf(codec_str, 64, "hvc1.%d.%d.L%d.B0", profile, tier, level / 3);
        }
        else 
        {
            // Fallback
            strcpy(codec_str, "hvc1.1.6.L93.B0"); // Main Profile, Level 3.1
        }
    }
    else 
    {
        strcpy(codec_str, "avc1.64001f");
    }
    return codec_str;
}

static void dash_format_duration_iso8601(double seconds, char* buffer, size_t buffer_size)
{
    int total_seconds = (int)seconds;
    int minutes = total_seconds / 60;
    int secs = total_seconds % 60;

    if (minutes > 0) 
    {
        snprintf(buffer, buffer_size, "PT%dM%dS", minutes, secs);
    }
    else {
        snprintf(buffer, buffer_size, "PT%dS", secs);
    }
}




char* dash_create_mpd(VideoMetadata* meta, FrameIndexList* frames, double fragment_duration_sec, size_t* output_length) 
{
    if (!meta || !frames || !output_length) {
        return NULL;
    }

    // Calcular parâmetros
    double   total_duration = dash_calculate_duration(frames, meta->Fps);
    uint64_t bitrate        = dash_calculate_bitrate(frames, total_duration);
    char*    codec_string   = dash_generate_codec_string(meta);

    // Formatar duração ISO 8601
    char duration_iso[32];
    dash_format_duration_iso8601(total_duration, duration_iso, sizeof(duration_iso));

    // Calcular número total de fragmentos
    int total_fragments = (int)((total_duration / fragment_duration_sec) + 0.5);

    // Alocar buffer (4KB deve ser suficiente para MPD)
    size_t buffer_size = 4096;
    char* mpd_content = (char*)malloc(buffer_size);
    if (!mpd_content) {
        free(codec_string);
        return NULL;
    }

    // Construir XML do MPD
    int written = snprintf(mpd_content, buffer_size,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<MPD xmlns=\"urn:mpeg:dash:schema:mpd:2011\"\n"
        "     type=\"static\"\n"
        "     mediaPresentationDuration=\"%s\"\n"
        "     minBufferTime=\"PT2S\"\n"
        "     profiles=\"urn:mpeg:dash:profile:isoff-main:2011\">\n"
        "\n"
        "  <Period id=\"0\" start=\"PT0S\" duration=\"%s\">\n"
        "    \n"
        "    <!-- Video Adaptation Set -->\n"
        "    <AdaptationSet\n"
        "        id=\"1\"\n"
        "        contentType=\"video\"\n"
        "        mimeType=\"video/mp4\"\n"
        "        codecs=\"%s\"\n"
        "        width=\"%d\"\n"
        "        height=\"%d\"\n"
        "        frameRate=\"%.2f\"\n"
        "        segmentAlignment=\"true\"\n"
        "        startWithSAP=\"1\">\n"
        "      \n"
        "      <!-- Segment Template -->\n"
        "      <SegmentTemplate\n"
        "          timescale=\"%u\"\n"
        "          duration=\"%u\"\n"
        "          initialization=\"init.mp4\"\n"
        "          media=\"fragment_$Number$.m4s\"\n"
        "          startNumber=\"0\">\n"
        "      </SegmentTemplate>\n"
        "      \n"
        "      <!-- Representation -->\n"
        "      <Representation\n"
        "          id=\"1\"\n"
        "          bandwidth=\"%lu\"\n"
        "          width=\"%d\"\n"
        "          height=\"%d\">\n"
        "      </Representation>\n"
        "      \n"
        "    </AdaptationSet>\n"
        "    \n"
        "  </Period>\n"
        "</MPD>\n",

        // Parâmetros MPD
        duration_iso,                                          // mediaPresentationDuration
        duration_iso,                                          // Period duration

        // Parâmetros AdaptationSet
        codec_string,                                          // codecs
        meta->Width,                                           // width
        meta->Height,                                          // height
        meta->Fps,                                             // frameRate

        // Parâmetros SegmentTemplate
        meta->Timescale,                                       // timescale
        (uint32_t)(fragment_duration_sec * meta->Timescale),   // duration em timescale units

        // Parâmetros Representation
        (unsigned long)bitrate,                                // bandwidth
        meta->Width,                                           // width
        meta->Height                                           // height
    );

    free(codec_string);

    if (written < 0 || written >= buffer_size) 
    {
        free(mpd_content);
        return NULL;
    }

    *output_length = written;
    return mpd_content;
}