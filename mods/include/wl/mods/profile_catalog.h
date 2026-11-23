#pragma once

#include "wl/mods/reference.h"
#include "wl/mods/store.h"

namespace wl::mods {

struct ProfileCatalog {
    std::string game_id, base_content_id, loader, profile_name;
    std::vector<ModReference> mods;
};

// Contrato de cuenta. Solo metadatos. No incluye rutas, claves del almacén ni la sesión temporal.
bool export_profile_catalog(const ModStore& store, const std::string& profile_name,
                            Json& out, std::string& error);
bool parse_profile_catalog(const std::string& text, ProfileCatalog& out, std::string& error);

struct CatalogRestore {
    Profile profile;
    std::vector<ModReference> downloads, manual;
    bool complete = false;   // catálogo completo y perfil reconstruible con las copias verificadas
};

// Plan de solo lectura. El llamador muestra las referencias pendientes y pide consentimiento antes de
// resolverlas con un proveedor, usando los mismos hashes que la instalación asistida de sala.
bool plan_catalog_restore(const ModStore& store, const ProfileCatalog& catalog,
                          CatalogRestore& out, std::string& error);

}  // namespace wl::mods
