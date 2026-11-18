#pragma once
#include <string>
#include <vector>

#include "wl/manifest.h"

namespace wl {

struct CatalogEntry {
    std::string file;
    Manifest manifest;
};

struct Catalog {
    std::vector<CatalogEntry> games;
    std::vector<std::string> errors;  // manifests rechazados: el juego no aparece
};

// Lee todos los *.toml de la carpeta, en orden alfabético. game_id repetido = error.
Catalog load_catalog(const std::string& folder);

}
