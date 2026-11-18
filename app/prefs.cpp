#include "prefs.h"

#include <fstream>

#include "wl/adapter.h"

namespace wl {

namespace {
fs::path prefs_path() { return local_app_data() / "WiiLauncher" / "prefs.ini"; }
}

void Prefs::load() {
    std::ifstream file(prefs_path(), std::ios::binary);
    for (std::string line; std::getline(file, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t eq = line.find('=');
        if (eq != std::string::npos && eq > 0) values_[line.substr(0, eq)] = line.substr(eq + 1);
    }
}

void Prefs::save() const {
    std::error_code e;
    fs::create_directories(prefs_path().parent_path(), e);
    fs::path tmp = prefs_path();
    tmp += ".tmp";
    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        for (const auto& [k, v] : values_) file << k << '=' << v << '\n';
        if (!file) return;
    }
    fs::rename(tmp, prefs_path(), e);  // escritura atómica: nunca un prefs.ini a medias
}

std::string Prefs::get(const std::string& key, const std::string& fallback) const {
    auto it = values_.find(key);
    return it == values_.end() ? fallback : it->second;
}

void Prefs::set(const std::string& key, const std::string& value) {
    std::string clean;
    for (char c : value) clean += (c == '\n' || c == '\r') ? ' ' : c;
    values_[key] = clean;
    save();
}

}
