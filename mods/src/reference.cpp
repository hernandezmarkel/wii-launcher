#include "wl/mods/reference.h"

#include <algorithm>
#include <limits>

#include "wl/mods/store.h"
#include "wl/update.h"

namespace wl::mods {
namespace {

bool hex(const std::string& text, size_t length) {
    return text.size() == length && std::all_of(text.begin(), text.end(),
        [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

bool provider_id(const std::string& text) {
    return !text.empty() && text.size() <= 12 && text.front() != '0' &&
        std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

bool short_text(const std::string& text, size_t max) {
    return text.size() <= max && std::none_of(text.begin(), text.end(), [](unsigned char c) { return c < 0x20; });
}

Json options_json(const std::map<std::string, int>& choices) {
    Json json = Json::object();
    for (const auto& [id, value] : choices) json.set(id, value);
    return json;
}

}  // namespace

bool ModReference::matches(const InstalledMod& installed) const {
    return id == installed.id() && version == installed.version() && content_sha256 == installed.content_sha();
}

bool ModReference::downloadable() const {
    return source == "gamebanana" && provider_id(source_id) && id == source + "." + source_id &&
        ((source_file_id.empty() && source_md5.empty()) || (provider_id(source_file_id) && hex(source_md5, 32)));
}

ModReference describe_mod(const InstalledMod& installed, int order, bool enabled,
                          const std::map<std::string, int>& choices) {
    ModReference mod;
    mod.id = installed.id();
    mod.name = installed.name();
    mod.version = installed.version();
    mod.source = installed.json["provider"].str();
    mod.source_id = installed.json["source_id"].str();
    mod.source_file_id = installed.json["source_file_id"].str();
    mod.source_md5 = installed.json["source_md5"].str();
    mod.package_sha256 = installed.json["package_sha256"].str();
    mod.content_sha256 = installed.content_sha();
    mod.config_sha256 = upd::sha256_hex(options_json(choices).dump());
    mod.load_order = order;
    mod.enabled = enabled;
    mod.choices = choices;
    std::string error;
    parse_dependencies(installed.json["dependencies"], mod.dependencies, error);
    return mod;
}

Json serialize_mod_reference(const ModReference& mod) {
    Json j = Json::object();
    j.set("id", mod.id);
    j.set("name", mod.name);
    j.set("version", mod.version);
    j.set("source", mod.source);
    j.set("source_id", mod.source_id);
    j.set("source_file_id", mod.source_file_id);
    j.set("source_md5", mod.source_md5);
    j.set("package_sha256", mod.package_sha256);
    j.set("effective_content_sha256", mod.content_sha256);
    j.set("config_sha256", mod.config_sha256);
    j.set("load_order", mod.load_order);
    j.set("enabled", mod.enabled);
    j.set("choices", options_json(mod.choices));
    if (!mod.dependencies.empty()) j.set("dependencies", serialize_dependencies(mod.dependencies));
    return j;
}

bool parse_mod_reference(const Json& j, ModReference& mod, std::string& error) {
    ModReference m;
    m.id = j["id"].str();
    m.name = j["name"].str();
    m.version = j["version"].str();
    m.source = j["source"].str();
    if (m.source != "local" && m.source != "gamebanana") m.source = "desconocido";
    m.source_id = j["source_id"].str();
    m.source_file_id = j["source_file_id"].str();
    m.source_md5 = j["source_md5"].str();
    m.package_sha256 = j["package_sha256"].str();
    m.content_sha256 = j["effective_content_sha256"].str();
    m.config_sha256 = j["config_sha256"].str();
    const auto order = j["load_order"].num(-2);
    if (!valid_mod_id(m.id) || !short_text(m.name, 200) || m.version.empty() || !short_text(m.version, 40) ||
        !hex(m.content_sha256, 64) || !hex(m.config_sha256, 64) ||
        (!m.package_sha256.empty() && !hex(m.package_sha256, 64)) ||
        order < -1 || order > std::numeric_limits<int>::max()) {
        error = "referencia de mod no válida";
        return false;
    }
    m.load_order = static_cast<int>(order);
    if ((j.has("enabled") && !j["enabled"].is_bool()) || (j.has("choices") && !j["choices"].is_object())) {
        error = "estado u opciones de mod no válidos";
        return false;
    }
    m.enabled = j["enabled"].boolean(true);    // v1 de salas anterior a este campo: todos activos
    for (const auto& [id, v] : j["choices"].items()) {
        if (!short_text(id, 120) || !v.is_int() || v.num() < 0 || v.num() > 64) {
            error = "opciones no válidas en la referencia";
            return false;
        }
        m.choices[id] = static_cast<int>(v.num());
    }
    if (upd::sha256_hex(options_json(m.choices).dump()) != m.config_sha256) {
        error = "las opciones no coinciden con la huella del mod";
        return false;
    }
    if (!parse_dependencies(j["dependencies"], m.dependencies, error)) return false;
    mod = std::move(m);
    return true;
}

}  // namespace wl::mods
