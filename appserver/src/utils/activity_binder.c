#include "activity_binder.h"
#include "stringlib.h"


FunctionBind* binder_route_exist(FunctionBindList* binders, StringArray* prefix, StringArray* route)
{
    if (route->Count > 0)
    {
        int ix = 0;
        if (prefix)
        {
            while (ix < prefix->Count && ix < route->Count)
            {
                if (!string_equals_s(prefix->Items[ix], route->Items[ix]))
                {
                    return false;
                }
                ix++;
            }
        }

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
    return 0;
}