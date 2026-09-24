//  Modulo auxiliar do teste NEGATIVO: linka o xplatbase ESTATICAMENTE DE PROPOSITO, para
//  provocar a segunda instancia no processo. E a unica forma de provar que o detector
//  funciona -- um detector que nunca foi visto acusando nao vale nada.
//
//  A politica de aborto e desligada pelo teste ANTES de carregar este modulo (variavel de
//  ambiente XPLATBASE_DUPLICATE_FATAL), porque o xplatbase se inicializa sozinho na carga:
//  quando o codigo daqui roda, a deteccao ja aconteceu.

#include "xplatbase.h"
#include "memory_pool.h"
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
  #define EXPORTA __declspec(dllexport)
#else
  #define EXPORTA __attribute__((visibility("default")))
#endif

// Devolve 1 se ESTA copia detectou que ja havia outra instancia, e escreve em 'primeiro'
// o modulo de onde a instancia original veio.
EXPORTA int dup_verifica(char* primeiro, int tam)
{
    XplatInstanceInfo first;
    int dup;

    platform_init();                 // idempotente; a carga do modulo ja disparou
    dup = xplat_instance_check(&first);
    if (primeiro && tam > 0) snprintf(primeiro, (size_t)tam, "%s", dup ? first.Module : xplat_instance_module());
    return dup;
}
