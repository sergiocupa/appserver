//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Extrai o PTS por frame de video (ms, ordem de decodificacao) do MP4 via stts+mdhd.
//  Usado para afinacao exata dos timestamps (suporta fps fracionario/VFR).
//  Obs.: usa duracoes de decodificacao (stts); com B-frames o PTS exato exigiria ctts.

#pragma once
#include <stdint.h>

// Retorna 0 em sucesso; *out_pts (memop_alloc_raw, ms) e *out_count preenchidos. Caller libera *out_pts.
int mp4_video_pts_ms(const char* mp4_path, int64_t** out_pts, uint32_t* out_count);

// Tempos de CADA amostra de video, na ordem do arquivo (ordem de decodificacao), em us e ja
// como apresentados (lista de edicao aplicada):
//   pts = apresentacao (stts + ctts)      dts = decodificacao (stts)      dts <= pts
// Com quadros B os dois diferem e o pts nao cresce na ordem do arquivo. Quadro com pts < 0
// e pre-rolo de arquivo cortado: precisa ser decodificado, nao e apresentado.
// 0 em sucesso; *out_pts e *out_dts sao do chamador (memop_free_raw).
int mp4_video_times_us(const char* mp4_path, int64_t** out_pts, int64_t** out_dts, uint32_t* out_count);

// Metadados de video (sem ffprobe): largura/altura, fps, codec ("h264"/"hevc") e duracao (s).
int mp4_video_info(const char* mp4_path, int* w, int* h, double* fps, char codec[8], double* duration);

// LISTA DE EDICAO (edts/elst). Diz em que ponto da midia de cada faixa a apresentacao comeca:
//   tempo de apresentacao = tempo de midia (o do stts, ordem de decodificacao) + deslocamento
// Devolve o deslocamento de cada faixa, em microssegundos (0 se a faixa nao existe):
//   deslocamento = edicao vazia inicial - media_time da 1a edicao + menor tempo de composicao
// Num arquivo comum da 0 (a edicao so compensa o atraso dos quadros B). Num arquivo CORTADO
// sem recodificar da negativo: a midia comeca no quadro-chave anterior ao corte e a edicao
// esconde esse comeco. Quem ignora a lista mostra o trecho escondido e, como o corte de cada
// faixa cai num ponto diferente, sai com audio e video desencontrados pela diferenca entre os
// dois deslocamentos. So a PRIMEIRA edicao nao vazia conta (montagens com varias nao sao lidas).
// 1 = leu o moov; 0 = nao leu (deslocamentos ficam 0).
int mp4_edit_offsets(const char* mp4_path, int64_t* video_us, int64_t* audio_us);

// Metadados de audio (sem ffprobe): codec ("aac"), sample rate e canais. <0 se nao houver audio.
int mp4_audio_info(const char* mp4_path, char codec[8], int* rate, int* channels);

// Metadados descritivos do MP4 (moov/udta/meta/ilst, e o udta antigo do QuickTime): titulo,
// autor, album, data, descricao, genero. Texto UTF-8; campo ausente fica vazio. Arquivos de
// encoder costumam trazer so o 'too' (o encoder), que nao identifica o video e e ignorado.
typedef struct
{
    char Titulo[256], Autor[256], Album[256], Data[64], Descricao[512], Genero[96];
} Mp4Meta;

// 1 = leu o moov (os campos podem vir vazios); 0 = sem moov legivel.
int mp4_metadata(const char* mp4_path, Mp4Meta* m);

// 1 se ha ao menos um campo descritivo (titulo, autor, album ou descricao).
int mp4_metadata_descritiva(const Mp4Meta* m);
