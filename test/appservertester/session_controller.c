//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Rotas de sessao de fragmentacao. Toda resposta e montada com yason (nada de sprintf
//  em buffer fixo, que truncava calado quando a lista crescia).

#include "session_controller.h"
#include "frag_session.h"
#include "yason_build.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// Copia o segmento de rota no indice absoluto 'idx' (vazio se fora da faixa).
static void route_at(Message* message, int idx, char* out, size_t out_size)
{
    if (out_size > 0) out[0] = '\0';
    if (idx < 0 || idx >= message->Route.Count) return;
    StringX* s = (StringX*)message->Route.Items[idx];
    int n = s->Length < (int)out_size - 1 ? s->Length : (int)out_size - 1;
    memcpy(out, s->Content, n);
    out[n] = '\0';
}

static void respond_element(Message* message, Element* root, int status)
{
    StringX* s = yb_render(root);
    if (s)
    {
        message->Response = message_response_create_content(status, APPLICATION_JSON, s->Content, s->Length);
        yb_free_render(s);
    }
    else message->Response = message_response_create_text(HTTP_STATUS_INTERNAL_ERROR, "render falhou");
    yb_free(root);
}

// Leitores tolerantes: campo ausente vira ""/0, que e a convencao de "nao escolhido".
static const char* field_text(Element* obj, const char* name)
{
    Element* e = obj ? yason_find_element(obj, name) : 0;
    return (e && e->Value.Content) ? e->Value.Content : "";
}

static int field_int(Element* obj, const char* name)
{
    Element* e = obj ? yason_find_element(obj, name) : 0;
    return (e && e->Value.Content) ? atoi(e->Value.Content) : 0;
}

static double field_dbl(Element* obj, const char* name)
{
    Element* e = obj ? yason_find_element(obj, name) : 0;
    return (e && e->Value.Content) ? atof(e->Value.Content) : 0.0;
}

static void respond_error(Message* message, int status, const char* msg)
{
    Element* root = yb_root_object();
    yb_bool(root, "ok", 0);
    yb_str(root, "error", msg);
    respond_element(message, root, status);
}

Element* session_route(Message* message)
{
    if (!message) return 0;

    // Rota: .../session/<action>[/<id>]  -- ancora em "session" para nao depender do
    // prefixo configurado no appserver_create.
    int base = -1;
    for (int i = 0; i < message->Route.Count; i++)
    {
        StringX* s = (StringX*)message->Route.Items[i];
        if (s->Length == 7 && memcmp(s->Content, "session", 7) == 0) { base = i; break; }
    }
    if (base < 0) { respond_error(message, HTTP_STATUS_BAD_REQUEST, "rota invalida"); return 0; }

    char action[32], id[80];
    route_at(message, base + 1, action, sizeof(action));
    route_at(message, base + 2, id, sizeof(id));

    if (strcmp(action, "list") == 0)
    {
        Element* root = frag_session_list();
        respond_element(message, root, HTTP_STATUS_OK);
        return 0;
    }

    if (strcmp(action, "create") == 0)
    {
        // Nome opcional no corpo JSON: {"name":"..."} (o appserver ja parseou em Object).
        const char* name = 0;
        Element* obj = (Element*)message->Object;
        if (obj)
        {
            Element* n = yason_find_element(obj, "name");
            if (n && n->Value.Length > 0) name = n->Value.Content;
        }

        char new_id[80];
        if (!frag_session_create(name, new_id, sizeof(new_id)))
        { respond_error(message, HTTP_STATUS_INTERNAL_ERROR, "nao foi possivel criar a sessao"); return 0; }

        Element* root = frag_session_read(new_id);
        if (!root) { respond_error(message, HTTP_STATUS_INTERNAL_ERROR, "sessao criada mas ilegivel"); return 0; }
        respond_element(message, root, HTTP_STATUS_OK);
        return 0;
    }

    if (id[0] == '\0')
    { respond_error(message, HTTP_STATUS_BAD_REQUEST, "id de sessao ausente (use create|list|get/<id>|cancel/<id>|delete/<id>)"); return 0; }

    if (strcmp(action, "get") == 0)
    {
        Element* root = frag_session_read(id);
        if (!root) { respond_error(message, HTTP_STATUS_NOT_FOUND, "sessao nao encontrada"); return 0; }
        yb_bool(root, "running", frag_session_running(id));
        respond_element(message, root, HTTP_STATUS_OK);
        return 0;
    }

    if (strcmp(action, "config") == 0)
    {
        // Grava as escolhas de ENTRADA e SAIDA na sessao. O que vier ausente ou zerado
        // fica como "nao escolhido": o gateway herda da entrada na hora de fragmentar.
        Element* obj = (Element*)message->Object;
        if (!obj) { respond_error(message, HTTP_STATUS_BAD_REQUEST, "corpo JSON ausente"); return 0; }

        Element* in = yason_find_element(obj, "input");
        if (in)
        {
            frag_session_set_input(id,
                field_text(in, "kind"), field_text(in, "device"), field_text(in, "codec"),
                field_int(in, "width"), field_int(in, "height"),
                field_dbl(in, "fps"), field_dbl(in, "duration"));
        }

        Element* out = yason_find_element(obj, "output");
        if (out)
        {
            frag_session_set_output(id,
                field_text(out, "protocol"), field_text(out, "codec"),
                field_int(out, "width"), field_int(out, "height"),
                field_dbl(out, "fps"), field_int(out, "bitrate"));
        }

        Element* root = frag_session_read(id);
        if (!root) { respond_error(message, HTTP_STATUS_NOT_FOUND, "sessao nao encontrada"); return 0; }
        respond_element(message, root, HTTP_STATUS_OK);
        return 0;
    }

    if (strcmp(action, "cancel") == 0)
    {
        int had = frag_session_cancel(id);
        Element* root = yb_root_object();
        yb_bool(root, "ok", 1);
        yb_str(root, "id", id);
        // 0 = nao havia nada rodando; nao e erro (cancelar duas vezes e inofensivo).
        yb_bool(root, "wasRunning", had);
        respond_element(message, root, HTTP_STATUS_OK);
        return 0;
    }

    if (strcmp(action, "delete") == 0)
    {
        int ok = frag_session_delete(id);
        Element* root = yb_root_object();
        yb_bool(root, "ok", ok);
        yb_str(root, "id", id);
        if (!ok) yb_str(root, "error", "nao foi possivel remover (arquivo em uso?)");
        respond_element(message, root, ok ? HTTP_STATUS_OK : HTTP_STATUS_INTERNAL_ERROR);
        return 0;
    }

    respond_error(message, HTTP_STATUS_BAD_REQUEST, "acao desconhecida");
    return 0;
}
