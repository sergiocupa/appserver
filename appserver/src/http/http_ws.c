//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  WebSocket incremental. Ver http_ws.h.

#include "http_ws.h"
#include <string.h>

struct HttpWs
{
    int64            Max;
    HttpWsAoMensagem AoMensagem;
    void*            Ctx;
    StringX*         Buf;      // bytes ainda nao consumidos (quadro pela metade)
    StringX*         Msg;      // mensagem fragmentada sendo remontada
    int              MsgOp;    // opcode da mensagem em remontagem (0 = nenhuma)
    bool             Fechado;
};

HttpWs* http_ws_criar(int64 max_mensagem, HttpWsAoMensagem ao_mensagem, void* ctx)
{
    HttpWs* w = (HttpWs*)memop_calloc_raw(1, sizeof(HttpWs));
    if (!w) return 0;
    w->Max = max_mensagem > 0 ? max_mensagem : 16 * 1024 * 1024;
    w->AoMensagem = ao_mensagem;
    w->Ctx = ctx;
    w->Buf = string_new();
    w->Msg = string_new();
    return w;
}

void http_ws_destruir(HttpWs* w)
{
    if (!w) return;
    if (w->Buf) { string_release(w->Buf); memop_free_raw(w->Buf); }
    if (w->Msg) { string_release(w->Msg); memop_free_raw(w->Msg); }
    memop_free_raw(w);
}

int64 http_ws_quadro(ResourceBuffer* out, int opcode, const byte* dados, int64 n)
{
    byte h[10]; int hl = 0;
    h[hl++] = (byte)(0x80 | (opcode & 0x0F));   // FIN + opcode
    if (n <= 125) h[hl++] = (byte)n;
    else if (n <= 0xFFFF) { h[hl++] = 126; h[hl++] = (byte)(n >> 8); h[hl++] = (byte)n; }
    else { h[hl++] = 127; for (int i = 7; i >= 0; i--) h[hl++] = (byte)((uint64_t)n >> (i * 8)); }
    resource_buffer_append(out, h, hl);
    if (n > 0) resource_buffer_append(out, (byte*)dados, (int)n);
    return hl + n;
}

// Tenta montar UM quadro do inicio do buffer. >0 = bytes consumidos; 0 = incompleto; <0 = invalido.
static int64 um_quadro(HttpWs* w, HttpWsResponder responder)
{
    const byte* b = (const byte*)w->Buf->Content;
    int64 tem = (int64)w->Buf->Length;
    if (tem < 2) return 0;
    int fin = b[0] & 0x80;
    int op  = b[0] & 0x0F;
    int mascarado = b[1] & 0x80;
    int64 len = b[1] & 0x7F;
    int64 pos = 2;
    if (len == 126) { if (tem < 4) return 0; len = ((int64)b[2] << 8) | b[3]; pos = 4; }
    else if (len == 127)
    {
        if (tem < 10) return 0;
        len = 0; for (int i = 0; i < 8; i++) len = (len << 8) | b[2 + i];
        pos = 10;
        if (len < 0) return -1;
    }
    if (!mascarado) return -1;                 // cliente TEM de mascarar (RFC 6455, 5.1)
    if (len > w->Max) return -1;
    if (tem < pos + 4 + len) return 0;         // quadro ainda chegando
    const byte* m = b + pos; pos += 4;

    // desmascara no proprio buffer (ninguem mais le estes bytes)
    byte* p = (byte*)w->Buf->Content + pos;
    for (int64 i = 0; i < len; i++) p[i] ^= m[i & 3];

    if (op >= 0x8)   // controle: nunca fragmentado, <= 125 bytes
    {
        if (!fin || len > 125) return -1;
        if (op == HTTP_WS_PING)
        {
            ResourceBuffer q; resource_buffer_init(&q);
            http_ws_quadro(&q, HTTP_WS_PONG, p, len);
            if (responder) responder(w->Ctx, q.Data, q.Length);
            resource_buffer_release(&q, true);
        }
        else if (op == HTTP_WS_FECHA)
        {
            ResourceBuffer q; resource_buffer_init(&q);
            http_ws_quadro(&q, HTTP_WS_FECHA, p, len >= 2 ? 2 : 0);   // devolve o codigo
            if (responder) responder(w->Ctx, q.Data, q.Length);
            resource_buffer_release(&q, true);
            w->Fechado = true;
        }
        return pos + len;
    }

    if (op == 0x0)   // continuacao
    {
        if (!w->MsgOp) return -1;
    }
    else if (op == HTTP_WS_TEXTO || op == HTTP_WS_BINARIO)
    {
        if (w->MsgOp) return -1;   // mensagem nova no meio de uma fragmentada
        w->MsgOp = op;
        w->Msg->Length = 0;
    }
    else return -1;

    if ((int64)w->Msg->Length + len > w->Max) return -1;
    if (len > 0) string_append_sub(w->Msg, (const char*)p, (int)len, 0, (int)len);
    if (fin)
    {
        w->MsgOp = 0;
        w->AoMensagem(w->Ctx, (const byte*)w->Msg->Content, (int64)w->Msg->Length);
        w->Msg->Length = 0;
    }
    return pos + len;
}

bool http_ws_receber(HttpWs* w, const byte* dados, int n, HttpWsResponder responder)
{
    if (w->Fechado) return false;
    if (n > 0) string_append_sub(w->Buf, (const char*)dados, n, 0, n);
    for (;;)
    {
        int64 k = um_quadro(w, responder);
        if (k < 0) { w->Fechado = true; return false; }
        if (k == 0) break;
        string_resize_forward(w->Buf, (int)k);
        if (w->Fechado) return false;
    }
    return true;
}
