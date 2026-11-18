#include "mods_service.h"

#include <algorithm>

#include "wl/mods/launch.h"
#include "wl/mods/safe_path.h"

namespace wl {

using namespace wl::mods;

ModsService::ModsService(std::function<void()> wake) : wake_(std::move(wake)) {
    thread_ = std::thread([this] { run(); });
}

ModsService::~ModsService() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
        cancel_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void ModsService::run() {
    while (true) {
        std::pair<std::string, std::function<void()>> job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
            if (stop_) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
            snap_.busy = true;
            snap_.busy_text = job.first;
            snap_.done = snap_.total = 0;
        }
        cancel_ = false;
        if (wake_) wake_();
        job.second();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snap_.busy = !jobs_.empty();
            if (!snap_.busy) snap_.busy_text.clear();
        }
        if (wake_) wake_();
    }
}

void ModsService::post(std::string busy_text, std::function<void()> job) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        jobs_.emplace_back(std::move(busy_text), std::move(job));
    }
    cv_.notify_one();
}

ModsSnapshot ModsService::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snap_;
}

void ModsService::clear_messages() {
    std::lock_guard<std::mutex> lock(mutex_);
    snap_.error.clear();
    snap_.notice.clear();
}

void ModsService::reload_locked() {
    if (!store_) return;
    snap_.installed = store_->installed();
    snap_.profiles = store_->profiles();
    snap_.active = store_->active_profile();
    std::string e;
    Profile p;
    if (!store_->load_profile(snap_.active, p, e)) {
        snap_.error = e;
        p = Profile{};
    }
    snap_.profile = p;
    snap_.effective = store_->compute(p);
}

void ModsService::open(const std::string& game_id, const std::string& game_name, const fs::path& install_root) {
    post("Abriendo los mods…", [this, game_id, game_name, install_root] {
        auto store = std::make_unique<ModStore>(game_id, default_mods_root(game_id), extracted_dir(game_id, install_root));
        std::string e;
        const GameModSupport* support = game_support(game_id);
        bool ok = support && store->open(e);
        std::lock_guard<std::mutex> lock(mutex_);
        snap_ = ModsSnapshot{};
        snap_.game_id = game_id;
        snap_.game_name = game_name;
        snap_.supported = support != nullptr;
        snap_.proven = support && support->proven;
        snap_.busy = true;
        if (!ok) {
            snap_.error = support ? e : "Este juego todavía no admite mods en el launcher.";
            store_.reset();
            return;
        }
        store_ = std::move(store);
        snap_.ready = true;
        snap_.base = store_->base();
        reload_locked();
    });
}

void ModsService::preview(const fs::path& source) {
    post("Analizando el paquete…", [this, source] {
        if (!store_) return;
        PackagePlan plan;
        std::string e;
        bool ok = store_->preview(source, plan, e);
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ok) {
            snap_.error = "No se puede abrir: " + e;
            snap_.has_preview = false;
            return;
        }
        snap_.has_preview = true;
        snap_.preview = std::move(plan);
        snap_.preview_source = to_utf8(source);
    });
}

void ModsService::discard_preview() {
    std::lock_guard<std::mutex> lock(mutex_);
    snap_.has_preview = false;
    snap_.preview = PackagePlan{};
}

void ModsService::install_preview(const std::string& name, const std::string& version, bool activate) {
    std::string source;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!snap_.has_preview) return;
        source = snap_.preview_source;
        snap_.has_preview = false;
    }
    post("Instalando…", [this, source, name, version, activate] {
        if (!store_) return;
        ModMeta meta;
        meta.name = name;
        meta.version = version;
        InstalledMod mod;
        std::string e;
        auto progress = [this](const std::string& what, uint64_t done, uint64_t total) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                snap_.busy_text = what.empty() ? "Instalando…" : "Instalando " + what;
                snap_.done = done;
                snap_.total = total;
            }
            if (wake_) wake_();
        };
        bool ok = store_->install(from_utf8(source), meta, mod, e, progress, [this] { return cancel_.load(); });
        std::string why;
        if (ok && activate) {
            Profile p;
            if (store_->load_profile(store_->active_profile(), p, why)) {
                bool found = false;
                for (auto& m : p.mods)
                    if (m.id == mod.id()) { m.package = mod.key; m.enabled = true; found = true; }   // actualización: misma id, paquete nuevo
                if (!found) p.mods.push_back({mod.id(), mod.key, true, {}});
                if (!store_->save_profile(p, why)) ok = false;
            } else {
                ok = false;
            }
            if (!ok) e = why;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (ok) snap_.notice = "Instalado «" + mod.name() + "» " + mod.version() + (activate ? " y activado en el perfil «" + store_->active_profile() + "»." : ".");
        else snap_.error = "No se ha instalado: " + e;
        reload_locked();
    });
}

void ModsService::save_profile(Profile profile) {
    post("Guardando el perfil…", [this, profile] {
        if (!store_) return;
        std::string e;
        bool ok = store_->save_profile(profile, e);
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ok) snap_.error = e;
        reload_locked();
    });
}

void ModsService::activate_profile(const std::string& name) {
    post("Cambiando de perfil…", [this, name] {
        if (!store_) return;
        std::string e;
        bool ok = store_->set_active_profile(name, e);
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ok) snap_.error = e;
        else snap_.notice = "Perfil activo: «" + name + "».";
        reload_locked();
    });
}

void ModsService::create_profile(const std::string& name, bool copy_current) {
    post("Creando el perfil…", [this, name, copy_current] {
        if (!store_) return;
        std::string e;
        Profile p;
        auto names = store_->profiles();
        if (std::find(names.begin(), names.end(), name) != names.end()) {
            std::lock_guard<std::mutex> lock(mutex_);
            snap_.error = "Ya existe un perfil con ese nombre.";
            return;
        }
        if (copy_current) store_->load_profile(store_->active_profile(), p, e);
        p.name = name;
        p.epoch = 0;
        bool ok = store_->save_profile(p, e) && store_->set_active_profile(name, e);
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ok) snap_.error = e;
        else snap_.notice = "Perfil «" + name + "» creado y activo.";
        reload_locked();
    });
}

void ModsService::delete_profile(const std::string& name) {
    post("Borrando el perfil…", [this, name] {
        if (!store_) return;
        std::string e;
        bool ok = store_->delete_profile(name, e);
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ok) snap_.error = e;
        else snap_.notice = "Perfil «" + name + "» borrado (los mods siguen instalados).";
        reload_locked();
    });
}

void ModsService::uninstall(const std::string& key) {
    post("Desinstalando…", [this, key] {
        if (!store_) return;
        std::string e;
        bool ok = store_->uninstall(key, e);
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ok) snap_.error = "No se ha desinstalado: " + e;
        else snap_.notice = "Desinstalado.";
        reload_locked();
    });
}

}  // namespace wl
