//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  frag_session: sessao de fragmentacao. Cada sessao e uma subpasta sob a raiz de videos,
//  com os arquivos gerados (init/chunks/manifesto) e um `session.json` (renderizado pelo
//  yason) descrevendo entrada, saida, estado e pistas.
//
//  Por que existe: antes a "sessao" era so o nome de uma pasta vindo de um header HTTP.
//  Nao havia listagem, nao havia como cancelar, e um processo interrompido no meio deixava
//  a pasta num estado meio-escrito que a proxima fragmentacao herdava. Aqui o estado mora
//  em DISCO (nada global no processo), entao reiniciar o servidor nao perde nem confunde
//  nada, e cada sessao e independente da anterior.
//
//  Concorrencia: o registro de jobs (id -> GwControl) e protegido por mutex. O cancelamento
//  so levanta a flag; quem observa e o gateway, que aborta de forma cooperativa e limpa.

#pragma once

#include <stddef.h>
#include "sync_gateway.h"   // GwControl

// StringX/ListX antes do yason: o gw_base.h (puxado pelo sync_gateway) liga
// XPB_SKIP_UMBRELLA, entao o xplatbase.h que o yason inclui nao traz mais esses tipos.
#include "list_hander.h"
#include "string_handler.h"
#include "yason_element.h"  // Element

typedef enum
{
    FRAG_IDLE = 0,      // criada, sem fonte processada
    FRAG_READY,         // fonte carregada e sondada; pronta para fragmentar
    FRAG_RUNNING,       // fragmentacao em andamento
    FRAG_DONE,          // saida publicada
    FRAG_CANCELLED,     // interrompida; a saida NAO foi publicada
    FRAG_ERROR
}
FragState;

const char* frag_state_name(FragState s);

// Define a raiz onde as sessoes vivem (ex.: "web/hls") e cria a pasta se faltar.
// Idempotente; chamar uma vez no boot. 1 = ok.
int  frag_session_init(const char* root);

// Cria uma sessao: gera o id, cria a pasta e grava o session.json inicial.
// 'name' e um rotulo humano opcional. out_id recebe o id gerado. 1 = ok.
int  frag_session_create(const char* name, char* out_id, size_t id_size);

int  frag_session_exists(const char* id);
int  frag_session_delete(const char* id);     // cancela o job em voo (se houver) e apaga a pasta

// Caminho da pasta da sessao (ex.: "web/hls/<id>"). 1 = ok.
int  frag_session_dir(const char* id, char* out, size_t size);

// Remove os arquivos gerados da sessao (init/chunks/manifesto/source) preservando o
// session.json. Refragmentar apagando a pasta inteira destruiria o proprio estado da
// sessao -- e um upload novo tem que comecar de uma pasta limpa, senao sobram segmentos
// da configuracao anterior e o player mistura as duas. 1 = pasta limpa ao final.
int  frag_session_clear_output(const char* id);

// Estado em disco. O Element devolvido pertence ao chamador (yb_free).
// frag_session_read: o session.json da sessao. NULL se nao existir.
// frag_session_list: {"sessions":[ ... ]} — sempre um objeto, nunca NULL.
Element* frag_session_read(const char* id);
Element* frag_session_list(void);

// Grava/atualiza campos do session.json. Os setters leem, alteram e regravam:
// o arquivo e pequeno e assim nunca fica meio escrito por um caminho de erro.
int frag_session_set_state(const char* id, FragState state, const char* message);
int frag_session_set_input(const char* id, const char* kind, const char* device, const char* codec,
                           int width, int height, double fps, double duration);
int frag_session_set_output(const char* id, const char* protocol, const char* codec,
                            int width, int height, double fps, int bitrate);
int frag_session_set_result(const char* id, const char* playlist, double elapsed_sec);
// Liga a varredura de alcancabilidade (mem_leak_watch da xplatbase) ao FIM de cada job.
// O monitor ja e iniciado pelo platform_init, mas com limiares de 70%/90% da RAM fisica:
// na pratica ele nunca dispara sozinho. Forcar o scan exatamente na fronteira da sessao e
// o que produz um relatorio util -- e caro (suspende as threads uma a uma), por isso fica
// sob demanda, nao ligado no uso normal.
void frag_session_set_leak_scan(int on);

// Balanco do memory_pool medido ENTRE o inicio e o fim do job desta sessao. E o
// instrumento que responde "a ferramenta acumula memoria de uma sessao para a outra?":
// se 'live' cresce um valor parecido a cada sessao, alguma coisa nao esta sendo liberada.
int frag_session_set_memory(const char* id, long long live_delta, long long os_reserved_delta,
                            unsigned long long allocs, unsigned long long frees);

// Le de volta o que a UI gravou. Campos ausentes ou vazios saem como 0/"" -- e a
// convencao de "nao escolhido, herda da entrada" que o MediaProfile ja entende.
// Retorna 1 se a sessao existe.
int frag_session_get_output(const char* id, char* codec, size_t codec_size,
                            int* width, int* height, double* fps, int* bitrate);
int frag_session_get_input(const char* id, char* kind, size_t kind_size,
                           char* device, size_t device_size, char* codec, size_t codec_size,
                           int* width, int* height, double* fps);

int frag_session_add_track(const char* id, const char* name, int width, int height, int bandwidth);
int frag_session_clear_tracks(const char* id);

// ---- Registro de jobs em execucao ----------------------------------------
// begin: marca a sessao como RUNNING e devolve o GwControl a passar ao gateway_run.
// NULL se a sessao nao existe ou ja tem um job em voo (evita duas fragmentacoes
// gravando na mesma pasta). Sempre parear com frag_session_job_end.
GwControl* frag_session_job_begin(const char* id);
void       frag_session_job_end(const char* id, FragState final_state, const char* message);

// Pede o cancelamento cooperativo. 1 = havia job em voo e a flag foi levantada.
int        frag_session_cancel(const char* id);
int        frag_session_running(const char* id);
