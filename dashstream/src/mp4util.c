#include "../include/dashstream.h"


static FrameIndexList* frame_index_list_new()
{
    FrameIndexList* db = malloc(sizeof(FrameIndexList));
    db->Max = 100;
    db->Count = 0;
    db->Frames = malloc(db->Max * sizeof(FrameIndexList*));
    return db;
}

static FrameIndex* frame_index_new()
{
    FrameIndex* db = malloc(sizeof(FrameIndex));
    db->Offset = 0;
    db->Size = 0;
    return db;
}

static void frame_index_list_add(FrameIndexList* list, uint64_t offset, uint64_t size, int nal_type, char ftype, double pts)
{
    if (list->Count >= list->Max)
    {
        list->Max = ((list->Count + sizeof(FrameIndex)) + list->Max) * 2;
        list->Frames = (void**)realloc((void**)list->Frames, list->Max * sizeof(FrameIndex*));
    }
    list->Frames[list->Count] = malloc(sizeof(FrameIndex));
    list->Frames[list->Count]->Offset    = offset;
    list->Frames[list->Count]->Size      = size;
    list->Frames[list->Count]->NalType   = nal_type;
    list->Frames[list->Count]->FrameType = ftype;
    list->Frames[list->Count]->PTS       = pts;
    list->Count++;
}

void dashstr_frame_index_list_release(FrameIndexList** list)
{
    if (*list)
    {
        int ix = 0;
        while (ix < (*list)->Count)
        {
            free((*list)->Frames[ix]);
            ix++;
        }
        free((*list)->Frames);
        free(*list);
        *list = 0;
    }
}


static int detect_codec_from_file(FILE* f) 
{
    uint8_t buf[6];
    fseek(f, 0, SEEK_SET);
    while (fread(buf, 1, 6, f) == 6) 
    {
        if (buf[0] == 0x00 && buf[1] == 0x00 && buf[2] == 0x01) 
        {
            uint8_t nal = buf[3];
            int type264 = nal & 0x1F;
            int type265 = (nal >> 1) & 0x3F;
            if (type264 == 7) return 264; // SPS
            if (type265 == 32) return 265; // VPS
        }
    }
    fseek(f, 0, SEEK_SET);
    return 264; // fallback
}



static int is_decodable_h264(int nalType) {
    return (nalType == 1 || nalType == 5);
}

static int is_decodable_h265(int nalType) {
    return ((nalType >= 0 && nalType <= 21) && !(nalType >= 32 && nalType <= 34));
}

static char guess_frame_type_h264(int nalType) {
    return (nalType == 5) ? 'I' : 'P';
}

static char guess_frame_type_h265(int nalType) {
    if (nalType >= 19 && nalType <= 21) return 'I';
    if (nalType >= 1 && nalType <= 9) return 'P';
    return 'B';
}

FrameIndexList* index_frames_full(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        perror("Erro ao abrir arquivo");
        return NULL;
    }

    FrameIndexList* list = calloc(1, sizeof(FrameIndexList));
    list->Codec = detect_codec_from_file(f);
    list->Fps = 30.0; // valor padrão estimado, pode ser extraído do moov posteriormente

    uint8_t b;
    uint8_t prev_bytes[3] = { 0 };
    uint64_t offset = 0;
    uint64_t start = 0;
    uint64_t size = 0;
    int prev_nalType = -1;
    double frame_time = 1.0 / list->Fps;
    double pts = 0.0;

    while (fread(&b, 1, 1, f) == 1) {
        if (prev_bytes[0] == 0x00 && prev_bytes[1] == 0x00 && b == 0x01) {
            // Achou novo start code
            if (size > 0 && prev_nalType >= 0) {
                int decodable = (list->Codec == 264)
                    ? is_decodable_h264(prev_nalType)
                    : is_decodable_h265(prev_nalType);

                if (decodable) {
                    char ftype = (list->Codec == 264)
                        ? guess_frame_type_h264(prev_nalType)
                        : guess_frame_type_h265(prev_nalType);
                    frame_index_list_add(list, start, size, prev_nalType, ftype, pts);
                    pts += frame_time;
                }
            }

            start = offset - 2;
            uint8_t nal_header;
            fread(&nal_header, 1, 1, f);
            offset += 4;

            prev_nalType = (list->Codec == 264)
                ? (nal_header & 0x1F)
                : ((nal_header >> 1) & 0x3F);

            size = 4;
        }
        else {
            size++;
        }

        prev_bytes[0] = prev_bytes[1];
        prev_bytes[1] = b;
        offset++;
    }

    // último frame
    if (size > 0 && prev_nalType >= 0) {
        int decodable = (list->Codec == 264)
            ? is_decodable_h264(prev_nalType)
            : is_decodable_h265(prev_nalType);
        if (decodable) {
            char ftype = (list->Codec == 264)
                ? guess_frame_type_h264(prev_nalType)
                : guess_frame_type_h265(prev_nalType);
            frame_index_list_add(list, start, size, prev_nalType, ftype, pts);
        }
    }

    fclose(f);
    return list;
}


