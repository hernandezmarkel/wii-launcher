// Manifiesto firmado de una versión del launcher: formato y verificación (UpdateVerifier).
#include <cctype>
#include <set>
#include <sstream>

#include "wl/update.h"

namespace wl::upd {

uint64_t UpdateManifest::total_size() const {
    uint64_t t = 0;
    for (const auto& f : files) t += f.size;
    return t;
}

namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

bool is_hex64(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

bool printable(const std::string& s) {
    for (unsigned char c : s)
        if (c < 0x20 || c == 0x7f) return false;
    return true;
}

bool simple_name(const std::string& s, size_t max) {
    if (s.empty() || s.size() > max || s[0] == '.') return false;
    for (char c : s)
        if (!(isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-')) return false;
    return s.find("..") == std::string::npos;
}

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace

bool allowed_asset_name(const std::string& name) { return simple_name(name, 100); }

// Lista cerrada: solo archivos del launcher. Nunca instaladores, servidor, juegos ni datos.
bool allowed_update_path(const std::string& path) {
    static const std::set<std::string> top = {"wii-launcher.exe", "LEEME.txt", "LICENCIAS.txt", "CHANGELOG.md"};
    if (top.count(path)) return true;
    size_t slash = path.find('/');
    if (slash == std::string::npos || path.find('/', slash + 1) != std::string::npos) return false;
    std::string dir = path.substr(0, slash), name = path.substr(slash + 1);
    if (!simple_name(name, 64)) return false;
    if (dir == "manifests") return ends_with(name, ".toml");
    if (dir == "fonts") return ends_with(name, ".ttf");
    return false;
}

bool parse_manifest_text(const std::string& text, UpdateManifest& out, std::string& error) {
    UpdateManifest m;
    if (text.size() > kMaxManifestBytes) {
        error = "El manifiesto de actualización es demasiado grande.";
        return false;
    }
    std::set<std::string> seen_keys, paths, assets;
    std::istringstream in(text);
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
        ++n;
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            error = "Manifiesto mal formado (línea " + std::to_string(n) + ").";
            return false;
        }
        std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        if (!printable(value)) {
            error = "Manifiesto con caracteres de control (línea " + std::to_string(n) + ").";
            return false;
        }
        if (key == "note") {
            if (m.notes.size() >= 40 || value.size() > 300) {
                error = "Demasiadas notas de versión.";
                return false;
            }
            m.notes.push_back(value);
            continue;
        }
        if (key == "file") {
            std::istringstream f(value);
            UpdateFile uf;
            std::string size, extra;
            f >> uf.sha256 >> size >> uf.asset >> uf.path;
            if (f >> extra || uf.path.empty()) {
                error = "Línea file inválida (línea " + std::to_string(n) + ").";
                return false;
            }
            if (!is_hex64(uf.sha256)) {
                error = "SHA-256 inválido para " + uf.path + ".";
                return false;
            }
            if (size.empty() || size.size() > 12 || size.find_first_not_of("0123456789") != std::string::npos) {
                error = "Tamaño inválido para " + uf.path + ".";
                return false;
            }
            uf.size = std::stoull(size);
            if (!allowed_update_path(uf.path)) {
                error = "Ruta no permitida en la actualización: " + uf.path;
                return false;
            }
            if (!allowed_asset_name(uf.asset)) {
                error = "Nombre de archivo no permitido: " + uf.asset;
                return false;
            }
            if (!paths.insert(uf.path).second || !assets.insert(uf.asset).second) {
                error = "Archivo repetido en el manifiesto: " + uf.path;
                return false;
            }
            if (uf.size == 0 || uf.size > kMaxFileBytes) {
                error = "Tamaño fuera de límites para " + uf.path + ".";
                return false;
            }
            m.files.push_back(uf);
            if (m.files.size() > 64) {
                error = "Demasiados archivos en la actualización.";
                return false;
            }
            continue;
        }
        if (!seen_keys.insert(key).second) {
            error = "Clave repetida en el manifiesto: " + key;
            return false;
        }
        if (key == "format") m.format = value == "1" ? 1 : -1;
        else if (key == "app") m.app = value;
        else if (key == "channel") m.channel = value;
        else if (key == "version") m.version = value;
        else if (key == "platform") m.platform = value;
        else if (key == "published") m.published = value;
        else if (key == "min_from") m.min_from = value;
        else {
            error = "Clave desconocida en el manifiesto: " + key;
            return false;
        }
    }
    if (m.format != 1) {
        error = "Formato de manifiesto no admitido por esta versión del launcher.";
        return false;
    }
    SemVer v;
    if (!parse_semver(m.version, v) || m.version[0] == 'v') {
        error = "Versión inválida en el manifiesto.";
        return false;
    }
    if (!m.min_from.empty() && !parse_semver(m.min_from, v)) {
        error = "min_from inválido en el manifiesto.";
        return false;
    }
    if (!paths.count("wii-launcher.exe")) {
        error = "La actualización no contiene wii-launcher.exe.";
        return false;
    }
    if (m.total_size() > kMaxTotalBytes) {
        error = "La actualización supera el tamaño máximo permitido.";
        return false;
    }
    out = m;
    return true;
}

VerifyResult verify_manifest(const std::string& text, const std::string& signature_hex, const VerifyPolicy& policy) {
    VerifyResult r;
    if (policy.public_key.empty()) {
        r.error = "Esta copia del launcher no tiene clave de actualizaciones.";
        return r;
    }
    if (text.size() > kMaxManifestBytes || signature_hex.size() > 200) {
        r.error = "Manifiesto o firma demasiado grandes.";
        return r;
    }
    // La firma va primero: nada del contenido se interpreta si no la ha firmado la clave anclada.
    if (!ecdsa_verify(policy.public_key, text, signature_hex)) {
        r.error = "La firma de la actualización no es válida. No se instala nada.";
        return r;
    }
    if (!parse_manifest_text(text, r.manifest, r.error)) return r;
    const UpdateManifest& m = r.manifest;
    if (m.app != kAppId) {
        r.error = "La actualización es de otra aplicación.";
        return r;
    }
    if (m.platform != kPlatform) {
        r.error = "La actualización es para otra plataforma (" + m.platform + ").";
        return r;
    }
    SemVer v;
    parse_semver(m.version, v);
    if (m.channel != "stable" && m.channel != "beta") {
        r.error = "Canal desconocido en la actualización.";
        return r;
    }
    if ((m.channel == "stable") != v.stable()) {
        r.error = "El canal no coincide con la versión.";
        return r;
    }
    if (policy.channel != "beta" && m.channel != "stable") {
        r.error = "Es una versión beta y tienes elegido el canal estable.";
        return r;
    }
    if (compare_versions(m.version, policy.installed) <= 0) {
        r.error = "La versión publicada (" + m.version + ") no es más nueva que la instalada (" + policy.installed + ").";
        return r;
    }
    if (!policy.highest_seen.empty() && compare_versions(m.version, policy.highest_seen) < 0) {
        r.error = "El servidor ofrece una versión más antigua que otra ya vista: posible repetición. No se instala.";
        return r;
    }
    if (!m.min_from.empty() && compare_versions(policy.installed, m.min_from) < 0) {
        r.error = "Esta versión necesita instalar antes la " + m.min_from + " a mano (ver guía).";
        return r;
    }
    r.ok = true;
    return r;
}

}  // namespace wl::upd
