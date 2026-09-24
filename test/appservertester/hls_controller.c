#include "hls_controller.h"

#ifdef _WIN32
#include <winsock2.h>   // antes do windows.h: senao entra o winsock 1 e conflita
#include <direct.h>
#include <windows.h>
#else
#include <sys/socket.h> // send() do SSE
typedef int SOCKET;
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#pragma comment(lib, "ws2_32.lib")

#include "pipeline.h"     // pipeline embutido (sem ffmpeg): {VP9|AV1}+Opus -> WebM/DASH (MediaGateway/media)
#include "mp4_timing.h"   // parse embutido do MP4 (metadados/timing, sem ffprobe) (MediaGateway/media)
#include "embed_ops.h"    // HLS passthrough (fMP4) + Converter (WebM) embutidos, sem ffmpeg (MediaFragmenter/include)
#include "sync_gateway.h" // gateway de sync (source -> decode/encode -> sink) (MediaGateway/media)
#include "gw_path.h"      // caminhos e FS multiplataforma (MediaGateway/media)
#include "frag_session.h" // sessao de fragmentacao: estado em disco + cancelamento

#define HLS_SEGMENT_SECONDS 5
#define HLS_MAX_SEGMENTS    4096
#define HLS_MAX_TRACKS      6

typedef struct HlsTrack
{
    int Height;
    int Width;
    int Bandwidth;
    const char* Name;
} HlsTrack;

static char g_last_error[512];

static int ensure_directory(const char* path)
{
    return gw_mkdir_p(path);   // cria os pais que faltarem; Windows + Linux
}

static int write_input(Message* message, const char* path)
{
    // O arquivo anterior pode estar com lock transitorio (antivirus escaneando o
    // mp4 recem-criado, exclusao pendente apos rmdir, ou leitura em voo). Remove
    // atributos, apaga e tenta reabrir algumas vezes ate liberar (errno 13 EACCES).
#ifdef _WIN32
    SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);   // tira somente-leitura antes de apagar
#endif
    DeleteFileA(path);

    FILE* file = 0;
    errno_t open_error = 0;
    for (int attempt = 0; attempt < 15; attempt++)
    {
        open_error = fopen_s(&file, path, "wb");
        if (open_error == 0 && file)
            break;
        Sleep(200);
    }
    if (open_error != 0 || !file)
    {
        sprintf_s(g_last_error, sizeof(g_last_error), "fopen falhou path='%s' errno=%d (apos retries)", path, (int)open_error);
        printf("[hls] %s\n", g_last_error);
        return 0;
    }
    if (!message->Content.Content)
    {
        fclose(file);
        sprintf_s(g_last_error, sizeof(g_last_error), "Content.Data nulo (length=%d)", message->Content.Length);
        printf("[hls] %s\n", g_last_error);
        return 0;
    }
    size_t written = fwrite(message->Content.Content, 1, message->Content.Length, file);
    fclose(file);
    if (written != (size_t)message->Content.Length)
    {
        sprintf_s(g_last_error, sizeof(g_last_error), "fwrite parcial path='%s' escrito=%zu esperado=%d", path, written, message->Content.Length);
        printf("[hls] %s\n", g_last_error);
    }
    return written == (size_t)message->Content.Length;
}

// Le um header HTTP pelo nome (case-insensitive) de message->Fields.
static int read_header(Message* message, const char* name, char* out, size_t out_size)
{
    size_t name_len = strlen(name);
    for (int i = 0; i < message->Fields.Count; i++)
    {
        MessageField* field = message->Fields.Items[i];
        if (!field || field->Name.Length != (int)name_len)
            continue;

        int equal = 1;
        for (int c = 0; c < field->Name.Length; c++)
        {
            if (tolower((unsigned char)field->Name.Content[c]) != tolower((unsigned char)name[c]))
            {
                equal = 0;
                break;
            }
        }
        if (!equal)
            continue;

        const char* value = field->Param.Value.Content;
        int value_len = field->Param.Value.Length;
        int start = 0;
        while (start < value_len && (value[start] == ' ' || value[start] == '\t'))
            start++;
        int copied = 0;
        for (int v = start; v < value_len && copied < (int)out_size - 1; v++)
            out[copied++] = value[v];
        out[copied] = '\0';
        return copied;
    }
    if (out_size > 0)
        out[0] = '\0';
    return 0;
}

