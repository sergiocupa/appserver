#include <stdio.h>
#include <stdint.h>
#include <windows.h>


typedef struct
{
    void (*Sender)(void* object, void* callback, void* client);
    void* Client;
} ThunkArgs;

typedef void(__stdcall* MessageCallback)(void* data, void* callback);


MessageCallback create_trampoline(ThunkArgs* args)
{
    // Aloca memória executável para o trampolim
    void* code = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!code) {
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

    return (MessageCallback)code;
}

// Função para liberar o trampolim quando não for mais necessário
void free_trampoline(MessageCallback trampoline)
{
    if (trampoline) {
        VirtualFree((void*)trampoline, 0, MEM_RELEASE);
    }
}

// Função de exemplo que será chamada pelo trampolim
void my_sender(int* data, int* callback, int* client)
{
    printf("\nSender called!\n");
    printf("Data: %p (valor: %d)\n", data, data ? *(int*)data : 0);
    printf("Callback: %p (valor: %d)\n", callback, callback ? *(int*)callback : 0);
    printf("Client: %p (valor: %d)\n", client, client ? *(int*)client : 0);
}

int main()
{
    // Testing point binding between server->client function

    int cli = 1234;

    ThunkArgs* args = (ThunkArgs*)malloc(sizeof(ThunkArgs));
    args->Sender = my_sender;
    args->Client = &cli;


    MessageCallback call = create_trampoline(args);


    int data     = 12;
    int callback = 34;

    call(&data, &callback);


	getchar();
	return 0;
}