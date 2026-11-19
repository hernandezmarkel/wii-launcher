#include "wl/manifest.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>

namespace wl {

namespace {

const char* const kCapabilities[] = {"local", "lan_host", "lan_join", "lan_wfc"};
const char* const kAdapters[] = {"mp9", "wiiparty", "mkwii", "none"};

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r");
    return s.substr(a, b - a + 1);
}

// Subconjunto de TOML: `clave = "texto" | entero | ["a", "b"]`, comentarios con #. Sin tablas.
struct Value {
    enum Kind { String, Integer, List } kind = String;
    std::string text;
    long long number = 0;
    std::vector<std::string> list;
};

bool parse_string(const std::string& s, size_t& i, std::string& out) {
    if (i >= s.size() || s[i] != '"') return false;
    for (++i; i < s.size(); ++i) {
        char c = s[i];
        if (c == '"') { ++i; return true; }
        if (c == '\\') {
            if (++i >= s.size()) return false;
            char e = s[i];
            if (e == '"' || e == '\\') out += e;
            else if (e == 'n') out += '\n';
            else return false;
        } else {
            out += c;
        }
    }
    return false;
}

bool parse_value(const std::string& raw, Value& v) {
    std::string s = trim(raw);
    size_t i = 0;
    if (s.empty()) return false;
    if (s[0] == '"') {
        v.kind = Value::String;
        if (!parse_string(s, i, v.text)) return false;
    } else if (s[0] == '[') {
        v.kind = Value::List;
        ++i;
        for (;;) {
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
            if (i < s.size() && s[i] == ']') { ++i; break; }
            std::string item;
            if (!parse_string(s, i, item)) return false;
            v.list.push_back(item);
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
            if (i < s.size() && s[i] == ',') { ++i; continue; }
            if (i < s.size() && s[i] == ']') { ++i; break; }
            return false;
        }
    } else {
        v.kind = Value::Integer;
        size_t end = 0;
        if (!(std::isdigit(static_cast<unsigned char>(s[0])) || s[0] == '-')) return false;
        try { v.number = std::stoll(s, &end); } catch (...) { return false; }
        i = end;
    }
    std::string rest = trim(s.substr(i));
    return rest.empty() || rest[0] == '#';
}

template <size_t N>
bool known(const char* const (&set)[N], const std::string& value) {
    return std::find_if(std::begin(set), std::end(set), [&](const char* x) { return value == x; }) != std::end(set);
}

}

bool Manifest::has(const std::string& capability) const {
    return std::find(capabilities.begin(), capabilities.end(), capability) != capabilities.end();
}

const char* status_name(GameStatus status) {
    switch (status) {
    case GameStatus::Playable: return "jugable";
    case GameStatus::InDevelopment: return "en desarrollo";
    case GameStatus::Beta: return "beta, puede fallar";
    default: return "previsto";
    }
}