// Mantem apenas caracteres seguros para nome de subpasta em web\hls\<folder>.
static void sanitize_folder(const char* in, char* out, size_t out_size)
{
    size_t o = 0;
    for (size_t i = 0; in && in[i] && o < out_size - 1; i++)
    {
        char ch = in[i];
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_')
        {
            out[o++] = ch;
        }
    }
    out[o] = '\0';
    if (o == 0)
        strcpy_s(out, out_size, "current");
}

// Preenche as pistas (Full HD reduzindo: 1080, 760, 480, 360) conforme a fonte.
static int compute_tracks(int source_width, int source_height, HlsTrack* tracks)
{
    const int heights[] = { 1080, 760, 480, 360, 240, 144 };
    const int bandwidths[] = { 5000000, 3000000, 1400000, 800000, 500000, 300000 };
    const char* names[] = { "1080p", "760p", "480p", "360p", "240p", "144p" };
    int count = 0;
    for (int i = 0; i < HLS_MAX_TRACKS; i++)
    {
        if (heights[i] > source_height)
            continue;
        int width = (int)((double)source_width * heights[i] / source_height + 0.5);
        if (width & 1) width++;
        tracks[count].Height = heights[i];
        tracks[count].Width = width;
        tracks[count].Bandwidth = bandwidths[i];
        tracks[count].Name = names[i];
        count++;
    }
    if (count == 0)
    {
        tracks[0].Height = source_height & ~1;
        tracks[0].Width = source_width & ~1;
        tracks[0].Bandwidth = 600000;
        tracks[0].Name = "source";
        count = 1;
    }
    return count;
}

// ---- SSE (Server-Sent Events) ---------------------------------------------

static void sse_write_raw(Message* message, const char* text, int length)
{
    SOCKET sock = (SOCKET)(UINT_PTR)message->Client->Handle;
    send(sock, text, length, 0);
}

static void sse_headers(Message* message)
{
    const char* headers =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "X-Accel-Buffering: no\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n";
    sse_write_raw(message, headers, (int)strlen(headers));
}

// Envia um evento SSE ("data: <json>\n\n").
static void sse_event(Message* message, const char* json)
{
    sse_write_raw(message, "data: ", 6);
    sse_write_raw(message, json, (int)strlen(json));
    sse_write_raw(message, "\n\n", 2);
}

// ---- Adaptador: eventos do gateway (GwEvent) -> SSE do protocolo do front ----
typedef struct
{
    Message*    msg;
    int         mode;          // 0 = convert, 1 = hls, 2 = dash
    const char* folder;
    const char* codec;         // convert
    const char* ext;           // convert
    double      duration;
    int         seg_ms;
    char        tracks_json[2048]; int tn;   // acumulo p/ o "done" do hls/dash
    unsigned long long start_ms;             // cronometro: tick no START, elapsed no DONE
}
SseAdapter;

