//  MIT License � Modified for Mandatory Attribution
//  
//  Copyright(c) 2025 Sergio Paludo
//
//  github.com/sergiocupa
//  
//  Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files, 
//  to use, copy, modify, merge, publish, distribute, and sublicense the software, including for commercial purposes, provided that:
//  
//     01. The original author�s credit is retained in all copies of the source code;
//     02. The original author�s credit is included in any code generated, derived, or distributed from this software, including templates, libraries, or code - generating scripts.
//  
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED.


#include "activity_binder.h"
#include "program_util.h"
#include "xpb_compat.h"
#include <stdlib.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif


static int string_endsoff_token(StringX* a, int a_start, int a_leng, StringX* token, int token_start, int token_leng)
{
    int ia  = a_start;
    int it  = token_start;
    int val = token->Length - token_start;

    while (ia < a->Length && it < token->Length)
    {
        if (a->Content[ia] != token->Content[it])
        {
            return -1;
        }
        ia++;
        it++;
    }
    return token->Length > 0 && ia == val;
}

static int string_ends_off_s(StringX* route, StringX* extension)
{
    if (route && extension)
    {
        if (route->Length > 0 && route->Length == extension->Length)
        {
            int ir = route->Length; int ie = extension->Length;
            while (ir > 0 && ie > 0)
            {
                ir--; ie--;
                if (route->Content[ir] != extension->Content[ie])
                {
                    break;
                }
            }
            if (ir == 0) return 1;
        }
    }
    return -1;
}


bool _file_exists(const char* path)
{
#ifdef _WIN32
    bool result = false;

    DWORD file_attr = GetFileAttributes(path);

    if (file_attr == INVALID_FILE_ATTRIBUTES)
    {
        result = (GetLastError() != ERROR_FILE_NOT_FOUND);
    }
    else
    {
        result = true;
    }
    return result;
#else
    struct stat st;
    return stat(path, &st) == 0;
#endif
}


static ContentTypeOption get_type_file(StringX* path)
{
    int ix = path->Length;
    while (ix > 0)
    {
        ix--;
        if (path->Content[ix] == '.')
        {
            break;
        }
    }
    if (ix + 1 < path->Length)
    {
        int cnt = path->Length - ix;
        if (string_equals_range(path, ix, cnt, ".html"))
        {
            return TEXT_HTML;
        }
        else if (string_equals_range(path, ix, cnt, ".css"))
        {
            return TEXT_CSS;
        }
        else if (string_equals_range(path, ix, cnt, ".js"))
        {
            return APPLICATION_JAVASCRIPT;
        }
        else if (string_equals_range(path, ix, cnt, ".json"))
        {
            return APPLICATION_JSON;
        }
        else if (string_equals_range(path, ix, cnt, ".xml"))
        {
            return APPLICATION_XML;
        }
        else if (string_equals_range(path, ix, cnt, ".jpeg"))
        {
            return IMAGE_JPEG;
        }
        else if (string_equals_range(path, ix, cnt, ".png"))
        {
            return IMAGE_PNG;
        }
        else if (string_equals_range(path, ix, cnt, ".gif"))
        {
            return IMAGE_GIF;
        }
        else if (string_equals_range(path, ix, cnt, ".svg"))
        {
            return IMAGE_SVG;
        }
        else if (string_equals_range(path, ix, cnt, ".mp3"))
        {
            return AUDIO_MPEG;
        }
        else if (string_equals_range(path, ix, cnt, ".ogg"))
        {
            return AUDIO_OGG;
        }
        else if (string_equals_range(path, ix, cnt, ".mp4"))
        {
            return VIDEO_MP4;
        }
        else if (string_equals_range(path, ix, cnt, ".webm"))
        {
            return VIDEO_WEBM;
        }
        else if (string_equals_range(path, ix, cnt, ".m3u8"))
        {
            return APPLICATION_MPEGURL;
        }
        else if (string_equals_range(path, ix, cnt, ".m4s"))
        {
            return VIDEO_MP4;
        }
        else if (string_equals_range(path, ix, cnt, ".pdf"))
        {
            return APPLICATION_PDF;
        }
        else if (string_equals_range(path, ix, cnt, ".gzip"))
        {
            return APPLICATION_GZIP;
        }
        else if (string_equals_range(path, ix, cnt, ".zip"))
        {
            return APPLICATION_ZIP;
        }
        else if (string_equals_range(path, ix, cnt, ".bin"))
        {
            return APPLICATION_OCTET_STREAM;
        }
        /* else if (string_equals_range(path, ix, cnt, ".txt"))
         {
             return TEXT_PLAIN;
         }*/

    }
    return TEXT_PLAIN;
}


ResourceBuffer _file_read_bin(const char* path_file)
{
    ResourceBuffer buffer;
    memset(&buffer,0,sizeof(ResourceBuffer));

    FILE* file;
    errno_t fe = fopen_s(&file, path_file, "rb");

    if (fe == 0)
    {
        fseek(file, 0, SEEK_END);
        buffer.Length = ftell(file);
        fseek(file, 0, SEEK_SET);

        buffer.Data = memop_alloc_raw(buffer.Length);
        size_t bytesRead = fread(buffer.Data, 1, buffer.Length, file);

        if (bytesRead != buffer.Length)
        {
            perror("Erro ao ler o arquivo");
            fclose(file);
            return buffer;
        }

        fclose(file);
        buffer.Type = APPLICATION_OCTET_STREAM;
    }
    return buffer;
}


