#include "wl/mods/store.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <fstream>
#include <set>

#ifdef _WIN32
#include <windows.h>
#endif

#include "wl/adapter.h"
#include "wl/mods/safe_path.h"
#include "wl/update.h"

namespace wl::mods {

using upd::sha256_file_hex;
using upd::sha256_hex;

// ------------------------------------------------------------------ utilidades

bool write_file_atomic(const fs::path& path, const std::string& data, std::string& error) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        out.flush();
        if (!out) { error = "no se puede escribir " + to_utf8(path.filename()) + " (¿disco lleno?)"; fs::remove(tmp, ec); return false; }
    }
#ifdef _WIN32
    if (!MoveFileExW(tmp.wstring().c_str(), path.wstring().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "no se puede reemplazar " + to_utf8(path.filename());
        fs::remove(tmp, ec);
        return false;
    }
#else
    fs::rename(tmp, path, ec);
    if (ec) { error = "no se puede reemplazar " + to_utf8(path.filename()); return false; }
#endif
    return true;
}

bool read_file_text(const fs::path& path, std::string& out, size_t max_bytes) {
    std::error_code ec;
    auto size = fs::file_size(path, ec);
    if (ec || size > max_bytes) return false;
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), {});
    return true;
}

std::string slug(const std::string& text) {
    std::string s;
    for (unsigned char c : text) {
        if (std::isalnum(c)) s += static_cast<char>(std::tolower(c));
        else if (!s.empty() && s.back() != '-') s += '-';
        if (s.size() >= 48) break;
    }
    while (!s.empty() && s.back() == '-') s.pop_back();
    return s.empty() ? "mod" : s;
}

fs::path default_mods_root(const std::string& game_id) { return local_app_data() / "WiiLauncher" / "mods" / game_id; }

StoreLock::StoreLock(const fs::path& file) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
#ifdef _WIN32
    HANDLE h = CreateFileW(file.wstring().c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h != INVALID_HANDLE_VALUE) { handle_ = h; held_ = true; }
#else
    held_ = true;
#endif
}

StoreLock::~StoreLock() {
#ifdef _WIN32
    if (handle_) CloseHandle(static_cast<HANDLE>(handle_));
#endif
}

