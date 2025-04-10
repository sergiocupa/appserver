#include "../event_server.h"
#include "callback_binder.h"
#include "stdlib.h"
#include <stdio.h>
#include <stdint.h>

#if defined(PLATFORM_WIN)
    #include <windows.h> 
#else 
    #include <sys/mman.h>
#endif 


static void* memo_map(size_t size) 
{
#if defined(PLATFORM_WIN)
    void* code = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    return code;
#else 
    void* code = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    return code;
#endif 
}


// Monta seguinte trampoline entre chamadas
// trampoline(ResourceBuffer* object, MessageMatchReceiverCalback callback)
//{
//    ThunkArgs.Sender(object, callback, ThunkArgs.Client);
//}
static MessageEmitterCalback create_trampoline_win_x64(ThunkArgs* args)
{
    // Aloca memória executável para o trampolim
    void* code = memo_map(4096);
    if (!code) 
    {
        return NULL;
    }

    unsigned char* ptr = (unsigned char*)code;
    int i = 0;

    // Prologo: Salva os registradores não voláteis que usaremos (rbx, rsi e rdi)
    ptr[i++] = 0x53;              // push rbx
    ptr[i++] = 0x56;              // push rsi
    ptr[i++] = 0x57;              // push rdi

    // Salva os parâmetros originais:
    //  - data está em RCX: salva em RBX
    //  - callback está em RDX: salva em RSI
    ptr[i++] = 0x48; ptr[i++] = 0x89; ptr[i++] = 0xCB;  // mov rbx, rcx
    ptr[i++] = 0x48; ptr[i++] = 0x89; ptr[i++] = 0xD6;  // mov rsi, rdx

    // Carrega o endereço de ThunkArgs em RDI
    ptr[i++] = 0x48; ptr[i++] = 0xBF;                  // mov rdi, imm64
    *(ThunkArgs**)(ptr + i) = args;                     // insere o endereço real de args
    i += 8;

    // Carrega args->Sender em RAX
    ptr[i++] = 0x48; ptr[i++] = 0x8B; ptr[i++] = 0x07;  // mov rax, [rdi]

    // Prepara os parâmetros para a chamada de Sender:
    // Restaura 'data' para RCX (valor salvo em RBX)
    ptr[i++] = 0x48; ptr[i++] = 0x89; ptr[i++] = 0xD9;  // mov rcx, rbx
    // Restaura 'callback' para RDX (valor salvo em RSI)
    ptr[i++] = 0x48; ptr[i++] = 0x89; ptr[i++] = 0xD2;  // mov rdx, rsi
    // Carrega o client (args->Client) em R8 a partir do endereço em RDI (args)
    ptr[i++] = 0x4C; ptr[i++] = 0x8B; ptr[i++] = 0x47; ptr[i++] = 0x08;  // mov r8, [rdi+8]

    // Chama args->Sender (endereço armazenado em RAX)
    ptr[i++] = 0xFF; ptr[i++] = 0xD0;                  // call rax

    // Epilogo: Restaura os registradores não voláteis (na ordem inversa)
    ptr[i++] = 0x5F;              // pop rdi
    ptr[i++] = 0x5E;              // pop rsi
    ptr[i++] = 0x5B;              // pop rbx
    ptr[i++] = 0xC3;              // ret

    return (MessageEmitterCalback)code;
}

// TO-DO: not tested
MessageEmitterCalback create_trampoline_linux_x64(ThunkArgs* args)
{
    void* code = memo_map(4096);
    if (!code)
    {
        return NULL;
    }

    unsigned char* ptr = (unsigned char*)code;
    int i = 0;

    // mov rax, <args>
    ptr[i++] = 0x48; ptr[i++] = 0xB8;
    *(ThunkArgs**)(&ptr[i]) = args;
    i += 8;

    // mov rdx, [rax+8]    ; rdx = args->Client
    ptr[i++] = 0x48; ptr[i++] = 0x8B; ptr[i++] = 0x50; ptr[i++] = 0x08;

    // mov rax, [rax]      ; rax = args->Sender
    ptr[i++] = 0x48; ptr[i++] = 0x8B; ptr[i++] = 0x00;

    // call rax
    ptr[i++] = 0xFF; ptr[i++] = 0xD0;

    // ret
    ptr[i++] = 0xC3;

    return (MessageEmitterCalback)code;
}

// TO-DO: not tested
MessageEmitterCalback create_trampoline_linux_arm64(ThunkArgs* args)
{
    void* code = memo_map(4096);
    if (!code)
    {
        return NULL;
    }

    uint32_t* instr = (uint32_t*)code;
    int i = 0;

    // LDR x3, =args         ; constante args no final do código
    instr[i++] = 0x58000063; // LDR x3, #offset (literal)

    // LDR x2, [x3, #8]      ; client = args->Client
    instr[i++] = 0xF9430062;

    // LDR x3, [x3]          ; x3 = args->Sender
    instr[i++] = 0xF9400063;

    // MOV x8, x3            ; preparar para BLR
    instr[i++] = 0xAA0303E8;

    // BLR x8                ; chama Sender(x0=data, x1=callback, x2=client)
    instr[i++] = 0xD63F0100;

    // RET
    instr[i++] = 0xD65F03C0;

    // Constante args literal (após 7 instruções)
    uintptr_t* literal = (uintptr_t*)&instr[i];
    *literal = (uintptr_t)args;

    return (MessageEmitterCalback)code;
}


static MessageEmitterCalback create_trampoline(ThunkArgs* args)
{
#if defined(PLATFORM_WIN)
    return create_trampoline_win_x64(args);
#else 
    return create_trampoline_linux_arm64(args);
#endif 

    // TO-DO: create preprocessor to identify ARM64
}





void binder_list_add_receiver(FunctionBindList* list, const char* route, MessageMatchReceiverCalback function, bool with_callback)
{
    FunctionBind* bind = bind_create(route);
    bind->WithCallback     = with_callback;

    bind_list_add(list, bind);
}

void binder_list_add_web_resource(FunctionBindList* list, const char* route, MessageMatchReceiverCalback function)
{
    FunctionBind* bind = bind_create(route);
    bind->IsWebApplication = true;

    bind_list_add(list, bind);
}

MessageEmitterCalback binder_list_add_emitter(FunctionBindList* list, const char* route)
{
    FunctionBind* bind = bind_create(route);

    bind->WithCallback     = true;
    bind->IsEventEmitter   = true;
    bind->Thung.Sender     = event_sender_server;

    bind_list_add(list, bind);

    MessageEmitterCalback emmiter = create_trampoline(&bind->Thung);

    return emmiter;
}