//  MIT License – Modified for Mandatory Attribution
//  
//  Copyright(c) 2025 Sergio Paludo
//
//  github.com/sergiocupa
//  
//  Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files, 
//  to use, copy, modify, merge, publish, distribute, and sublicense the software, including for commercial purposes, provided that:
//  
//     01. The original author’s credit is retained in all copies of the source code;
//     02. The original author’s credit is included in any code generated, derived, or distributed from this software, including templates, libraries, or code - generating scripts.
//  
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED.


#include "activity_binder.h"
#include "program_util.h"
#include "stringlib.h"
#include "filelib.h"
#include <stdlib.h>


static int string_endsoff_token(String* a, int a_start, int a_leng, String* token, int token_start, int token_leng)
{
    int ia  = a_start;
    int it  = token_start;
    int val = token->Length - token_start;

    while (ia < a->Length && it < token->Length)
    {
        if (a->Data[ia] != token->Data[it])
        {
            return -1;
        }
        ia++;
        it++;
    }
    return token->Length > 0 && ia == val;
}

static int string_ends_off_s(String* route, String* extension)
{
    if (route && extension)
    {
        if (route->Length > 0 && route->Length == extension->Length)
        {
            int ir = route->Length; int ie = extension->Length;
            while (ir > 0 && ie > 0)
            {
                ir--; ie--;
                if (route->Data[ir] != extension->Data[ie])
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
}


static ContentTypeOption get_type_file(String* path)
{
    int ix = path->Length;
    while (ix > 0)
    {
        ix--;
        if (path->Data[ix] == '.')
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

        buffer.Data = malloc(buffer.Length);
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


void binder_append_route(String* content, StringArray* route, int route_start, bool append_backslash)
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


bool binder_prefix_exist(StringArray* prefix, StringArray* route, int* ix)
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





FunctionBind* binder_extension_exist(FunctionBindList* binders, StringArray* prefix, String* extension)
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
                if (found) return 1;
                ax++;
            }
        }
    }
    return 0;
}


FunctionBind* binder_route_exist(FunctionBindList* binders, StringArray* prefix, StringArray* route, int* route_rest_index)
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
                        String* a = bind->Route.Items[iu];
                        String* b = route->Items[iz];

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
                        String* tk = route->Items[route->Count - 1];
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


bool binder_get_web_resource(FunctionBindList* binders, StringArray* prefix, StringArray* route, String* abs_path, ResourceBuffer* buffer)
{
    if (route->Count > 0)
    {
        int ix = 0;
        bool found = binder_prefix_exist(prefix, route, &ix);
        if (found)
        {
            if (ix < route->Count)
            {
                bool found2 = false;
                int ax = 0;
                while (ax < binders->Count)
                {
                    FunctionBind* bind = binders->Items[ax];

                    // Pelo menos primeiro nivel
                    if (bind->Route.Count > 0 && string_equals_s(bind->Route.Items[0], route->Items[ix]))
                    {
                        found2 = true;
                        break;
                    }
                    ax++;
                }

                if (found2)  
                {
                    if(!buffer) return true;

                    String amm;
                    string_init(&amm);
                    string_append_s(&amm, abs_path);
                    binder_append_route(&amm, route, ix, true);

                    ContentTypeOption type = get_type_file(&amm);
                    bool is_text = type == TEXT_HTML || type == TEXT_CSS || type == TEXT_JAVASCRIPT || type == TEXT_PLAIN || type == APPLICATION_JAVASCRIPT || type == APPLICATION_JSON || type == APPLICATION_XML;

                    // TO-DO: corrigir file_exists() na biblioteca 'filelib'
                    if (_file_exists(amm.Data))
                    {
                        byte* data = 0; int length = 0;
                        bool s = is_text ? file_read_text(amm.Data, (byte**)&data, &length) : file_read_bin(amm.Data, (byte**)&data, &length);
                        if (s != 0)
                        {
                            buffer->Type   = type;
                            buffer->Length = length;
                            buffer->Data   = data;
                            return true;
                        }
                    }
                }
            }
        }
    }
    return false;
}


void binder_assemble_local(String* local, const char* abs_path, StringArray* prefix, StringArray* route)
{
    int leng = local->Length;

    if (abs_path)
    {
        string_append(local, abs_path);
    }

    binder_append_route(local, prefix, 0, (local->Length > leng));
    binder_append_route(local, route, 0, (local->Length > leng));
}


void binder_get_web_content_path(FunctionBindList* bind_list, StringArray* prefix, String* local)
{
    char path[1024];
    program_get_path(path);

    if (local)
    {
        string_append(local, path);
        binder_append_route(local, prefix,0, true);
    }

    int ix = 0;
    while (ix < bind_list->Count)
    {
        FunctionBind* bind = bind_list->Items[ix];
        if (bind->IsWebApplication && bind->AbsPathWebContent.MaxLength <= 0)
        {
            binder_assemble_local(&bind->AbsPathWebContent, path, prefix, &bind->Route);
        }
        ix++;
    }
}