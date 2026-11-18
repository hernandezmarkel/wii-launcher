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
    mods::PackagePlan preview;
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
    void clear_messages();

private:
    void post(std::string busy_text, std::function<void()> job);
    void reload_locked();   // con mutex_ tomado; lee perfiles y paquetes (rápido)
    void run();

    std::function<void()> wake_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::pair<std::string, std::function<void()>>> jobs_;
    std::thread thread_;
    bool stop_ = false;
    std::atomic<bool> cancel_{false};
    std::unique_ptr<mods::ModStore> store_;
    ModsSnapshot snap_;
};

}  // namespace wl