void binder_append_route(StringX* content, ListX* route, int route_start, bool append_backslash)
{
    if (route && route->Count > route_start)
    {
        if (append_backslash) string_append_char(content, '\\');

        int CNT = route->Count - 1;
        int im = route_start;
        while (im < CNT)
        {
            string_append_s(content, route->Items[im]);
            string_append_char(content, '\\');
            im++;
        }
        string_append_s(content, route->Items[im]);
    }
}


bool binder_prefix_exist(ListX* prefix, ListX* route, int* ix)
{
    int i = (*ix);
    if (prefix && prefix->Count > 0)
    {
        while (i < prefix->Count && i < route->Count)
        {
            if (!string_equals_s(prefix->Items[i], route->Items[i]))
            {
                i++;
                (*ix) = i;
                return false;
            }
            i++;
        }
        (*ix) = i;
        return true;
    }
    else
    {
        (*ix) = i;
        return true;
    }
}





FunctionBind* binder_extension_exist(FunctionBindList* binders, ListX* prefix, StringX* extension)
{
    if (extension->Length > 0)
    {
        int ix = 0;
        bool found = binder_prefix_exist(prefix, extension, &ix);
        if (found)
        {
            int ax = 0;
            while (ax < binders->Count)
            {
                FunctionBind* bind = binders->Items[ax];
                int found = string_ends_off_s(&bind->Route, extension);
                if (found) return bind;   // era "return 1": ponteiro 0x1 para quem chamasse
                ax++;
            }
        }
    }
    return 0;
}


FunctionBind* binder_route_exist(FunctionBindList* binders, ListX* prefix, ListX* route, int* route_rest_index)
{
    if (route->Count > 0)
    {
        int ix = 0;
        bool found = binder_prefix_exist(prefix, route, &ix);
        if (found)
        {
            int ax = 0;
            while (ax < binders->Count)
            {
                int cnt = 0;
                FunctionBind* bind = binders->Items[ax];

                if (bind->Route.Count > 0)
                {
                    int cnt = 0;
                    int iz = ix;
                    int testing = 0;
                    int start_rest = 0;
                    int iu = 0;
                    while (iz < route->Count && iu < bind->Route.Count)
                    {
                        StringX* a = bind->Route.Items[iu];
                        StringX* b = route->Items[iz];

                        if (!testing && string_equals_s(bind->Route.Items[iu], route->Items[iz]))
                        {
                            cnt++;
                        }
                        else
                        {
                            testing = 1;
                            if (!start_rest)
                            {
                                start_rest = 1;
                                *route_rest_index = ix;
                            }
                        }
                        iz++;
                        iu++;
                    }

                    if (bind->Route.Count > 0 && cnt == bind->Route.Count)
                    {
                        return bind;
                    }
                }
                else if (bind->Extension.Length > 0)// testa somente ultimo item
                {
                    if (route->Count > 0) 
                    {
                        StringX* tk = route->Items[route->Count - 1];
                        int tp = tk->Length - bind->Extension.Length;
                        if (tp < 0) tp = 0;

                        if (string_endsoff_token(&bind->Extension, 0, bind->Extension.Length, tk, tp, bind->Extension.Length) == 1)
                        {
                            return bind;
                        }
                    }
                }
                ax++;
            }
        }
    }
    return 0;
}


bool binder_get_web_resource(ListX* route, StringX* abs_path, ResourceBuffer* buffer)
{
    if (!route || !abs_path || abs_path->Length <= 0) return false;

    for (int i = 0; i < route->Count; i++)
    {
        StringX* segment = route->Items[i];
        bool invalid = string_equals_c(segment, "..");
        for (int c = 0; !invalid && c < segment->Length; c++)
        {
            if (segment->Content[c] == '\\' || segment->Content[c] == ':')
            {
                invalid = true;
            }
        }
        if (invalid)
        {
            return false;
        }
    }

    StringX path;
    string_init(&path);
    string_append_s(&path, abs_path);
    if (route->Count > 0)
    {
        binder_append_route(&path, route, 0, true);
    }
    else
    {
        string_appends(&path, "\\index.html", (int)strlen("\\index.html"), 0, (int)strlen("\\index.html"));
    }

    bool result = false;
    if (_file_exists(path.Content))
    {
        if (!buffer)
        {
            result = true;
        }
        else
        {
            ContentTypeOption type = get_type_file(&path);
            bool is_text = type == TEXT_HTML || type == TEXT_CSS || type == TEXT_JAVASCRIPT || type == TEXT_PLAIN || type == APPLICATION_JAVASCRIPT || type == APPLICATION_JSON || type == APPLICATION_XML || type == APPLICATION_MPEGURL;
            byte* data = 0;
            int length = 0;
            bool loaded = is_text ? file_read_text(path.Content, (char**)&data, &length) : file_read_bin(path.Content, &data, &length);
            if (loaded)
            {
                buffer->Type = type;
                buffer->Length = length;
                buffer->Data = data;
                result = true;
            }
        }
    }

    string_release_data(&path);
    return result;
}
