// ReleaseFeed y UpdateDownloader: qué versión hay publicada y descarga verificada a staging.
#include <chrono>
#include <fstream>
#include <regex>
#include <sstream>
#include <thread>

#include "wl/update.h"

namespace wl::upd {

namespace {

std::string base_url(const FeedConfig& f) {
    if (!f.feed_base.empty()) return f.feed_base.back() == '/' ? f.feed_base : f.feed_base + "/";
    return "https://github.com/" + f.repo + "/releases/";
}

std::string latest_manifest_url(const FeedConfig& f) {
    return f.feed_base.empty() ? base_url(f) + "latest/download/update.txt" : base_url(f) + "latest/update.txt";
}

std::string releases_api_url(const FeedConfig& f) {
    return f.feed_base.empty() ? "https://api.github.com/repos/" + f.repo + "/releases?per_page=10" : base_url(f) + "releases.json";
}

int retry_after(const HttpResponse& r) {
    std::string ra = r.header("retry-after");
    if (!ra.empty() && ra.size() < 7 && ra.find_first_not_of("0123456789") == std::string::npos) return std::stoi(ra);
    std::string reset = r.header("x-ratelimit-reset");
    if (!reset.empty() && reset.size() < 12 && reset.find_first_not_of("0123456789") == std::string::npos) {
        long long now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        long long wait = std::stoll(reset) - now;
        if (wait > 0) return (int)std::min<long long>(wait, 24 * 3600);
    }
    return 0;
}

// Traduce un fallo HTTP a CheckResult. 403 de GitHub con cuota agotada = límite.
CheckResult http_failure(const HttpResponse& r, const char* what) {
    CheckResult c;
    c.http_status = r.status;
    if (r.status == 429 || (r.status == 403 && (r.header("x-ratelimit-remaining") == "0" || !r.header("retry-after").empty()))) {
        c.kind = CheckKind::RateLimited;
        c.retry_after_s = std::max(retry_after(r), 15 * 60);
        c.error = "GitHub limita las consultas ahora mismo. Se volverá a intentar más tarde; puedes seguir jugando.";
        return c;
    }
    c.kind = CheckKind::Error;
    if (r.status == 0) c.error = r.error.empty() ? "Error de red." : r.error;
    else if (r.status == 404) c.error = std::string("No se encontró ") + what + " (404).";
    else if (r.status == 403) c.error = std::string("Acceso denegado a ") + what + " (403).";
    else c.error = std::string("El servidor respondió ") + std::to_string(r.status) + " al pedir " + what + ".";
    return c;
}

// La versión que dice el texto (sin verificar aún) para pedir la firma de esa misma versión.
std::string unverified_version(const std::string& text) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("version", 0) == 0) {
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string v = line.substr(eq + 1);
            while (!v.empty() && (v[0] == ' ' || v[0] == '\t')) v.erase(0, 1);
            while (!v.empty() && (v.back() == ' ' || v.back() == '\r' || v.back() == '\t')) v.pop_back();
            SemVer s;
            return parse_semver(v, s) ? v : std::string();
        }
    }
    return {};
}

std::string staged_name(const UpdateFile& f) { return f.asset; }

}  // namespace

std::string asset_url(const FeedConfig& feed, const std::string& version, const std::string& asset) {
    return base_url(feed) + "download/v" + version + "/" + asset;
}

