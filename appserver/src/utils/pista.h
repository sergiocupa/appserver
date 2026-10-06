//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Pista: fila de trabalho com threads proprias que DORMEM de verdade quando nao ha trabalho.
//
//  Para trabalho que bloqueia ou demora (rotas longas, jobs de video). Diferente do pool de
//  tarefas do xplatbase, que gira (spin) um tempo antes de dormir para responder rapido a
//  tarefas curtas: aqui nao ha giro nem thread de monitor. Parada, a pista custa zero CPU --
//  o que importa num aparelho com bateria.
//
//    - threads criadas sob demanda, ate 'max'; nenhuma enquanto ninguem submeter nada;
//    - com todas ocupadas, o trabalho espera na fila (ordem de chegada);
//    - thread sem trabalho espera numa variavel de condicao (bloqueio do sistema).

#ifndef APPSERVER_PISTA_H
#define APPSERVER_PISTA_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

    typedef struct Pista Pista;

    Pista* pista_criar(int max_threads);
    bool   pista_submeter(Pista* p, void (*fn)(void*), void* arg);   // false = sem memoria/sem thread

    // Diagnostico
    int    pista_threads(Pista* p);      // threads criadas
    int    pista_na_fila(Pista* p);      // trabalhos esperando thread

#ifdef __cplusplus
}
#endif

#endif  // APPSERVER_PISTA_H
