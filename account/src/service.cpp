#include "wl/account/service.h"

#include <algorithm>
#include <utility>
#include "wl/mods/launch.h"
#include "wl/mods/safe_path.h"
#include "wl/account/queue.h"

namespace wl::account {

AccountService::AccountService(Config config, const fs::path& data_root, std::function<void()> wake)
    : http_(upd::make_winhttp_client()), client_(*http_, config), cloud_(*http_, config),
      data_root_(data_root / upd::sha256_hex(config.origin).substr(0, 24)),
      vault_(data_root_ / "session.dat", config.origin), wake_(std::move(wake)) {
    view_.configured = client_.configured();
    view_.identity_ready = !view_.configured;
    if (view_.configured) {
        view_.busy = true;
        job_ = [this] { resume(); };
        worker_ = std::thread([this] { run(); });
    }
}

AccountService::~AccountService() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
        job_ = {};
    }
    changed_.notify_one();
    if (worker_.joinable()) worker_.join();
}

AccountView AccountService::view() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return view_;
}

bool AccountService::post(std::function<void()> job, bool lock_save) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!view_.configured || view_.busy || stop_) return false;
        view_.busy = true;
        view_.message.clear();
        view_.warning = false;
        view_.save_locked = lock_save;
        job_ = std::move(job);
    }
    changed_.notify_one();
    return true;
}

void AccountService::run() {
    for (;;) {
        std::function<void()> work;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            const auto ready = [this] { return stop_ || static_cast<bool>(job_); };
            const auto due = std::min(next_refresh_, next_save_retry_);
            if (due == Clock::time_point::max()) changed_.wait(lock, ready);
            else changed_.wait_until(lock, due, ready);
            if (stop_) return;
            work = std::exchange(job_, {});
            view_.busy = true;
        }
        if (wake_) wake_();
        if (work) work();
        else if (next_refresh_ <= Clock::now()) renew();
        else retry_save_uploads();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            view_.busy = false;
            view_.save_locked = false;
        }
        if (wake_) wake_();
    }
}

void AccountService::feedback(std::string message, bool warning) {
    std::lock_guard<std::mutex> lock(mutex_);
    view_.message = std::move(message);
    view_.warning = warning;
}

void AccountService::account(bool signed_in, std::string alias) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!signed_in || view_.local_owner != session_.user_id) {
        view_.history_location.clear();
        view_.save_history.clear();
    }
    view_.signed_in = signed_in;
    view_.local_owner = signed_in ? session_.user_id : std::string{};
    view_.alias = std::move(alias);
}

void AccountService::resume() {
    std::string refresh, error, owner;
    const bool read = vault_.load(refresh, error, &owner);
    if (read && !owner.empty()) {
        session_.user_id = std::move(owner);
        account(true);
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        view_.identity_ready = true;
    }
    if (wake_) wake_();
    if (!read) {
        feedback(error, true);
        return;
    }
    if (refresh.empty()) return;
    session_.refresh_token = std::move(refresh);
    renew();
}

void AccountService::renew() {
    auto result = client_.refresh(session_.refresh_token);
    if (result.ok()) {
        if (!session_.user_id.empty() && result.session.user_id != session_.user_id) {
            next_refresh_ = Clock::time_point::max();
            session_ = {};
            account(false);
            std::string error;
            if (!vault_.clear(error)) feedback(error, true);
            else feedback("La sesión cambió de cuenta. Vuelve a iniciar sesión.", true);
            return;
        }
        accept(std::move(result));
        return;
    }
    if (result.state == AuthState::InvalidSession) {
        next_refresh_ = Clock::time_point::max();
        session_ = {};
        account(false);
        std::string error;
        if (!vault_.clear(error)) feedback(error, true);
        else feedback(result.message, true);
        return;
    }
    next_refresh_ = Clock::now() + backoff_;
    backoff_ = std::min(backoff_ * 2, std::chrono::seconds(120));
    feedback(result.message, true);
}