namespace {

std::string op_id() {
    static std::atomic<unsigned> counter{0};
    auto now = std::chrono::system_clock::now().time_since_epoch().count();
    return std::to_string(now) + "-" + std::to_string(counter++);
}

std::string iso_now() {
    auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char buf[32];
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

bool valid_profile_name(const std::string& n) {
    if (n.empty() || n.size() > 40) return false;
    for (unsigned char c : n)
        if (!std::isalnum(c) && c != '-' && c != '_') return false;
    return true;
}

bool valid_key(const std::string& k) {
    if (k.size() != 32) return false;
    for (unsigned char c : k)
        if (!std::isxdigit(c) || std::isupper(c)) return false;
    return true;
}

std::vector<uint8_t> read_bytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

}  // namespace

// ------------------------------------------------------------------ perfiles

Json Profile::to_json() const {
    Json j = Json::object();
    j.set("schema", 1);
    j.set("name", name);
    j.set("epoch", epoch);
    if (!restore.empty()) j.set("restore", restore);
    if (!lobby.empty()) j.set("lobby", lobby);
    Json arr = Json::array();
    for (const auto& m : mods) {
        Json e = Json::object();
        e.set("id", m.id);
        e.set("package", m.package);
        e.set("enabled", m.enabled);
        Json ch = Json::object();
        for (const auto& [k, v] : m.choices) ch.set(k, v);
        e.set("choices", ch);
        arr.push(e);
    }
    j.set("mods", arr);
    return j;
}

bool Profile::from_json(const Json& j, Profile& out, std::string& error) {
    out = Profile{};
    if (j["schema"].num() != 1) { error = "perfil con formato desconocido"; return false; }
    out.name = j["name"].str();
    out.epoch = static_cast<uint64_t>(j["epoch"].num());
    out.restore = j["restore"].str();
    out.lobby = j["lobby"].str();
    std::set<std::string> ids;
    for (const auto& e : j["mods"].elements()) {
        ProfileEntry m;
        m.id = e["id"].str();
        m.package = e["package"].str();
        m.enabled = e["enabled"].boolean(true);
        for (const auto& [k, v] : e["choices"].items()) m.choices[k] = static_cast<int>(v.num());
        if (m.id.empty() || !valid_key(m.package)) { error = "perfil con una entrada no válida"; return false; }
        if (!ids.insert(m.id).second) { error = "perfil con un mod repetido: " + m.id; return false; }
        out.mods.push_back(std::move(m));
    }
    return true;
}

// ------------------------------------------------------------------ almacén

ModStore::ModStore(std::string game_id, fs::path data_root, fs::path extracted_dir)
    : game_(std::move(game_id)), root_(std::move(data_root)), extracted_(std::move(extracted_dir)) {}

fs::path ModStore::download_path() const { return root_ / "cache" / "downloads" / (op_id() + ".zip"); }

bool ModStore::open(std::string& error) {
    support_ = game_support(game_);
    if (!support_) { error = "este juego no admite mods en el launcher"; return false; }
    std::error_code ec;
    for (const char* d : {"packages", "staging", "profiles", "vdisc", "locks", "cache"}) fs::create_directories(root_ / d, ec);
    if (ec) { error = "no se puede crear la carpeta de mods"; return false; }
    // Recuperación (D10): lo que quedó en staging o como vdisc a medias es de una operación interrumpida.
    StoreLock lock(root_ / "locks" / "store.lock");
    if (lock.held()) {
        for (auto& e : fs::directory_iterator(root_ / "staging", ec)) fs::remove_all(e.path(), ec);
        for (auto& e : fs::directory_iterator(root_ / "vdisc", ec))
            if (!fs::exists(e.path() / ".complete")) fs::remove_all(e.path(), ec);
        for (auto& e : fs::directory_iterator(root_ / "profiles", ec))
            if (e.path().extension() == ".tmp") fs::remove(e.path(), ec);
    }
    // Disco base del usuario (solo lectura).
    auto boot = read_bytes(extracted_ / "sys" / "boot.bin");
    auto fst = read_bytes(extracted_ / "sys" / "fst.bin");
    if (boot.size() < 0x440 || fst.empty()) { error = "no se encuentra el disco extraído del juego"; return false; }
    if (!parse_fst(fst, fst_, error)) return false;
    base_.disc_id.assign(reinterpret_cast<const char*>(boot.data()), 6);
    base_.fst_sha256 = sha256_hex(std::string(fst.begin(), fst.end()));
    base_.base_content_id = base_.disc_id + ":" + base_.fst_sha256.substr(0, 16);
    if (!fs::exists(root_ / "profiles" / "default.json")) {
        Profile p;
        std::string e;
        write_file_atomic(root_ / "profiles" / "default.json", p.to_json().pretty(), e);
    }
    return true;
}

bool ModStore::preview(const fs::path& source, PackagePlan& plan, std::string& error) const {
    auto src = open_source(source, error);
    if (!src) return false;
    plan = analyze_package(*src, fst_, *support_, base_.disc_id);
    return true;
}

bool ModStore::install(const fs::path& source, const ModMeta& meta, InstalledMod& out, std::string& error,
                       const Progress& progress, const Cancel& cancel, const InstallHashes& expected) {
    StoreLock lock(root_ / "locks" / "store.lock");
    if (!lock.held()) { error = "hay otra operación de mods en curso (¿otro launcher abierto?)"; return false; }
    auto src = open_source(source, error);
    if (!src) return false;
    PackagePlan plan = analyze_package(*src, fst_, *support_, base_.disc_id);
    if (!plan.ok()) {
        error = plan.blocking.empty() ? "el paquete no cambia nada" : plan.blocking.front();
        return false;
    }
    fs::path stage = root_ / "staging" / op_id();
    std::error_code ec;
    auto cleanup = [&] { fs::remove_all(stage, ec); };
    auto sources = plan.referenced_sources();
    uint64_t done = 0;
    Json files = Json::object();
    Json content = Json::object();
    for (const auto& s : sources) {
        if (cancel && cancel()) { cleanup(); error = "cancelado"; return false; }
        fs::path dest = join_inside(stage / "content", s);
        if (dest.empty()) { cleanup(); error = "ruta no segura: " + s; return false; }
        if (progress) progress(s, done, plan.payload_bytes);
        if (!src->extract(s, dest, error, cancel)) { cleanup(); return false; }
        std::string sha = sha256_file_hex(dest);
        uint64_t size = fs::file_size(dest, ec);
        if (sha.empty() || size > 0xFFFFFFFFull) { cleanup(); error = "no se puede validar " + s; return false; }
        done += size;
        Json f = Json::object();
        f.set("sha256", sha);
        f.set("size", size);
        files.set(s, f);
        content.set(s, sha);
    }
    std::string content_sha = sha256_hex(content.dump());
    std::string package_sha = fs::is_directory(source, ec) ? content_sha : sha256_file_hex(source);
    if (package_sha.empty() || (!expected.package_sha256.empty() && package_sha != expected.package_sha256) ||
        (!expected.content_sha256.empty() && content_sha != expected.content_sha256)) {
        cleanup();
        error = "el archivo descargado no coincide con los hashes de la versión solicitada";
        return false;
    }
    if (cancel && cancel()) { cleanup(); error = "cancelado"; return false; }

    Json mod = Json::object();
    std::string id = meta.id;
    if (id.empty()) id = meta.provider + "." + slug(meta.name.empty() ? to_utf8(source.stem()) : meta.name);
    std::string id_norm, why;
    if (id.size() > 80 || !normalize_relative(id, id_norm, why) || id_norm != id || id.find('/') != std::string::npos) {
        cleanup();
        error = "identificador de mod no válido";
        return false;
    }
    mod.set("schema", 1);
    mod.set("id", id);
    mod.set("name", meta.name.empty() ? to_utf8(source.stem()) : meta.name);
    mod.set("version", meta.version.empty() ? "local-" + content_sha.substr(0, 8) : meta.version);
    mod.set("game_id", game_);
    mod.set("provider", meta.provider);
    mod.set("source_id", meta.source_id);
    mod.set("source_page", meta.source_page);
    mod.set("license", meta.license);
    mod.set("author", meta.author);
    mod.set("source_file_id", meta.source_file_id);
    mod.set("source_md5", meta.source_md5);
    mod.set("package_sha256", package_sha);
    mod.set("content_sha256", content_sha);
    mod.set("format", format_name(plan.format));
    mod.set("kind", "data");   // solo se instalan mods de datos; los de código se rechazan antes
    mod.set("loader", support_->loader);
    mod.set("base_content_id", base_.base_content_id);
    mod.set("installed_at", iso_now());
    mod.set("files", files);
    Json patches = Json::object();
    for (const auto& [pid, ops] : plan.patches) {
        Json arr = Json::array();
        for (const auto& op : ops) {
            Json o = Json::object();
            o.set("disc", op.disc);
            o.set("src", op.src);
            arr.push(o);
        }
        patches.set(pid, arr);
    }
    mod.set("patches", patches);
    Json always = Json::array();
    for (const auto& a : plan.always) always.push(a);
    mod.set("always", always);
    Json options = Json::array();
    for (const auto& o : plan.options) {
        Json jo = Json::object();
        jo.set("id", o.id);
        jo.set("name", o.name);
        jo.set("default", o.default_choice);
        Json chs = Json::array();
        for (const auto& c : o.choices) {
            Json jc = Json::object();
            jc.set("name", c.name);
            Json ps = Json::array();
            for (const auto& p : c.patches) ps.push(p);
            jc.set("patches", ps);
            chs.push(jc);
        }
        jo.set("choices", chs);
        options.push(jo);
    }
    mod.set("options", options);
    Json warnings = Json::array();
    for (const auto& w : plan.warnings) warnings.push(w);
    mod.set("warnings", warnings);

    if (!write_file_atomic(stage / "mod.json", mod.pretty(), error)) { cleanup(); return false; }
    std::string key = sha256_hex(id + "\n" + mod["version"].str() + "\n" + content_sha).substr(0, 32);
    fs::path final_dir = root_ / "packages" / key;
    if (fs::exists(final_dir / "mod.json")) {
        cleanup();   // ya instalado idéntico: no se duplica (B11)
        if (find_installed(key, out)) return verify(out, error);
        error = "la copia instalada del paquete no se puede leer";
        return false;
    } else {
        fs::remove_all(final_dir, ec);
        fs::rename(stage, final_dir, ec);
        if (ec) { cleanup(); error = "no se puede completar la instalación"; return false; }
    }
    if (progress) progress("", plan.payload_bytes, plan.payload_bytes);
    return find_installed(key, out) ? true : (error = "instalación incompleta", false);
}

bool ModStore::find_installed(const std::string& key, InstalledMod& out) const {
    if (!valid_key(key)) return false;
    std::string text, error;
    if (!read_file_text(root_ / "packages" / key / "mod.json", text)) return false;
    Json j;
    if (!parse_json(text, j, error) || j["game_id"].str() != game_) return false;
    out.key = key;
    out.json = std::move(j);
    return true;
}

std::vector<InstalledMod> ModStore::installed() const {
    std::vector<InstalledMod> out;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(root_ / "packages", ec)) {
        InstalledMod m;
        if (find_installed(to_utf8(e.path().filename()), m)) out.push_back(std::move(m));
    }
    std::sort(out.begin(), out.end(), [](const InstalledMod& a, const InstalledMod& b) { return a.name() < b.name(); });
    return out;
}

