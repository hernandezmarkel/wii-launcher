#pragma once
// Puente entre el gestor de mods y el lanzamiento de un juego (offline u online).
// Los adaptadores no cambian: el launcher pide la LaunchSpec de siempre y, si el perfil activo tiene mods,
// le añade como argv[1] la carpeta del disco virtual. Con perfil vanilla la LaunchSpec queda idéntica.
#include <filesystem>
#include <string>

#include "wl/adapter.h"
#include "wl/mods/store.h"

namespace wl::mods {

struct LaunchMods {
    std::string profile;        // perfil usado
    uint64_t epoch = 0;
    std::string fingerprint = kVanilla;
    size_t mods = 0;            // mods activos
    fs::path vdisc;             // vacío = disco original
    std::vector<std::string> conflicts;
};

// Carpeta del disco extraído de un port wii_recomp (convención de RecompAdapter).
fs::path extracted_dir(const std::string& game_id, const fs::path& install_root);

// Prepara el lanzamiento con el perfil `profile` (vacío = el activo). false si el perfil tiene problemas:
// nunca se lanza «a medias» con un conjunto distinto del que se ve en la UI.
// `allow_unproven`: solo para pruebas (WL_MODS_EXPERIMENTAL=1); la UI no ofrece juegos sin prueba A/B.
bool prepare_launch(const std::string& game_id, const fs::path& install_root, LaunchSpec& spec, LaunchMods& info,
                    std::string& error, const std::string& profile = {}, bool allow_unproven = false);

// Token de sala con mods: el de siempre si el perfil es vanilla; si no, se le añade el fingerprint,
// de modo que el anfitrión (con el protocolo actual) rechaza a quien no tenga exactamente lo mismo.
std::string mod_compat_token(const std::string& base_token, const std::string& fingerprint);

}  // namespace wl::mods