CheckResult check_for_update(HttpClient& http, const FeedConfig& feed, const VerifyPolicy& policy) {
    CheckResult result;
    result.beta_etag = feed.beta_etag;
    result.beta_tag = feed.beta_tag;
    if (feed.repo.empty() && feed.feed_base.empty()) {
        result.error = "Esta copia no tiene configurado el repositorio de actualizaciones.";
        return result;
    }
    std::string text;
    if (feed.channel == "beta") {
        // Beta (opt-in): lista de releases de la API con ETag. Un 304 no gasta cuota.
        HttpRequest req;
        req.url = releases_api_url(feed);
        req.max_bytes = 512 * 1024;
        req.headers = {{"Accept", "application/vnd.github+json"}, {"X-GitHub-Api-Version", "2022-11-28"}};
        if (!feed.beta_etag.empty() && !feed.beta_tag.empty()) req.headers.push_back({"If-None-Match", feed.beta_etag});
        HttpResponse r = fetch(http, req);
        std::string tag = feed.beta_tag;
        if (r.status == 200) {
            static const std::regex re("\"tag_name\"\\s*:\\s*\"(v[0-9A-Za-z.\\-]{1,40})\"");
            std::string best;
            for (auto it = std::sregex_iterator(r.body.begin(), r.body.end(), re); it != std::sregex_iterator(); ++it) {
                std::string t = (*it)[1];
                SemVer s;
                if (parse_semver(t, s) && (best.empty() || compare_versions(t, best) > 0)) best = t;
            }
            if (best.empty()) {
                result.kind = CheckKind::NoReleases;
                result.error = "Todavía no hay versiones publicadas.";
                return result;
            }
            tag = best;
            result.beta_tag = best;
            result.beta_etag = r.header("etag");
        } else if (r.status != 304) {
            return http_failure(r, "la lista de versiones");
        }
        if (compare_versions(tag, policy.installed) <= 0) {
            result.kind = CheckKind::UpToDate;
            return result;
        }
        HttpRequest mr;
        mr.url = asset_url(feed, tag.substr(1), "update.txt");
        HttpResponse m = fetch(http, mr);
        if (m.status != 200) return http_failure(m, "el manifiesto de la versión");
        text = m.body;
    } else {
        HttpRequest req;
        req.url = latest_manifest_url(feed);
        HttpResponse r = fetch(http, req);
        if (r.status == 404) {
            result.kind = CheckKind::NoReleases;
            result.http_status = 404;
            result.error = "Todavía no hay versiones publicadas.";
            return result;
        }
        if (r.status != 200) return http_failure(r, "el manifiesto de la última versión");
        text = r.body;
    }

    std::string version = unverified_version(text);
    if (version.empty()) {
        result.error = "El manifiesto publicado no es válido.";
        return result;
    }
    if (compare_versions(version, policy.installed) <= 0) {  // nada nuevo: no hace falta ni pedir la firma
        result.kind = CheckKind::UpToDate;
        return result;
    }
    HttpRequest sr;
    sr.url = asset_url(feed, version, "update.txt.sig");
    sr.max_bytes = 1024;
    HttpResponse s = fetch(http, sr);
    if (s.status != 200) return http_failure(s, "la firma de la versión");
    std::string sig = s.body;
    while (!sig.empty() && (sig.back() == '\n' || sig.back() == '\r' || sig.back() == ' ')) sig.pop_back();

    VerifyResult v = verify_manifest(text, sig, policy);
    if (!v.ok) {
        result.kind = CheckKind::Error;
        result.error = v.error;
        log("manifiesto rechazado: " + v.error);
        return result;
    }
    result.kind = CheckKind::Available;
    result.manifest = v.manifest;
    result.manifest_text = text;
    result.signature = sig;
    return result;
}

