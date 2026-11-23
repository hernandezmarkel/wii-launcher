#include "wl/mods/profile_catalog.h"

#include <algorithm>
#include <set>
#include <tuple>

namespace wl::mods {
namespace {

constexpr const char* kCatalogSchema = "wii-launcher-mod-catalog";
constexpr size_t kMaxReferences = 1000;

bool portable_name(const std::string& name) {
    return !name.empty() && name.size() <= 40 && name != "session" &&
        name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") == std::string::npos;
}

}  // namespace

bool export_profile_catalog(const ModStore& store, const std::string& profile_name,
                            Json& out, std::string& error) {
    if (!portable_name(profile_name)) { error = "perfil no sincronizable"; return false; }
    Profile profile;
    if (!store.load_profile(profile_name, profile, error)) return false;
    Json refs = Json::array();
    std::set<std::tuple<std::string, std::string, std::string>> included;
    int order = 0;
    for (const auto& entry : profile.mods) {
        InstalledMod mod;
        if (!store.find_installed(entry.package, mod) || entry.id != mod.id() ||
            mod.json["base_content_id"].str() != store.base().base_content_id) {
            error = "falta el paquete de «" + entry.id + "»";
            return false;
        }
        refs.push(serialize_mod_reference(describe_mod(mod, order++, entry.enabled, entry.choices)));
        included.emplace(mod.id(), mod.version(), mod.content_sha());
    }
    for (const auto& mod : store.installed()) {
        std::string integrity_error;
        if (mod.json["base_content_id"].str() == store.base().base_content_id && store.verify(mod, integrity_error) &&
            included.emplace(mod.id(), mod.version(), mod.content_sha()).second)
            refs.push(serialize_mod_reference(describe_mod(mod, -1, false, {})));
    }
    if (refs.size() > kMaxReferences) { error = "el catálogo excede el límite de referencias"; return false; }
    out = Json::object();
    out.set("schema", kCatalogSchema);
    out.set("version", 1);
    out.set("game_id", store.support().game_id);
    out.set("base_content_id", store.base().base_content_id);
    out.set("loader", store.support().loader);
    out.set("profile", profile_name);
    out.set("mods", std::move(refs));
    return true;
}

bool parse_profile_catalog(const std::string& text, ProfileCatalog& out, std::string& error) {
    Json data;
    JsonLimits limits;
    limits.max_depth = 8;
    limits.max_string = 512;
    if (!parse_json(text, data, error, limits)) return false;
    ProfileCatalog catalog;
    catalog.game_id = data["game_id"].str();
    catalog.base_content_id = data["base_content_id"].str();
    catalog.loader = data["loader"].str();
    catalog.profile_name = data["profile"].str();
    if (data["schema"].str() != kCatalogSchema || data["version"].num() != 1 ||
        !game_support(catalog.game_id) || catalog.base_content_id.empty() || catalog.base_content_id.size() > 40 ||
        catalog.loader.empty() || catalog.loader.size() > 32 || !portable_name(catalog.profile_name) ||
        !data["mods"].is_array() || data["mods"].size() > kMaxReferences) {
        error = "catálogo de cuenta no válido";
        return false;
    }
    std::set<std::tuple<std::string, std::string, std::string>> identities;
    std::set<std::string> profile_ids;
    std::set<int> positions;
    for (const auto& row : data["mods"].elements()) {
        ModReference mod;
        if (!parse_mod_reference(row, mod, error)) return false;
        if (!identities.emplace(mod.id, mod.version, mod.content_sha256).second ||
            (mod.load_order < 0 && (mod.enabled || !mod.choices.empty())) ||
            (mod.load_order >= 0 && (!positions.insert(mod.load_order).second || !profile_ids.insert(mod.id).second))) {
            error = "referencia duplicada o fuera de su perfil";
            return false;
        }
        catalog.mods.push_back(std::move(mod));
    }
    if (!positions.empty() && static_cast<size_t>(*positions.rbegin()) + 1 != positions.size()) {
        error = "orden de carga incompleto";
        return false;
    }
    std::stable_sort(catalog.mods.begin(), catalog.mods.end(),
        [](const ModReference& a, const ModReference& b) { return a.load_order < b.load_order; });
    out = std::move(catalog);
    return true;
}

bool plan_catalog_restore(const ModStore& store, const ProfileCatalog& catalog,
                          CatalogRestore& out, std::string& error) {
    out = CatalogRestore{};
    if (catalog.game_id != store.support().game_id || catalog.base_content_id != store.base().base_content_id ||
        catalog.loader != store.support().loader) {
        error = "el catálogo requiere otro disco o cargador de mods";
        return false;
    }
    const auto installed = store.installed();
    Profile profile;
    profile.name = catalog.profile_name;
    for (const auto& reference : catalog.mods) {
        std::string verify_error;
        auto local = std::find_if(installed.begin(), installed.end(), [&](const InstalledMod& mod) {
            return reference.matches(mod) && mod.json["base_content_id"].str() == catalog.base_content_id &&
                store.verify(mod, verify_error);
        });
        if (local == installed.end()) {
            (reference.downloadable() ? out.downloads : out.manual).push_back(reference);
        } else if (reference.load_order >= 0) {
            profile.mods.push_back({reference.id, local->key, reference.enabled, reference.choices});
        }
    }
    out.complete = out.downloads.empty() && out.manual.empty();
    if (out.complete) out.profile = std::move(profile);
    return true;
}

}  // namespace wl::mods
