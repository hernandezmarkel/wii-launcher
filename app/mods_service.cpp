#include "mods_service.h"

#include <algorithm>

#include "wl/adapter.h"
#include "wl/mods/launch.h"
#include "wl/mods/lobby_mods.h"
#include "wl/mods/safe_path.h"

namespace wl {

using namespace wl::mods;

ModsService::ModsService(std::function<void()> wake, std::unique_ptr<upd::HttpClient> http)
    : wake_(std::move(wake)), http_(std::move(http)) {
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
    clear_pending_download();
}

void ModsService::clear_pending_download() {
    std::error_code ec;
    if (!pending_download_.empty()) fs::remove(pending_download_, ec);
    pending_download_.clear();
    pending_meta_ = ModMeta{};
    pending_old_key_.clear();
    pending_profile_.clear();
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
        // Ocupado desde que se encola (no desde que empieza): la UI nunca ve un estado intermedio «libre».
        if (!snap_.busy) snap_.busy_text = busy_text;
        snap_.busy = true;
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
    snap_.session_active = snap_.active == "session";
}

void ModsService::open(const std::string& game_id, const std::string& game_name, const fs::path& install_root) {
    post("Abriendo los mods…", [this, game_id, game_name, install_root] {
        clear_pending_download();
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
        game_revision_ = read_first_line(install_root / ".launcher-revision");
        snap_.ready = true;
        snap_.gb_game = gamebanana_game(game_id);
        if (!http_) http_ = upd::make_winhttp_client();
        gb_ = std::make_unique<GameBananaProvider>(*http_, store_->root() / "cache" / "catalog");
        snap_.base = store_->base();
        reload_locked();
    });
}

void ModsService::preview(const fs::path& source) {
    post("Analizando el paquete…", [this, source] {
        if (!store_) return;
        if (source != pending_download_) clear_pending_download();
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
        snap_.preview_update.clear();
        snap_.preview_profile.clear();
    });
}

void ModsService::discard_preview() {
    post("Descartando…", [this] {
        clear_pending_download();
        std::lock_guard<std::mutex> lock(mutex_);
        snap_.has_preview = false;
        snap_.preview = PackagePlan{};
    });
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
        ModMeta meta = pending_meta_;   // del proveedor, si viene de Explorar
        const std::string old_key = pending_old_key_, update_profile = pending_profile_;
        if (!name.empty()) meta.name = name;
        if (!version.empty() && meta.provider == "local") meta.version = version;
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
        clear_pending_download();
        std::string why;
        if (ok && !update_profile.empty()) {
            ok = store_->replace_profile_package(update_profile, old_key, mod.key, why);
        } else if (ok && activate && old_key.empty()) {
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
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (ok) {
            snap_.updates_checked = false;
            snap_.notice = "Instalado «" + mod.name() + "» " + mod.version() +
                (!update_profile.empty() ? ". Se conservan la versión anterior y los ajustes del perfil."
                    : activate && old_key.empty() ? " y activado en el perfil «" + store_->active_profile() + "»." : ".");
        } else if (!why.empty()) {
            snap_.error = "El paquete queda instalado. No se cambió el perfil: " + why;
        } else {
            snap_.error = "No se ha instalado: " + e;
        }
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

// ------------------------------------------------------------------ Explorar / actualizaciones

void ModsService::browse(int page, const std::string& search) {
    post("Consultando GameBanana…", [this, page, search] {
        if (!gb_) return;
        CatalogPage cp;
        std::string e;
        bool ok = gb_->list(gamebanana_game(snap_.game_id), page, search, cp, e);
        std::lock_guard<std::mutex> lock(mutex_);
        snap_.page = page;
        snap_.search = search;
        snap_.catalog_loaded = true;
        if (ok) {
            snap_.catalog = std::move(cp);
            snap_.catalog_error.clear();
        } else {
            snap_.catalog = CatalogPage{};
            snap_.catalog_error = e + " El resto del launcher y los mods instalados siguen funcionando.";
        }
    });
}

void ModsService::open_detail(int64_t mod_id, const std::string& old_key) {
    post("Cargando la ficha…", [this, mod_id, old_key] {
        if (!gb_ || !store_) return;
        CatalogDetail d;
        std::string e;
        bool ok = gb_->detail(mod_id, d, e);
        std::vector<QuickCheck> checks;
        if (ok && !d.available_for(gamebanana_game(store_->support().game_id))) {
            e = "Esta ficha no está disponible para el juego seleccionado.";
            ok = false;
        }
        if (ok && !old_key.empty()) {
            InstalledMod old;
            std::vector<CatalogFile> files;
            if (store_->find_installed(old_key, old))
                for (const auto& file : d.files)
                    if (is_catalog_update(d, file, old)) files.push_back(file);
            d.files = std::move(files);
            if (d.files.empty()) { ok = false; e = "No quedan archivos de actualización para esta versión."; }
        }
        if (ok) {
            for (const auto& f : d.files) {
                QuickCheck q;
                std::vector<std::string> tree;
                std::string te;
                if (!f.supported_archive()) {
                    q.verdict = QuickVerdict::Incompatible;
                    q.reason = "se necesita un ZIP de hasta 512 MiB; para 7z/RAR importa la carpeta descomprimida";
                } else if (gb_->file_tree(f.id, tree, te)) {
                    q = quick_check(tree, store_->fst(), store_->support(), store_->base().disc_id);
                } else {
                    q.reason = te;
                }
                checks.push_back(q);
            }
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ok) { snap_.error = e; return; }
        snap_.has_detail = true;
        snap_.detail_update = old_key;
        snap_.detail = std::move(d);
        snap_.checks = std::move(checks);
    });
}

void ModsService::close_detail() {
    std::lock_guard<std::mutex> lock(mutex_);
    snap_.has_detail = false;
}

void ModsService::download_file(int64_t mod_id, int64_t file_id) {
    CatalogFile chosen;
    std::string old_key;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!snap_.has_detail || snap_.detail.item.id != mod_id) return;
        const auto& files = snap_.detail.files;
        auto file = std::find_if(files.begin(), files.end(), [&](const CatalogFile& f) { return f.id == file_id; });
        if (file == files.end()) return;
        chosen = *file;
        old_key = snap_.detail_update;
    }
    post("Descargando…", [this, mod_id, chosen, old_key] {
        if (!gb_ || !store_) return;
        CatalogDetail d;
        std::string e;
        // Se vuelve a pedir la ficha: los datos de la descarga salen del proveedor, nunca de la UI ni de una sala.
        if (!gb_->detail(mod_id, d, e)) { std::lock_guard<std::mutex> lock(mutex_); snap_.error = e; return; }
        if (!d.available_for(gamebanana_game(store_->support().game_id))) {
            std::lock_guard<std::mutex> lock(mutex_);
            snap_.error = "Esta ficha no está disponible para el juego seleccionado.";
            return;
        }
        const CatalogFile* file = nullptr;
        for (const auto& f : d.files)
            if (f.id == chosen.id && f.md5 == chosen.md5 && f.size == chosen.size) file = &f;
        if (!file || !file->supported_archive()) {
            std::lock_guard<std::mutex> lock(mutex_);
            snap_.error = "Ese ZIP ya no está disponible en GameBanana.";
            return;
        }
        std::string update_profile;
        if (!old_key.empty()) {
            Profile p;
            if (!store_->load_profile(store_->active_profile(), p, e)) {
                std::lock_guard<std::mutex> lock(mutex_);
                snap_.error = e;
                return;
            }
            if (std::any_of(p.mods.begin(), p.mods.end(), [&](const ProfileEntry& entry) { return entry.package == old_key; }))
                update_profile = p.name;
        }
        fs::path dest = store_->download_path();
        auto progress = [this](uint64_t got, uint64_t total) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                snap_.done = got;
                snap_.total = total;
            }
            if (wake_) wake_();
            return !cancel_.load();
        };
        bool ok = gb_->download(*file, dest, e, progress);
        if (!ok) { std::lock_guard<std::mutex> lock(mutex_); snap_.error = "Descarga fallida: " + e + " No se ha instalado nada."; return; }
        PackagePlan plan;
        ok = store_->preview(dest, plan, e);
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ok) {
            std::error_code ec;
            fs::remove(dest, ec);
            snap_.error = "El archivo descargado no se puede abrir: " + e;
            return;
        }
        clear_pending_download();
        pending_download_ = dest;
        pending_meta_ = catalog_meta("gamebanana", d, *file);
        pending_old_key_ = old_key;
        pending_profile_ = update_profile;
        snap_.has_detail = false;
        snap_.has_preview = true;
        snap_.preview_update = old_key;
        snap_.preview_profile = update_profile;
        snap_.preview = std::move(plan);
        snap_.preview_source = to_utf8(dest);
    });
}

