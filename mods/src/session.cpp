#include "wl/mods/session.h"

#include <algorithm>
#include <cctype>
#include <set>

#include "wl/update.h"

namespace wl::mods {

namespace {

bool is_hex64(const std::string& s) {
    return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) || (c >= 'a' && c <= 'f'); });
}

bool valid_mod_id(const std::string& s) {
    if (s.empty() || s.size() > 80) return false;
    for (unsigned char c : s)
        if (!(std::islower(c) || std::isdigit(c) || c == '.' || c == '-' || c == '_')) return false;
    return s.find("..") == std::string::npos;
}

bool short_text(const std::string& s, size_t max) {
    if (s.size() > max) return false;
    for (unsigned char c : s)
        if (c < 0x20) return false;
    return true;
}

}  // namespace

const char* diff_name(ModDiff d) {
    switch (d) {
        case ModDiff::ExactMatch: return "EXACT_MATCH";
        case ModDiff::Missing: return "MISSING";
        case ModDiff::WrongVersion: return "WRONG_VERSION";
        case ModDiff::WrongHash: return "WRONG_HASH";
        case ModDiff::Disabled: return "DISABLED";
        case ModDiff::ExtraActive: return "EXTRA_ACTIVE";
        case ModDiff::LoadOrder: return "LOAD_ORDER";
        case ModDiff::ConfigMismatch: return "CONFIG_MISMATCH";
        case ModDiff::Unsupported: return "UNSUPPORTED";
        case ModDiff::NoAuthorizedSource: return "NO_AUTHORIZED_SOURCE";
        case ModDiff::IncompatibleGame: return "INCOMPATIBLE_GAME";
    }
    return "?";
}

Json build_session_manifest(const ModStore& store, const Profile& profile, const std::string& lobby_id, uint64_t revision) {
    Effective eff = store.compute(profile);
    Json j = Json::object();
    j.set("protocol", kSessionProtocol);
    j.set("protocol_version", kSessionProtocolVersion);
    j.set("lobby_id", lobby_id);
    j.set("manifest_revision", revision);
    j.set("game_id", store.support().game_id);
    j.set("base_game_compatibility_id", store.base().base_content_id);
    j.set("mod_loader_version", store.support().loader);
    j.set("compatibility_policy", "strict-gameplay-default");
    j.set("effective_mod_fingerprint", eff.problems.empty() ? eff.fingerprint : std::string("invalid"));
    Json mods = Json::array();
    size_t i = 0;
    for (const auto& m : eff.manifest_mods.elements()) {
        Json o = Json::object();
        for (const char* k : {"id", "name", "version", "source", "source_id", "effective_content_sha256", "config_sha256", "load_order"}) o.set(k, m[k]);
        // Opciones elegidas (Riivolution): son configuración, no órdenes; el cliente las usa en su perfil de sesión.
        Json ch = Json::object();
        size_t n = 0;
        for (const auto& e : profile.mods) {
            if (!e.enabled) continue;
            if (n++ == i) for (const auto& [k, v] : e.choices) ch.set(k, v);
        }
        o.set("choices", ch);
        mods.push(o);
        ++i;
    }
    j.set("mods", mods);
    Json caps = Json::array();
    caps.push("mod-diff-v1");
    caps.push("ready-gate-v1");
    j.set("capabilities", caps);
    return j;
}

