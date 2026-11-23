#include "wl/mods/package.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <memory>
#include <set>

#ifdef _WIN32
#include <windows.h>
#endif

#include "wl/mods/safe_path.h"
#include "wl/mods/xml.h"
#include "wl/mods/zip.h"

namespace wl::mods {

// ------------------------------------------------------------------ soporte por juego

const GameModSupport* game_support(const std::string& game_id) {
    // Wii Party y Mario Party 9 comparten motor (wii_recomp): sus REL se enlazan con código recompilado
    // estático (engine/src/core/modules.cpp), así que cambiarlos no cambia el juego: se bloquean.
    // `proven` solo pasa a true tras la prueba A/B en el port real (docs/mods/06-pruebas_resultados.md).
    static const GameModSupport wiiparty{"wiiparty", "wl-vdisc-1", true, {"rel/"}, {".rel", ".rel.lz", ".dol"}};
    static const GameModSupport mp9{"mp9", "wl-vdisc-1", true, {"modules/", "rel/"}, {".rel", ".rel.lz", ".dol"}};
    if (game_id == wiiparty.game_id) return &wiiparty;
    if (game_id == mp9.game_id) return &mp9;
    return nullptr;
}

std::string format_name(ModFormat f) {
    switch (f) {
        case ModFormat::Files: return "files";
        case ModFormat::Riivolution: return "riivolution";
        default: return "unknown";
    }
}

std::vector<std::string> PackagePlan::referenced_sources() const {
    std::set<std::string> s;
    for (const auto& [id, ops] : patches)
        for (const auto& op : ops) s.insert(op.src);
    return {s.begin(), s.end()};
}

// ------------------------------------------------------------------ orígenes

namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool ends_with(const std::string& s, const std::string& suf) {
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

std::string basename(const std::string& p) {
    size_t s = p.rfind('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

class ZipSource : public PackageSource {
public:
    bool open(const fs::path& p, std::string& error) {
        if (!zip_.open(p, error)) return false;
        for (const auto& z : zip_.entries()) {
            if (z.directory) continue;
            index_[z.name] = &z;
            entries_.push_back({z.name, z.size});
        }
        return true;
    }
    const std::vector<SourceEntry>& entries() const override { return entries_; }
    bool read_small(const std::string& name, std::string& out, size_t max_bytes, std::string& error) const override {
        auto it = index_.find(name);
        if (it == index_.end()) { error = "no existe en el paquete: " + name; return false; }
        if (it->second->size > max_bytes) { error = "demasiado grande: " + name; return false; }
        fs::path tmp = fs::temp_directory_path() / ("wl-mods-read-" + std::to_string(reinterpret_cast<uintptr_t>(this)) + ".tmp");
        bool ok = zip_.extract(*it->second, tmp, error);
        if (ok) {
            std::ifstream in(tmp, std::ios::binary);
            out.assign(std::istreambuf_iterator<char>(in), {});
        }
        std::error_code ec;
        fs::remove(tmp, ec);
        return ok;
    }
    bool extract(const std::string& name, const fs::path& dest, std::string& error, const std::function<bool()>& cancel) const override {
        auto it = index_.find(name);
        if (it == index_.end()) { error = "no existe en el paquete: " + name; return false; }
        return zip_.extract(*it->second, dest, error, cancel);
    }

private:
    ZipReader zip_;
    std::map<std::string, const ZipEntry*> index_;
    std::vector<SourceEntry> entries_;
};

bool is_reparse(const fs::path& p) {
#ifdef _WIN32
    DWORD a = GetFileAttributesW(p.wstring().c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    std::error_code ec;
    return fs::is_symlink(fs::symlink_status(p, ec));
#endif
}

class FolderSource : public PackageSource {
public:
    bool open(const fs::path& root, std::string& error) {
        root_ = root;
        if (is_reparse(root)) { error = "la carpeta es un enlace o unión: no admitida"; return false; }
        std::error_code ec;
        std::set<std::string> seen;
        uint64_t total = 0;
        for (fs::recursive_directory_iterator it(root, fs::directory_options::none, ec), end; it != end; it.increment(ec)) {
            if (ec) { error = "no se puede recorrer la carpeta"; return false; }
            const fs::path& p = it->path();
            // Nunca seguir enlaces simbólicos, uniones ni otros puntos de reanálisis.
            if (is_reparse(p)) { error = "enlace o unión dentro de la carpeta: " + to_utf8(p.filename()); return false; }
            if (it->is_directory(ec)) continue;
            if (!it->is_regular_file(ec)) { error = "elemento que no es un archivo: " + to_utf8(p.filename()); return false; }
            std::string rel = to_utf8(p.lexically_relative(root)), norm, why;
            if (!normalize_relative(rel, norm, why)) { error = "ruta no segura («" + rel + "»): " + why; return false; }
            if (!seen.insert(fold_key(norm)).second) { error = "archivo repetido salvo mayúsculas: " + norm; return false; }
            uint64_t size = it->file_size(ec);
            total += size;
            if (entries_.size() >= 20000 || total > (1024ull << 20)) { error = "la carpeta es demasiado grande para un mod"; return false; }
            entries_.push_back({norm, size});
        }
        return true;
    }
    const std::vector<SourceEntry>& entries() const override { return entries_; }
    bool read_small(const std::string& name, std::string& out, size_t max_bytes, std::string& error) const override {
        fs::path p = join_inside(root_, name);
        std::error_code ec;
        if (p.empty() || fs::file_size(p, ec) > max_bytes || ec) { error = "no se puede leer: " + name; return false; }
        std::ifstream in(p, std::ios::binary);
        out.assign(std::istreambuf_iterator<char>(in), {});
        return true;
    }
    bool extract(const std::string& name, const fs::path& dest, std::string& error, const std::function<bool()>& cancel) const override {
        fs::path p = join_inside(root_, name);
        if (p.empty() || is_reparse(p)) { error = "ruta no segura: " + name; return false; }
        std::error_code ec;
        fs::create_directories(dest.parent_path(), ec);
        std::ifstream in(p, std::ios::binary);
        std::ofstream out(dest, std::ios::binary | std::ios::trunc);
        if (!in || !out) { error = "no se puede copiar: " + name; return false; }
        std::vector<char> buf(64 * 1024);
        while (in) {
            if (cancel && cancel()) { out.close(); fs::remove(dest, ec); error = "cancelado"; return false; }
            in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            out.write(buf.data(), in.gcount());
        }
        out.close();
        if (!out) { fs::remove(dest, ec); error = "error de escritura (¿disco lleno?)"; return false; }
        return true;
    }

private:
    fs::path root_;
    std::vector<SourceEntry> entries_;
};

}  // namespace

std::unique_ptr<PackageSource> open_source(const fs::path& path, std::string& error) {
    std::error_code ec;
    if (fs::is_directory(path, ec)) {
        auto s = std::make_unique<FolderSource>();
        if (!s->open(path, error)) return nullptr;
        return s;
    }
    std::string ext = lower(to_utf8(path.extension()));
    if (ext == ".7z" || ext == ".rar") {
        error = "los archivos ." + ext.substr(1) + " no se abren directamente: descomprímelo y elige la carpeta";
        return nullptr;
    }
    auto s = std::make_unique<ZipSource>();
    if (!s->open(path, error)) return nullptr;
    return s;
}

// ------------------------------------------------------------------ clasificación

namespace {

enum class Kind { Payload, Doc, Exec, DiscImage, Gecko, DolphinTexture, RiivoXml, RiivoConfig };

Kind classify(const std::string& name) {
    std::string l = lower(name), b = basename(l);
    static const char* exec[] = {".exe", ".dll", ".bat", ".cmd", ".com", ".ps1", ".psm1", ".vbs", ".vbe", ".js", ".jse", ".wsf",
                                 ".msi", ".scr", ".py", ".sh", ".lnk", ".reg", ".hta", ".jar", ".cpl", ".sys", ".app", ".elf", ".dol"};
    static const char* images[] = {".iso", ".wbfs", ".rvz", ".wia", ".gcz", ".ciso", ".nkit", ".wad", ".nfs"};
    static const char* docs[] = {".txt", ".md", ".pdf", ".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp", ".url", ".html", ".htm",
                                 ".rtf", ".nfo", ".docx", ".odt", ".ini", ".json"};
    for (const char* e : exec)
        if (ends_with(l, e)) return Kind::Exec;
    for (const char* e : images)
        if (ends_with(l, e)) return Kind::DiscImage;
    if (ends_with(l, ".gct")) return Kind::Gecko;
    if (b.rfind("tex1_", 0) == 0 && (ends_with(b, ".png") || ends_with(b, ".dds"))) return Kind::DolphinTexture;
    if (ends_with(l, ".xml") && (l.rfind("riivolution/", 0) == 0 || l.find("/riivolution/") != std::string::npos))
        return l.find("riivolution/config/") != std::string::npos ? Kind::RiivoConfig : Kind::RiivoXml;
    // Los .txt dentro de codes/ o txtcodes/ son códigos Gecko en texto.
    if (ends_with(l, ".txt") && (l.find("codes/") != std::string::npos)) return Kind::Gecko;
    for (const char* e : docs)
        if (ends_with(l, e)) return Kind::Doc;
    return Kind::Payload;
}

void check_blocked(PackagePlan& plan, const GameModSupport& support) {
    for (const auto& [id, ops] : plan.patches) {
        for (const auto& op : ops) {
            std::string l = lower(op.disc);
            bool code = false;
            for (const auto& p : support.blocked_prefixes) code |= l.rfind(p, 0) == 0;
            for (const auto& s : support.blocked_suffixes) code |= ends_with(l, s);
            if (code) {
                plan.blocking.push_back("modifica código del juego (" + op.disc +
                                        "): este port es una recompilación estática, ese cambio requiere recompilar");
                return;
            }
        }
    }
}

// ---------------- formato «files»

void analyze_files(const std::vector<SourceEntry>& payload, const Fst& fst, PackagePlan& plan) {
    // Para cada archivo, prefijos que, al quitarlos, dejan una ruta que existe en el disco del usuario.
    std::map<std::string, int> votes;
    for (const auto& e : payload) {
        std::string rest = e.name, prefix;
        while (true) {
            if (fst.find(rest)) votes[lower(prefix)]++;
            size_t s = rest.find('/');
            if (s == std::string::npos) break;
            prefix += rest.substr(0, s + 1);
            rest = rest.substr(s + 1);
        }
    }
    std::string best;
    int best_votes = 0;
    for (const auto& [prefix, n] : votes) {
        if (n > best_votes || (n == best_votes && prefix.size() < best.size())) { best = prefix; best_votes = n; }
    }
    if (best_votes == 0) {
        plan.blocking.push_back("ningún archivo coincide con tu disco: no es un mod de este juego, o es de otra región o versión");
        return;
    }
    auto& ops = plan.patches["main"];
    plan.always.push_back("main");
    for (const auto& e : payload) {
        if (lower(e.name).rfind(best, 0) != 0) { plan.ignored.push_back(e.name + ": fuera de la carpeta del mod"); continue; }
        std::string rel = e.name.substr(best.size());
        const FstFile* f = fst.find(rel);
        if (!f) { plan.ignored.push_back(e.name + ": no existe en tu disco (¿otra región o versión?)"); continue; }
        ops.push_back({f->path, e.name});
        plan.payload_bytes += e.size;
    }
}

// ---------------- formato «riivolution»

struct RiivoContext {
    const std::vector<SourceEntry>* entries;
    std::map<std::string, const SourceEntry*> by_key;
    const Fst* fst;
    PackagePlan* plan;
};

void riivo_patch(const XmlNode& patch, const std::string& sd_root, RiivoContext& ctx) {
    std::string id = patch.attr("id");
    std::string root = patch.attr("root");
    // root es absoluta desde la raíz de la SD; external relativa a root si no empieza por '/'.
    std::string base = sd_root;
    if (!root.empty() && root != "/") {
        std::string r, why;
        std::string joined = (sd_root.empty() ? "" : sd_root + "/") + (root[0] == '/' ? root.substr(1) : root);
        if (normalize_relative(joined, r, why)) base = r;
        else { ctx.plan->blocking.push_back("carpeta raíz no segura en «" + id + "»: " + why); return; }
    }
    auto& ops = ctx.plan->patches[id];
    auto ext_path = [&](const std::string& external, std::string& out) {
        std::string why, joined;
        if (!external.empty() && external[0] == '/') joined = (sd_root.empty() ? "" : sd_root + "/") + external.substr(1);
        else joined = (base.empty() ? "" : base + "/") + external;
        return normalize_relative(joined, out, why);
    };
    for (const auto& op : patch.children) {
        if (op.name == "file") {
            if (op.attrs.count("offset") || op.attrs.count("length")) {
                ctx.plan->blocking.push_back("el parche «" + id + "» modifica trozos de archivos (offset): no soportado");
                continue;
            }
            std::string src;
            if (!ext_path(op.attr("external"), src)) { ctx.plan->blocking.push_back("ruta externa no segura en «" + id + "»"); continue; }
            auto se = ctx.by_key.find(fold_key(src));
            if (se == ctx.by_key.end()) { ctx.plan->ignored.push_back(src + ": lo pide el XML pero no está en el paquete"); continue; }
            std::string disc = op.attr("disc");
            std::vector<const FstFile*> targets;
            if (!disc.empty() && disc[0] == '/') {
                std::string norm, why;
                if (normalize_relative(disc.substr(1), norm, why)) {
                    if (const FstFile* f = ctx.fst->find(norm)) targets.push_back(f);
                }
            } else {
                // Sin '/' inicial, Riivolution busca ese nombre en todo el disco.
                for (const auto& f : ctx.fst->files)
                    if (lower(basename(f.path)) == lower(disc)) targets.push_back(&f);
            }
            if (targets.empty()) {
                if (op.attr("create") == "true") ctx.plan->blocking.push_back("el parche «" + id + "» crea archivos nuevos en el disco: no soportado");
                else ctx.plan->ignored.push_back(disc + ": no existe en tu disco (¿otra región o versión?)");
                continue;
            }
            for (const FstFile* f : targets) ops.push_back({f->path, se->second->name});
        } else if (op.name == "folder") {
            std::string src;
            if (!ext_path(op.attr("external"), src)) { ctx.plan->blocking.push_back("ruta externa no segura en «" + id + "»"); continue; }
            std::string disc = op.attr("disc"), disc_norm, why;
            bool to_root = disc.empty() || disc == "/";
            if (!to_root && !normalize_relative(disc[0] == '/' ? disc.substr(1) : disc, disc_norm, why)) { ctx.plan->blocking.push_back("ruta de disco no segura en «" + id + "»"); continue; }
            bool recursive = op.attr("recursive", "true") != "false";
            std::string prefix = lower(src) + "/";
            for (const auto& e : *ctx.entries) {
                if (lower(e.name).rfind(prefix, 0) != 0) continue;
                std::string rel = e.name.substr(prefix.size());
                if (!recursive && rel.find('/') != std::string::npos) continue;
                if (classify(e.name) == Kind::Doc) continue;
                std::string target = to_root ? rel : disc_norm + "/" + rel;
                if (const FstFile* f = ctx.fst->find(target)) ops.push_back({f->path, e.name});
                else ctx.plan->ignored.push_back(e.name + ": no existe en tu disco (¿otra región o versión?)");
            }
        } else if (op.name == "memory") {
            ctx.plan->blocking.push_back("el parche «" + id + "» escribe en memoria (código PowerPC): no soportado en un port recompilado");
        } else if (op.name == "savegame") {
            ctx.plan->blocking.push_back("el parche «" + id + "» redirige las partidas guardadas: no soportado (protege tus guardados)");
        } else if (op.name == "dlc") {
            ctx.plan->blocking.push_back("el parche «" + id + "» usa contenido descargable (DLC): no soportado");
        } else {
            ctx.plan->blocking.push_back("operación desconocida <" + op.name + "> en «" + id + "»");
        }
    }
}

void analyze_riivolution(const PackageSource& source, const std::vector<std::string>& xmls, const Fst& fst,
                         const std::string& disc_id, PackagePlan& plan) {
    RiivoContext ctx{&source.entries(), {}, &fst, &plan};
    for (const auto& e : source.entries()) ctx.by_key[fold_key(e.name)] = &e;
    for (const auto& xml_name : xmls) {
        std::string text, error;
        if (!source.read_small(xml_name, text, 512 * 1024, error)) { plan.blocking.push_back("no se puede leer " + xml_name + ": " + error); continue; }
        XmlNode doc;
        if (!parse_xml(text, doc, error)) { plan.blocking.push_back(xml_name + ": " + error); continue; }
        const XmlNode* wiidisc = nullptr;
        for (const auto& c : doc.children)
            if (c.name == "wiidisc") wiidisc = &c;
        if (!wiidisc) { plan.blocking.push_back(xml_name + ": no es un XML de Riivolution"); continue; }
        size_t r = lower(xml_name).rfind("riivolution/");
        std::string sd_root = r == 0 ? "" : xml_name.substr(0, r - 1);
        for (const auto& c : wiidisc->children) {
            if (c.name == "id") {
                std::string game = c.attr("game");
                if (!game.empty() && disc_id.compare(0, game.size(), game) != 0) {
                    plan.blocking.push_back("es para otro juego o región (" + game + "; tu disco es " + disc_id + ")");
                }
                plan.region_hint = game;
                std::vector<std::string> regions;
                for (const auto& reg : c.children)
                    if (reg.name == "region") regions.push_back(reg.attr("type"));
                if (!regions.empty() && disc_id.size() >= 4 &&
                    std::find(regions.begin(), regions.end(), std::string(1, disc_id[3])) == regions.end()) {
                    plan.blocking.push_back("es para otra región (tu disco es " + disc_id + ")");
                }
            } else if (c.name == "options") {
                for (const auto& section : c.children) {
                    if (section.name != "section") continue;
                    for (const auto& opt : section.children) {
                        if (opt.name != "option") continue;
                        RiivoOption o;
                        o.name = opt.attr("name");
                        o.id = section.attr("name") + "/" + o.name;
                        o.default_choice = std::atoi(opt.attr("default", "0").c_str());
                        for (const auto& ch : opt.children) {
                            if (ch.name != "choice") continue;
                            RiivoChoice choice{ch.attr("name"), {}};
                            for (const auto& pr : ch.children)
                                if (pr.name == "patch") choice.patches.push_back(pr.attr("id"));
                            o.choices.push_back(std::move(choice));
                        }
                        if (o.default_choice < 0 || o.default_choice > static_cast<int>(o.choices.size())) o.default_choice = 0;
                        // Riivolution deja la opción desactivada si no hay «default»: aquí elegimos la primera
                        // y lo avisamos; el usuario puede cambiarla y cuenta para la compatibilidad online.
                        if (o.default_choice == 0 && !o.choices.empty()) {
                            o.default_choice = 1;
                            plan.warnings.push_back("opción «" + o.id + "» sin valor por defecto: se usa «" + o.choices[0].name + "»");
                        }
                        plan.options.push_back(std::move(o));
                    }
                }
            } else if (c.name == "patch") {
                riivo_patch(c, sd_root, ctx);
            }
        }
    }
    for (auto it = plan.patches.begin(); it != plan.patches.end();) {
        if (it->second.empty()) it = plan.patches.erase(it);
        else ++it;
    }
    if (plan.options.empty() && !plan.patches.empty()) {
        for (const auto& [id, ops] : plan.patches) plan.always.push_back(id);
    }
    for (const auto& [id, ops] : plan.patches)
        for (const auto& op : ops)
            for (const auto& e : source.entries())
                if (e.name == op.src) plan.payload_bytes += e.size;
}

}  // namespace

PackagePlan analyze_package(const PackageSource& source, const Fst& fst, const GameModSupport& support, const std::string& disc_id) {
    PackagePlan plan;
    std::vector<SourceEntry> payload;
    std::vector<std::string> xmls;
    std::set<std::string> reasons;
    for (const auto& e : source.entries()) {
        switch (classify(e.name)) {
            case Kind::Payload: payload.push_back(e); break;
            case Kind::Doc: plan.ignored.push_back(e.name + ": documentación"); break;
            case Kind::RiivoConfig: plan.ignored.push_back(e.name + ": configuración guardada de Riivolution"); break;
            case Kind::RiivoXml: xmls.push_back(e.name); break;
            case Kind::Exec: reasons.insert("contiene programas o scripts (" + basename(e.name) + "): nunca se instalan ni se ejecutan"); break;
            case Kind::DiscImage: reasons.insert("contiene una imagen de disco (" + basename(e.name) + "): no es un mod y no se admite"); break;
            case Kind::Gecko: reasons.insert("contiene códigos Gecko/Ocarina (código PowerPC): no funcionan en un port recompilado"); break;
            case Kind::DolphinTexture: reasons.insert("son texturas de Dolphin (tex1_*.png): este motor no tiene cargador de texturas"); break;
        }
        if (e.size > support.max_file_bytes) reasons.insert("archivo demasiado grande: " + e.name);
        // Un paquete que trae módulos de código (aunque el XML todavía no se haya leído) es un mod de código.
        for (const auto& suf : support.blocked_suffixes)
            if (ends_with(lower(e.name), suf))
                reasons.insert("trae módulos de código (" + basename(e.name) + "): este port es una recompilación estática, ese cambio requiere recompilar");
    }
    plan.blocking.assign(reasons.begin(), reasons.end());
    if (!plan.blocking.empty()) return plan;
    if (!xmls.empty()) {
        plan.format = ModFormat::Riivolution;
        analyze_riivolution(source, xmls, fst, disc_id, plan);
    } else {
        plan.format = ModFormat::Files;
        analyze_files(payload, fst, plan);
    }
    check_blocked(plan, support);
    if (plan.blocking.empty() && plan.patches.empty()) plan.blocking.push_back("el paquete no cambia ningún archivo de tu disco");
    return plan;
}

namespace {
class NameSource : public PackageSource {
public:
    std::vector<SourceEntry> list;
    const std::vector<SourceEntry>& entries() const override { return list; }
    bool read_small(const std::string&, std::string&, size_t, std::string& error) const override { error = "sin descargar"; return false; }
    bool extract(const std::string&, const fs::path&, std::string& error, const std::function<bool()>&) const override { error = "sin descargar"; return false; }
};
}  // namespace

QuickCheck quick_check(const std::vector<std::string>& names, const Fst& fst, const GameModSupport& support, const std::string& disc_id) {
    QuickCheck q;
    NameSource src;
    std::set<std::string> seen;
    bool xml = false;
    for (const auto& raw : names) {
        if (!raw.empty() && (raw.back() == '/' || raw.back() == '\\')) continue;
        std::string norm, why;
        if (!normalize_relative(raw, norm, why)) {
            // Nombres no ASCII en documentación son habituales; en el resto, el ZIP se rechazará al abrirlo.
            if (classify(raw) == Kind::Doc) continue;
            q.verdict = QuickVerdict::Incompatible;
            q.reason = "ruta no segura en el paquete (" + why + ")";
            return q;
        }
        if (!seen.insert(fold_key(norm)).second) continue;
        xml |= classify(norm) == Kind::RiivoXml;
        src.list.push_back({norm, 0});
    }
    PackagePlan plan = analyze_package(src, fst, support, disc_id);
    if (xml) {
        // El XML hay que leerlo: solo cuentan los bloqueos que no vienen de no haberlo descargado.
        for (const auto& b : plan.blocking)
            if (b.find("sin descargar") == std::string::npos) { q.verdict = QuickVerdict::Incompatible; q.reason = b; return q; }
        q.verdict = QuickVerdict::NeedsDownload;
        q.reason = "Riivolution: se comprobará el XML al descargar";
        return q;
    }
    if (!plan.blocking.empty()) { q.verdict = QuickVerdict::Incompatible; q.reason = plan.blocking.front(); return q; }
    for (const auto& [id, ops] : plan.patches) q.files += ops.size();
    q.verdict = q.files ? QuickVerdict::Compatible : QuickVerdict::Incompatible;
    if (!q.files) q.reason = "no cambia ningún archivo de tu disco";
    return q;
}

std::vector<DiscOp> effective_ops(const Json& mod, const std::map<std::string, int>& choices) {
    std::vector<std::string> order;
    for (const auto& a : mod["always"].elements()) order.push_back(a.str());
    for (const auto& o : mod["options"].elements()) {
        std::string id = o["id"].str();
        int pick = static_cast<int>(o["default"].num(0));
        auto it = choices.find(id);
        if (it != choices.end()) pick = it->second;
        if (pick <= 0 || pick > static_cast<int>(o["choices"].size())) continue;
        for (const auto& p : o["choices"][static_cast<size_t>(pick - 1)]["patches"].elements()) order.push_back(p.str());
    }
    std::vector<DiscOp> ops;
    std::map<std::string, size_t> where;  // fold_key(disc) -> índice; el último gana
    for (const auto& id : order) {
        for (const auto& op : mod["patches"][id].elements()) {
            DiscOp d{op["disc"].str(), op["src"].str()};
            auto key = fold_key(d.disc);
            auto w = where.find(key);
            if (w != where.end()) ops[w->second] = d;
            else { where[key] = ops.size(); ops.push_back(d); }
        }
    }
    return ops;
}

}  // namespace wl::mods