void ModsService::check_updates() {
    post("Buscando actualizaciones…", [this] {
        if (!gb_ || !store_) return;
        std::vector<ModsSnapshot::Update> out;
        Profile profile;
        std::string profile_error;
        if (!store_->load_profile(store_->active_profile(), profile, profile_error)) {
            std::lock_guard<std::mutex> lock(mutex_);
            snap_.error = profile_error;
            return;
        }
        for (const auto& m : store_->installed()) {
            if (m.json["provider"].str() != "gamebanana") continue;
            const auto selected = std::find_if(profile.mods.begin(), profile.mods.end(),
                [&](const ProfileEntry& entry) { return entry.id == m.id(); });
            if (selected != profile.mods.end() && selected->package != m.key) continue;
            ModsSnapshot::Update u;
            u.key = m.key;
            u.name = m.name();
            u.installed_version = m.version();
            u.mod_id = std::atoll(m.json["source_id"].str().c_str());
            CatalogDetail d;
            std::string e;
            if (!gb_->detail(u.mod_id, d, e)) { u.note = e; out.push_back(u); continue; }
            if (!d.available_for(gamebanana_game(store_->support().game_id))) {
                u.note = "ficha no disponible para este juego";
            } else if (m.json["source_file_id"].str().empty() || m.json["source_md5"].str().empty()) {
                u.note = "falta la referencia al ZIP original. Consulta la ficha en Explorar.";
            } else {
                u.available = std::any_of(d.files.begin(), d.files.end(),
                    [&](const CatalogFile& file) { return is_catalog_update(d, file, m); });
                u.note = u.available ? "Hay archivos nuevos. Revisa cuál corresponde a tu variante." : "Sin actualizaciones identificadas.";
            }
            out.push_back(u);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        snap_.updates = std::move(out);
        snap_.updates_checked = true;
    });
}

// ------------------------------------------------------------------ sala

void ModsService::session_compare(const std::string& address, uint16_t room_port) {
    post("Comparando tus mods con los del anfitrión…", [this, address, room_port] {
        if (!store_) return;
        std::string text, e;
        SessionManifest m;
        bool ok = false;
        if (room_port > 65535 - kManifestPortOffset) e = "puerto fuera del intervalo del manifiesto";
        else ok = fetch_manifest(address, static_cast<uint16_t>(room_port + kManifestPortOffset), text, e) && parse_session_manifest(text, m, e);
        Profile p;
        std::string pe;
        if (ok) ok = store_->load_profile(store_->active_profile(), p, pe) || (e = pe, false);
        SessionDiff d;
        if (ok) d = diff_session(*store_, p, m, game_revision_);
        std::lock_guard<std::mutex> lock(mutex_);
        snap_.session_checked = true;
        snap_.session_address = address;
        snap_.session_port = room_port;
        snap_.sync_planned = false;
        snap_.sync.clear();
        if (!ok) {
            snap_.session_error = e;
            snap_.session_diff = SessionDiff{};
            return;
        }
        snap_.session_error.clear();
        snap_.session_manifest = std::move(m);
        snap_.session_diff = std::move(d);
    });
}

void ModsService::sync_prepare() {
    post("Buscando los mods que faltan en GameBanana…", [this] {
        if (!store_ || !gb_) return;
        SessionDiff d;
        SessionManifest host;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            d = snap_.session_diff;
            host = snap_.session_manifest;
        }
        std::vector<ModsSnapshot::SyncItem> plan;
        for (const auto& it : d.items) {
            if (it.state != ModDiff::Missing && it.state != ModDiff::WrongVersion &&
                it.state != ModDiff::WrongHash && it.state != ModDiff::NoAuthorizedSource) continue;
            ModsSnapshot::SyncItem s;
            s.id = it.id;
            s.name = it.name;
            s.source_id = it.source_id;
            if (!it.downloadable) { s.reason = it.detail.empty() ? "sin fuente autorizada" : it.detail; plan.push_back(s); continue; }
            // Se busca en GameBanana por su id: la sala nunca da URLs.
            s.mod_id = std::atoll(it.source_id.c_str());
            CatalogDetail det;
            std::string e;
            if (!gb_->detail(s.mod_id, det, e)) { s.reason = e; plan.push_back(s); continue; }
            auto wanted = std::find_if(host.mods.begin(), host.mods.end(), [&](const SessionMod& mod) { return mod.id == it.id; });
            const CatalogFile* pick = wanted != host.mods.end() && det.available_for(gamebanana_game(store_->support().game_id))
                ? resolve_catalog_file(det, *wanted) : nullptr;
            if (!pick) { s.reason = "No se encuentra la versión exacta en GameBanana. Puedes importarla manualmente."; plan.push_back(s); continue; }
            std::vector<std::string> tree;
            if (gb_->file_tree(pick->id, tree, e)) {
                auto check = quick_check(tree, store_->fst(), store_->support(), store_->base().disc_id);
                if (check.verdict == QuickVerdict::Incompatible) { s.reason = check.reason; plan.push_back(s); continue; }
            }
            s.file_id = pick->id;
            s.file_name = pick->name;
            s.md5 = pick->md5;
            s.size = pick->size;
            s.ok = true;
            plan.push_back(s);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        snap_.sync = std::move(plan);
        snap_.sync_planned = true;
    });
}

void ModsService::sync_run() {
    post("Descargando los mods de la sala…", [this] {
        if (!store_ || !gb_) return;
        std::vector<ModsSnapshot::SyncItem> plan;
        SessionManifest host;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            plan = snap_.sync;
            host = snap_.session_manifest;
        }
        for (auto& s : plan) {
            if (!s.ok || s.done || cancel_) continue;
            auto wanted = std::find_if(host.mods.begin(), host.mods.end(), [&](const SessionMod& mod) { return mod.id == s.id; });
            if (wanted == host.mods.end()) continue;
            CatalogDetail det;
            std::string e;
            const CatalogFile* file = nullptr;
            if (gb_->detail(s.mod_id, det, e) && det.available_for(gamebanana_game(store_->support().game_id)))
                file = resolve_catalog_file(det, *wanted);
            if (!file || file->id != s.file_id || file->md5 != s.md5 || file->size != s.size) {
                s.reason = e.empty() ? "el archivo cambió. Vuelve a comparar con la sala." : e;
                s.ok = false;
                continue;
            }
            fs::path dest = store_->download_path();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                snap_.busy_text = "Descargando " + s.name;
            }
            auto progress = [this](uint64_t got, uint64_t total) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    snap_.done = got;
                    snap_.total = total;
                }
                if (wake_) wake_();
                return !cancel_.load();
            };
            std::error_code ec;
            if (!gb_->download(*file, dest, e, progress)) { s.reason = e; s.ok = false; continue; }
            ModMeta meta = catalog_meta("gamebanana", det, *file);
            meta.version = wanted->version;
            InstalledMod mod;
            bool ok = store_->install(dest, meta, mod, e, {}, [this] { return cancel_.load(); },
                                      {wanted->content_sha256, wanted->package_sha256});
            fs::remove(dest, ec);
            if (!ok) { s.reason = e; s.ok = false; continue; }
            s.done = true;
        }
        Profile p;
        std::string e;
        bool use = false;
        {
            const bool loaded = store_->load_profile(store_->active_profile(), p, e);
            SessionDiff d = loaded ? diff_session(*store_, p, host, game_revision_) : SessionDiff{};
            use = loaded && d.session_ready && !cancel_;
            std::lock_guard<std::mutex> lock(mutex_);
            snap_.sync = std::move(plan);
            snap_.session_diff = std::move(d);
            if (!loaded) snap_.error = e;
            reload_locked();
        }
        if (use) apply_room_profile();
    });
}