static void sse_feedback(void* user, const GwEvent* ev)
{
    SseAdapter* a = (SseAdapter*)user; char buf[1200];
    const char* proto = (a->mode == 2) ? "dash" : "hls";
    switch (ev->Kind)
    {
        case GW_EV_START:
            a->start_ms = GetTickCount64();   // inicia o cronometro da fragmentacao
            if (a->mode == 1 || a->mode == 2)
                sprintf_s(buf, sizeof(buf), "{\"type\":\"start\",\"protocol\":\"%s\",\"folder\":\"%s\",\"total\":%d,\"duration\":%.3f,\"segmentDuration\":%d}",
                          proto, a->folder, ev->Total, a->duration, a->seg_ms / 1000);
            else
                sprintf_s(buf, sizeof(buf), "{\"type\":\"start\",\"codec\":\"%s\",\"ext\":\"%s\"}", a->codec, a->ext);
            sse_event(a->msg, buf); break;

        case GW_EV_TRACK_START:
            if (a->mode == 1 || a->mode == 2)
            {
                sprintf_s(buf, sizeof(buf), "{\"type\":\"track-start\",\"index\":%d,\"name\":\"%s\",\"width\":%d,\"height\":%d,\"bandwidth\":%d,\"encoder\":\"%s\"}",
                          ev->TrackIndex, ev->Name ? ev->Name : "", ev->Width, ev->Height, ev->Bandwidth, ev->Message ? ev->Message : "");
                sse_event(a->msg, buf);
            }
            break;

        case GW_EV_TRACK_DONE:
            if (a->mode == 1 || a->mode == 2)
            {
                if (a->mode == 1)   // HLS tem playlist por rendition; DASH usa SegmentTemplate (sem playlist por rep)
                    sprintf_s(buf, sizeof(buf), "{\"type\":\"track-done\",\"index\":%d,\"name\":\"%s\",\"width\":%d,\"height\":%d,\"bandwidth\":%d,\"playlist\":\"/hls/%s/%s.m3u8\"}",
                              ev->TrackIndex, ev->Name ? ev->Name : "", ev->Width, ev->Height, ev->Bandwidth, a->folder, ev->Name ? ev->Name : "");
                else
                    sprintf_s(buf, sizeof(buf), "{\"type\":\"track-done\",\"index\":%d,\"name\":\"%s\",\"width\":%d,\"height\":%d,\"bandwidth\":%d}",
                              ev->TrackIndex, ev->Name ? ev->Name : "", ev->Width, ev->Height, ev->Bandwidth);
                sse_event(a->msg, buf);
                a->tn += sprintf_s(a->tracks_json + a->tn, sizeof(a->tracks_json) - a->tn, "%s{\"name\":\"%s\",\"width\":%d,\"height\":%d,\"bandwidth\":%d}",
                                   a->tn ? "," : "", ev->Name ? ev->Name : "", ev->Width, ev->Height, ev->Bandwidth);
            }
            break;

        case GW_EV_DONE:
        {
            double elapsed = a->start_ms ? (double)(GetTickCount64() - a->start_ms) / 1000.0 : 0.0;
            if (a->mode == 1 || a->mode == 2)
                sprintf_s(buf, sizeof(buf), "{\"type\":\"done\",\"protocol\":\"%s\",\"playlist\":\"/hls/%s/%s\",\"folder\":\"%s\",\"duration\":%.3f,\"segmentDuration\":%d,\"elapsed\":%.1f,\"tracks\":[%s]}",
                          proto, a->folder, ev->Playlist ? ev->Playlist : (a->mode == 2 ? "manifest.mpd" : "master.m3u8"), a->folder, a->duration, a->seg_ms / 1000, elapsed, a->tracks_json);
            else
                sprintf_s(buf, sizeof(buf), "{\"type\":\"done\",\"codec\":\"%s\",\"file\":\"/hls/%s/%s\",\"elapsed\":%.1f}",
                          a->codec, a->folder, ev->Playlist ? ev->Playlist : "output.mp4", elapsed);
            sse_event(a->msg, buf); break;
        }

        case GW_EV_ERROR:
            sprintf_s(buf, sizeof(buf), "{\"type\":\"error\",\"message\":\"%s\"}", ev->Message ? ev->Message : "erro"); sse_event(a->msg, buf); break;

        case GW_EV_CANCELLED:
            // Nao e "done": a saida nao foi publicada. O front precisa distinguir para
            // nao tentar abrir um manifesto que nao existe.
            sprintf_s(buf, sizeof(buf), "{\"type\":\"cancelled\",\"message\":\"%s\"}", ev->Message ? ev->Message : "interrompida");
            sse_event(a->msg, buf); break;

        case GW_EV_PROGRESS:
            // Progresso ao vivo. Dois modos:
            //  - POR PISTA (segment/faseado): ev->Name + ev->Progress (0..1) -> {name,percent}.
            //  - GLOBAL (per-frame): ev->Pts (us) / duracao -> {percent} (sem name).
            if (a->mode == 1 || a->mode == 2)
            {
                int pct;
                if (ev->Name && ev->Name[0]) pct = (int)(ev->Progress * 100.0 + 0.5);
                else if (a->duration > 0.0)  pct = (int)((double)ev->Pts / 1000000.0 / a->duration * 100.0 + 0.5);
                else                         pct = -1;
                if (pct >= 0)
                {
                    if (pct > 100) pct = 100;
                    if (ev->Name && ev->Name[0])
                        sprintf_s(buf, sizeof(buf), "{\"type\":\"progress\",\"name\":\"%s\",\"percent\":%d}", ev->Name, pct);
                    else
                        sprintf_s(buf, sizeof(buf), "{\"type\":\"progress\",\"percent\":%d}", pct);
                    sse_event(a->msg, buf);
                }
            }
            break;

        case GW_EV_MEM: default: break;   // memoria: nao usado pelo front
    }
}

