#include "update_service.h"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <map>
#include <mutex>
#include <thread>

#include "wl/update.h"

namespace wl {

namespace {
long long now_s() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string clock_text(long long t) {
    std::time_t tt = (std::time_t)t;
    std::tm tmv{};
    localtime_s(&tmv, &tt);
    char buf[32];
    std::strftime(buf, sizeof buf, "%d/%m %H:%M", &tmv);
    return buf;
}

constexpr long long kAutoInterval = 6 * 3600;
constexpr long long kManualGap = 30;
}  // namespace

struct UpdateService::Shared {
    mutable std::mutex m;
    UpdateSnapshot snap;
    upd::CheckResult check;              // última versión disponible verificada
    std::map<std::string, std::string> pending_prefs;  // lo escribe pump() en el hilo de la UI
    upd::FeedConfig feed;
    upd::VerifyPolicy policy;
    upd::DownloadProgress progress;
    std::filesystem::path staging;
    bool busy = false;
    std::function<void()> wake;
};

UpdateService::UpdateService(Prefs& prefs, std::filesystem::path install_dir, std::function<void()> wake)
    : s_(std::make_shared<Shared>()), prefs_(prefs), install_dir_(std::move(install_dir)) {
    s_->wake = std::move(wake);
    auto& snap = s_->snap;
    snap.installed = upd::current_version();
    snap.channel = prefs_.get("update.channel", "stable") == "beta" ? "beta" : "stable";
    snap.auto_check = prefs_.get("update.auto", "1") == "1";
    long long last = std::atoll(prefs_.get("update.last_check", "0").c_str());
    if (last > 0) snap.last_check = clock_text(last);
    s_->feed.repo = upd::update_repo();
    s_->feed.channel = snap.channel;
    s_->feed.beta_etag = prefs_.get("update.beta_etag");
    s_->feed.beta_tag = prefs_.get("update.beta_tag");
    if (upd::testing_build())
        if (const char* f = std::getenv("WL_UPDATE_FEED")) s_->feed.feed_base = f;
    s_->policy.public_key = upd::public_key_hex();
    s_->policy.installed = snap.installed;
    s_->policy.channel = snap.channel;
    s_->policy.highest_seen = prefs_.get("update.highest_seen");
    snap.enabled = (!s_->feed.repo.empty() || !s_->feed.feed_base.empty()) && s_->policy.public_key.size() == 128;
    if (!snap.enabled) snap.info = "Esta copia del launcher no tiene configuradas las actualizaciones.";
}

UpdateService::~UpdateService() { s_->progress.cancel = true; }  // los hilos se desprenden; no se espera a la red al salir

void UpdateService::pump() {
    std::map<std::string, std::string> todo;
    {
        std::lock_guard<std::mutex> lock(s_->m);
        todo.swap(s_->pending_prefs);
    }
    for (const auto& [k, v] : todo) prefs_.set(k, v);
}

void UpdateService::start_auto() {
    if (!s_->snap.enabled || !s_->snap.auto_check) return;
    long long now = now_s();
    long long last = std::atoll(prefs_.get("update.last_check", "0").c_str());
    long long next = std::atoll(prefs_.get("update.next_allowed", "0").c_str());
    long long interval = kAutoInterval;
    if (upd::testing_build())
        if (const char* i = std::getenv("WL_UPDATE_INTERVAL_S")) interval = std::atoll(i);
    if (now < next || (last > 0 && now - last < interval && now >= last)) {
        upd::log("comprobación automática omitida (hecha hace poco o límite de GitHub)");
        return;
    }
    run_check(false);
}

void UpdateService::check_now() {
    long long now = now_s();
    if (now - last_manual_ < kManualGap) {
        std::lock_guard<std::mutex> lock(s_->m);
        s_->snap.info = "Ya se ha comprobado hace un momento.";
        return;
    }
    long long next = std::atoll(prefs_.get("update.next_allowed", "0").c_str());
    if (now < next) {
        std::lock_guard<std::mutex> lock(s_->m);
        s_->snap.state = UpdState::Error;
        s_->snap.quiet_error = false;
        s_->snap.error = "GitHub limita las consultas. Vuelve a intentarlo después de las " + clock_text(next) + ".";
        return;
    }
    last_manual_ = now;
    run_check(true);
}

void UpdateService::run_check(bool manual) {
    auto s = s_;
    {
        std::lock_guard<std::mutex> lock(s->m);
        if (s->busy || !s->snap.enabled) return;
        s->busy = true;
        s->snap.state = UpdState::Checking;
        s->snap.error.clear();
        s->snap.info.clear();
    }
    upd::FeedConfig feed = s->feed;
    upd::VerifyPolicy policy = s->policy;
    std::filesystem::path install = install_dir_;
    std::thread([s, feed, policy, manual, install] {
        auto t0 = std::chrono::steady_clock::now();
        auto http = upd::make_winhttp_client();
        upd::CheckResult c = upd::check_for_update(*http, feed, policy);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        upd::log("comprobación " + std::string(manual ? "manual" : "automática") + " (" + feed.channel + "): " +
                 std::to_string((int)c.kind) + " en " + std::to_string((int)ms) + " ms" + (c.error.empty() ? "" : " - " + c.error));
        uint64_t to_download = 0;
        if (c.kind == upd::CheckKind::Available) {
            std::error_code e;
            for (const auto& f : c.manifest.files) {
                auto p = install / std::filesystem::u8path(f.path);
                if (!(std::filesystem::file_size(p, e) == f.size && !e && upd::sha256_file_hex(p) == f.sha256)) to_download += f.size;
            }
        }
        {
            std::lock_guard<std::mutex> lock(s->m);
            s->busy = false;
            long long now = now_s();
            auto& snap = s->snap;
            if (c.kind != upd::CheckKind::Error) {
                s->pending_prefs["update.last_check"] = std::to_string(now);
                snap.last_check = clock_text(now);
            }
            if (feed.channel == "beta") {
                s->pending_prefs["update.beta_etag"] = c.beta_etag;
                s->pending_prefs["update.beta_tag"] = c.beta_tag;
                s->feed.beta_etag = c.beta_etag;
                s->feed.beta_tag = c.beta_tag;
            }
            switch (c.kind) {
                case upd::CheckKind::Available:
                    snap.state = UpdState::Available;
                    snap.available = c.manifest.version;
                    snap.published = c.manifest.published.substr(0, 10);
                    snap.notes = c.manifest.notes;
                    snap.update_size = c.manifest.total_size();
                    snap.download_size = to_download;
                    s->check = c;
                    if (upd::compare_versions(c.manifest.version, s->policy.highest_seen) > 0) {
                        s->policy.highest_seen = c.manifest.version;
                        s->pending_prefs["update.highest_seen"] = c.manifest.version;
                    }
                    break;
                case upd::CheckKind::UpToDate:
                case upd::CheckKind::NoReleases:
                    snap.state = UpdState::UpToDate;
                    snap.info = c.kind == upd::CheckKind::NoReleases ? "Todavía no hay versiones publicadas." : "";
                    break;
                case upd::CheckKind::RateLimited:
                    s->pending_prefs["update.next_allowed"] = std::to_string(now + c.retry_after_s);
                    [[fallthrough]];
                default:
                    snap.state = UpdState::Error;
                    snap.error = c.error;
                    snap.quiet_error = !manual;
                    break;
            }
        }
        s->wake();
    }).detach();
}

void UpdateService::download() {
    auto s = s_;
    std::filesystem::path install = install_dir_;
    {
        std::lock_guard<std::mutex> lock(s->m);
        if (s->busy || s->snap.state != UpdState::Available) return;
        if (!upd::folder_writable(install)) {
            s->snap.state = UpdState::Error;
            s->snap.quiet_error = false;
            s->snap.error = "Esta carpeta del launcher es de solo lectura (p. ej. Archivos de programa). Mueve la carpeta a tu "
                            "Escritorio o Documentos y vuelve a intentarlo. No se pide permiso de administrador.";
            return;
        }
        s->busy = true;
        s->progress.cancel = false;
        s->progress.done = 0;
        s->progress.total = s->snap.download_size;
        s->snap.state = UpdState::Downloading;
        s->snap.error.clear();
    }
    upd::CheckResult check = s->check;
    upd::FeedConfig feed = s->feed;
    upd::VerifyPolicy policy = s->policy;
    std::thread([s, check, feed, policy, install] {
        std::string err;
        bool ok = false;
        std::filesystem::path staging = upd::updates_dir() / check.manifest.version;
        upd::UpdateLock lock;
        if (!lock.acquire(0)) {
            err = "Otra ventana del launcher está instalando una actualización.";
        } else {
            auto http = upd::make_winhttp_client();
            ok = upd::download_update(*http, feed, check, install, staging, s->progress, err);
            if (ok) {
                {
                    std::lock_guard<std::mutex> l(s->m);
                    s->snap.state = UpdState::Verifying;
                }
                s->wake();
                ok = upd::verify_staging(staging, policy, err).ok;
            }
        }
        {
            std::lock_guard<std::mutex> l(s->m);
            s->busy = false;
            if (ok) {
                s->snap.state = UpdState::Ready;
                s->staging = staging;
            } else if (s->progress.cancel) {
                s->snap.state = UpdState::Available;
                s->snap.info = "Descarga cancelada. No se ha cambiado nada.";
            } else {
                s->snap.state = UpdState::Error;
                s->snap.quiet_error = false;
                s->snap.error = err;
            }
        }
        upd::log(ok ? "descarga verificada: " + check.manifest.version : "descarga no completada: " + err);
        s->wake();
    }).detach();
}

void UpdateService::cancel() { s_->progress.cancel = true; }

bool UpdateService::apply(bool game_running, std::string& why_not) {
    std::filesystem::path staging;
    {
        std::lock_guard<std::mutex> lock(s_->m);
        if (s_->snap.state != UpdState::Ready) return false;
        staging = s_->staging;
    }
    if (game_running) {
        why_not = "Hay un juego abierto desde el launcher. Ciérralo y vuelve a pulsar; la actualización ya está descargada.";
        upd::log("instalación pospuesta: juego en marcha");
        return false;
    }
    if (upd::other_launcher_instances(install_dir_ / "wii-launcher.exe", 0) > 0) {
        why_not = "Hay otra ventana del launcher abierta. Ciérrala para poder actualizar.";
        upd::log("instalación pospuesta: otra ventana del launcher abierta");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(s_->m);
        s_->snap.state = UpdState::Installing;
    }
    prefs_.set("update.from_version", upd::current_version());
    if (!upd::start_apply(staging, install_dir_, why_not)) {
        std::lock_guard<std::mutex> lock(s_->m);
        s_->snap.state = UpdState::Error;
        s_->snap.quiet_error = false;
        s_->snap.error = why_not;
        return false;
    }
    return true;
}

void UpdateService::set_channel(bool beta) {
    std::string ch = beta ? "beta" : "stable";
    prefs_.set("update.channel", ch);
    std::lock_guard<std::mutex> lock(s_->m);
    s_->snap.channel = ch;
    s_->feed.channel = ch;
    s_->policy.channel = ch;
    if (!beta && (s_->snap.state == UpdState::Available || s_->snap.state == UpdState::Ready)) {
        upd::SemVer v;
        if (upd::parse_semver(s_->snap.available, v) && !v.stable()) s_->snap.state = UpdState::Idle;  // una beta ofrecida deja de valer
    }
}

void UpdateService::set_auto(bool on) {
    prefs_.set("update.auto", on ? "1" : "0");
    std::lock_guard<std::mutex> lock(s_->m);
    s_->snap.auto_check = on;
}

UpdateSnapshot UpdateService::snapshot() const {
    std::lock_guard<std::mutex> lock(s_->m);
    UpdateSnapshot snap = s_->snap;
    snap.done = s_->progress.done;
    snap.total = s_->progress.total;
    return snap;
}

}  // namespace wl