bool parse_session_manifest(const std::string& text, SessionManifest& out, std::string& error) {
    out = SessionManifest{};
    JsonLimits lim;
    lim.max_bytes = kMaxSessionManifest;
    lim.max_depth = 6;
    lim.max_string = 512;
    lim.max_elements = 6000;
    Json j;
    if (!parse_json(text, j, error, lim)) { error = "manifiesto de sala no válido: " + error; return false; }
    if (j["protocol"].str() != kSessionProtocol) { error = "la sala no usa el protocolo de mods"; return false; }
    out.protocol_version = static_cast<int>(j["protocol_version"].num());
    if (out.protocol_version != kSessionProtocolVersion) { error = "versión del protocolo de mods distinta"; return false; }
    out.lobby_id = j["lobby_id"].str();
    out.revision = static_cast<uint64_t>(std::max<int64_t>(0, j["manifest_revision"].num()));
    out.game_id = j["game_id"].str();
    out.base_content_id = j["base_game_compatibility_id"].str();
    out.loader = j["mod_loader_version"].str();
    out.fingerprint = j["effective_mod_fingerprint"].str();
    if (!short_text(out.lobby_id, 64) || !short_text(out.game_id, 32) || !short_text(out.base_content_id, 40) || !short_text(out.loader, 32)) {
        error = "manifiesto de sala con campos no válidos";
        return false;
    }
    if (out.fingerprint != kVanilla && !is_hex64(out.fingerprint)) { error = "huella de mods no válida"; return false; }
    if (!j["mods"].is_array() || j["mods"].size() > kMaxSessionMods) { error = "lista de mods no válida"; return false; }
    std::set<std::string> ids;
    for (const auto& m : j["mods"].elements()) {
        SessionMod s;
        s.id = m["id"].str();
        s.name = m["name"].str();
        s.version = m["version"].str();
        s.source = m["source"].str();
        s.source_id = m["source_id"].str();
        s.content_sha256 = m["effective_content_sha256"].str();
        s.config_sha256 = m["config_sha256"].str();
        s.load_order = static_cast<int>(m["load_order"].num(-1));
        if (!valid_mod_id(s.id) || !ids.insert(s.id).second || !short_text(s.name, 200) || !short_text(s.version, 40) ||
            !is_hex64(s.content_sha256) || !is_hex64(s.config_sha256) || s.load_order < 0) {
            error = "un mod del manifiesto no es válido";
            return false;
        }
        if (s.source != "local" && s.source != "gamebanana") s.source = "desconocido";   // nunca se descarga de ahí
        if (s.source_id.size() > 12 || !std::all_of(s.source_id.begin(), s.source_id.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
            s.source_id.clear();
        for (const auto& [k, v] : m["choices"].items()) {
            if (!short_text(k, 120) || !v.is_int() || v.num() < 0 || v.num() > 64) { error = "opciones no válidas en el manifiesto"; return false; }
            s.choices[k] = static_cast<int>(v.num());
        }
        out.mods.push_back(std::move(s));
    }
    std::sort(out.mods.begin(), out.mods.end(), [](const SessionMod& a, const SessionMod& b) { return a.load_order < b.load_order; });
    return true;
}

SessionDiff diff_session(const ModStore& store, const Profile& local, const SessionManifest& host) {
    SessionDiff d;
    if (host.game_id != store.support().game_id) d.game_problem = "La sala es de otro juego.";
    else if (host.base_content_id != store.base().base_content_id) d.game_problem = "Tu disco no es la misma versión/región que el del anfitrión (" + host.base_content_id + "): descargar mods no lo arregla.";
    else if (host.loader != store.support().loader) d.game_problem = "El anfitrión usa otro cargador de mods (" + host.loader + "): actualizad los dos el launcher.";
    d.game_ok = d.game_problem.empty();
    if (!d.game_ok) {
        DiffItem it;
        it.state = ModDiff::IncompatibleGame;
        it.detail = d.game_problem;
        d.items.push_back(it);
        return d;
    }
    Effective mine = store.compute(local);
    auto installed = store.installed();
    // Mods activos en local, en orden.
    std::vector<std::string> local_active;
    for (const auto& e : local.mods)
        if (e.enabled) local_active.push_back(e.id);

    Profile session;
    session.name = "session";
    bool all_present = true;
    for (size_t i = 0; i < host.mods.size(); ++i) {
        const SessionMod& h = host.mods[i];
        DiffItem it;
        it.id = h.id;
        it.name = h.name;
        it.wanted_version = h.version;
        it.source = h.source;
        it.source_id = h.source_id;
        const InstalledMod* exact = nullptr;
        const InstalledMod* same_id = nullptr;
        for (const auto& m : installed) {
            if (m.id() != h.id) continue;
            same_id = &m;
            if (m.content_sha() == h.content_sha256) { exact = &m; break; }
        }
        auto entry = std::find_if(local.mods.begin(), local.mods.end(), [&](const ProfileEntry& e) { return e.id == h.id; });
        if (exact) {
            it.local_key = exact->key;
            it.local_version = exact->version();
            Json ch = Json::object();
            for (const auto& [k, v] : h.choices) ch.set(k, v);
            bool active = entry != local.mods.end() && entry->enabled && entry->package == exact->key;
            Json mine_ch = Json::object();
            if (entry != local.mods.end()) for (const auto& [k, v] : entry->choices) mine_ch.set(k, v);
            auto pos = std::find(local_active.begin(), local_active.end(), h.id);
            if (!active) it.state = ModDiff::Disabled, d.disabled++;
            else if (upd::sha256_hex(mine_ch.dump()) != h.config_sha256) it.state = ModDiff::ConfigMismatch, d.wrong++;
            else if (pos == local_active.end() || static_cast<size_t>(pos - local_active.begin()) != i) it.state = ModDiff::LoadOrder, d.wrong++;
            else it.state = ModDiff::ExactMatch;
            // En el perfil de sesión se reproduce exactamente lo del anfitrión (opciones incluidas).
            session.mods.push_back({h.id, exact->key, true, h.choices});
        } else {
            all_present = false;
            if (same_id) {
                it.local_version = same_id->version();
                it.state = same_id->version() == h.version ? ModDiff::WrongHash : ModDiff::WrongVersion;
                d.wrong++;
            } else {
                it.state = ModDiff::Missing;
                d.missing++;
            }
            it.downloadable = h.source == "gamebanana" && !h.source_id.empty();
            if (!it.downloadable) {
                it.detail = h.source == "local" ? "El anfitrión lo importó a mano: pídele el archivo o su página e impórtalo tú."
                                                : "Sin fuente autorizada para descargarlo.";
                if (it.state == ModDiff::Missing) it.state = ModDiff::NoAuthorizedSource;
            } else if (it.state == ModDiff::WrongHash) {
                it.detail = "Misma versión pero contenido distinto: hace falta exactamente el archivo del anfitrión.";
            }
        }
        d.items.push_back(it);
    }
    for (const auto& e : local.mods) {
        if (!e.enabled) continue;
        bool in_host = std::any_of(host.mods.begin(), host.mods.end(), [&](const SessionMod& h) { return h.id == e.id; });
        if (in_host) continue;
        DiffItem it;
        it.state = ModDiff::ExtraActive;
        it.id = e.id;
        InstalledMod m;
        if (store.find_installed(e.package, m)) { it.name = m.name(); it.local_version = m.version(); }
        it.detail = "Se desactiva solo durante la partida (perfil de sesión); tu perfil habitual no cambia.";
        d.items.push_back(it);
        d.extra++;
    }
    d.compatible = mine.problems.empty() && mine.fingerprint == host.fingerprint;
    if (all_present) {
        Effective se = store.compute(session);
        d.session_ready = se.problems.empty() && se.fingerprint == host.fingerprint;
        if (d.session_ready) d.session_profile = session;
    }
    return d;
}

}  // namespace wl::mods
