#pragma once

#include "appserver.h"

// Raiz da saida (upload, fragmentos, sessoes): <pasta web>/hls. Chamar antes de subir o servidor.
void hls_set_raiz(const char* dir);
Element* hls_prepare_video(Message* message);
Element* hls_stream_video(Message* message);
Element* dash_stream_video(Message* message);
Element* convert_file(Message* message);

