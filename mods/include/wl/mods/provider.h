#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace wl::mods {

struct CatalogItem {
    int64_t id = 0, game = 0;
    std::string name, category, author, version, profile_url;
    int64_t updated = 0, views = 0;
};

struct CatalogFile {
    int64_t id = 0;
    std::string name, md5, av_result, analysis, version, download_url;
    uint64_t size = 0;
    int64_t added = 0;
    bool supported_archive() const;
};

struct CatalogDetail {
    CatalogItem item;
    std::string license, description;
    std::vector<CatalogFile> files;
    bool withheld = false, trashed = false, is_private = false;
    bool available_for(int64_t game) const;
    std::string version_for(const CatalogFile& file) const;
};

struct CatalogPage {
    std::vector<CatalogItem> items;
    int64_t total = 0;
    bool complete = false, from_cache = false;
};

constexpr uint64_t kMaxModDownload = 512ull << 20;

class ModProvider {
public:
    virtual ~ModProvider() = default;
    virtual bool list(int64_t game, int page, const std::string& search, CatalogPage& out, std::string& error) = 0;
    virtual bool detail(int64_t mod_id, CatalogDetail& out, std::string& error) = 0;
    virtual bool file_tree(int64_t file_id, std::vector<std::string>& out, std::string& error) = 0;
    virtual bool download(const CatalogFile& file, const std::filesystem::path& dest, std::string& error,
                          const std::function<bool(uint64_t, uint64_t)>& progress = {}) = 0;
};

struct ModReference;
struct ModMeta;
struct InstalledMod;
const CatalogFile* resolve_catalog_file(const CatalogDetail& detail, const ModReference& wanted);
ModMeta catalog_meta(const std::string& provider, const CatalogDetail& detail, const CatalogFile& file);
bool is_catalog_update(const CatalogDetail& detail, const CatalogFile& file, const InstalledMod& installed);

}  // namespace wl::mods
