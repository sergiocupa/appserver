//  Nucleo dos testes, em C.
//
//  A LOGICA de todo teste vive em arquivos .c, compilados como C, iguais as bibliotecas que
//  eles testam -- mesmos tipos, mesmas convencoes, acesso direto as funcoes internas.
//  O unico C++ do projeto e o registro (registro.cpp): uma linha por teste, sem logica,
//  so para o Gerenciador de Testes do Visual Studio listar caso a caso.
//
//  Um teste e uma funcao 'void f(TestResult*)'. Ela comeca chamando t_start e usa T_ASSERT;
//  a primeira falha encerra a funcao com a mensagem, arquivo e linha.

#ifndef CTEST_CORE_H
#define CTEST_CORE_H

typedef struct
{
    int  ok;
    int  skipped;     // 1 = nao verificou nada (faltou codec, hardware, ...)
    char msg[512];
}
TestResult;

void t_start(TestResult* r);
void t_failf(TestResult* r, const char* file, int line, const char* fmt, ...);

// Marca o teste como PULADO, com o motivo. Um teste que sai cedo porque falta um codec ou
// um hardware passava calado, indistinguivel de um que verificou tudo -- e era impossivel
// saber, olhando o verde, o que de fato foi exercitado. T_SKIP diz.
void t_skipf(TestResult* r, const char* fmt, ...);
#define T_SKIP(r, ...) do { t_skipf((r), __VA_ARGS__); return; } while (0)

// Sempre com mensagem: quando falha, ela e a unica coisa que o Gerenciador mostra.
#define T_ASSERT(r, cond, ...)                                  \
    do {                                                        \
        if (!(cond)) {                                          \
            t_failf((r), __FILE__, __LINE__, __VA_ARGS__);      \
            return;                                             \
        }                                                       \
    } while (0)

#endif
