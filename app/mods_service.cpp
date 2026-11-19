#include "mods_service.h"

#include <algorithm>
#include <cctype>

#include "wl/mods/launch.h"
#include "wl/mods/lobby_mods.h"
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
        snap_.gb_game = gamebanana_game(game_id);
        if (!http_) http_ = upd::make_winhttp_client();
        gb_ = std::make_unique<GameBananaProvider>(*http_, store_->root() / "cache" / "catalog");
        std::error_code ec;
        fs::remove_all(store_->root() / "cache" / "downloads", ec);   // descargas que nadie llegó a instalar
        pending_download_.clear();
        pending_meta_ = ModMeta{};
        snap_.base = store_->base();
        reload_locked();
    });
}

void ModsService::preview(const fs::path& source) {
    post("Analizando el paquete…", [this, source] {
        if (!store_) return;
        if (source != pending_download_) {   // importación manual: olvida una descarga pendiente
            std::error_code ec;
            if (!pending_download_.empty()) fs::remove(pending_download_, ec);
            pending_download_.clear();
            pending_meta_ = ModMeta{};
        }
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
    post("Descartando…", [this] {
        std::error_code ec;
        if (!pending_download_.empty()) fs::remove(pending_download_, ec);
        pending_download_.clear();
        pending_meta_ = ModMeta{};
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
        if (!name.empty()) meta.name = name;
        if (!version.empty()) meta.version = version;
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
        std::error_code ec;
        if (!pending_download_.empty()) fs::remove(pending_download_, ec);
        pending_download_.clear();
        pending_meta_ = ModMeta{};
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

void ModsService::open_detail(int64_t mod_id) {
    post("Cargando la ficha…", [this, mod_id] {
        if (!gb_ || !store_) return;
        CatalogDetail d;
        std::string e;
        bool ok = gb_->detail(mod_id, d, e);
        std::vector<QuickCheck> checks;
        if (ok) {
            for (const auto& f : d.files) {
                QuickCheck q;
                std::string lower = f.name;
                for (auto& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                std::vector<std::string> tree;
                std::string te;
                if (lower.size() > 3 && (lower.rfind(".7z") == lower.size() - 3 || lower.rfind(".rar") == lower.size() - 4)) {
                    q.verdict = QuickVerdict::NeedsDownload;
                    q.reason = "archivo .7z/.rar: descárgalo desde la página, descomprímelo e importa la carpeta";
                } else if (f.size > kMaxModDownload) {
                    q.verdict = QuickVerdict::Incompatible;
                    q.reason = "demasiado grande para un mod (¿un disco entero?)";
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
        snap_.detail = std::move(d);
        snap_.checks = std::move(checks);
    });
}

void ModsService::close_detail() {
    std::lock_guard<std::mutex> lock(mutex_);
    snap_.has_detail = false;
}

void ModsService::download_file(int64_t mod_id, int64_t file_id) {
    post("Descargando…", [this, mod_id, file_id] {
        if (!gb_ || !store_) return;
        CatalogDetail d;
        std::string e;
        // Se vuelve a pedir la ficha: los datos de la descarga salen del proveedor, nunca de la UI ni de una sala.
        if (!gb_->detail(mod_id, d, e)) { std::lock_guard<std::mutex> lock(mutex_); snap_.error = e; return; }
        const CatalogFile* file = nullptr;
        for (const auto& f : d.files)
            if (f.id == file_id) file = &f;
        if (!file) { std::lock_guard<std::mutex> lock(mutex_); snap_.error = "Ese archivo ya no está en GameBanana."; return; }
        fs::path dest = store_->root() / "cache" / "downloads" / (std::to_string(file_id) + ".zip");
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
        pending_download_ = dest;
        pending_meta_ = ModMeta{};
        pending_meta_.id = "gamebanana." + std::to_string(d.item.id);
        pending_meta_.name = d.item.name;
        pending_meta_.version = file->version.empty() ? d.item.version : file->version;
        pending_meta_.provider = "gamebanana";
        pending_meta_.source_id = std::to_string(d.item.id);
        pending_meta_.source_page = d.item.profile_url;
        pending_meta_.license = d.license.substr(0, 300);
        pending_meta_.author = d.item.author;
        pending_meta_.source_file_id = std::to_string(file->id);
        pending_meta_.source_md5 = file->md5;
        snap_.has_detail = false;
        snap_.has_preview = true;
        snap_.preview = std::move(plan);
        snap_.preview_source = to_utf8(dest);
    });
}

void ModsService::check_updates() {
    post("Buscando actualizaciones…", [this] {
        if (!gb_ || !store_) return;
        std::vector<ModsSnapshot::Update> out;
        for (const auto& m : store_->installed()) {
            if (m.json["provider"].str() != "gamebanana") continue;
            ModsSnapshot::Update u;
            u.key = m.key;
            u.name = m.name();
            u.installed_version = m.version();
            u.mod_id = std::atoll(m.json["source_id"].str().c_str());
            CatalogDetail d;
            std::string e;
            if (!gb_->detail(u.mod_id, d, e)) { u.note = e; out.push_back(u); continue; }
            if (d.withheld || d.trashed) { u.note = "retirado de GameBanana"; out.push_back(u); continue; }
            // Más reciente = archivo añadido más tarde. Se compara la identidad del archivo (id + MD5), no la fecha
            // ni el número de versión: dos archivos con la misma versión pueden ser distintos.
            const CatalogFile* newest = nullptr;
            for (const auto& f : d.files)
                if (!newest || f.added > newest->added) newest = &f;
            if (!newest) { u.note = "sin archivos descargables"; out.push_back(u); continue; }
            u.latest_version = newest->version.empty() ? d.item.version : newest->version;
            u.latest_file = std::to_string(newest->id);
            u.available = newest->md5 != m.json["source_md5"].str() || u.latest_file != m.json["source_file_id"].str();
            u.note = u.available ? "hay un archivo nuevo: se instala aparte y puedes volver a la versión anterior" : "al día";
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
        bool ok = fetch_manifest(address, static_cast<uint16_t>(room_port + kManifestPortOffset), text, e) && parse_session_manifest(text, m, e);
        Profile p;
        std::string pe;
        if (ok) ok = store_->load_profile(store_->active_profile(), p, pe) || (e = pe, false);
        SessionDiff d;
        if (ok) d = diff_session(*store_, p, m);
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
        {
            std::lock_guard<std::mutex> lock(mutex_);
            d = snap_.session_diff;
        }
        std::vector<ModsSnapshot::SyncItem> plan;
        for (const auto& it : d.items) {
            if (it.state != ModDiff::Missing && it.state != ModDiff::WrongVersion && it.state != ModDiff::WrongHash) continue;
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
            const CatalogFile* pick = nullptr;
            for (const auto& f : det.files) {
                std::vector<std::string> tree;
                std::string te;
                if (f.size > kMaxModDownload || !gb_->file_tree(f.id, tree, te)) continue;
                if (quick_check(tree, store_->fst(), store_->support(), store_->base().disc_id).verdict == QuickVerdict::Incompatible) continue;
                if (!pick || f.added > pick->added) pick = &f;
            }
            if (!pick) { s.reason = "GameBanana no tiene un archivo compatible"; plan.push_back(s); continue; }
            s.file_id = pick->id;
            s.file_name = pick->name;
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
            if (!s.ok || cancel_) continue;
            CatalogDetail det;
            std::string e;
            const CatalogFile* file = nullptr;
            if (gb_->detail(s.mod_id, det, e))
                for (const auto& f : det.files)
                    if (f.id == s.file_id) file = &f;
            if (!file) { s.reason = e.empty() ? "el archivo ya no está" : e; s.ok = false; continue; }
            fs::path dest = store_->root() / "cache" / "downloads" / (std::to_string(file->id) + ".zip");
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
            ModMeta meta;
            meta.id = "gamebanana." + std::to_string(det.item.id);
            meta.name = det.item.name;
            meta.version = file->version.empty() ? det.item.version : file->version;
            meta.provider = "gamebanana";
            meta.source_id = std::to_string(det.item.id);
            meta.source_page = det.item.profile_url;
            meta.license = det.license.substr(0, 300);
            meta.author = det.item.author;
            meta.source_file_id = std::to_string(file->id);
            meta.source_md5 = file->md5;
            InstalledMod mod;
            bool ok = store_->install(dest, meta, mod, e, {}, [this] { return cancel_.load(); });
            fs::remove(dest, ec);
            if (!ok) { s.reason = e; s.ok = false; continue; }
            // Revalidación: tiene que ser exactamente el contenido del anfitrión.
            auto wanted = std::find_if(host.mods.begin(), host.mods.end(), [&](const SessionMod& h) { return h.id == mod.id(); });
            if (wanted == host.mods.end() || wanted->content_sha256 != mod.content_sha()) {
                std::string ue;
                store_->uninstall(mod.key, ue);
                s.reason = "el archivo de GameBanana no es el que usa el anfitrión (otra versión): pídele el suyo";
                s.ok = false;
                continue;
            }
            s.done = true;
        }
        Profile p;
        std::string e;
        store_->load_profile(store_->active_profile(), p, e);
        SessionDiff d = diff_session(*store_, p, host);
        std::lock_guard<std::mutex> lock(mutex_);
        snap_.sync = std::move(plan);
        snap_.session_diff = std::move(d);
        reload_locked();
    });
}

void ModsService::session_use() {
    post("Preparando el perfil de la sala…", [this] {
        if (!store_) return;
        SessionDiff d;
        SessionManifest host;
        std::string address;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            d = snap_.session_diff;
            host = snap_.session_manifest;
            address = snap_.session_address;
        }
        std::string e;
        bool ok = d.session_ready && store_->begin_session(d.session_profile, address, e);
        Profile p;
        if (ok) store_->load_profile("session", p, e);
        SessionDiff again = ok ? diff_session(*store_, p, host) : d;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ok) snap_.error = e.empty() ? "Todavía faltan mods: no se puede preparar el perfil de la sala." : e;
        else snap_.notice = "Perfil de la sala activo. Tu perfil habitual vuelve al salir de la sala o al cerrar la partida.";
        snap_.session_diff = std::move(again);
        reload_locked();
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