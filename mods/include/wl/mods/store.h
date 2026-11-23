#pragma once
// Almacén de mods de un juego: paquetes inmutables, staging, perfiles, disco virtual y fingerprint.
//
//   %LOCALAPPDATA%/WiiLauncher/mods/<game_id>/
//     packages/<clave>/mod.json + content/...   instalados y validados (nunca se modifican)
//     staging/<operación>/                       temporal; se borra al abrir el almacén (recuperación)
//     profiles/<nombre>.json                     default, custom-*, session
//     active.txt                                 perfil activo
//     vdisc/<fp>/sys + files                     disco virtual: enlaces duros al disco extraído + mods
//     locks/store.lock                           una sola operación de escritura a la vez
//
// La ROM y el disco extraído del juego solo se leen. Los guardados (nand/) no se tocan nunca.
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "wl/mods/fst.h"
#include "wl/mods/json.h"
#include "wl/mods/package.h"

namespace wl::mods {

namespace fs = std::filesystem;

constexpr int kFingerprintSchema = 1;
constexpr const char* kVanilla = "vanilla";

struct ModMeta {               // lo que se sabe del origen (el usuario o el proveedor); NO es prueba de nada
    std::string id;            // «local.nombre» o «gamebanana.123456»; vacío = se deduce del archivo
    std::string name, version, provider = "local", source_id, source_page, license, author;
    std::string source_file_id, source_md5;   // archivo concreto del proveedor (para detectar actualizaciones)
    int64_t source_added = 0;
};

struct InstalledMod {
    std::string key;           // carpeta en packages/
    Json json;                 // mod.json
    std::string id() const { return json["id"].str(); }
    std::string name() const { return json["name"].str(); }
    std::string version() const { return json["version"].str(); }
    std::string content_sha() const { return json["content_sha256"].str(); }
};

struct InstallHashes {
    std::string content_sha256, package_sha256;
};

struct ProfileEntry {
    std::string id;
    std::string package;       // clave en packages/
    bool enabled = true;
    std::map<std::string, int> choices;   // opciones de Riivolution elegidas
};

struct Profile {
    std::string name = "default";
    uint64_t epoch = 0;        // sube con cada cambio (activar, orden, opciones...)
    std::string restore;       // solo en el perfil de sesión: perfil al que volver
    std::string lobby;         // solo en el perfil de sesión
    std::vector<ProfileEntry> mods;   // orden de carga: el último gana
    Json to_json() const;
    static bool from_json(const Json& j, Profile& out, std::string& error);
};

struct EffectiveFile {
    std::string disc;          // ruta en files/
    fs::path source;           // archivo del paquete
    std::string sha256;
    uint64_t size = 0;
    std::string mod_id;
};

struct Effective {
    std::vector<EffectiveFile> files;
    std::vector<std::string> conflicts;   // "ruta: modA lo pisa modB" (orden de carga)
    std::vector<std::string> problems;    // impiden lanzar (paquete que falta, alterado...)
    Json manifest_mods = Json::array();   // para el manifiesto de sesión
    std::string fingerprint = kVanilla;   // «vanilla» sin mods activos
    bool vanilla() const { return files.empty(); }
};

struct BaseInfo {
    std::string disc_id;       // SUPP01
    std::string fst_sha256;
    std::string base_content_id;   // "SUPP01:<16 hex de la FST>"
};

class ModStore {
public:
    ModStore(std::string game_id, fs::path data_root, fs::path extracted_dir);

    // Crea las carpetas, limpia operaciones a medias y lee el disco base. false si el juego no admite mods.
    bool open(std::string& error);
    const BaseInfo& base() const { return base_; }
    const Fst& fst() const { return fst_; }
    const GameModSupport& support() const { return *support_; }
    const fs::path& root() const { return root_; }
    fs::path download_path() const;

    using Progress = std::function<void(const std::string& what, uint64_t done, uint64_t total)>;
    using Cancel = std::function<bool()>;

    // Solo lectura: analiza un ZIP o carpeta contra el disco del usuario.
    bool preview(const fs::path& source, PackagePlan& plan, std::string& error) const;
    // Instala en packages/ de forma transaccional. No toca ningún perfil.
    bool install(const fs::path& source, const ModMeta& meta, InstalledMod& out, std::string& error,
                 const Progress& progress = {}, const Cancel& cancel = {}, const InstallHashes& expected = {});
    std::vector<InstalledMod> installed() const;
    bool find_installed(const std::string& key, InstalledMod& out) const;
    // Borra un paquete que ningún perfil usa.
    bool uninstall(const std::string& key, std::string& error);
    // Comprueba que los archivos del paquete siguen intactos (hash).
    bool verify(const InstalledMod& mod, std::string& error) const;
    bool verify_profile(const Profile& profile, const std::string& fingerprint, std::string& error) const;

    // Perfiles
    std::vector<std::string> profiles() const;
    bool load_profile(const std::string& name, Profile& out, std::string& error) const;
    bool save_profile(Profile profile, std::string& error);   // sube epoch
    bool replace_profile_package(const std::string& name, const std::string& old_key,
                                 const std::string& new_key, std::string& error);
    bool delete_profile(const std::string& name, std::string& error);
    std::string active_profile() const;
    bool set_active_profile(const std::string& name, std::string& error);
    // Perfil de sesión temporal para una sala: guarda a qué perfil volver y lo activa. end_session() vuelve al
    // perfil habitual (también tras un cierre inesperado: se llama al abrir el launcher). Nunca borra mods.
    bool begin_session(Profile session, const std::string& lobby, std::string& error);
    bool end_session(std::string& error);

    // Conjunto efectivo de un perfil, con fingerprint canónico.
    Effective compute(const Profile& profile) const;
    // Disco virtual para lanzar el juego con ese conjunto. `out` = carpeta para argv[1].
    bool build_vdisc(const Effective& eff, fs::path& out, std::string& error, const Cancel& cancel = {});

private:
    std::string game_;
    fs::path root_, extracted_;
    const GameModSupport* support_ = nullptr;
    BaseInfo base_;
    Fst fst_;
};

// Utilidades compartidas
bool write_file_atomic(const fs::path& path, const std::string& data, std::string& error);
bool read_file_text(const fs::path& path, std::string& out, size_t max_bytes = 4 << 20);
std::string slug(const std::string& text);
// Carpeta de datos de mods de un juego (bajo wl::local_app_data()).
fs::path default_mods_root(const std::string& game_id);

// Bloqueo exclusivo entre procesos (dos launchers abiertos). Se libera al destruirse.
class StoreLock {
public:
    explicit StoreLock(const fs::path& file);
    ~StoreLock();
    bool held() const { return held_; }
    StoreLock(const StoreLock&) = delete;
    StoreLock& operator=(const StoreLock&) = delete;
private:
    void* handle_ = nullptr;
    bool held_ = false;
};

}  // namespace wl::mods
