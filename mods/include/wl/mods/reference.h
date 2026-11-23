#pragma once

#include <map>
#include <string>

#include "wl/mods/json.h"
#include "wl/mods/package.h"

namespace wl::mods {

struct InstalledMod;

// Identidad portátil: un paquete instalado, una entrada de sala o una referencia guardada en una cuenta.
struct ModReference {
    std::string id, name, version, source, source_id;
    std::string source_file_id, source_md5, package_sha256;
    std::string content_sha256, config_sha256;
    int load_order = -1;  // -1: instalado fuera del perfil
    bool enabled = false;
    std::map<std::string, int> choices;
    std::vector<ModDependency> dependencies;

    bool matches(const InstalledMod& installed) const;
    bool downloadable() const;
};

ModReference describe_mod(const InstalledMod& installed, int order, bool enabled,
                          const std::map<std::string, int>& choices);
Json serialize_mod_reference(const ModReference& mod);
bool parse_mod_reference(const Json& json, ModReference& mod, std::string& error);

}  // namespace wl::mods
