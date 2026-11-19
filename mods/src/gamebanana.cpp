#include "wl/mods/gamebanana.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#endif

#include "wl/mods/json.h"
#include "wl/mods/safe_path.h"
#include "wl/mods/store.h"

namespace wl::mods {

using upd::HttpRequest;
using upd::HttpResponse;

namespace {

const char* kUserAgent = "wii-launcher-mods/1 (+https://github.com/hernandezmarkel/wii-launcher)";

JsonLimits provider_limits() {
    JsonLimits l;
    l.max_bytes = 4 << 20;
    l.max_depth = 32;
    l.max_string = 512 * 1024;
    l.max_elements = 200000;
    l.allow_fractions = true;
    return l;
}

std::string url_encode(const std::string& s) {
    std::string out;
    char buf[4];
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += static_cast<char>(c);
        else { std::snprintf(buf, sizeof buf, "%%%02X", c); out += buf; }
    }
    return out;
}

bool host_of(const std::string& url, std::string& scheme, std::string& host, int& port) {
    size_t s = url.find("://");
    if (s == std::string::npos) return false;
    scheme = url.substr(0, s);
    size_t start = s + 3;
    size_t end = url.find_first_of("/?#", start);
    std::string authority = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (authority.find('@') != std::string::npos) return false;   // sin credenciales en la URL
    size_t colon = authority.rfind(':');
    port = scheme == "https" ? 443 : 80;
    if (colon != std::string::npos) {
        port = std::atoi(authority.substr(colon + 1).c_str());
        authority = authority.substr(0, colon);
    }
    host = authority;
    for (auto& c : host) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return !host.empty();
}

std::string cache_name(const std::string& url) { return upd::sha256_hex(url).substr(0, 32) + ".json"; }

CatalogItem item_from(const Json& r) {
    CatalogItem it;
    it.id = r["_idRow"].num();
    it.name = r["_sName"].str().substr(0, 200);
    it.category = r["_aRootCategory"]["_sName"].str().substr(0, 80);
    it.author = r["_aSubmitter"]["_sName"].str().substr(0, 80);
    it.version = r["_sVersion"].str().substr(0, 40);
    it.profile_url = r["_sProfileUrl"].str();
    if (!gamebanana_url_allowed(it.profile_url)) it.profile_url = "https://gamebanana.com/mods/" + std::to_string(it.id);
    it.updated = r["_tsDateModified"].num(r["_tsDateAdded"].num());
    it.views = r["_nViewCount"].num();
    return it;
}

}  // namespace

bool gamebanana_url_allowed(const std::string& url) {
    std::string scheme, host;
    int port = 0;
    if (!host_of(url, scheme, host, port)) return false;
    if (scheme != "https" || port != 443) return false;
    return host == "gamebanana.com" || host == "api.gamebanana.com" || host == "files.gamebanana.com";
}

int64_t gamebanana_game(const std::string& game_id) {
    if (game_id == "wiiparty") return 6370;
    if (game_id == "mp9") return 6248;
    if (game_id == "mkwii") return 5896;
    if (game_id == "brawl") return 5678;
    return 0;
}

std::string strip_html(const std::string& html, size_t max_chars) {
    std::string out;
    bool tag = false;
    for (size_t i = 0; i < html.size() && out.size() < max_chars; ++i) {
        char c = html[i];
        if (c == '<') {
            tag = true;
            if (html.compare(i, 3, "<br") == 0 || html.compare(i, 3, "<p>") == 0 || html.compare(i, 4, "</p>") == 0) out += '\n';
            continue;
        }
        if (c == '>') { tag = false; continue; }
        if (tag) continue;
        if (c == '&') {
            static const std::pair<const char*, char> ents[] = {{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&#39;", '\''}, {"&nbsp;", ' '}};
            bool done = false;
            for (const auto& [e, ch] : ents)
                if (html.compare(i, std::strlen(e), e) == 0) { out += ch; i += std::strlen(e) - 1; done = true; break; }
            if (done) continue;
        }
        if (static_cast<unsigned char>(c) < 0x20 && c != '\n') c = ' ';
        out += c;
    }
    // Espacios repetidos fuera.
    std::string clean;
    for (char c : out)
        if (!(c == ' ' && !clean.empty() && (clean.back() == ' ' || clean.back() == '\n'))) clean += c;
    while (!clean.empty() && std::isspace(static_cast<unsigned char>(clean.back()))) clean.pop_back();
    return clean;
}

std::string md5_file_hex(const fs::path& file) {
#ifdef _WIN32
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_MD5_ALGORITHM, nullptr, 0) != 0) return {};
    std::string result;
    if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
        std::ifstream in(file, std::ios::binary);
        std::vector<char> buf(1 << 16);
        bool ok = static_cast<bool>(in);
        while (ok && in) {
            in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            if (in.gcount() > 0) ok = BCryptHashData(h, reinterpret_cast<PUCHAR>(buf.data()), static_cast<ULONG>(in.gcount()), 0) == 0;
        }
        unsigned char digest[16];
        if (ok && BCryptFinishHash(h, digest, sizeof digest, 0) == 0) {
            static const char* hex = "0123456789abcdef";
            for (unsigned char b : digest) { result += hex[b >> 4]; result += hex[b & 15]; }
        }
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return result;
#else
    (void)file;
    return {};
#endif
}

