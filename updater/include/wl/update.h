#pragma once
// Autoactualización del launcher (ADR docs/updater/02-adr-arquitectura.md).
// Sin SDL ni ImGui: lo usa la UI (app/update_ui.cpp), el aplicador (--apply-update) y los tests.
// Solo actualiza el launcher (exe, manifests, fuentes, textos). Nunca juegos, instaladores ni datos de usuario.
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace wl::upd {

namespace fs = std::filesystem;

// ------------------------------------------------------------------ versión (VersionProvider)

struct SemVer {
    int major = 0, minor = 0, patch = 0;
    std::string pre;  // "beta.2"; vacío = estable
    bool stable() const { return pre.empty(); }
};
bool parse_semver(const std::string& text, SemVer& out);  // admite "v" delante
int compare(const SemVer& a, const SemVer& b);             // <0, 0, >0. 1.0.0-beta.2 < 1.0.0
int compare_versions(const std::string& a, const std::string& b);  // texto inválido = el menor
std::string current_version();   // la de este exe (CMake: project VERSION + WL_VERSION_SUFFIX)
std::string update_repo();       // "dueño/repo" de GitHub fijado al compilar; vacío = updater desactivado
std::string public_key_hex();    // clave pública anclada (X||Y de P-256, 128 hex)
bool testing_build();            // -DWL_UPDATER_TESTING=ON: feed local por http://127.0.0.1 y fallos simulados

// ------------------------------------------------------------------ criptografía (CNG de Windows)

std::string sha256_hex(const std::string& data);
std::string sha256_file_hex(const fs::path& file);  // vacío si no se puede leer
bool ecdsa_verify(const std::string& public_key_hex, const std::string& data, const std::string& signature_hex);
// Solo para la herramienta wl_sign y los tests:
bool ecdsa_generate(std::string& private_key_hex, std::string& public_key_hex);
bool ecdsa_sign(const std::string& private_key_hex, const std::string& data, std::string& signature_hex);

// ------------------------------------------------------------------ manifiesto firmado (UpdateVerifier)

constexpr uint64_t kMaxManifestBytes = 64 * 1024;
constexpr uint64_t kMaxFileBytes = 64ull * 1024 * 1024;
constexpr uint64_t kMaxTotalBytes = 128ull * 1024 * 1024;
constexpr const char* kPlatform = "windows-x86_64";
constexpr const char* kAppId = "wii-launcher";

struct UpdateFile {
    std::string sha256;  // 64 hex en minúsculas
    uint64_t size = 0;
    std::string asset;   // nombre del archivo en la Release
    std::string path;    // ruta relativa dentro de la carpeta del launcher, con '/'
};

struct UpdateManifest {
    int format = 0;
    std::string app, channel, version, platform, published, min_from;
    std::vector<std::string> notes;
    std::vector<UpdateFile> files;
    uint64_t total_size() const;
};

struct VerifyPolicy {
    std::string channel = "stable";  // canal elegido por el usuario
    std::string installed;           // versión instalada
    std::string highest_seen;        // mayor versión verificada vista antes (anti-replay); vacío = ninguna
    std::string public_key;          // hex
};

struct VerifyResult {
    bool ok = false;
    UpdateManifest manifest;
    std::string error;  // en español
};

// Sin verificar firma: solo formato. Lo usan verify_manifest() y la herramienta de release.
bool parse_manifest_text(const std::string& text, UpdateManifest& out, std::string& error);
// Firma + formato + app/plataforma/canal + versión + rutas + límites. Nunca lanza.
VerifyResult verify_manifest(const std::string& text, const std::string& signature_hex, const VerifyPolicy& policy);
bool allowed_update_path(const std::string& path);
bool allowed_asset_name(const std::string& name);

// ------------------------------------------------------------------ HTTP

struct HttpRequest {
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    uint64_t max_bytes = kMaxManifestBytes;  // se corta (error) si la respuesta es mayor
    fs::path to_file;                        // si no está vacío, el cuerpo va a este archivo y no a body
    std::function<bool(uint64_t got, uint64_t total)> progress;  // false = cancelar
};

struct HttpResponse {
    int status = 0;                             // 0 = no hubo respuesta HTTP (red, DNS, TLS, timeout)
    std::string body;
    std::map<std::string, std::string> headers;  // nombres en minúsculas
    std::string error;                           // en español, sin URLs con datos personales
    bool cancelled = false;
    std::string header(const std::string& name) const;
};

// Un salto, sin seguir redirecciones (las sigue fetch() con la lista de hosts).
class HttpClient {
public:
    virtual ~HttpClient() = default;
    virtual HttpResponse get(const HttpRequest& request) = 0;
};
std::unique_ptr<HttpClient> make_winhttp_client();

// HTTPS a github.com, api.github.com, objects/release-assets.githubusercontent.com en el puerto 443.
// En builds de prueba, además http://127.0.0.1:<puerto>.
bool allowed_url(const std::string& url);
HttpResponse fetch(HttpClient& http, HttpRequest request, int max_redirects = 5);

// ------------------------------------------------------------------ feed (ReleaseFeed)

struct FeedConfig {
    std::string repo;        // "dueño/repo"
    std::string channel = "stable";
    std::string feed_base;   // solo pruebas: "http://127.0.0.1:8765/" (estructura en docs/updater/04-publicacion.md)
    std::string beta_etag;   // caché de la API de releases (canal beta)
    std::string beta_tag;
};

enum class CheckKind { UpToDate, Available, NoReleases, RateLimited, Error };

struct CheckResult {
    CheckKind kind = CheckKind::Error;
    UpdateManifest manifest;
    std::string manifest_text, signature;  // tal como se verificaron (se guardan en staging)
    std::string error;
    int http_status = 0;
    int retry_after_s = 0;
    std::string beta_etag, beta_tag;  // para guardar en preferencias
};

CheckResult check_for_update(HttpClient& http, const FeedConfig& feed, const VerifyPolicy& policy);
std::string asset_url(const FeedConfig& feed, const std::string& version, const std::string& asset);

// ------------------------------------------------------------------ descarga (UpdateDownloader)

struct DownloadProgress {
    std::atomic<uint64_t> done{0};
    std::atomic<uint64_t> total{0};
    std::atomic<bool> cancel{false};
};

// Descarga a staging solo los archivos cuyo hash difiere del instalado, verifica tamaño y SHA-256 y guarda el
// manifiesto firmado. true si staging queda completo y verificado.
bool download_update(HttpClient& http, const FeedConfig& feed, const CheckResult& check, const fs::path& install_dir,
                     const fs::path& staging, DownloadProgress& progress, std::string& error);
// Vuelve a verificar staging contra su manifiesto firmado (lo hace el aplicador antes de tocar nada).
VerifyResult verify_staging(const fs::path& staging, const VerifyPolicy& policy, std::string& error);

// ------------------------------------------------------------------ instalación (UpdateInstaller)

fs::path updates_dir();  // %LOCALAPPDATA%\WiiLauncher\updates

// Cerrojo entre procesos (updates\.lock): una sola descarga o instalación a la vez, aunque haya dos ventanas.
class UpdateLock {
public:
    UpdateLock() = default;
    UpdateLock(const UpdateLock&) = delete;
    UpdateLock& operator=(const UpdateLock&) = delete;
    ~UpdateLock() { release(); }
    bool acquire(int wait_ms = 0);
    void release();
    bool held() const { return handle_ != nullptr; }

private:
    void* handle_ = nullptr;
};

bool folder_writable(const fs::path& dir);
int other_launcher_instances(const fs::path& exe, unsigned long ignore_pid);  // misma ruta de exe

struct ApplyArgs {
    fs::path staging, target, journal;
    unsigned long wait_pid = 0;
    int health_timeout_ms = 60000;
};
// Proceso aplicador (copia del exe actual). Devuelve el código de salida.
int run_apply(const ApplyArgs& args);
// Al arrancar: si hay un diario a medias y ningún aplicador vivo, deshace. Devuelve un mensaje para la UI o vacío.
std::string recover_interrupted_update();
// El exe nuevo, tras su primer fotograma con --post-update <diario>.
void report_health(const fs::path& journal);
// Lanza el aplicador y devuelve true si arrancó (el launcher debe cerrarse a continuación).
bool start_apply(const fs::path& staging, const fs::path& install_dir, std::string& error);

// ------------------------------------------------------------------ registro (UpdateTelemetry, solo local)

void log(const std::string& line);  // %LOCALAPPDATA%\WiiLauncher\updater.log, sin rutas personales
std::string redact(const std::string& text);

}