void ModsService::session_use() {
    post("Preparando el perfil de la sala…", [this] { apply_room_profile(); });
}

void ModsService::apply_room_profile() {
    if (!store_ || cancel_) return;
    SessionDiff diff;
    SessionManifest host;
    std::string address;
    uint16_t port;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!snap_.session_checked) return;
        diff = snap_.session_diff;
        host = snap_.session_manifest;
        address = snap_.session_address;
        port = snap_.session_port;
        snap_.busy_text = "Preparando el perfil de la sala…";
        snap_.done = snap_.total = 0;
    }
    if (wake_) wake_();
    std::string error, text;
    SessionManifest current;
    bool ok = diff.session_ready && port > 0 && port <= 65535 - kManifestPortOffset &&
        fetch_manifest(address, static_cast<uint16_t>(port + kManifestPortOffset), text, error) &&
        parse_session_manifest(text, current, error);
    if (ok && (current.lobby_id != host.lobby_id || current.fingerprint != host.fingerprint ||
               current.game_revision != host.game_revision || current.base_content_id != host.base_content_id ||
               current.game_id != host.game_id || current.loader != host.loader)) {
        error = "la sala cambió durante la preparación. Vuelve a comparar los mods.";
        ok = false;
    }
    bool activated = false;
    if (ok) {
        ok = store_->verify_profile(diff.session_profile, host.fingerprint, error);
        if (cancel_) return;
        if (ok) ok = activated = store_->begin_session(diff.session_profile, address, error);
    }
    Profile profile;
    if (ok && store_->load_profile("session", profile, error)) {
        diff = diff_session(*store_, profile, host, game_revision_);
        ok = diff.compatible;
    } else ok = false;
    if (!ok) {
        diff.compatible = false;
        if (activated) {
            std::string restore_error;
            if (!store_->end_session(restore_error)) error = restore_error;
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ok) snap_.error = error.empty() ? "No se pudo verificar el perfil de la sala." : error;
    else snap_.notice = "Perfil de la sala activo. Tu perfil habitual vuelve al salir de la sala o al cerrar la partida.";
    snap_.session_diff = std::move(diff);
    reload_locked();
}

void ModsService::verify_ready(const std::string& fingerprint) {
    post("Verificando los mods de la partida…", [this, fingerprint] {
        std::string error;
        Profile p;
        if (!store_) error = "no se puede abrir el almacén de mods";
        const bool ok = store_ && store_->load_profile(store_->active_profile(), p, error) &&
            store_->verify_profile(p, fingerprint, error);
        std::lock_guard<std::mutex> lock(mutex_);
        snap_.ready_verified = ok;
        snap_.ready_error = std::move(error);
    });
}

void ModsService::session_end() {
    post("Volviendo a tu perfil…", [this] {
        if (!store_) return;
        std::string e;
        bool ok = store_->end_session(e);
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ok) snap_.error = e;
        snap_.session_checked = false;
        snap_.sync.clear();
        snap_.sync_planned = false;
        reload_locked();
    });
}

}  // namespace wl