GameBananaProvider::GameBananaProvider(upd::HttpClient& http, fs::path cache_dir) : http_(http), cache_(std::move(cache_dir)) {}

HttpResponse GameBananaProvider::get(HttpRequest req, std::string& error) {
    req.headers.push_back({"User-Agent", kUserAgent});
    // Ritmo educado: nunca más de ~3 peticiones por segundo; reintentos con espera ante 429 y 5xx.
    for (int attempt = 0;; ++attempt) {
        auto wait = last_ + min_interval_ - std::chrono::steady_clock::now();
        if (wait.count() > 0) std::this_thread::sleep_for(wait);
        last_ = std::chrono::steady_clock::now();
        HttpResponse r;
        std::string url = req.url;
        for (int hop = 0;; ++hop) {
            if (!gamebanana_url_allowed(req.url)) {
                error = hop ? "GameBanana redirigió a un sitio no permitido: se cancela la descarga." : "Dirección no permitida.";
                req.url = url;
                return HttpResponse{};
            }
            r = http_.get(req);
            bool redirect = r.status == 301 || r.status == 302 || r.status == 303 || r.status == 307 || r.status == 308;
            if (!redirect) break;
            std::string loc = r.header("location");
            if (loc.empty() || hop >= 4) { error = "Demasiadas redirecciones."; req.url = url; return HttpResponse{}; }
            if (loc[0] == '/') {
                std::string scheme, host;
                int port;
                host_of(req.url, scheme, host, port);
                loc = "https://" + host + loc;
            }
            req.url = loc;
        }
        req.url = url;
        if (r.cancelled) { error = "Cancelado."; return r; }
        if ((r.status == 429 || r.status >= 500 || r.status == 0) && attempt < 2) {
            std::this_thread::sleep_for(std::chrono::milliseconds(min_interval_.count() ? 1500 * (attempt + 1) : 0));
            continue;
        }
        if (r.status == 429) error = "GameBanana pide esperar (demasiadas peticiones). Prueba en un minuto.";
        else if (r.status == 0) error = "No se puede conectar con GameBanana (sin internet o tiempo agotado).";
        else if (r.status != 200) error = "GameBanana respondió con el código " + std::to_string(r.status) + ".";
        return r;
    }
}

bool GameBananaProvider::get_json(const std::string& url, std::string& body, std::string& error, bool use_cache, bool* from_cache) {
    fs::path cached = cache_ / cache_name(url);
    std::error_code ec;
    if (use_cache && fs::exists(cached, ec)) {
        auto age = fs::file_time_type::clock::now() - fs::last_write_time(cached, ec);
        if (!ec && age < std::chrono::minutes(10) && read_file_text(cached, body, 4 << 20)) {
            if (from_cache) *from_cache = true;
            return true;
        }
    }
    HttpRequest req;
    req.url = url;
    req.max_bytes = 4 << 20;
    req.headers.push_back({"Accept", "application/json"});
    HttpResponse r = get(req, error);
    if (r.status != 200) {
        // Sin red: si hay una copia antigua en caché, se usa avisando (modo sin conexión, E02/B05).
        if (use_cache && read_file_text(cached, body, 4 << 20)) {
            if (from_cache) *from_cache = true;
            return true;
        }
        if (error.empty()) error = r.error.empty() ? "Sin respuesta de GameBanana." : r.error;
        return false;
    }
    body = r.body;
    std::string e;
    write_file_atomic(cached, body, e);
    return true;
}

bool GameBananaProvider::list(int64_t game, int page, const std::string& search, CatalogPage& out, std::string& error) {
    out = CatalogPage{};
    if (game <= 0) { error = "Este juego no tiene catálogo en GameBanana."; return false; }
    page = std::clamp(page, 1, 500);
    std::string url = search.empty()
                          ? "https://gamebanana.com/apiv11/Mod/Index?_nPage=" + std::to_string(page) + "&_nPerpage=20&_aFilters%5BGeneric_Game%5D=" + std::to_string(game)
                          : "https://gamebanana.com/apiv11/Util/Search/Results?_sModelName=Mod&_sOrder=best_match&_idGameRow=" + std::to_string(game) +
                                "&_sSearchString=" + url_encode(search.substr(0, 60)) + "&_nPage=" + std::to_string(page);
    std::string body;
    if (!get_json(url, body, error, true, &out.from_cache)) return false;
    Json j;
    if (!parse_json(body, j, error, provider_limits())) { error = "Respuesta de GameBanana no válida (" + error + ")."; return false; }
    out.total = j["_aMetadata"]["_nRecordCount"].num();
    out.complete = j["_aMetadata"]["_bIsComplete"].boolean();
    for (const auto& r : j["_aRecords"].elements()) {
        if (r["_sModelName"].str("Mod") != "Mod") continue;
        if (r["_aGame"].is_object() && r["_aGame"]["_idRow"].num() != game) continue;   // nunca mods de otro juego
        CatalogItem it = item_from(r);
        if (it.id <= 0 || it.name.empty()) continue;   // B12: registros incompletos fuera
        out.items.push_back(std::move(it));
    }
    return true;
}

