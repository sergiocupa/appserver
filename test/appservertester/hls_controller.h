#pragma once

#include "appserver.h"

// Raiz da saida (upload, fragmentos, sessoes): <pasta web>/hls. Chamar antes de subir o servidor.
void hls_set_raiz(const char* dir);
// Arquivo cortado sem recodificar, em pedido de copia no HLS: "recode" (padrao; recodifica
// tudo), "smart" (so o primeiro grupo de quadros) ou "copy" (copia; o manifesto manda comecar
// depois do trecho escondido). Vem de "cutFiles" no appservertester.json. 1 = modo conhecido.
int  hls_set_cortado(const char* modo);
Element* hls_prepare_video(Message* message);
Element* hls_stream_video(Message* message);
Element* dash_stream_video(Message* message);
Element* convert_file(Message* message);