static MediaCodec codec_from_slug(const char* slug)
{
    if (!slug || !*slug) return MEDIA_CODEC_NONE;
    if (_stricmp(slug, "h264") == 0 || _stricmp(slug, "avc")  == 0) return MEDIA_CODEC_H264;
    if (_stricmp(slug, "h265") == 0 || _stricmp(slug, "hevc") == 0) return MEDIA_CODEC_H265;
    if (_stricmp(slug, "vp9")  == 0) return MEDIA_CODEC_VP9;
    if (_stricmp(slug, "av1")  == 0) return MEDIA_CODEC_AV1;
    return MEDIA_CODEC_NONE;
}

// Aplica no perfil o que a UI gravou em "output" da sessao. Campo em branco/zero fica
// como esta: o gateway trata 0/NONE como "herda da entrada" e, se NADA mudar, faz bypass.
// O codec so e sobrescrito quando o container aceita (WebM nao carrega H.26x).
static void apply_session_output(const char* session_id, MediaProfile* profile)
{
    char codec[32] = { 0 };
    int  w = 0, h = 0, bitrate = 0;
    double fps = 0.0;

    if (!frag_session_get_output(session_id, codec, sizeof(codec), &w, &h, &fps, &bitrate)) return;

    MediaCodec chosen = codec_from_slug(codec);
    if (chosen != MEDIA_CODEC_NONE)
    {
        int webm = (profile->Container == CONT_DASH_WEBM);
        int webm_ok = (chosen == MEDIA_CODEC_VP9 || chosen == MEDIA_CODEC_AV1);
        if (webm == webm_ok) profile->VideoCodec = chosen;
    }

    if (w > 0)       profile->Width      = w;
    if (h > 0)       profile->Height     = h;
    if (fps > 0.0)   profile->Fps        = (int)(fps + 0.5);
    if (bitrate > 0) profile->BitrateBps = bitrate;

    // Resolucao escolhida a mao = uma pista so. A escada de renditions e o modo
    // "adaptativo"; misturar os dois entregaria uma escada que ignora o pedido.
    if (w > 0 && h > 0) { profile->Renditions = 0; profile->RenditionCount = 0; }

    // Formato "do arquivo" (protocol=auto) fixa o CODEC, nao a resolucao: a escada de
    // renditions continua igual aos formatos fixos. Pista unica (e, com ela, a chance de
    // BYPASS) so acontece quando a resolucao e escolhida a mao, acima.
}

// Roda o gateway sob o registro de jobs da sessao: o id fica associado a um GwControl,
// entao POST /api/session/cancel/<id> interrompe de forma cooperativa e a saida NAO e
// publicada. Sem isso, fechar o EventSource no browser nao parava nada -- a fragmentacao
// seguia ate o fim gravando numa sessao que o usuario ja tinha abandonado.
// Assume a posse de 'src': fecha a fonte AQUI, antes de encerrar o job. O balanco de
// memoria da sessao e medido no job_end, e a fonte MP4 segura o indice de frames e os
// buffers de leitura -- medir antes de fecha-la contabilizava esses blocos como se
// fossem sobra da sessao. Tambem garante que a fonte fecha em todos os caminhos.
static int run_gateway_session(const char* session_id, MediaSource* src, const MediaProfile* profile,
                               const char* base_dir, const GwFeedback* fb)
{
    GwControl* ctl = frag_session_job_begin(session_id);
    if (!ctl && frag_session_running(session_id))
    {
        gw_error(fb, "ja existe uma fragmentacao em andamento nesta sessao");
        src->Close(src);
        return -1;
    }

    int rc = gateway_run(src, profile, base_dir, fb, ctl);
    src->Close(src);   // antes do job_end: a medicao de memoria precisa de um ponto quiescente

    if (ctl)
    {
        int cancelled = ctl->Stop;
        frag_session_job_end(session_id, cancelled ? FRAG_CANCELLED : (rc == 0 ? FRAG_DONE : FRAG_ERROR),
                             cancelled ? "cancelada pelo usuario" : (rc == 0 ? "" : "falha na preparacao"));
    }
    return rc;
}