void AccountService::accept(AuthResult result) {
    if (!result.ok()) {
        feedback(result.message, result.state != AuthState::ConfirmEmail);
        return;
    }
    const bool same_user = session_.user_id == result.session.user_id;
    session_ = std::move(result.session);
    const auto delay = std::chrono::duration_cast<std::chrono::seconds>(
        session_.expires_at - std::chrono::system_clock::now());
    next_refresh_ = Clock::now() + std::max(delay - std::chrono::seconds(60), std::chrono::seconds(1));
    if (!save_retries_.empty()) next_save_retry_ = Clock::now() + std::chrono::seconds(1);
    backoff_ = std::chrono::seconds(5);
    std::string error;
    const bool persisted = vault_.save(session_.refresh_token, error, session_.user_id);
    const auto current = client_.profile(session_);
    const auto previous = view();
    account(true, current.ok() ? current.alias : same_user ? previous.alias : std::string{});
    if (!persisted) feedback(error, true);
    else feedback(current.message, !current.ok());
}

void AccountService::sign_in(std::string email, std::string password, bool register_account) {
    post([this, email = std::move(email), password = std::move(password), register_account] {
        accept(register_account ? client_.sign_up(email, password) : client_.sign_in(email, password));
    });
}

void AccountService::sign_out() {
    post([this] {
        next_refresh_ = Clock::time_point::max();
        next_save_retry_ = Clock::time_point::max();
        save_retries_.clear();
        const auto access = std::move(session_.access_token);
        session_ = {};
        account(false);
        std::string error;
        const bool cleared = vault_.clear(error);
        const auto result = access.empty() ? AuthResult{AuthState::Ok, {}, {}} : client_.sign_out(access);
        if (!cleared) feedback(error, true);
        else if (!result.ok()) feedback("Sesión cerrada en este equipo. Revocación remota pendiente.", true);
        else feedback("Sesión cerrada.", false);
    });
}

void AccountService::request_recovery(std::string email) {
    post([this, email = std::move(email)] {
        const auto result = client_.request_recovery(email);
        feedback(result.message, !result.ok());
    });
}

void AccountService::reset_password(std::string email, std::string code, std::string password) {
    post([this, email = std::move(email), code = std::move(code), password = std::move(password)] {
        const auto result = client_.reset_password(email, code, password);
        feedback(result.message, !result.ok());
    });
}

void AccountService::set_alias(std::string alias) {
    post([this, alias = std::move(alias)] {
        const auto result = client_.set_alias(session_, alias);
        if (result.ok()) account(true, result.alias);
        feedback(result.ok() ? "Alias guardado." : result.message, !result.ok());
    });
}


void AccountService::sync_mod_catalog(std::string game_id, fs::path install_root, bool upload) {
    post([this, game_id = std::move(game_id), install_root = std::move(install_root), upload] {
        if (!valid_user_id(session_.user_id)) return;
        mods::ModStore store(game_id, mods::default_mods_root(game_id), mods::extracted_dir(game_id, install_root));
        std::string error;
        if (!store.open(error)) {
            feedback("No se pudo verificar el almacén local de mods.", true);
            return;
        }
        const auto active = store.active_profile();
        if (active == "session") {
            feedback("Selecciona un perfil habitual de mods antes de sincronizar.", true);
            return;
        }
        if (upload) {
            mods::Json catalog;
            if (!mods::export_profile_catalog(store, active, catalog, error)) {
                feedback("El gestor no pudo exportar el perfil activo.", true);
                return;
            }
            const auto result = cloud_.push_mod_catalog(session_, catalog.dump(), 0);
            if (result.state == SaveState::Conflict)
                feedback("Ya existe una copia de ese perfil. Puedes compararla desde aquí.", true);
            else if (result.state == SaveState::Ok)
                feedback("Referencias de mods guardadas en tu cuenta.", false);
            else
                feedback("No se pudo guardar el perfil de mods.", true);
            return;
        }
        const auto remote = cloud_.mod_catalog(session_, game_id, active);
        if (remote.state == SaveState::Missing) {
            feedback("Todavía no hay una copia remota de ese perfil.", false);
        } else if (remote.state != SaveState::Ok) {
            feedback("No se pudo consultar el perfil de mods.", true);
        } else {
            mods::CatalogRestore plan;
            if (!mods::plan_catalog_restore(store, remote.catalog, plan, error)) {
                feedback("La copia remota requiere otro disco o cargador de mods.", true);
                return;
            }
            feedback("Copia remota · " + std::to_string(remote.catalog.mods.size()) +
                     " referencias · " + std::to_string(plan.downloads.size()) + " por descargar · " +
                     std::to_string(plan.manual.size()) + " por importar manualmente.", false);
        }
    });
}


