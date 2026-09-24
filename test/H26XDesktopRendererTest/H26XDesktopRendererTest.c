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


#include "MediaFragmenter.h"



#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

// ============================================
// Contexto de erro
// ============================================

typedef struct {
    const char* func;
    const char* file;
    int line;
} CallContext;

// Handler global - pode ser substituído
typedef void (*ErrorHandler)(const CallContext* ctx, const char* msg);

static void default_handler(const CallContext* ctx, const char* msg) {
    fprintf(stderr, "\n[ERRO] %s\n", msg);
    fprintf(stderr, "  Origem: %s()\n", ctx->func);
    fprintf(stderr, "  Arquivo: %s:%d\n", ctx->file, ctx->line);
}

static ErrorHandler g_error_handler = default_handler;

void set_error_handler(ErrorHandler handler) {
    g_error_handler = handler ? handler : default_handler;
}

// ============================================
// Funções internas (não chamar diretamente)
// ============================================

static void trigger_error(const CallContext* ctx, const char* fmt, ...) {
    char buffer[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    if (g_error_handler) {
        g_error_handler(ctx, buffer);
    }
}

static void* alloc_impl(size_t size, const char* func, const char* file, int line) {
    void* ptr = malloc(size);

    if (!ptr) {
        CallContext ctx = { func, file, line };
        trigger_error(&ctx, "Falha ao alocar %zu bytes", size);
        return NULL;
    }

    return ptr;
}

static void* realloc_impl(void* ptr, size_t size, const char* func, const char* file, int line) {
    void* new_ptr = realloc(ptr, size);

    if (!new_ptr && size > 0) {
        CallContext ctx = { func, file, line };
        trigger_error(&ctx, "Falha ao realocar para %zu bytes", size);
        return NULL;
    }

    return new_ptr;
}


#define mem_alloc(size)        alloc_impl(size, __func__, __FILE__, __LINE__)
#define mem_realloc(ptr, size) realloc_impl(ptr, size, __func__, __FILE__, __LINE__)


void funcao_que_aloca(void) 
{
    // Simula falha pedindo memoria absurda
    void* ptr = mem_alloc((size_t)-1);

    if (!ptr) {
        printf("Retornou NULL como esperado\n");
    }
}

void outra_funcao(void)
{
    funcao_que_aloca();
}

// Handler customizado para teste
void meu_handler(const CallContext* ctx, const char* msg) {
    printf("\n=== MEU HANDLER ===\n");
    printf("Mensagem: %s\n", msg);
    printf("Chamado de: %s() em %s linha %d\n", ctx->func, ctx->file, ctx->line);
    printf("===================\n");
    exit(1);
}




int main()
{
    printf("--- Teste com handler padrao ---\n");
    funcao_que_aloca();

    printf("\n--- Teste com handler customizado ---\n");
    set_error_handler(meu_handler);
    outra_funcao();



	const char* path1 = "e:/AmostraVideo/sample-3.mp4";
    const char* path2 = "e:/AmostraVideo/BigH265.mp4";

    FrameIndexList* list = mp4builder_get_frames(path2);
    if (!list) return;


    FILE* file = fopen(path2, "rb");
    if (!file) {
        perror("Erro ao abrir arquivo");
        return;
    }

    MediaSourceSession* session = media_sim_create(list->Metadata.Width / 2, list->Metadata.Height / 2, list->Metadata.Codec);


    MediaBuffer mi;
    h26x_create_annexb(&list->Metadata, &mi);
    media_sim_feed(session, &mi);
    free(mi.Data);

    int res = 0;

    // Loop por frames
    for (uint32_t i = 0; i < list->Count; i++)
    {
        FrameIndex* frame = list->Frames[i];

        MediaBuffer mb;
        res = h26x_put_single_frame(file, frame, &list->Metadata, &mb);

        if (!res)
        {
            int fe = media_sim_feed(session, &mb);
            free(mb.Data);
        }

        Sleep(33);
    }

    // Fecha sessão
    // media_sim_destroy(session);
    fclose(file);
}