// ---- Passo 1: upload + probe (POST /video/prepare) ------------------------

Element* hls_prepare_video(Message* message)
{
    if (!message || message->Content.Length <= 0)
    {
        message->Response = message_response_create_text(HTTP_STATUS_BAD_REQUEST, "Video body is empty.");
        return 0;
    }

    char folder_raw[128] = { 0 };
    char folder[128];
    read_header(message, "X-Hls-Folder", folder_raw, sizeof(folder_raw));
    sanitize_folder(folder_raw, folder, sizeof(folder));

    char base[MAX_PATH];
    char source_path[MAX_PATH];
    sprintf_s(base, sizeof(base), "web/hls/%s", folder);
    sprintf_s(source_path, sizeof(source_path), "%s/source.mp4", base);

    printf("[hls] prepare folder='%s' base='%s' content_length=%d\n", folder, base, message->Content.Length);

    if (!ensure_directory("web/hls"))
    {
        printf("[hls] falha ao criar web/hls\n");
        message->Response = message_response_create_text(HTTP_STATUS_INTERNAL_ERROR, "Unable to create HLS base directory.");
        return 0;
    }

    // Se a pasta e uma sessao, preserva o session.json e limpa so o que foi gerado.
    // Fora de uma sessao (uso legado com X-Hls-Folder), limpa a pasta inteira.
    int cleared = frag_session_exists(folder) ? frag_session_clear_output(folder) : gw_rmtree(base);
    if (!cleared)
    {
        printf("[hls] falha ao limpar base='%s' (arquivo em uso?)\n", base);
        message->Response = message_response_create_text(HTTP_STATUS_INTERNAL_ERROR,
            "Unable to clear output folder (file in use).");
        return 0;
    }

    if (!ensure_directory(base))
    {
        printf("[hls] falha ao criar base='%s'\n", base);
        message->Response = message_response_create_text(HTTP_STATUS_INTERNAL_ERROR, "Unable to create output folder.");
        return 0;
    }
    if (!write_input(message, source_path))
    {
        char detail[640];
        sprintf_s(detail, sizeof(detail), "Unable to save uploaded video. %s", g_last_error);
        message->Response = message_response_create_text(HTTP_STATUS_INTERNAL_ERROR, detail);
        return 0;
    }

    // Metadados por parse EMBUTIDO do MP4 (sem ffprobe).
    int    source_width = 0, source_height = 0;
    double duration = 0.0, fps_d = 0.0;
    char   v_codec[16] = { 0 };
    mp4_video_info(source_path, &source_width, &source_height, &fps_d, v_codec, &duration);
    if (source_width <= 0 || source_height <= 0 || duration <= 0.0)
    {
        message->Response = message_response_create_text(HTTP_STATUS_BAD_REQUEST,
            "Video invalido ou nao suportado (parse MP4 embutido).");
        return 0;
    }

    // Estatisticas do fluxo de origem para o painel (sem ffprobe).
    char v_fps[16]; sprintf_s(v_fps, sizeof(v_fps), "%.3f", fps_d);
    const char* v_pixfmt = "yuv420p";   // H.264/H.265 8-bit tipico (parse nao le pix_fmt)
    int  v_bitrate = (duration > 0.0) ? (int)((double)message->Content.Length * 8.0 / duration) : 0; // total aprox.
    char a_codec[16] = { 0 };
    int  a_channels = 0, a_sample = 0, a_bitrate = 0;
    mp4_audio_info(source_path, a_codec, &a_sample, &a_channels);

    HlsTrack tracks[HLS_MAX_TRACKS];
    int track_count = compute_tracks(source_width, source_height, tracks);

    // Retorna o plano de pistas (sem fragmentar ainda). A fragmentacao ocorre
    // no passo 2 via EventSource em /video/prepare-stream/<folder>.
    char json[2048];
    int offset = sprintf_s(json, sizeof(json),
        "{\"folder\":\"%s\",\"duration\":%.3f,\"width\":%d,\"height\":%d,\"segmentDuration\":%d,"
        "\"stream\":\"/api/video/prepare-stream/%s\",\"dashStream\":\"/api/video/prepare-dash/%s\","
        "\"source\":{\"vcodec\":\"%s\",\"pixfmt\":\"%s\",\"fps\":\"%s\",\"vbitrate\":%d,\"acodec\":\"%s\",\"achannels\":%d,\"asamplerate\":%d,\"abitrate\":%d},"
        "\"tracks\":[",
        folder, duration, source_width, source_height, HLS_SEGMENT_SECONDS, folder, folder,
        v_codec, v_pixfmt, v_fps, v_bitrate, a_codec, a_channels, a_sample, a_bitrate);
    for (int i = 0; i < track_count; i++)
    {
        offset += sprintf_s(json + offset, sizeof(json) - offset,
            "%s{\"name\":\"%s\",\"width\":%d,\"height\":%d,\"bandwidth\":%d}",
            i ? "," : "", tracks[i].Name, tracks[i].Width, tracks[i].Height, tracks[i].Bandwidth);
    }
    sprintf_s(json + offset, sizeof(json) - offset, "]}");

    message->Response = message_response_create_content(
        HTTP_STATUS_OK, APPLICATION_JSON, json, (int)strlen(json));
    return 0;
}

