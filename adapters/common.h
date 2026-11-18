#pragma once
// Base para los ports del motor wii_recomp: misma estructura de instalación que crean sus launchers.
#include <algorithm>

#include "wl/adapter.h"

namespace wl {

class RecompAdapter : public GameAdapter {
public:
    RecompAdapter(Manifest manifest, std::string game_folder) : m_(std::move(manifest)), game_(std::move(game_folder)) {}

    InstallStatus probe(const fs::path& root) const override {
        InstallStatus s;
        std::error_code e;
        s.root = root;
        s.folder = fs::is_directory(root, e);
        if (!s.folder) {
            s.problem = "No está instalado. Pulsa Instalar y elige tu copia del juego.";
            return s;
        }
        auto exe = resolve_inside(root, m_.executable);
        s.built = exe && fs::is_regular_file(*exe, e);
        s.revision = read_first_line(root / ".launcher-revision");
        s.disc_id = read_disc_id(root / "games" / game_ / "extracted" / "sys" / "boot.bin");
        s.disc_ok = !s.disc_id.empty() && std::find(m_.disc_ids.begin(), m_.disc_ids.end(), s.disc_id) != m_.disc_ids.end();
        if (!s.built) s.problem = "El juego no está compilado. Pulsa Instalar o actualizar.";
        else if (s.disc_id.empty()) s.problem = "No se encuentra el disco extraído.";
        else if (!s.disc_ok) s.problem = "Disco no admitido (" + s.disc_id + ").";
        return s;
    }

    LaunchSpec offline(const fs::path& root) const override {
        LaunchSpec spec;
        spec.executable = resolve_inside(root, m_.executable).value_or(fs::path());
        spec.working_dir = resolve_inside(root, m_.working_dir).value_or(root);
        // Igual que App::play() de los launchers actuales.
        spec.env["WP_LOG_FILE"] = (root / "logs" / "game.log").u8string();
        return spec;
    }

protected:
    Manifest m_;
    std::string game_;
};

}
