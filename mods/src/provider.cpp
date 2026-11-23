#include "wl/mods/provider.h"

#include <algorithm>
#include <cctype>

#include "wl/mods/session.h"

namespace wl::mods {

bool CatalogFile::supported_archive() const {
    auto ext = std::filesystem::u8path(name).extension().u8string();
    for (auto& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return ext == ".zip" && size > 0 && size <= kMaxModDownload;
}

bool CatalogDetail::available_for(int64_t game) const {
    return game > 0 && item.game == game && !withheld && !trashed && !is_private;
}

std::string CatalogDetail::version_for(const CatalogFile& file) const {
    return file.version.empty() ? item.version : file.version;
}

const CatalogFile* resolve_catalog_file(const CatalogDetail& detail, const SessionMod& wanted) {
    if (std::to_string(detail.item.id) != wanted.source_id) return nullptr;
    const CatalogFile* match = nullptr;
    for (const auto& file : detail.files) {
        if (!file.supported_archive()) continue;
        if (!wanted.source_file_id.empty()) {
            if (std::to_string(file.id) != wanted.source_file_id || file.md5 != wanted.source_md5) continue;
        } else if (detail.version_for(file) != wanted.version && wanted.package_sha256.empty()) {
            continue;
        }
        // Si una ficha antigua ofrece varias variantes, hace falta una referencia exacta.
        if (match) return nullptr;
        match = &file;
    }
    return match;
}

ModMeta catalog_meta(const std::string& provider, const CatalogDetail& detail, const CatalogFile& file) {
    ModMeta meta;
    meta.source_id = std::to_string(detail.item.id);
    meta.id = provider + "." + meta.source_id;
    meta.name = detail.item.name;
    meta.version = detail.version_for(file);
    meta.provider = provider;
    meta.source_page = detail.item.profile_url;
    meta.license = detail.license.substr(0, 300);
    meta.author = detail.item.author;
    meta.source_file_id = std::to_string(file.id);
    meta.source_md5 = file.md5;
    return meta;
}

}  // namespace wl::mods
