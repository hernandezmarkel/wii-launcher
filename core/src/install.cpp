#include <cstdlib>
#include <fstream>
#ifdef _WIN32
#include <windows.h>
#endif

#include "wl/adapter.h"

namespace wl {

fs::path local_app_data() {
    if (const char* test = std::getenv("WL_LOCALAPPDATA")) return fs::u8path(test);
    if (const char* local = std::getenv("LOCALAPPDATA")) return fs::u8path(local);
    if (const char* home = std::getenv("HOME")) return fs::u8path(home) / ".local" / "share";
    return fs::current_path();
}

fs::path roaming_app_data() {
    if (const char* test = std::getenv("WL_APPDATA")) return fs::u8path(test);
    if (const char* roaming = std::getenv("APPDATA")) return fs::u8path(roaming);
    return local_app_data();
}

namespace {

// root= que guardó el launcher individual del juego cuando el usuario cambió la carpeta de instalación.
// Lo guarda en %APPDATA%\<app>\launcher\launcher.ini y, en versiones antiguas, en el registro.
std::string launcher_root_pref(const std::string& app) {
    std::ifstream file(roaming_app_data() / fs::u8path(app) / "launcher" / "launcher.ini", std::ios::binary);
    std::string line;
    while (std::getline(file, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.rfind("root=", 0) == 0) return line.substr(5);
    }
#ifdef _WIN32
    if (!std::getenv("WL_APPDATA")) {   // en pruebas no se mira el registro real
        wchar_t value[1024];
        DWORD size = sizeof value;
        std::wstring key = L"Software\\" + std::wstring(app.begin(), app.end()) + L"\\launcher";
        if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), L"root", RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS) {
            auto u8 = fs::path(value).u8string();
            return std::string(u8.begin(), u8.end());
        }
    }
#endif
    return {};
}

}  // namespace

fs::path install_root(const Manifest& manifest) {
    // Si el usuario movió el juego con su launcher de siempre, se usa esa carpeta (aunque esté vacía: así la
    // ficha dice «no instalado» en vez de mirar, o crear cosas, en la ruta por defecto). Solo rutas absolutas.
    if (!manifest.launcher_prefs.empty()) {
        std::string root = launcher_root_pref(manifest.launcher_prefs);
        if (!root.empty()) {
            fs::path p = fs::u8path(root).lexically_normal();
            if (p.is_absolute()) return p;
        }
    }
    return local_app_data() / fs::u8path(manifest.install_folder);
}

std::string read_first_line(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::string line;
    std::getline(file, line);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    return line;
}

std::string read_disc_id(const fs::path& boot_bin) {
    std::ifstream file(boot_bin, std::ios::binary);
    char id[6] = {};
    file.read(id, sizeof id);
    if (!file) return "";
    for (char c : id) {
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return "";
    }
    return std::string(id, sizeof id);
}

std::optional<fs::path> resolve_inside(const fs::path& root, const std::string& relative) {
    if (!safe_relative_path(relative)) return std::nullopt;
    fs::path base = root.lexically_normal();
    fs::path full = (base / fs::u8path(relative)).lexically_normal();
    if (full.has_parent_path() && full.filename().empty()) full = full.parent_path();  // "raiz/." -> "raiz"
    auto b = base.begin();
    auto f = full.begin();
    for (; b != base.end(); ++b, ++f) {
        if (b->empty()) continue;
        if (f == full.end() || *f != *b) return std::nullopt;
    }
    return full;
}

}
