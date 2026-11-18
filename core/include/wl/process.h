#pragma once
#include <string>

#include "wl/adapter.h"

namespace wl {

struct Spawned {
    bool ok = false;
    long long pid = 0;
    std::string error;
};

// Arranca el juego como proceso independiente con el entorno heredado más spec.env. No espera.
Spawned spawn(const LaunchSpec& spec);
// Espera hasta `timeout_ms` (o -1 = siempre). Devuelve el código de salida o -1 si sigue vivo.
int wait_exit(long long pid, int timeout_ms);

}