// ---- Passo 2: fragmentacao com progresso via SSE --------------------------
// GET /api/video/prepare-stream/<folder>  (EventSource)

Element* hls_stream_video(Message* message)
{
    // A pasta chega como ultimo segmento da rota (query-string nao e suportada).
    char folder[128] = { 0 };
    if (message->Route.Count > 0)
    {
        StringX* last = (StringX*)message->Route.Items[message->Route.Count - 1];
        char raw[128] = { 0 };
        int n = last->Length < (int)sizeof(raw) - 1 ? last->Length : (int)sizeof(raw) - 1;
        memcpy(raw, last->Content, n);
        raw[n] = '\0';
        sanitize_folder(raw, folder, sizeof(folder));
    }
    else
    {
        sanitize_folder("", folder, sizeof(folder));
    }

    char base[MAX_PATH];
    char source_path[MAX_PATH];
    sprintf_s(base, sizeof(base), "web/hls/%s", folder);
    sprintf_s(source_path, sizeof(source_path), "%s/source.mp4", base);

    sse_headers(message);
    message->StreamHandled = true;

    // Metadados por parse EMBUTIDO (sem ffprobe).
    int sw = 0, sh = 0; double duration = 0.0;
    mp4_video_info(source_path, &sw, &sh, 0, 0, &duration);
    if (sw <= 0 || sh <= 0 || duration <= 0.0)
    {
        sse_event(message, "{\"type\":\"error\",\"message\":\"Fonte nao encontrada. Refaca o upload.\"}");
        return 0;
    }

    // HLS MULTI-RESOLUCAO via GATEWAY DE SYNC: source_mp4 -> decode/scale/OpenH264 -> sink_hls.
    HlsTrack tracks[HLS_MAX_TRACKS];
    int track_count = compute_tracks(sw, sh, tracks);

    PipeTrack pts[HLS_MAX_TRACKS];
    for (int i = 0; i < track_count; i++)
    {
        pts[i].Width = tracks[i].Width; pts[i].Height = tracks[i].Height;
        pts[i].BitrateBps = tracks[i].Bandwidth; pts[i].Name = tracks[i].Name;
    }

    MediaProfile profile; memset(&profile, 0, sizeof(profile));
    profile.Container = CONT_HLS_FMP4; profile.VideoCodec = MEDIA_CODEC_H264; profile.AudioCodec = MEDIA_CODEC_AAC;
    profile.SegmentMs = HLS_SEGMENT_SECONDS * 1000; profile.Mode = GW_VOD;
    profile.Renditions = pts; profile.RenditionCount = track_count;
    apply_session_output(folder, &profile);

    SseAdapter ad; memset(&ad, 0, sizeof(ad));
    ad.msg = message; ad.mode = 1; ad.folder = folder; ad.duration = duration; ad.seg_ms = profile.SegmentMs;
    GwFeedback fb = { sse_feedback, &ad };

    MediaSource* src = source_file_open(source_path);
    if (!src) { sse_event(message, "{\"type\":\"error\",\"message\":\"Fonte nao encontrada. Refaca o upload.\"}"); return 0; }
    run_gateway_session(folder, src, &profile, base, &fb);   // fecha a fonte
    return 0;
}

