#pragma once
// Manifest declarativo de un juego (versión 1). Ver docs/01-arquitectura.md.
#include <string>
#include <vector>

namespace wl {

enum class GameStatus { Playable, InDevelopment, Beta, Planned };

struct Manifest {
    int manifest_version = 0;
    std::string game_id;
    std::string display_name;
    GameStatus status = GameStatus::Planned;
    std::string adapter;            // nombre de un adaptador compilado en el launcher
    int adapter_version = 0;
    std::string install_folder;     // un solo nombre de carpeta dentro de %LOCALAPPDATA%
    std::string executable;         // relativa a install_folder
    std::string working_dir = ".";  // relativa a install_folder
    std::vector<std::string> disc_ids;
    int max_players = 1;
    std::vector<std::string> capabilities;  // local, lan_host, lan_join
    std::string notes;
    std::string lan_server_script;  // opcional (lan_wfc): servidor del anfitrión, relativo a la carpeta del launcher
    std::string installer;          // opcional: instalador incluido, relativo a la carpeta del launcher (.exe o .ps1)

    bool has(const std::string& capability) const;
};

struct ManifestResult {
    bool ok = false;
    Manifest manifest;
    std::vector<std::string> errors;  // en español, aptos para mostrar
};

// Analiza y valida. Nunca lanza excepciones.
ManifestResult parse_manifest(const std::string& text);
ManifestResult load_manifest(const std::string& path);

// true si `relative` es una ruta relativa segura (sin "..", sin raíz, sin unidad, sin UNC, sin variables).
bool safe_relative_path(const std::string& relative);

const char* status_name(GameStatus status);

}
