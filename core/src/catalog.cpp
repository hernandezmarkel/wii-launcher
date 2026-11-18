#include "wl/catalog.h"

#include <algorithm>
#include <filesystem>
#include <set>

namespace wl {

Catalog load_catalog(const std::string& folder) {
    Catalog catalog;
    std::error_code error;
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
        if (entry.is_regular_file(error) && entry.path().extension() == ".toml") files.push_back(entry.path());
    }
    if (error) catalog.errors.push_back("no se puede leer la carpeta de manifests " + folder);
    std::sort(files.begin(), files.end());
    std::set<std::string> ids;
    for (const auto& path : files) {
        ManifestResult r = load_manifest(path.string());
        std::string name = path.filename().string();
        if (!r.ok) {
            for (const auto& e : r.errors) catalog.errors.push_back(name + ": " + e);
            continue;
        }
        if (!ids.insert(r.manifest.game_id).second) {
            catalog.errors.push_back(name + ": game_id repetido '" + r.manifest.game_id + "'");
            continue;
        }
        catalog.games.push_back({name, r.manifest});
    }
    return catalog;
}

}