// Le a pasta (penultimo segmento) e/ou o codec (ultimo segmento) da rota.
static void route_segment(Message* message, int from_end, char* out, size_t out_size)
{
    if (out_size > 0) out[0] = '\0';
    int idx = message->Route.Count - 1 - from_end;
    if (idx < 0 || idx >= message->Route.Count) return;
    StringX* s = (StringX*)message->Route.Items[idx];
    int n = s->Length < (int)out_size - 1 ? s->Length : (int)out_size - 1;
    memcpy(out, s->Content, n);
    out[n] = '\0';
}

// ---- DASH + VP9 (streaming adaptativo, WebM) ------------------------------
// GET /api/video/prepare-dash/<folder>[/<codec>]  (EventSource)
// Gera manifest.mpd + segmentos WebM ({VP9|AV1}) via pipeline EMBUTIDO (sem ffmpeg).
// Reproduzido no front com dash.js.

Element* dash_stream_video(Message* message)
{
    // rota: /api/video/prepare-dash/<folder>[/<codec>]  (codec = vp9 | av1; default vp9)
    char last[64] = { 0 }, prev[128] = { 0 };
    route_segment(message, 0, last, sizeof(last));
    route_segment(message, 1, prev, sizeof(prev));

    int  is_av1 = 0;
    char folder_raw[128] = { 0 };
    if (strcmp(last, "vp9") == 0 || strcmp(last, "av1") == 0)
    {
        is_av1 = (strcmp(last, "av1") == 0);
        sprintf_s(folder_raw, sizeof(folder_raw), "%s", prev);
    }
    else
    {
        sprintf_s(folder_raw, sizeof(folder_raw), "%s", last);
    }
    char folder[128];
    sanitize_folder(folder_raw, folder, sizeof(folder));

    char base[MAX_PATH];
    char source_path[MAX_PATH];
    sprintf_s(base, sizeof(base), "web/hls/%s", folder);
    sprintf_s(source_path, sizeof(source_path), "%s/source.mp4", base);

    sse_headers(message);
    message->StreamHandled = true;

    // Metadados por parse EMBUTIDO do MP4 (sem ffprobe). O audio e detectado dentro do pipeline.
    int    source_width = 0, source_height = 0;
    double duration = 0.0;
    mp4_video_info(source_path, &source_width, &source_height, 0, 0, &duration);
    if (source_width <= 0 || source_height <= 0 || duration <= 0.0)
    {
        sse_event(message, "{\"type\":\"error\",\"message\":\"Fonte nao encontrada. Refaca o upload.\"}");
        return 0;
    }

    // DASH-WebM via GATEWAY: source_mp4 -> decode/scale/encode {VP9|AV1} + AAC->Opus -> sink_dash.
    HlsTrack tracks[HLS_MAX_TRACKS];
    int track_count = compute_tracks(source_width, source_height, tracks);

    PipeTrack pts[HLS_MAX_TRACKS];
    for (int i = 0; i < track_count; i++)
    { pts[i].Width = tracks[i].Width; pts[i].Height = tracks[i].Height; pts[i].BitrateBps = tracks[i].Bandwidth; pts[i].Name = tracks[i].Name; }

    MediaProfile profile; memset(&profile, 0, sizeof(profile));
    profile.Container = CONT_DASH_WEBM; profile.VideoCodec = is_av1 ? MEDIA_CODEC_AV1 : MEDIA_CODEC_VP9; profile.AudioCodec = MEDIA_CODEC_OPUS;
    profile.SegmentMs = 2000; profile.Mode = GW_VOD; profile.Renditions = pts; profile.RenditionCount = track_count;
    apply_session_output(folder, &profile);

    SseAdapter ad; memset(&ad, 0, sizeof(ad));
    ad.msg = message; ad.mode = 2; ad.folder = folder; ad.duration = duration; ad.seg_ms = profile.SegmentMs;
    GwFeedback fb = { sse_feedback, &ad };

    MediaSource* src = source_file_open(source_path);
    if (!src) { sse_event(message, "{\"type\":\"error\",\"message\":\"Fonte nao encontrada. Refaca o upload.\"}"); return 0; }
    run_gateway_session(folder, src, &profile, base, &fb);   // fecha a fonte
    return 0;
}

// ---- Conversao para ARQUIVO unico -----------------------------------------
// GET /api/video/convert/<folder>/<codec>   (EventSource)
// Transcodifica source.mp4 inteiro para um arquivo no codec escolhido.
// codec: h264/h265 -> .mp4 ; vp9/av1 -> .webm. Sem HLS/DASH (arquivo direto).

