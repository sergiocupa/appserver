//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  gw_path: caminhos e sistema de arquivos, Windows + Linux. O gateway montava caminhos
//  com '\\' literal e o controlador apagava pasta chamando o shell ("rmdir /s /q"), o que
//  prendia a thread, nao reportava erro e nao existia fora do Windows.
//
//  Convencao: TODO caminho montado aqui usa '/' como separador. O Windows aceita '/' em
//  todas as APIs de arquivo, entao um separador so serve as duas plataformas.

#pragma once
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>

#define GW_PATH_SEP '/'

// Concatena 'base' + '/' + 'leaf' em out (sempre termina em '\0'). Retorna out.
char* gw_path_join(char* out, size_t size, const char* base, const char* leaf);

// Troca '\\' por '/' no lugar (para caminhos vindos de fora).
void  gw_path_normalize(char* path);

// Cria o diretorio e todos os pais que faltarem. 1 = existe ao final; 0 = falhou.
int   gw_mkdir_p(const char* path);

// Remove recursivamente arquivo ou diretorio. 1 = nao existe mais ao final (inclui o
// caso "ja nao existia"); 0 = alguma remocao falhou (arquivo em uso, permissao).
int   gw_rmtree(const char* path);

int   gw_file_exists(const char* path);
int   gw_dir_exists(const char* path);

// Enumera as entradas diretas de 'dir' (sem "." e ".."), chamando cb para cada uma.
// 'is_dir' diz se a entrada e um diretorio. Retorna a quantidade visitada, ou -1 se
// 'dir' nao pode ser aberto. cb pode ser NULL (so conta).
typedef void (*GwDirEntryFn)(void* user, const char* name, int is_dir);
int   gw_dir_list(const char* dir, GwDirEntryFn cb, void* user);

// ---- arquivo grande (>2GB) -------------------------------------------------
// fseek/ftell usam long, que no Windows e' 32 bits: um .webm/.mp4 de 3GB estoura
// silenciosamente. Estes tres embrulham a variante 64-bit de cada plataforma.
FILE* gw_fopen_rb(const char* path);
int   gw_fseek64(FILE* f, int64_t off, int origin);
int64_t gw_ftell64(FILE* f);
