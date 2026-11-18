// Adaptador de Mario Kart Wii (WiiCompiled, rama lan12) en beta. Sigue
// mkwii-recomp/lan/docs/06-contrato-launcher.md:
// - El juego está en una carpeta portable (portable.txt) con UserData\Config.toml.
// - El online va por un servidor WFC en la LAN que arranca el anfitrión. Cada cliente pone su IP en
//   [network] lan_server. Sin lan_server, el juego funciona offline como el original.
// - UserData guarda la identidad de la consola: este launcher nunca la copia ni la toca, solo edita [network].
#include <algorithm>
#include <fstream>
#include <vector>
#include <sstream>

#include "wl/adapter.h"

namespace wl {

bool valid_ipv4(const std::string& text) {
    int parts = 0;
    size_t i = 0;
    while (i <= text.size()) {
        size_t dot = text.find('.', i);
        std::string part = text.substr(i, dot == std::string::npos ? std::string::npos : dot - i);
        if (part.empty() || part.size() > 3) return false;
        for (char c : part) if (c < '0' || c > '9') return false;
        if (std::stoi(part) > 255) return false;
        ++parts;
        if (dot == std::string::npos) break;
        i = dot + 1;
    }
    return parts == 4;
}

namespace {

std::string read_all(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
    size_t a = s.find_first_not_of(" \t");
    return a == std::string::npos ? std::string() : s.substr(a);
}

// Valor de `key` dentro de [section] en un TOML sencillo, sin comillas.
std::string toml_value(const std::string& text, const std::string& section, const std::string& key) {
    std::stringstream in(text);
    std::string line, current;
    while (std::getline(in, line)) {
        std::string t = trim(line);
        if (t.size() > 2 && t.front() == '[') { current = t.substr(1, t.find(']') - 1); continue; }
        if (current != section) continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos || trim(t.substr(0, eq)) != key) continue;
        std::string v = trim(t.substr(eq + 1));
        size_t hash = v.find('#');
        if (!v.empty() && v[0] != '\'' && v[0] != '"' && hash != std::string::npos) v = trim(v.substr(0, hash));
        if (v.size() >= 2 && (v[0] == '\'' || v[0] == '"')) v = v.substr(1, v.find(v[0], 1) - 1);
        return v;
    }
    return {};
}

// Pone (o quita, con value vacío) claves de [network] sin tocar el resto del archivo.
std::string set_network(const std::string& text, const std::vector<std::pair<std::string, std::string>>& values) {
    std::stringstream in(text);
    std::string line, out, current;
    bool seen = false;
    std::vector<bool> written(values.size(), false);
    auto flush_missing = [&] {
        for (size_t i = 0; i < values.size(); ++i)
            if (!written[i] && !values[i].second.empty()) { out += values[i].first + " = " + values[i].second + "\n"; written[i] = true; }
    };
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string t = trim(line);
        if (t.size() > 2 && t.front() == '[') {
            if (current == "network") flush_missing();
            current = t.substr(1, t.find(']') - 1);
            if (current == "network") seen = true;
            out += line + "\n";
            continue;
        }
        if (current == "network") {
            size_t eq = t.find('=');
            std::string k = eq == std::string::npos ? std::string() : trim(t.substr(0, eq));
            bool replaced = false;
            for (size_t i = 0; i < values.size(); ++i) {
                if (k == values[i].first) {
                    if (!values[i].second.empty() && !written[i]) out += k + " = " + values[i].second + "\n";
                    written[i] = true;
                    replaced = true;
                }
            }
            if (replaced) continue;
        }
        out += line + "\n";
    }
    if (current == "network") flush_missing();
    if (!seen) {
        bool any = false;
        for (const auto& v : values) any = any || !v.second.empty();
        if (any) {
            if (!out.empty() && out.back() != '\n') out += "\n";
            out += "\n[network]\n";
            flush_missing();
        }
    }
    return out;
}

