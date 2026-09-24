//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  yason_build: montagem de arvores yason (JSON/YAML) sem sprintf em buffer fixo.
//  As rotas montavam JSON concatenando em `char json[2048]`: qualquer sessao ou lista
//  de pistas um pouco maior truncava calada e o front recebia JSON invalido.
//
//  Uso:
//      Element* root = yb_root_object();
//      yb_str(root, "id", "abc");  yb_int(root, "width", 1920);
//      Element* arr = yb_array(root, "tracks");
//      Element* it  = yb_array_object(arr);  yb_str(it, "name", "720p");
//      StringX* s = yb_render(root);   ...   yb_free_render(s);  yb_free(root);
//
//  NOTA: este header ja teve tres contornos (escape manual, selo de campo final e
//  conversao de container vazio) para defeitos do JSON do yason. Todos foram corrigidos
//  na lib (yason abd150f) e REMOVIDOS daqui -- manter o escape manual passaria a
//  escapar duas vezes, agora que o render tambem escapa.

#pragma once

// O yason usa StringX/ListX da xplatbase. Quem inclui gw_base.h antes ativa
// XPB_SKIP_UMBRELLA (para evitar colisao com a stringlib do appserver), e ai o
// xplatbase.h puxado pelo yason nao traz mais esses tipos. Incluir os dois modulos
// diretamente deixa este header independente da ordem de inclusao.
#include "list_hander.h"
#include "string_handler.h"

#include "yason.h"
#include "yason_element.h"
#include "yason_compat.h"
#include <stdio.h>

static inline Element* yb_root_object(void)
{
    Element* e = yason_element_new();
    e->Type = NODE_TYPE_OBJECT;
    e->TreeType = TREE_TYPE_JSON;
    return e;
}

static inline Element* yb__child(Element* parent, const char* name, ElementType type)
{
    Element* e = yason_element_new();
    e->Type = type;
    e->TreeType = parent ? parent->TreeType : TREE_TYPE_JSON;
    e->Parent = parent;
    if (name) yason_string_append(&e->Name, name);
    if (parent) yason_element_array_add(&parent->Children, e);
    return e;
}

// O valor vai CRU na arvore; o escape para JSON e feito pelo render do yason.
static inline void yb_str(Element* parent, const char* name, const char* value)
{
    Element* e = yb__child(parent, name, NODE_TYPE_FIELD);
    e->IsString = 1;
    yason_string_append(&e->Value, value ? value : "");
}

static inline void yb_int(Element* parent, const char* name, long long value)
{
    Element* e = yb__child(parent, name, NODE_TYPE_FIELD);
    char b[32]; snprintf(b, sizeof(b), "%lld", value);
    yason_string_append(&e->Value, b);
}

static inline void yb_num(Element* parent, const char* name, double value, int decimals)
{
    Element* e = yb__child(parent, name, NODE_TYPE_FIELD);
    char fmt[16]; snprintf(fmt, sizeof(fmt), "%%.%df", decimals < 0 ? 3 : decimals);
    char b[64];   snprintf(b, sizeof(b), fmt, value);
    yason_string_append(&e->Value, b);
}

static inline void yb_bool(Element* parent, const char* name, int value)
{
    Element* e = yb__child(parent, name, NODE_TYPE_FIELD);
    yason_string_append(&e->Value, value ? "true" : "false");
}

// Objeto aninhado nomeado: "name": { ... }
static inline Element* yb_object(Element* parent, const char* name)
{ return yb__child(parent, name, NODE_TYPE_OBJECT); }

// Array nomeado: "name": [ ... ]
static inline Element* yb_array(Element* parent, const char* name)
{ return yb__child(parent, name, NODE_TYPE_ARRAY); }

// Objeto anonimo dentro de um array.
static inline Element* yb_array_object(Element* array)
{ return yb__child(array, 0, NODE_TYPE_OBJECT); }

// Escalar (string) dentro de um array.
static inline void yb_array_str(Element* array, const char* value)
{
    Element* e = yb__child(array, 0, NODE_TYPE_SCALAR);
    e->IsString = 1;
    yason_string_append(&e->Value, value ? value : "");
}

// Escalar numerico dentro de um array.
static inline void yb_array_int(Element* array, long long value)
{
    Element* e = yb__child(array, 0, NODE_TYPE_SCALAR);
    char b[32]; snprintf(b, sizeof(b), "%lld", value);
    yason_string_append(&e->Value, b);
}

// Escalar numerico (com casas decimais) dentro de um array.
static inline void yb_array_num(Element* array, double value, int decimals)
{
    Element* e = yb__child(array, 0, NODE_TYPE_SCALAR);
    char fmt[16]; snprintf(fmt, sizeof(fmt), "%%.%df", decimals < 0 ? 3 : decimals);
    char b[64];   snprintf(b, sizeof(b), fmt, value);
    yason_string_append(&e->Value, b);
}

static inline StringX* yb_render(Element* root) { return yason_render(root, 0); }

static inline void yb_free_render(StringX* s)
{
    if (!s) return;
    string_release(s);
    memop_free_raw(s);
}

static inline void yb_free(Element* e)
{
    if (!e) return;
    for (int i = 0; i < e->Children.Count; i++) yb_free(e->Children.Items[i]);
    memop_free_raw(e->Children.Items);
    string_release(&e->Name);
    string_release(&e->Value);
    string_release(&e->Comment);
    memop_free_raw(e);
}