bool download_update(HttpClient& http, const FeedConfig& feed, const CheckResult& check, const fs::path& install_dir,
                     const fs::path& staging, DownloadProgress& progress, std::string& error) {
    std::error_code e;
    fs::create_directories(staging / "files", e);
    if (e) {
        error = "No se pudo crear la carpeta temporal de la actualización.";
        return false;
    }
    // Primero el manifiesto firmado: el aplicador vuelve a verificarlo todo antes de tocar nada.
    {
        std::ofstream(staging / "update.txt", std::ios::binary | std::ios::trunc) << check.manifest_text;
        std::ofstream(staging / "update.txt.sig", std::ios::binary | std::ios::trunc) << check.signature;
    }
    std::vector<const UpdateFile*> todo;
    uint64_t total = 0;
    for (const auto& f : check.manifest.files) {
        fs::path installed = install_dir / fs::u8path(f.path);
        fs::path staged = staging / "files" / fs::u8path(staged_name(f));
        if (fs::is_regular_file(staged, e) && fs::file_size(staged, e) == f.size && sha256_file_hex(staged) == f.sha256) continue;  // ya bajado
        if (fs::is_regular_file(installed, e) && fs::file_size(installed, e) == f.size && sha256_file_hex(installed) == f.sha256) continue;  // no cambió
        todo.push_back(&f);
        total += f.size;
    }
    progress.total = total;
    progress.done = 0;
    log("descarga de " + check.manifest.version + ": " + std::to_string(todo.size()) + " archivos, " + std::to_string(total) + " bytes");
    for (const UpdateFile* f : todo) {
        fs::path final_path = staging / "files" / fs::u8path(staged_name(*f));
        fs::path part = final_path;
        part += ".part";
        uint64_t base = progress.done;
        bool ok = false;
        std::string last;
        const int waits[] = {1, 4, 10};
        for (int attempt = 0; attempt < 3 && !ok; ++attempt) {
            if (progress.cancel) break;
            if (attempt) std::this_thread::sleep_for(std::chrono::seconds(waits[attempt - 1]));
            HttpRequest req;
            req.url = asset_url(feed, check.manifest.version, f->asset);
            req.max_bytes = f->size;  // ni un byte más de lo firmado
            req.to_file = part;
            req.progress = [&](uint64_t got, uint64_t) {
                progress.done = base + std::min(got, f->size);
                return !progress.cancel.load();
            };
            HttpResponse r = fetch(http, req);
            if (r.cancelled || progress.cancel) break;
            if (r.status != 200) {
                last = r.status ? "El servidor respondió " + std::to_string(r.status) + " al descargar " + f->path + "." : r.error;
                fs::remove(part, e);
                if (r.status == 404 || r.status == 403) break;  // no se arregla reintentando
                continue;
            }
            if (fs::file_size(part, e) != f->size || sha256_file_hex(part) != f->sha256) {
                last = "El archivo " + f->path + " no coincide con la firma (corrupto o alterado). Se descarta.";
                log("hash incorrecto en " + f->path);
                fs::remove(part, e);
                continue;
            }
            fs::rename(part, final_path, e);
            ok = !e;
            if (e) last = "No se pudo guardar " + f->path + ".";
        }
        if (progress.cancel) {
            error = "Descarga cancelada. No se ha cambiado nada.";
            fs::remove(part, e);
            return false;
        }
        if (!ok) {
            error = last.empty() ? "No se pudo descargar " + f->path + "." : last;
            log("descarga fallida: " + error);
            return false;
        }
        progress.done = base + f->size;
    }
    return true;
}

VerifyResult verify_staging(const fs::path& staging, const VerifyPolicy& policy, std::string& error) {
    auto slurp = [](const fs::path& p, size_t max) {
        std::ifstream in(p, std::ios::binary);
        std::string s;
        if (!in) return s;
        s.assign(std::istreambuf_iterator<char>(in), {});
        if (s.size() > max) s.clear();
        return s;
    };
    std::string text = slurp(staging / "update.txt", kMaxManifestBytes);
    std::string sig = slurp(staging / "update.txt.sig", 1024);
    while (!sig.empty() && (sig.back() == '\n' || sig.back() == '\r')) sig.pop_back();
    VerifyResult v = verify_manifest(text, sig, policy);
    if (!v.ok) {
        error = v.error;
        return v;
    }
    v.ok = false;
    std::error_code e;
    for (const auto& f : v.manifest.files) {
        fs::path staged = staging / "files" / fs::u8path(staged_name(f));
        if (!fs::is_regular_file(staged, e)) continue;  // no cambió: se queda el instalado (se comprueba al aplicar)
        if (fs::file_size(staged, e) != f.size || sha256_file_hex(staged) != f.sha256) {
            error = "El archivo preparado " + f.path + " no coincide con la firma.";
            return v;
        }
    }
    v.ok = true;
    return v;
}

}  // namespace wl::upd
