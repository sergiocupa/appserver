#pragma once

#include "appserver.h"

// Rota UNICA de configuracao de entrada:
//   GET /api/media/devices            -> todas as cameras + suas combinacoes
//   GET /api/media/devices/<sessao>   -> idem, mais o arquivo da sessao como um
//                                        "device" de kind "file" (campos read-only)
//
// Uma consulta so devolve a arvore inteira (device -> stream -> resolucao -> fps),
// porque as opcoes NAO sao independentes: uma camera pode dar 1080p a 30fps em MJPG
// e so 5fps em YUY2. Rotas separadas por parametro entregariam combinacoes invalidas.
Element* device_route(Message* message);
