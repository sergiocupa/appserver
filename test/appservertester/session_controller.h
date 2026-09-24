#pragma once

#include "appserver.h"

// Rotas de sessao de fragmentacao. O appserver nao encaminha verbo nem query-string,
// entao a acao vai no caminho:
//   POST /api/session/create        (corpo opcional: {"name":"..."})
//   GET  /api/session/list
//   GET  /api/session/get/<id>
//   GET  /api/session/cancel/<id>
//   GET  /api/session/delete/<id>
Element* session_route(Message* message);