std::optional<SaveLocation> AccountService::local_save(const std::string& game, const fs::path& install_root,
                                                       const InstallStatus& status) const {
    return save_location(data_root_, view().local_owner, game, install_root, status);
}

void AccountService::enable_save(SaveLocation location) {
    post([this, location = std::move(location)] {
        if (location.owner != session_.user_id) return;
        if (game_process_running(location.target.game_id)) {
            feedback("Cierra el juego antes de activar un guardado de cuenta.", true);
            return;
        }
        const bool enabled = enable_save_location(location);
        feedback(enabled ? "Guardado de cuenta activado para el juego sin conexión." : "No se pudo crear el guardado separado.", !enabled);
    }, true);
}

bool AccountService::synchronize_save(SaveLocation location) {
    return post([this, location = std::move(location)] {
        if (location.owner != session_.user_id || !save_location_enabled(location)) return;
        std::string error;
        SaveBaseline baseline;
        if (!read_save_baseline(location, baseline)) {
            feedback("La referencia del guardado local está dañada.", true);
            return;
        }
        SaveBundle snapshot;
        const auto running = [&] { return game_process_running(location.target.game_id); };
        if (!snapshot_save(location.target, running, snapshot, error)) {
            feedback(error, true);
            return;
        }
        SaveQueue queue(data_root_ / "queue" / location.target.game_id / location.target.slot_id, location.owner);
        if (snapshot.sha256 != baseline.sha256) {
            const auto id = new_id();
            const auto device = load_device_id(data_root_);
            const auto& target = location.target;
            const SaveRevision revision{id, baseline.revision, device, target.game_id, target.slot_id,
                                        target.compatibility, snapshot.sha256, snapshot.bytes.size()};
            if (!queue.enqueue(revision, snapshot, error)) {
                feedback(error, true);
                return;
            }
        }
        upload_save_queue(location);
    });
}

void AccountService::defer_save_upload(const SaveLocation& location) {
    save_retries_.insert_or_assign(location.work, location);
    next_save_retry_ = std::min(next_save_retry_, Clock::now() + std::chrono::seconds(20));
}

bool AccountService::resume_save_queue(SaveLocation location) {
    const auto dir = data_root_ / "queue" / location.target.game_id / location.target.slot_id / location.owner;
    std::error_code ec;
    if (!fs::exists(dir, ec)) return !ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() == ".pending")
            return post([this, location = std::move(location)] { upload_save_queue(location); });
    }
    return !ec;
}

void AccountService::retry_save_uploads() {
    next_save_retry_ = Clock::time_point::max();
    auto retry = std::exchange(save_retries_, {});
    for (const auto& [_, location] : retry) upload_save_queue(location);
}