Element* convert_file(Message* message)
{
    char codec_raw[32] = { 0 };
    char folder_raw[128] = { 0 };
    char folder[128];
    route_segment(message, 0, codec_raw, sizeof(codec_raw));   // ultimo = codec
    route_segment(message, 1, folder_raw, sizeof(folder_raw)); // penultimo = folder
    sanitize_folder(folder_raw, folder, sizeof(folder));

    // Normaliza o codec (minusculas, apenas letras/numeros).
    char codec[32]; size_t ci = 0;
    for (size_t i = 0; codec_raw[i] && ci < sizeof(codec) - 1; i++)
    {
        char ch = codec_raw[i];
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) codec[ci++] = ch;
    }
    codec[ci] = '\0';

    char base[MAX_PATH];
    char source_path[MAX_PATH];
    sprintf_s(base, sizeof(base), "web/hls/%s", folder);
    sprintf_s(source_path, sizeof(source_path), "%s/source.mp4", base);

    sse_headers(message);
    message->StreamHandled = true;

    // Validacao + codec da fonte (parse embutido, sem ffprobe).
    int sw = 0, sh = 0; double dur = 0.0; char src_codec[16] = { 0 };
    mp4_video_info(source_path, &sw, &sh, 0, src_codec, &dur);
    if (dur <= 0.0)
    {
        sse_event(message, "{\"type\":\"error\",\"message\":\"Fonte nao encontrada. Refaca o upload.\"}");
        return 0;
    }

    const char* ext = 0;
    char out_path[MAX_PATH];

    if (strcmp(codec, "vp9") == 0 || strcmp(codec, "av1") == 0)
    {
        ext = "webm";
        char start[128]; sprintf_s(start, sizeof(start), "{\"type\":\"start\",\"codec\":\"%s\",\"ext\":\"%s\"}", codec, ext);
        sse_event(message, start);

        sprintf_s(out_path, sizeof(out_path), "%s/output.%s", base, ext);
        MediaCodec mc = (strcmp(codec, "av1") == 0) ? MEDIA_CODEC_AV1 : MEDIA_CODEC_VP9;
        if (embed_transcode_webm(source_path, out_path, mc) != 0)
        {
            sse_event(message, "{\"type\":\"error\",\"message\":\"Conversao embutida falhou (verifique libs VP9/AV1/Opus).\"}");
            return 0;
        }
    }
    else if (strcmp(codec, "h264") == 0 || strcmp(codec, "h265") == 0)
    {
        // H.264/H.265 via GATEWAY: source_mp4 -> (decode->encode | passthrough) -> sink_mp4 (.mp4).
        // Fonte no mesmo codec (ex.: HEVC->h265) => passthrough (remux); senao reencoda (OpenH264/x265).
        MediaProfile profile; memset(&profile, 0, sizeof(profile));
        profile.Container = CONT_MP4_FILE;
        profile.VideoCodec = (strcmp(codec, "h265") == 0) ? MEDIA_CODEC_H265 : MEDIA_CODEC_H264;
        profile.AudioCodec = MEDIA_CODEC_AAC; profile.SegmentMs = 2000; profile.Mode = GW_VOD;
        apply_session_output(folder, &profile);

        SseAdapter ad; memset(&ad, 0, sizeof(ad));
        ad.msg = message; ad.mode = 0; ad.folder = folder; ad.codec = codec; ad.ext = "mp4";
        GwFeedback fb = { sse_feedback, &ad };

        MediaSource* src = source_file_open(source_path);
        if (!src) { sse_event(message, "{\"type\":\"error\",\"message\":\"Fonte nao encontrada. Refaca o upload.\"}"); return 0; }
        run_gateway_session(folder, src, &profile, base, &fb);   // fecha a fonte
        return 0;
    }
    else
    {
        sse_event(message, "{\"type\":\"error\",\"message\":\"Codec desconhecido (use h264/h265/vp9/av1).\"}");
        return 0;
    }

    char done[256];
    sprintf_s(done, sizeof(done),
        "{\"type\":\"done\",\"codec\":\"%s\",\"file\":\"/hls/%s/output.%s\"}", codec, folder, ext);
    sse_event(message, done);
    return 0;
}