bool ModStore::verify(const InstalledMod& mod, std::string& error) const {
    for (const auto& [src, info] : mod.json["files"].items()) {
        fs::path p = join_inside(root_ / "packages" / mod.key / "content", src);
        if (p.empty() || sha256_file_hex(p) != info["sha256"].str()) {
            error = "el archivo " + src + " de «" + mod.name() + "» falta o ha cambiado";
            return false;
        }
    }
    return true;
}

bool ModStore::uninstall(const std::string& key, std::string& error) {
    StoreLock lock(root_ / "locks" / "store.lock");
    if (!lock.held()) { error = "hay otra operación de mods en curso"; return false; }
    if (!valid_key(key)) { error = "clave no válida"; return false; }
    for (const auto& name : profiles()) {
        Profile p;
        std::string e;
        if (!load_profile(name, p, e)) continue;
        for (const auto& m : p.mods)
            if (m.package == key) { error = "lo usa el perfil «" + name + "»: quítalo de ese perfil primero"; return false; }
    }
    std::error_code ec;
    fs::path trash = root_ / "staging" / ("borrar-" + op_id());
    fs::rename(root_ / "packages" / key, trash, ec);   // atómico; si se corta, open() lo limpia
    if (ec) { error = "no se puede desinstalar"; return false; }
    fs::remove_all(trash, ec);
    return true;
}