void AccountService::upload_save_queue(const SaveLocation& location) {
    if (location.owner != session_.user_id || !save_location_enabled(location)) return;
    if (session_.access_token.empty() || session_.expires_at <= std::chrono::system_clock::now()) {
        defer_save_upload(location);
        feedback("Copia local pendiente. Se reintentará con conexión.", false);
        return;
    }
    std::string error;
    SaveQueue queue(data_root_ / "queue" / location.target.game_id / location.target.slot_id, location.owner);
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto result = queue.process_next(cloud_, session_, now, error);
    switch (result.state) {
    case QueueState::Uploaded: {
        const auto head = cloud_.head(session_, location.target);
        if (head.state != SaveState::Ok) {
            defer_save_upload(location);
            feedback("Se subió la revisión. Queda por confirmar el avance en este equipo.", true);
        } else if (head.revision_id != result.revision_id) {
            queue.acknowledge(result.revision_id, error);
            feedback("La copia se conservó. Hay una revisión remota posterior.", true);
        } else if (!write_save_baseline(location, {result.revision_id, result.sha256}) ||
                   !queue.acknowledge(result.revision_id, error)) {
            feedback("Se subió la revisión. Queda por confirmar el avance en este equipo.", true);
        } else {
            feedback("Guardado sincronizado.", false);
        }
        break;
    }
    case QueueState::Empty:
        feedback("El guardado local coincide con la última revisión de este equipo.", false);
        break;
    case QueueState::Conflict:
        feedback("Hay dos avances distintos. Ambas copias se han conservado.", true);
        break;
    case QueueState::Deferred:
        defer_save_upload(location);
        feedback("Copia pendiente. Se reintentará con conexión.", false);
        break;
    case QueueState::Rejected:
        feedback(error.empty() ? "Supabase rechazó la copia. El guardado local está disponible." : error, true);
        break;
    }
}

void AccountService::list_cloud_saves(SaveLocation location) {
    post([this, location = std::move(location)] {
        if (location.owner != session_.user_id || !save_location_enabled(location)) return;
        const auto result = cloud_.history(session_, location.target);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            view_.history_location = location.work;
            view_.save_history = result.revisions;
        }
        const bool ok = result.state == SaveState::Ok;
        feedback(ok ? (result.revisions.empty() ? "Todavía no hay revisiones compatibles." : "Elige una revisión para restaurarla.") :
                      "No se pudo consultar el historial remoto.", !ok);
    });
}

void AccountService::export_local_save(SaveLocation location) {
    post([this, location = std::move(location)] {
        if (location.owner != session_.user_id || !save_location_enabled(location)) return;
        SaveBundle bundle;
        std::string error;
        if (!snapshot_save(location.target, [&] { return game_process_running(location.target.game_id); }, bundle, error)) {
            feedback(error, true);
            return;
        }
        const auto file = export_save_bundle(location, bundle);
        feedback(file.empty() ? "No se pudo exportar la copia." : "Copia exportada en " + mods::to_utf8(file), file.empty());
    });
}

void AccountService::restore_cloud_save(SaveLocation location, std::string revision_id) {
    post([this, location = std::move(location), revision = std::move(revision_id)] {
        if (location.owner != session_.user_id || !save_location_enabled(location)) return;
        if (session_.access_token.empty() || session_.expires_at <= std::chrono::system_clock::now()) {
            feedback("Se necesita conexión para descargar el guardado.", true);
            return;
        }
        const auto running = [&] { return game_process_running(location.target.game_id); };
        if (running()) {
            feedback("Cierra el juego antes de restaurar el guardado.", true);
            return;
        }
        std::string chosen = revision;
        if (chosen.empty()) {
            const auto head = cloud_.head(session_, location.target);
            if (head.state != SaveState::Ok) {
                feedback(head.state == SaveState::Missing ? "Todavía no hay un guardado remoto." : "No se pudo consultar el guardado remoto.",
                         head.state != SaveState::Missing);
                return;
            }
            chosen = head.revision_id;
        }
        SaveBundle bundle;
        if (cloud_.pull(session_, location.target, chosen, bundle) != SaveState::Ok) {
            feedback("No se pudo verificar la copia remota para esta versión del juego.", true);
            return;
        }
        std::string error;
        if (!restore_save(location.target, running, bundle, error)) {
            feedback(error, true);
            return;
        }
        const bool saved = write_save_baseline(location, {chosen, bundle.sha256});
        feedback(saved ? "Guardado restaurado. Se ha conservado la copia anterior." :
                         "Guardado restaurado. No se pudo registrar su revisión en este equipo.", !saved);
    }, true);
}

}  // namespace wl::account