bool GameBananaProvider::detail(int64_t mod_id, CatalogDetail& out, std::string& error) {
    out = CatalogDetail{};
    std::string body;
    if (!get_json("https://gamebanana.com/apiv11/Mod/" + std::to_string(mod_id) + "/ProfilePage", body, error, true)) return false;
    Json j;
    if (!parse_json(body, j, error, provider_limits())) { error = "Ficha de GameBanana no válida (" + error + ")."; return false; }
    if (j["_idRow"].num() != mod_id) { error = "La ficha no corresponde al mod pedido."; return false; }
    out.item = item_from(j);
    out.license = strip_html(j["_sLicense"].str(), 600);
    out.description = strip_html(j["_sText"].str(j["_sDescription"].str()), 1200);
    out.withheld = j["_bIsWithheld"].boolean();
    out.trashed = j["_bIsTrashed"].boolean();
    out.is_private = j["_bIsPrivate"].boolean();
    for (const auto& f : j["_aFiles"].elements()) {
        CatalogFile cf;
        cf.id = f["_idRow"].num();
        cf.name = f["_sFile"].str().substr(0, 200);
        cf.size = static_cast<uint64_t>(std::max<int64_t>(0, f["_nFilesize"].num()));
        cf.md5 = f["_sMd5Checksum"].str();
        cf.av_result = f["_sAvResult"].str(f["_sAnalysisResult"].str());
        cf.analysis = f["_sAnalysisResultVerbose"].str().substr(0, 200);
        cf.version = f["_sVersion"].str().substr(0, 40);
        cf.added = f["_tsDateAdded"].num();
        cf.download_url = f["_sDownloadUrl"].str();
        bool md5_ok = cf.md5.size() == 32 && std::all_of(cf.md5.begin(), cf.md5.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); });
        if (cf.id <= 0 || !gamebanana_url_allowed(cf.download_url) || !md5_ok) continue;   // B12
        std::transform(cf.md5.begin(), cf.md5.end(), cf.md5.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
        out.files.push_back(std::move(cf));
    }
    return true;
}

bool GameBananaProvider::file_tree(int64_t file_id, std::vector<std::string>& out, std::string& error) {
    out.clear();
    std::string body;
    std::string url = "https://api.gamebanana.com/Core/Item/Data?itemtype=File&itemid=" + std::to_string(file_id) + "&fields=Metadata().aArchiveFilesList()";
    if (!get_json(url, body, error, true)) return false;
    Json j;
    if (!parse_json(body, j, error, provider_limits())) { error = "Lista de archivos no válida (" + error + ")."; return false; }
    for (const auto& name : j[0].elements())
        if (name.is_string() && out.size() < 20000) out.push_back(name.str());
    if (out.empty()) { error = "GameBanana no da la lista de archivos de este paquete."; return false; }
    return true;
}

bool GameBananaProvider::download(const CatalogFile& file, const fs::path& dest, std::string& error,
                                  const std::function<bool(uint64_t, uint64_t)>& progress) {
    if (file.size > kMaxModDownload) { error = "El archivo es demasiado grande para un mod (" + std::to_string(file.size >> 20) + " MB)."; return false; }
    HttpRequest req;
    req.url = file.download_url;
    req.max_bytes = std::max<uint64_t>(file.size + 4096, 1 << 20);   // nunca más de lo anunciado
    req.to_file = dest;
    req.progress = progress;
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    HttpResponse r = get(req, error);
    if (r.status != 200) {
        fs::remove(dest, ec);
        if (error.empty()) error = r.error.empty() ? "La descarga ha fallado." : r.error;
        return false;
    }
    uint64_t got = fs::file_size(dest, ec);
    if (ec || (file.size && got != file.size)) { fs::remove(dest, ec); error = "El tamaño descargado no coincide con el anunciado."; return false; }
    if (md5_file_hex(dest) != file.md5) { fs::remove(dest, ec); error = "El archivo descargado no coincide con su suma MD5 (corrupto o cambiado)."; return false; }
    return true;
}

}  // namespace wl::mods
