#include "activity_binder.h"
#include "program_util.h"
#include "stringlib.h"
#include "filelib.h"


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



FunctionBind* binder_route_exist(FunctionBindList* binders, StringArray* prefix, StringArray* route)
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
                FunctionBind* bind = binders->Items[ax];

                int cnt = 0;
                int iz = ix;
                int iu = 0;
                while (iz < route->Count && iu < bind->Route.Count)
                {
                    if (string_equals_s(bind->Route.Items[iu], route->Items[iz]))
                    {
                        cnt++;
                    }
                    iz++;
                    iu++;
                }

                if (cnt == bind->Route.Count)
                {
                    return  bind;
                }

                ax++;
            }
        }
    }
    return 0;
}


bool binder_get_web_content(FunctionBindList* binders, StringArray* prefix, StringArray* route, String* abs_path, ResourceBuffer* buffer)
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
                    String amm;
                    string_init(&amm);
                    string_append_s(&amm, abs_path);
                    binder_append_route(&amm, route, ix, true);

                    // pegar extensão do arquivo para definir o type para o request
                    // corrigir erro na funcao file_exists()

                    

                  //  if (file_exists(amm.Data))
                   // {
                        byte* data = 0; int length = 0;
                        bool read = file_read_bin(amm.Data, (byte**)&data, &length);
                        if (read)
                        {
                            buffer->Length = length;
                            buffer->Data   = data;
                            return true;
                        }
                    //}
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