std::vector<std::string> ModStore::profiles() const {
    std::vector<std::string> out;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(root_ / "profiles", ec)) {
        if (e.path().extension() != ".json") continue;
        std::string n = to_utf8(e.path().stem());
        if (valid_profile_name(n)) out.push_back(n);
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool ModStore::load_profile(const std::string& name, Profile& out, std::string& error) const {
    if (!valid_profile_name(name)) { error = "nombre de perfil no válido"; return false; }
    std::string text;
    if (!read_file_text(root_ / "profiles" / (name + ".json"), text, 1 << 20)) {
        if (name == "default") { out = Profile{}; return true; }
        error = "no existe el perfil «" + name + "»";
        return false;
    }
    Json j;
    if (!parse_json(text, j, error) || !Profile::from_json(j, out, error)) return false;
    out.name = name;
    return true;
}

bool ModStore::save_profile(Profile profile, std::string& error) {
    if (!valid_profile_name(profile.name)) { error = "nombre de perfil no válido (letras, números, - y _)"; return false; }
    StoreLock lock(root_ / "locks" / "store.lock");
    if (!lock.held()) { error = "hay otra operación de mods en curso"; return false; }
    Profile old;
    std::string e;
    if (load_profile(profile.name, old, e)) profile.epoch = std::max(profile.epoch, old.epoch) + 1;
    else profile.epoch += 1;
    return write_file_atomic(root_ / "profiles" / (profile.name + ".json"), profile.to_json().pretty(), error);
}

bool ModStore::delete_profile(const std::string& name, std::string& error) {
    if (name == "default") { error = "el perfil «default» no se puede borrar"; return false; }
    if (!valid_profile_name(name)) { error = "nombre de perfil no válido"; return false; }
    if (active_profile() == name && !set_active_profile("default", error)) return false;
    std::error_code ec;
    fs::remove(root_ / "profiles" / (name + ".json"), ec);
    return !ec;
}

std::string ModStore::active_profile() const {
    std::string text;
    if (!read_file_text(root_ / "active.txt", text, 256)) return "default";
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
    return valid_profile_name(text) && fs::exists(root_ / "profiles" / (text + ".json")) ? text : "default";
}

bool ModStore::set_active_profile(const std::string& name, std::string& error) {
    if (!valid_profile_name(name)) { error = "nombre de perfil no válido"; return false; }
    return write_file_atomic(root_ / "active.txt", name + "\n", error);
}

bool ModStore::begin_session(Profile session, const std::string& lobby, std::string& error) {
    std::string current = active_profile();
    session.name = "session";
    session.restore = current == "session" ? (session.restore.empty() ? "default" : session.restore) : current;
    if (current == "session") {
        Profile old;
        std::string e;
        if (load_profile("session", old, e) && !old.restore.empty()) session.restore = old.restore;
    }
    session.lobby = lobby.substr(0, 64);
    return save_profile(session, error) && set_active_profile("session", error);
}

bool ModStore::end_session(std::string& error) {
    if (active_profile() != "session") return true;
    Profile s;
    std::string back = "default";
    if (load_profile("session", s, error) && !s.restore.empty() && s.restore != "session") back = s.restore;
    if (!set_active_profile(back, error)) return false;
    std::error_code ec;
    fs::remove(root_ / "profiles" / "session.json", ec);
    return true;
}
Effective ModStore::compute(const Profile& profile) const {
    Effective eff;
    std::map<std::string, size_t> where;   // fold_key(disc) -> índice en eff.files
    int order = 0;
    for (const auto& entry : profile.mods) {
        if (!entry.enabled) continue;
        InstalledMod mod;
        if (!find_installed(entry.package, mod) || mod.id() != entry.id) {
            eff.problems.push_back("falta el mod «" + entry.id + "» (paquete no instalado)");
            continue;
        }
        if (mod.json["base_content_id"].str() != base_.base_content_id) {
            eff.problems.push_back("«" + mod.name() + "» se instaló para otro disco base: vuelve a instalarlo");
            continue;
        }
        Json choices = Json::object();
        for (const auto& [k, v] : entry.choices) choices.set(k, v);
        for (const auto& op : effective_ops(mod.json, entry.choices)) {
            const Json& info = mod.json["files"][op.src];
            fs::path src = join_inside(root_ / "packages" / mod.key / "content", op.src);
            if (src.empty() || !info.is_object() || !fst_.find(op.disc)) {
                eff.problems.push_back("«" + mod.name() + "» tiene una entrada no válida: " + op.disc);
                continue;
            }
            EffectiveFile f{fst_.find(op.disc)->path, src, info["sha256"].str(), static_cast<uint64_t>(info["size"].num()), mod.id()};
            auto key = fold_key(f.disc);
            auto w = where.find(key);
            if (w != where.end()) {
                if (eff.files[w->second].mod_id != f.mod_id)
                    eff.conflicts.push_back(f.disc + ": «" + eff.files[w->second].mod_id + "» queda tapado por «" + f.mod_id + "» (orden de carga)");
                eff.files[w->second] = f;
            } else {
                where[key] = eff.files.size();
                eff.files.push_back(f);
            }
        }
        Json m = Json::object();
        m.set("id", mod.id());
        m.set("name", mod.name());
        m.set("version", mod.version());
        m.set("source", mod.json["provider"].str());
        m.set("source_id", mod.json["source_id"].str());
        m.set("source_file_id", mod.json["source_file_id"].str());
        m.set("source_md5", mod.json["source_md5"].str());
        m.set("package_sha256", mod.json["package_sha256"].str());
        m.set("effective_content_sha256", mod.content_sha());
        m.set("load_order", order++);
        m.set("config_sha256", sha256_hex(choices.dump()));
        m.set("required_for_online", true);   // política estricta: todo mod activo cuenta
        eff.manifest_mods.push(m);
    }
    if (eff.files.empty() && eff.manifest_mods.size() == 0) return eff;
    // Fingerprint canónico: identidad de base + loader + mods en orden + bytes que de verdad se aplican.
    std::sort(eff.files.begin(), eff.files.end(), [](const EffectiveFile& a, const EffectiveFile& b) { return fold_key(a.disc) < fold_key(b.disc); });
    Json canon = Json::object();
    canon.set("fingerprint_schema", kFingerprintSchema);
    canon.set("game_id", game_);
    canon.set("base", base_.base_content_id);
    canon.set("loader", support_->loader);
    Json mods = Json::array();
    for (const auto& m : eff.manifest_mods.elements()) {
        Json c = Json::object();
        for (const char* k : {"id", "version", "effective_content_sha256", "load_order", "config_sha256"}) c.set(k, m[k]);
        mods.push(c);
    }
    canon.set("mods", mods);
    Json files = Json::array();
    for (const auto& f : eff.files) {
        Json pair = Json::array();
        pair.push(fold_key(f.disc));
        pair.push(f.sha256);
        files.push(pair);
    }
    canon.set("effective", files);
    eff.fingerprint = sha256_hex(canon.dump());
    return eff;
}

bool ModStore::build_vdisc(const Effective& eff, fs::path& out, std::string& error, const Cancel& cancel) {
    if (!eff.problems.empty()) { error = eff.problems.front(); return false; }
    if (eff.vanilla()) { out = extracted_; return true; }
    StoreLock lock(root_ / "locks" / "store.lock");
    if (!lock.held()) { error = "hay otra operación de mods en curso"; return false; }
    std::string fp16 = eff.fingerprint.substr(0, 16);
    fs::path dir = root_ / "vdisc" / fp16;
    std::string mark;
    std::error_code ec;
    if (read_file_text(dir / ".complete", mark, 256) && mark == eff.fingerprint) {
        // Reutilizar, pero comprobando que los archivos del mod siguen siendo los que dice el fingerprint.
        bool intact = true;
        for (const auto& f : eff.files) {
            if (sha256_file_hex(dir / "files" / from_utf8(f.disc)) != f.sha256) { intact = false; break; }
        }
        if (intact) { out = dir; return true; }
    }
    // Solo se guarda un disco virtual por juego: los demás se borran (son enlaces, no datos).
    for (auto& e : fs::directory_iterator(root_ / "vdisc", ec)) fs::remove_all(e.path(), ec);
    fs::path tmp = root_ / "vdisc" / (fp16 + ".tmp");
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp / "sys", ec);
    auto fail = [&](const std::string& why) {
        fs::remove_all(tmp, ec);
        error = why;
        return false;
    };
    // sys/: enlaces a los originales salvo fst.bin, que se regenera.
    for (const char* name : {"boot.bin", "bi2.bin", "apploader.img", "main.dol"}) {
        fs::path from = extracted_ / "sys" / name;
        if (!fs::exists(from)) continue;
        fs::create_hard_link(from, tmp / "sys" / name, ec);
        if (ec) return fail("no se pueden crear enlaces duros (¿el juego y los datos del launcher están en discos distintos?)");
    }
    std::map<uint32_t, uint32_t> sizes;
    std::map<std::string, const EffectiveFile*> overrides;
    for (const auto& f : eff.files) {
        const FstFile* ff = fst_.find(f.disc);
        if (!ff) return fail("ruta desconocida: " + f.disc);
        if (f.size != ff->size) sizes[ff->index] = static_cast<uint32_t>(f.size);
        overrides[fold_key(ff->path)] = &f;
    }
    // Fin del disco ocupado: archivos y la propia FST (su desplazamiento está en boot.bin, 0x424).
    auto boot = read_bytes(extracted_ / "sys" / "boot.bin");
    uint64_t fst_end = 0;
    if (boot.size() >= 0x430) {
        auto be = [&](size_t o) { return (uint32_t(boot[o]) << 24) | (uint32_t(boot[o + 1]) << 16) | (uint32_t(boot[o + 2]) << 8) | boot[o + 3]; };
        fst_end = (uint64_t(be(0x424)) << 2) + (uint64_t(be(0x428)) << 2);
    }
    auto fst = rebuild_fst(fst_, sizes, fst_end);
    if (fst.size() != fst_.raw.size()) return fail("FST regenerada con tamaño distinto");
    if (!write_file_atomic(tmp / "sys" / "fst.bin", std::string(fst.begin(), fst.end()), error)) return fail(error);
    size_t n = 0;
    for (const auto& file : fst_.files) {
        if (cancel && (++n % 256) == 0 && cancel()) return fail("cancelado");
        fs::path dest = tmp / "files" / from_utf8(file.path);
        fs::create_directories(dest.parent_path(), ec);
        auto it = overrides.find(fold_key(file.path));
        fs::path from = it != overrides.end() ? it->second->source : extracted_ / "files" / from_utf8(file.path);
        fs::create_hard_link(from, dest, ec);
        if (ec) {
            // Archivo original ausente: el juego tampoco lo leería; se deja sin enlazar.
            if (it == overrides.end() && !fs::exists(from)) { ec.clear(); continue; }
            return fail("no se pueden crear enlaces duros (¿el juego y los datos del launcher están en discos distintos?)");
        }
    }
    // Revalidación de lo que se aplica de verdad antes de declarar el disco listo.
    for (const auto& f : eff.files) {
        if (sha256_file_hex(tmp / "files" / from_utf8(fst_.find(f.disc)->path)) != f.sha256)
            return fail("el archivo " + f.disc + " no coincide con su hash: reinstala el mod");
    }
    if (!write_file_atomic(tmp / ".complete", eff.fingerprint, error)) return fail(error);
    fs::rename(tmp, dir, ec);
    if (ec) return fail("no se puede completar el disco virtual");
    out = dir;
    return true;
}

}  // namespace wl::mods