bool safe_relative_path(const std::string& p) {
    if (p.empty() || p.size() > 260) return false;
    if (p[0] == '/' || p[0] == '\\') return false;                      // raíz o UNC
    if (p.find(':') != std::string::npos) return false;                 // unidad o flujo NTFS
    if (p.find('%') != std::string::npos || p.find('$') != std::string::npos || p.find('~') != std::string::npos) return false;
    for (char c : p) {
        if (static_cast<unsigned char>(c) < 0x20 || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') return false;
    }
    std::string part;
    std::stringstream in(p);
    while (std::getline(in, part, '/')) {
        std::stringstream sub(part);
        std::string piece;
        while (std::getline(sub, piece, '\\')) {
            std::string t = trim(piece);
            if (t == "..") return false;
            if (!t.empty() && (t.back() == '.' || t.back() == ' ') && t != ".") return false;  // nombres que Windows recorta
        }
    }
    return true;
}

ManifestResult parse_manifest(const std::string& text) {
    ManifestResult r;
    std::map<std::string, Value> values;
    std::stringstream in(text);
    std::string line;
    int number = 0;
    while (std::getline(in, line)) {
        ++number;
        std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) {
            r.errors.push_back("línea " + std::to_string(number) + ": se esperaba clave = valor");
            continue;
        }
        std::string key = trim(t.substr(0, eq));
        Value v;
        if (!std::regex_match(key, std::regex("[a-z_]{1,40}")) || !parse_value(t.substr(eq + 1), v)) {
            r.errors.push_back("línea " + std::to_string(number) + ": valor no válido");
            continue;
        }
        if (values.count(key)) {
            r.errors.push_back("línea " + std::to_string(number) + ": clave repetida '" + key + "'");
            continue;
        }
        values[key] = v;
    }

    Manifest& m = r.manifest;
    auto text_of = [&](const char* key, bool required, std::string& out) {
        auto it = values.find(key);
        if (it == values.end()) {
            if (required) r.errors.push_back(std::string("falta '") + key + "'");
            return;
        }
        if (it->second.kind != Value::String) { r.errors.push_back(std::string("'") + key + "' debe ser texto"); return; }
        out = it->second.text;
        values.erase(it);
    };
    auto int_of = [&](const char* key, bool required, int& out) {
        auto it = values.find(key);
        if (it == values.end()) {
            if (required) r.errors.push_back(std::string("falta '") + key + "'");
            return;
        }
        if (it->second.kind != Value::Integer) { r.errors.push_back(std::string("'") + key + "' debe ser un entero"); return; }
        out = static_cast<int>(it->second.number);
        values.erase(it);
    };
    auto list_of = [&](const char* key, std::vector<std::string>& out) {
        auto it = values.find(key);
        if (it == values.end()) return;
        if (it->second.kind != Value::List) { r.errors.push_back(std::string("'") + key + "' debe ser una lista"); return; }
        out = it->second.list;
        values.erase(it);
    };

    std::string status;
    int_of("manifest_version", true, m.manifest_version);
    text_of("game_id", true, m.game_id);
    text_of("display_name", true, m.display_name);
    text_of("status", true, status);
    text_of("adapter", true, m.adapter);
    int_of("adapter_version", true, m.adapter_version);
    text_of("install_folder", false, m.install_folder);
    text_of("executable", false, m.executable);
    text_of("working_dir", false, m.working_dir);
    list_of("disc_ids", m.disc_ids);
    int_of("max_players", true, m.max_players);
    list_of("capabilities", m.capabilities);
    text_of("notes", false, m.notes);
    text_of("installer", false, m.installer);
    text_of("lan_server_script", false, m.lan_server_script);
    text_of("launcher_prefs", false, m.launcher_prefs);
    for (const auto& [key, _] : values) r.errors.push_back("clave desconocida '" + key + "'");

    if (m.manifest_version != 1) r.errors.push_back("manifest_version no soportada (solo 1)");
    if (!std::regex_match(m.game_id, std::regex("[a-z0-9_]{2,32}"))) r.errors.push_back("game_id debe ser [a-z0-9_]{2,32}");
    if (m.display_name.empty() || m.display_name.size() > 64) r.errors.push_back("display_name vacío o demasiado largo");
    if (status == "playable") m.status = GameStatus::Playable;
    else if (status == "in_development") m.status = GameStatus::InDevelopment;
    else if (status == "beta") m.status = GameStatus::Beta;
    else if (status == "planned") m.status = GameStatus::Planned;
    else r.errors.push_back("status debe ser playable, in_development, beta o planned");
    if (!m.installer.empty()) {
        std::string lower = m.installer;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
        bool kind = lower.size() > 4 && (lower.substr(lower.size() - 4) == ".exe" || lower.substr(lower.size() - 4) == ".ps1");
        if (!safe_relative_path(m.installer) || !kind) r.errors.push_back("installer debe ser una ruta relativa segura a un .exe o .ps1");
    }
    if (!m.lan_server_script.empty()) {
        std::string lower = m.lan_server_script;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
        if (!safe_relative_path(m.lan_server_script) || lower.size() < 5 || lower.substr(lower.size() - 4) != ".ps1")
            r.errors.push_back("lan_server_script debe ser una ruta relativa segura a un .ps1");
        if (!m.has("lan_wfc")) r.errors.push_back("lan_server_script necesita la capacidad 'lan_wfc'");
    }
    if (!m.launcher_prefs.empty() && !std::regex_match(m.launcher_prefs, std::regex("[a-z0-9-]{1,40}")))
        r.errors.push_back("launcher_prefs debe ser [a-z0-9-]{1,40}");
    if (!known(kAdapters, m.adapter)) r.errors.push_back("adaptador desconocido '" + m.adapter + "'");
    if (m.max_players < 1 || m.max_players > 12) r.errors.push_back("max_players fuera de 1..12");
    for (const auto& c : m.capabilities) {
        if (!known(kCapabilities, c)) r.errors.push_back("capacidad desconocida '" + c + "' (internet no existe: no hay servidores)");
    }
    for (const auto& id : m.disc_ids) {
        if (!std::regex_match(id, std::regex("[A-Z0-9]{6}"))) r.errors.push_back("ID de disco no válido '" + id + "'");
    }
    if ((m.has("lan_host") || m.has("lan_join")) && m.max_players < 2) r.errors.push_back("un juego online necesita max_players >= 2");

    bool launchable = m.status != GameStatus::Planned;
    if (launchable) {
        if (!std::regex_match(m.install_folder, std::regex("[A-Za-z0-9_.-]{1,64}")) || m.install_folder == "." || m.install_folder == "..")
            r.errors.push_back("install_folder debe ser un solo nombre de carpeta");
        if (!safe_relative_path(m.executable)) r.errors.push_back("executable debe ser una ruta relativa segura dentro de la instalación");
        else {
            std::string lower = m.executable;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
            if (lower.size() < 5 || lower.substr(lower.size() - 4) != ".exe") r.errors.push_back("executable debe ser un .exe");
        }
        if (!safe_relative_path(m.working_dir)) r.errors.push_back("working_dir debe ser una ruta relativa segura");
        if (!m.has("local")) r.errors.push_back("un juego lanzable debe declarar 'local'");
    } else if (!m.capabilities.empty() || !m.executable.empty()) {
        r.errors.push_back("un juego previsto no puede declarar ejecutable ni capacidades");
    }
    r.ok = r.errors.empty();
    return r;
}

ManifestResult load_manifest(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        ManifestResult r;
        r.errors.push_back("no se puede leer " + path);
        return r;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    if (buffer.str().size() > 64 * 1024) {
        ManifestResult r;
        r.errors.push_back("manifest demasiado grande");
        return r;
    }
    return parse_manifest(buffer.str());
}

}
