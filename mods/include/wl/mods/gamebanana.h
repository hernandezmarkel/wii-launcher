#pragma once
// Proveedor GameBanana (API pública v11 + API clásica para el árbol de archivos). Todo lo que devuelve es
// dato NO confiable: se valida, se limita y se muestra como texto. Nada de aquí instala ni ejecuta nada:
// la descarga va a staging y después pasa por el mismo análisis que una importación manual.
//
// Endpoints usados (verificados el 18/11/2026, docs/mods/03-proveedores.md):
//   GET https://gamebanana.com/apiv11/Mod/Index?_nPage=&_nPerpage=&_aFilters[Generic_Game]=<juego>
//   GET https://gamebanana.com/apiv11/Util/Search/Results?_sModelName=Mod&_idGameRow=<juego>&_sSearchString=&_nPage=
//   GET https://gamebanana.com/apiv11/Mod/<id>/ProfilePage
//   GET https://api.gamebanana.com/Core/Item/Data?itemtype=File&itemid=<id>&fields=Metadata().aArchiveFilesList()
//   GET https://gamebanana.com/dl/<id>  → 302 a https://files.gamebanana.com/...
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "wl/update.h"

namespace wl::mods {

namespace fs = std::filesystem;

struct CatalogItem {
    int64_t id = 0;
    std::string name, category, author, version, profile_url;
    int64_t updated = 0;   // epoch
    int64_t views = 0;
};

struct CatalogFile {
    int64_t id = 0;
    std::string name, md5, av_result, analysis, version, download_url;
    uint64_t size = 0;
    int64_t added = 0;
};

struct CatalogDetail {
    CatalogItem item;
    std::string license;       // texto, sin HTML
    std::string description;   // texto corto, sin HTML
    std::vector<CatalogFile> files;
    bool withheld = false, trashed = false, is_private = false;
};

struct CatalogPage {
    std::vector<CatalogItem> items;
    int64_t total = 0;
    bool complete = false;
    bool from_cache = false;
};

// Límites de descarga de un mod (antes de nada se compara con el tamaño que anuncia el proveedor).
constexpr uint64_t kMaxModDownload = 512ull << 20;

bool gamebanana_url_allowed(const std::string& url);
// ID de juego de GameBanana para un game_id del launcher (0 = sin catálogo).
int64_t gamebanana_game(const std::string& game_id);
std::string strip_html(const std::string& html, size_t max_chars = 2000);

class GameBananaProvider {
public:
    GameBananaProvider(upd::HttpClient& http, fs::path cache_dir);

    bool list(int64_t game, int page, const std::string& search, CatalogPage& out, std::string& error);
    bool detail(int64_t mod_id, CatalogDetail& out, std::string& error);
    // Árbol del archivo comprimido sin descargarlo (para decidir compatibilidad antes de bajar nada).
    bool file_tree(int64_t file_id, std::vector<std::string>& out, std::string& error);
    // Descarga a `dest` (staging). Comprueba hosts en cada salto, tamaño anunciado y real, y MD5 del proveedor.
    bool download(const CatalogFile& file, const fs::path& dest, std::string& error,
                  const std::function<bool(uint64_t, uint64_t)>& progress = {});

    // Pruebas: sin esperas entre peticiones.
    void set_min_interval(std::chrono::milliseconds ms) { min_interval_ = ms; }

private:
    bool get_json(const std::string& url, std::string& body, std::string& error, bool use_cache, bool* from_cache = nullptr);
    upd::HttpResponse get(upd::HttpRequest req, std::string& error);

    upd::HttpClient& http_;
    fs::path cache_;
    std::chrono::milliseconds min_interval_{350};
    std::chrono::steady_clock::time_point last_{};
};

std::string md5_file_hex(const fs::path& file);

}  // namespace wl::mods
