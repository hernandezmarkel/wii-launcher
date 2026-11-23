#pragma once
// Servicio de mods para la interfaz: todo el trabajo de disco y red va en un hilo propio, en orden, y la UI
// solo lee instantáneas. Así importar, instalar o construir el disco virtual nunca congela la ventana.
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "wl/mods/gamebanana.h"
#include "wl/mods/session.h"
#include "wl/mods/store.h"

namespace wl {

struct ModsSnapshot {
    std::string game_id, game_name;
    bool ready = false;            // almacén abierto
    bool supported = false;        // el juego admite mods en este launcher
    bool proven = false;           // A/B superada
    bool busy = false;
    std::string busy_text;
    uint64_t done = 0, total = 0;
    std::string error, notice;
    mods::BaseInfo base;
    std::vector<mods::InstalledMod> installed;
    std::vector<std::string> profiles;
    std::string active;            // perfil activo
    mods::Profile profile;         // perfil que se está editando (el activo)
    mods::Effective effective;     // su conjunto efectivo
    // Importación pendiente de confirmar
    bool has_preview = false;
    std::string preview_source;    // ruta en UTF-8
    std::string preview_update, preview_profile;
    mods::PackagePlan preview;
    // Explorar (GameBanana)
    int64_t gb_game = 0;
    int page = 1;
    std::string search;
    mods::CatalogPage catalog;
    bool catalog_loaded = false;
    std::string catalog_error;
    bool has_detail = false;
    mods::CatalogDetail detail;
    std::string detail_update;
    std::vector<mods::QuickCheck> checks;   // una por archivo de la ficha
    // Actualizaciones
    struct Update {
        std::string key, name, installed_version;
        int64_t mod_id = 0;
        bool available = false;
        std::string note;
    };
    std::vector<Update> updates;
    bool updates_checked = false;
    // Sala: comparación con el anfitrión y sincronización asistida
    struct SyncItem {
        std::string id, name, source_id, file_name, md5, reason;
        uint64_t size = 0;
        int64_t mod_id = 0, file_id = 0;
        bool ok = false;           // se puede descargar
        bool done = false;
    };
    bool session_checked = false;
    std::string session_address, session_error;
    uint16_t session_port = 0;
    mods::SessionManifest session_manifest;
    mods::SessionDiff session_diff;
    bool sync_planned = false;
    std::vector<SyncItem> sync;
    bool session_active = false;   // el perfil activo es el temporal de una sala
    bool ready_verified = false;
    std::string ready_error;
};

class ModsService {
public:
    explicit ModsService(std::function<void()> wake);
    ~ModsService();

    void open(const std::string& game_id, const std::string& game_name, const std::filesystem::path& install_root);
    ModsSnapshot snapshot() const;

    void preview(const std::filesystem::path& source);
    void discard_preview();
    void install_preview(const std::string& name, const std::string& version, bool activate);
    void save_profile(mods::Profile profile);
    void activate_profile(const std::string& name);
    void create_profile(const std::string& name, bool copy_current);
    void delete_profile(const std::string& name);
    void uninstall(const std::string& key);
    void cancel() { cancel_ = true; }
    void browse(int page, const std::string& search);
    void open_detail(int64_t mod_id, const std::string& old_key = {});
    void close_detail();
    // Tras el consentimiento explícito del usuario: descarga a staging y abre la revisión previa.
    void download_file(int64_t mod_id, int64_t file_id);
    void check_updates();
    // Sala (cliente): pide el manifiesto del anfitrión y compara. Nada se descarga sin sync_run().
    void session_compare(const std::string& address, uint16_t room_port);
    void sync_prepare();           // consulta tamaños y archivos de lo que falta (sin descargar)
    void sync_run();               // tras el consentimiento: descarga, instala y vuelve a comparar
    void session_use();            // activa el perfil de sesión que reproduce el del anfitrión
    void session_end();            // vuelve al perfil habitual
    void verify_ready(const std::string& fingerprint);
    void clear_messages();

private:
    void post(std::string busy_text, std::function<void()> job);
    void reload_locked();   // con mutex_ tomado; lee perfiles y paquetes (rápido)
    void clear_pending_download();
    void apply_room_profile();
    void run();

    std::function<void()> wake_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::pair<std::string, std::function<void()>>> jobs_;
    std::thread thread_;
    bool stop_ = false;
    std::atomic<bool> cancel_{false};
    std::unique_ptr<mods::ModStore> store_;
    std::unique_ptr<upd::HttpClient> http_;
    std::unique_ptr<mods::ModProvider> gb_;
    std::string game_revision_;
    mods::ModMeta pending_meta_;     // metadatos del proveedor para la instalación pendiente
    std::filesystem::path pending_download_;   // archivo descargado en staging (se borra al instalar o descartar)
    std::string pending_old_key_, pending_profile_;
    ModsSnapshot snap_;
};

}  // namespace wl