class MkwiiAdapter final : public GameAdapter {
public:
    explicit MkwiiAdapter(Manifest m) : m_(std::move(m)) {}

    // UserData está junto al exe (modo portable): <instalación>/<working_dir>/UserData/Config.toml.
    fs::path game_dir(const fs::path& root) const { return resolve_inside(root, m_.working_dir).value_or(root); }
    fs::path config(const fs::path& root) const { return game_dir(root) / "UserData" / "Config.toml"; }

    InstallStatus probe(const fs::path& root) const override {
        InstallStatus s;
        s.root = root;
        std::error_code e;
        s.folder = fs::is_directory(root, e);
        if (!s.folder) {
            s.problem = "No está instalado. Pulsa Instalar y elige tu propia copia de Mario Kart Wii.";
            return s;
        }
        auto exe = resolve_inside(root, m_.executable);
        s.built = exe && fs::is_regular_file(*exe, e);
        std::string cfg = read_all(config(root));
        std::string dvd = toml_value(cfg, "paths", "dvd_root");
        if (!dvd.empty()) s.disc_id = read_disc_id(fs::u8path(dvd) / "sys" / "boot.bin");
        s.disc_ok = !s.disc_id.empty() && std::find(m_.disc_ids.begin(), m_.disc_ids.end(), s.disc_id) != m_.disc_ids.end();
        s.revision = read_first_line(game_dir(root) / "lan-version.txt");
        if (!s.built) s.problem = "Falta WiiCompiled.exe: termina la instalación.";
        else if (dvd.empty()) s.problem = "Falta dvd_root en UserData\\Config.toml (la carpeta extraída de tu disco).";
        else if (s.disc_id.empty()) s.problem = "No se encuentra el disco extraído en " + dvd + ".";
        else if (!s.disc_ok) s.problem = "Disco no admitido (" + s.disc_id + ").";
        return s;
    }

    std::string compat_token(const InstallStatus& s) const override { return s.revision; }

    LaunchSpec offline(const fs::path& root) const override {
        LaunchSpec spec;
        spec.executable = resolve_inside(root, m_.executable).value_or(fs::path());
        spec.working_dir = resolve_inside(root, m_.working_dir).value_or(root);
        return spec;
    }

    // El online es dentro del juego (Nintendo WFC contra el servidor de la LAN): no hay sala del launcher.
    std::optional<LaunchSpec> online(const fs::path&, const SessionStart&) const override { return std::nullopt; }

    std::string lan_server(const fs::path& root) const override { return toml_value(read_all(config(root)), "network", "lan_server"); }

    bool set_lan_server(const fs::path& root, const std::string& ip, std::string& error) const override {
        if (!ip.empty() && !valid_ipv4(ip)) { error = "Escribe una IP como 192.168.1.20."; return false; }
        std::error_code e;
        if (!fs::is_directory(game_dir(root) / "UserData", e)) { error = "Falta la carpeta UserData: termina la instalación primero."; return false; }
        std::string text = read_all(config(root));
        std::string updated = ip.empty() ? set_network(text, {{"lan_server", ""}})
                                         : set_network(text, {{"enabled", "true"}, {"lan_server", "'" + ip + "'"}, {"lan_nas_port", "8080"}});
        fs::path tmp = config(root);
        tmp += ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            f << updated;
            if (!f) { error = "No se pudo escribir Config.toml."; return false; }
        }
        fs::rename(tmp, config(root), e);
        if (e) {
            // Windows no deja reemplazar un archivo abierto por otro programa: se escribe encima.
            fs::remove(tmp, e);
            std::ofstream f(config(root), std::ios::binary | std::ios::trunc);
            f << updated;
            if (!f) { error = "No se pudo guardar Config.toml (¿lo tienes abierto?)."; return false; }
        }
        return true;
    }

private:
    Manifest m_;
};

}

std::unique_ptr<GameAdapter> make_mkwii_adapter(const Manifest& m) { return std::make_unique<MkwiiAdapter>(m); }

}
