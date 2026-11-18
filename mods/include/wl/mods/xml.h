#pragma once
// Analizador XML mínimo para los XML de Riivolution (datos no confiables): sin DTD, sin entidades
// externas, solo las cinco entidades predefinidas y referencias numéricas ASCII. Con límites.
#include <map>
#include <string>
#include <vector>

namespace wl::mods {

struct XmlNode {
    std::string name;
    std::map<std::string, std::string> attrs;
    std::vector<XmlNode> children;
    std::string attr(const std::string& key, const std::string& def = {}) const {
        auto it = attrs.find(key);
        return it == attrs.end() ? def : it->second;
    }
};

bool parse_xml(const std::string& text, XmlNode& root, std::string& error, size_t max_bytes = 512 * 1024);

}  // namespace wl::mods