//
//
//
//// ---------- Filtros de NAL ----------
//
//static int is_decodable_h264(int nalType) {
//    return (nalType == 1 || nalType == 5); // somente P/B e I
//}
//
//static int is_decodable_h265(int nalType) {
//    return (nalType >= 0 && nalType <= 21); // slices decodificáveis
//}
//
//static char guess_frame_type_h264(int nalType) {
//    return (nalType == 5) ? 'I' : 'P';
//}
//
//static char guess_frame_type_h265(int nalType) {
//    if (nalType >= 19 && nalType <= 21) return 'I';
//    if (nalType >= 1 && nalType <= 9) return 'P';
//    return 'B';
//}
//
//// ---------- Função principal ----------
//
//FrameIndexList* index_frames_full(const char* path) {
//    FILE* f = fopen(path, "rb");
//    if (!f) { perror("Erro ao abrir arquivo"); return NULL; }
//
//    FrameIndexList* list = calloc(1, sizeof(FrameIndexList));
//    list->Codec = detect_codec_from_file(f);
//    list->Fps = 30.0; // default, ajustar conforme necessário
//
//    uint8_t b;
//    uint8_t prev_bytes[2] = { 0 };
//    uint64_t offset = 0;
//    uint64_t start = 0;
//    uint64_t size = 0;
//    int prev_nalType = -1;
//    double frame_time = 1.0 / list->Fps;
//    double pts = 0.0;
//
//    while (fread(&b, 1, 1, f) == 1) {
//        // Detecta start code 00 00 01
//        if (prev_bytes[0] == 0x00 && prev_bytes[1] == 0x00 && b == 0x01) {
//            // Adiciona frame anterior somente se decodificável
//            if (size > 0 && prev_nalType >= 0) {
//                int decodable = 0;
//                char ftype = 0;
//
//                if (list->Codec == 264) {
//                    decodable = is_decodable_h264(prev_nalType);
//                    if (decodable) ftype = guess_frame_type_h264(prev_nalType);
//                }
//                else {
//                    decodable = is_decodable_h265(prev_nalType);
//                    if (decodable) ftype = guess_frame_type_h265(prev_nalType);
//                }
//
//                if (decodable) {
//                    frame_index_list_add(list, start, size, prev_nalType, ftype, pts);
//                    pts += frame_time;
//                }
//            }
//
//            // Start do novo NAL
//            start = offset - 2; // início do start code
//            uint8_t nal_header;
//            if (fread(&nal_header, 1, 1, f) == 1) {
//                offset += 4; // start code + nal_header
//                prev_nalType = (list->Codec == 264) ? (nal_header & 0x1F) : ((nal_header >> 1) & 0x3F);
//                size = 4; // inclui start code + nal_header
//            }
//        }
//        else {
//            size++;
//        }
//
//        prev_bytes[0] = prev_bytes[1];
//        prev_bytes[1] = b;
//        offset++;
//    }
//
//    // Último frame
//    if (size > 0 && prev_nalType >= 0) {
//        int decodable = (list->Codec == 264) ? is_decodable_h264(prev_nalType) : is_decodable_h265(prev_nalType);
//        if (decodable) {
//            char ftype = (list->Codec == 264) ? guess_frame_type_h264(prev_nalType) : guess_frame_type_h265(prev_nalType);
//            frame_index_list_add(list, start, size, prev_nalType, ftype, pts);
//        }
//    }
//
//    fclose(f);
//    return list;
//}
