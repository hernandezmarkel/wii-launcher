#include <cstdlib>
#include <fstream>

#include "wl/adapter.h"

namespace wl {

fs::path local_app_data() {
    if (const char* test = std::getenv("WL_LOCALAPPDATA")) return fs::u8path(test);
    if (const char* local = std::getenv("LOCALAPPDATA")) return fs::u8path(local);
    if (const char* home = std::getenv("HOME")) return fs::u8path(home) / ".local" / "share";
    return fs::current_path();
}

fs::path install_root(const Manifest& manifest) {
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
