#pragma once
// Manifiesto de sesión (lo publica el anfitrión) y comparación con el estado local (docs/mods/05-protocolo_salas.md).
// El manifiesto es SOLO para comparar: no lleva URLs ni rutas y el cliente nunca obedece órdenes suyas. Para
// conseguir un mod que falta, el cliente busca en sus proveedores permitidos por (source, source_id) y vuelve a
// comprobar los hashes después de instalar.
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "wl/mods/json.h"
#include "wl/mods/reference.h"
#include "wl/mods/store.h"

namespace wl::mods {

constexpr const char* kSessionProtocol = "launcher-lobby-mods";
constexpr int kSessionProtocolVersion = 1;
constexpr size_t kMaxSessionManifest = 64 * 1024;
constexpr size_t kMaxSessionMods = 200;

using SessionMod = ModReference;

struct SessionManifest {
    int protocol_version = 0;
    std::string lobby_id;
    uint64_t revision = 0;
    std::string game_id, game_revision, base_content_id, loader;
    std::string fingerprint;   // «vanilla» o sha256
    std::vector<SessionMod> mods;
};

// Manifiesto del perfil `profile` de este PC (anfitrión).
Json build_session_manifest(const ModStore& store, const Profile& profile, const std::string& lobby_id,
                            uint64_t revision, const std::string& game_revision = {});
// Validación estricta de un manifiesto recibido (D07): tamaño, tipos, formatos de id/hash, número de mods.
bool parse_session_manifest(const std::string& text, SessionManifest& out, std::string& error);

enum class ModDiff {
    ExactMatch, Missing, WrongVersion, WrongHash, Disabled, ExtraActive, LoadOrder, ConfigMismatch,
    Unsupported, NoAuthorizedSource, IncompatibleGame
};
const char* diff_name(ModDiff d);

struct DiffItem {
    ModDiff state = ModDiff::ExactMatch;
    std::string id, name, wanted_version, local_version, source, source_id;
    std::string local_key;        // paquete local que coincide exactamente (si lo hay)
    bool downloadable = false;    // se puede pedir a un proveedor permitido (nunca a una URL del anfitrión)
    std::string detail;
};

struct SessionDiff {
    bool game_ok = false;         // mismo juego, disco base y loader
    std::string game_problem;
    std::vector<DiffItem> items;
    bool compatible = false;      // fingerprint efectivo idéntico
    int missing = 0, wrong = 0, extra = 0, disabled = 0;
    // Perfil de sesión que reproduce el del anfitrión con lo ya instalado (vacío si falta algo).
    bool session_ready = false;
    Profile session_profile;
};

SessionDiff diff_session(const ModStore& store, const Profile& local, const SessionManifest& host,
                         const std::string& game_revision = {});

}  // namespace wl::mods
