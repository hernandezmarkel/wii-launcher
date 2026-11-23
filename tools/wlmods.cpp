// Herramienta de línea de comandos del gestor de mods (pruebas reproducibles y scripts). Usa exactamente el
// mismo núcleo que la interfaz. Uso:
//   wlmods <juego> preview <zip|carpeta>
//   wlmods <juego> install <zip|carpeta> [--name N] [--version V]
//   wlmods <juego> list
//   wlmods <juego> profile <perfil> show | add <clave> | remove <id> | enable <id> | disable <id> | activate
//   wlmods <juego> fingerprint [--profile P]
//   wlmods <juego> launch [--profile P] [--seconds N]
// Variables: WL_MODS_EXPERIMENTAL=1 permite lanzar un juego sin prueba A/B (solo para hacer esa prueba).
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>

#include <zlib.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "wl/catalog.h"
#include "wl/mods/gamebanana.h"
#include "wl/mods/launch.h"
#include "wl/mods/store.h"
#include "wl/process.h"

using namespace wl;
using namespace wl::mods;

static int fail(const std::string& why) {
    std::cerr << "error: " << why << "\n";
    return 1;
}

// ---- mod de prueba (igual que tools/crear-mod-textura.py, sin Python): invierte una textura TPL de
// locale/<idioma>/menu/common.arc.lz del disco DEL USUARIO y escribe un ZIP sin comprimir. El resultado es
// determinista: dos PCs con el mismo disco obtienen exactamente los mismos bytes.
static uint32_t upd_crc32(const std::vector<uint8_t>& d) { return uint32_t(crc32(0L, d.data(), uInt(d.size()))); }

namespace prueba {

std::vector<uint8_t> lz11_decode(const std::vector<uint8_t>& d) {
    if (d.size() < 4 || d[0] != 0x11) return {};
    size_t size = d[1] | (d[2] << 8) | (d[3] << 16), p = 4;
    if (size == 0) { size = d[4] | (d[5] << 8) | (d[6] << 16) | (size_t(d[7]) << 24); p = 8; }
    std::vector<uint8_t> out;
    out.reserve(size);
    while (out.size() < size && p < d.size()) {
        uint8_t flags = d[p++];
        for (int bit = 0; bit < 8 && out.size() < size && p < d.size(); ++bit) {
            if (!(flags & (0x80 >> bit))) { out.push_back(d[p++]); continue; }
            int ind = d[p] >> 4;
            size_t n, disp;
            if (ind == 0) { n = (((d[p] & 0xF) << 4) | (d[p + 1] >> 4)) + 0x11; disp = (((d[p + 1] & 0xF) << 8) | d[p + 2]) + 1; p += 3; }
            else if (ind == 1) { n = (((d[p] & 0xF) << 12) | (d[p + 1] << 4) | (d[p + 2] >> 4)) + 0x111; disp = (((d[p + 2] & 0xF) << 8) | d[p + 3]) + 1; p += 4; }
            else { n = ind + 1; disp = (((d[p] & 0xF) << 8) | d[p + 1]) + 1; p += 2; }
            if (disp > out.size()) return {};
            for (size_t i = 0; i < n; ++i) out.push_back(out[out.size() - disp]);
        }
    }
    return out.size() == size ? out : std::vector<uint8_t>{};
}

std::vector<uint8_t> lz11_literal(const std::vector<uint8_t>& raw) {
    std::vector<uint8_t> out{0x11, uint8_t(raw.size()), uint8_t(raw.size() >> 8), uint8_t(raw.size() >> 16)};
    for (size_t i = 0; i < raw.size(); i += 8) {
        out.push_back(0);
        for (size_t j = i; j < i + 8 && j < raw.size(); ++j) out.push_back(raw[j]);
    }
    while (out.size() % 4) out.push_back(0);
    return out;
}

uint32_t be32(const std::vector<uint8_t>& d, size_t o) { return (uint32_t(d[o]) << 24) | (uint32_t(d[o + 1]) << 16) | (uint32_t(d[o + 2]) << 8) | d[o + 3]; }
uint16_t be16(const std::vector<uint8_t>& d, size_t o) { return uint16_t((d[o] << 8) | d[o + 1]); }

int invert_tpl(std::vector<uint8_t>& buf, size_t start) {
    if (be32(buf, start) != 0x0020AF30) return 0;
    uint32_t images = be32(buf, start + 4), table = be32(buf, start + 8);
    int changed = 0;
    for (uint32_t k = 0; k < images; ++k) {
        uint32_t hdr = be32(buf, start + table + k * 8);
        uint16_t h = be16(buf, start + hdr), w = be16(buf, start + hdr + 2);
        uint32_t fmt = be32(buf, start + hdr + 4), data = be32(buf, start + hdr + 8);
        static const int bpp[] = {4, 8, 8, 16};
        static const int bw[] = {8, 8, 8, 4}, bh[] = {8, 4, 4, 4};
        if (fmt > 3) return -1;
        size_t size = size_t((w + bw[fmt] - 1) / bw[fmt] * bw[fmt]) * ((h + bh[fmt] - 1) / bh[fmt] * bh[fmt]) * bpp[fmt] / 8;
        size_t o = start + data;
        for (size_t j = o; j < o + size && j < buf.size(); ++j) {
            if (fmt <= 1) buf[j] ^= 0xFF;
            else if (fmt == 2) buf[j] ^= 0x0F;
            else if ((j - o) % 2 == 1) buf[j] ^= 0xFF;
        }
        ++changed;
    }
    return changed;
}

void le16(std::string& o, uint16_t v) { o += char(v & 0xFF); o += char(v >> 8); }
void le32(std::string& o, uint32_t v) { for (int i = 0; i < 4; ++i) o += char((v >> (8 * i)) & 0xFF); }

// ZIP de una sola entrada, sin comprimir (método 0), con fecha fija para que sea reproducible.
std::string zip_one(const std::string& name, const std::vector<uint8_t>& data) {
    uint32_t crc = upd_crc32(data);
    std::string out, cd;
    auto header = [&](std::string& o, bool central) {
        le32(o, central ? 0x02014b50 : 0x04034b50);
        if (central) le16(o, 20);
        le16(o, 20); le16(o, 0); le16(o, 0); le16(o, 0); le16(o, 0x21);
        le32(o, crc); le32(o, uint32_t(data.size())); le32(o, uint32_t(data.size()));
        le16(o, uint16_t(name.size())); le16(o, 0);
        if (central) { le16(o, 0); le16(o, 0); le16(o, 0); le32(o, 0); le32(o, 0); }
        o += name;
    };
    header(out, false);
    out.append(reinterpret_cast<const char*>(data.data()), data.size());
    uint32_t cd_off = uint32_t(out.size());
    header(cd, true);
    out += cd;
    le32(out, 0x06054b50); le16(out, 0); le16(out, 0); le16(out, 1); le16(out, 1);
    le32(out, uint32_t(cd.size())); le32(out, cd_off); le16(out, 0);
    return out;
}

}  // namespace prueba

int main(int argc, char** argv) {
    std::vector<std::string> a;
    std::map<std::string, std::string> opt;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        if (s.rfind("--", 0) == 0 && i + 1 < argc) opt[s.substr(2)] = argv[++i];
        else a.push_back(s);
    }
    if (a.size() < 2) {
        std::cerr << "uso: wlmods <juego> preview|install|list|profile|fingerprint|launch ...\n";
        return 1;
    }
    const std::string game = a[0], cmd = a[1];
    // Como wii-launcher.exe: la carpeta manifests\ junto al exe manda; la del repo solo en desarrollo.
    std::string manifest_dir = WL_MANIFEST_DIR;
    {
        std::error_code ec;
        fs::path exe = fs::absolute(fs::path(argv[0]), ec).parent_path();
#ifdef _WIN32
        wchar_t buf[MAX_PATH];
        DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
        if (n > 0 && n < MAX_PATH) exe = fs::path(std::wstring(buf, n)).parent_path();
#endif
        if (fs::is_directory(exe / "manifests", ec)) manifest_dir = (exe / "manifests").string();
    }
    if (opt.count("manifests")) manifest_dir = opt["manifests"];
    Catalog catalog = load_catalog(manifest_dir);
    const Manifest* manifest = nullptr;
    for (const auto& g : catalog.games)
        if (g.manifest.game_id == game) manifest = &g.manifest;
    if (!manifest) return fail("juego desconocido: " + game);
    fs::path root = install_root(*manifest);
    ModStore store(game, default_mods_root(game), extracted_dir(game, root));
    std::string error;
    if (!store.open(error)) return fail(error);

    if (cmd == "crear-mod-prueba" && a.size() >= 3) {
        std::string textura = opt.count("textura") ? opt["textura"] : "copyright.tpl";
        std::string idioma = opt.count("idioma") ? opt["idioma"] : "sp_EU";
        std::string rel = "locale/" + idioma + "/menu/common.arc.lz";
        fs::path src = extracted_dir(game, root) / "files" / fs::path(rel);
        std::ifstream in(src, std::ios::binary);
        std::vector<uint8_t> packed((std::istreambuf_iterator<char>(in)), {});
        std::vector<uint8_t> raw = prueba::lz11_decode(packed);
        if (raw.size() < 32 || raw[0] != 0x55 || raw[1] != 0xAA || raw[2] != 0x38 || raw[3] != 0x2D) return fail("no se puede leer " + rel);
        uint32_t root_off = prueba::be32(raw, 4), count = prueba::be32(raw, root_off + 8);
        size_t names = root_off + size_t(count) * 12;
        int changed = 0;
        for (uint32_t i = 1; i < count; ++i) {
            size_t e = root_off + size_t(i) * 12;
            if (raw[e]) continue;   // carpeta
            uint32_t name_off = prueba::be32(raw, e) & 0x00FFFFFF;
            std::string name(reinterpret_cast<const char*>(&raw[names + name_off]));
            if (name == textura) changed += prueba::invert_tpl(raw, prueba::be32(raw, e + 4));
        }
        if (changed <= 0) return fail("no se encuentra o no se admite " + textura);
        std::string zip = prueba::zip_one("PruebaTextura/files/" + rel, prueba::lz11_literal(raw));
        std::ofstream(fs::path(a[2]), std::ios::binary) << zip;
        std::cout << "creado " << a[2] << " (" << zip.size() << " bytes, " << textura << " invertida)\n";
        return 0;
    }
    if (cmd == "preview" && a.size() >= 3) {
        PackagePlan plan;
        if (!store.preview(fs::path(a[2]), plan, error)) return fail(error);
        std::cout << "formato: " << format_name(plan.format) << "\nbytes: " << plan.payload_bytes << "\n";
        for (const auto& [id, ops] : plan.patches)
            for (const auto& op : ops) std::cout << "aplica [" << id << "] " << op.disc << " <- " << op.src << "\n";
        for (const auto& o : plan.options) std::cout << "opción " << o.id << " (por defecto " << o.default_choice << ")\n";
        for (const auto& s : plan.ignored) std::cout << "ignora " << s << "\n";
        for (const auto& s : plan.warnings) std::cout << "aviso " << s << "\n";
        for (const auto& s : plan.blocking) std::cout << "BLOQUEA " << s << "\n";
        return plan.ok() ? 0 : 2;
    }
    if (cmd == "install" && a.size() >= 3) {
        ModMeta meta;
        meta.name = opt["name"];
        meta.version = opt["version"];
        InstalledMod mod;
        if (!store.install(fs::path(a[2]), meta, mod, error)) return fail(error);
        std::cout << "instalado " << mod.id() << " " << mod.version() << " clave " << mod.key << "\n";
        return 0;
    }
    if (cmd == "list") {
        for (const auto& m : store.installed())
            std::cout << m.key << "\t" << m.id() << "\t" << m.version() << "\t" << m.json["format"].str() << "\t" << m.name() << "\n";
        std::cout << "perfil activo: " << store.active_profile() << "\n";
        return 0;
    }
    if (cmd == "profile" && a.size() >= 4) {
        Profile p;
        if (!store.load_profile(a[2], p, error)) {
            if (a[3] != "add") return fail(error);
            p = Profile{};
        }
        p.name = a[2];
        const std::string sub = a[3];
        if (sub == "show") {
            std::cout << p.to_json().pretty();
            return 0;
        }
        if (sub == "activate") {
            if (!store.set_active_profile(p.name, error)) return fail(error);
            std::cout << "perfil activo: " << p.name << "\n";
            return 0;
        }
        if (a.size() < 5) return fail("falta el argumento");
        if (sub == "add") {
            InstalledMod m;
            if (!store.find_installed(a[4], m)) return fail("no hay ningún paquete con esa clave");
            for (auto& e : p.mods)
                if (e.id == m.id()) { e.package = m.key; e.enabled = true; goto save; }
            p.mods.push_back({m.id(), m.key, true, {}});
        } else if (sub == "remove" || sub == "enable" || sub == "disable") {
            bool found = false;
            for (auto it = p.mods.begin(); it != p.mods.end(); ++it) {
                if (it->id != a[4]) continue;
                found = true;
                if (sub == "remove") { p.mods.erase(it); break; }
                it->enabled = sub == "enable";
            }
            if (!found) return fail("ese mod no está en el perfil");
        } else {
            return fail("subcomando desconocido");
        }
    save:
        if (!store.save_profile(p, error)) return fail(error);
        std::cout << "perfil " << p.name << " guardado\n";
        return 0;
    }
    if (cmd == "fingerprint") {
        Profile p;
        if (!store.load_profile(opt.count("profile") ? opt["profile"] : store.active_profile(), p, error)) return fail(error);
        Effective eff = store.compute(p);
        for (const auto& s : eff.problems) std::cout << "PROBLEMA " << s << "\n";
        for (const auto& s : eff.conflicts) std::cout << "conflicto " << s << "\n";
        for (const auto& f : eff.files) std::cout << "archivo " << f.disc << " " << f.sha256.substr(0, 16) << " (" << f.mod_id << ")\n";
        std::cout << "base " << store.base().base_content_id << "\nfingerprint " << eff.fingerprint << "\n";
        return eff.problems.empty() ? 0 : 2;
    }
    if (cmd == "catalog" || cmd == "gb-check" || cmd == "gb-install") {
        auto http = upd::make_winhttp_client();
        GameBananaProvider gb(*http, store.root() / "cache" / "catalog");
        int64_t gid = gamebanana_game(game);
        if (cmd == "catalog") {
            CatalogPage page;
            auto t0 = std::chrono::steady_clock::now();
            if (!gb.list(gid, opt.count("page") ? std::atoi(opt["page"].c_str()) : 1, a.size() > 2 ? a[2] : "", page, error)) return fail(error);
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            for (const auto& it : page.items) std::cout << it.id << "\t[" << it.category << "]\t" << it.name << "\t" << it.author << "\n";
            std::cout << page.items.size() << " de " << page.total << (page.from_cache ? " (caché)" : "") << " en " << (int)ms << " ms\n";
            return 0;
        }
        if (a.size() < 3) return fail("falta el id del mod");
        CatalogDetail d;
        if (!gb.detail(std::atoll(a[2].c_str()), d, error)) return fail(error);
        if (!d.available_for(gid)) return fail("la ficha no está disponible para este juego");
        std::cout << d.item.name << " · " << d.item.author << " · licencia: " << d.license.substr(0, 120) << "\n";
        for (const auto& f : d.files) {
            std::vector<std::string> tree;
            std::string te;
            QuickCheck q;
            if (gb.file_tree(f.id, tree, te)) q = quick_check(tree, store.fst(), store.support(), store.base().disc_id);
            else q.reason = te;
            const char* v = q.verdict == QuickVerdict::Compatible ? "COMPATIBLE" : q.verdict == QuickVerdict::Incompatible ? "INCOMPATIBLE" : "POR COMPROBAR";
            std::cout << "  archivo " << f.id << " " << f.name << " " << f.size << " B av=" << f.av_result << " -> " << v << " " << q.reason
                      << (q.files ? " (" + std::to_string(q.files) + " archivos)" : "") << "\n";
            if (cmd == "gb-install" && opt.count("file") && std::to_string(f.id) == opt["file"]) {
                if (q.verdict == QuickVerdict::Incompatible) return fail("incompatible: " + q.reason);
                if (!f.supported_archive()) return fail("se necesita un ZIP compatible de hasta 512 MiB");
                fs::path dl = store.download_path();
                if (!gb.download(f, dl, error)) return fail(error);
                ModMeta meta = catalog_meta("gamebanana", d, f);
                InstalledMod mod;
                bool ok = store.install(dl, meta, mod, error);
                std::error_code ec;
                fs::remove(dl, ec);
                if (!ok) return fail(error);
                std::cout << "instalado " << mod.id() << " " << mod.version() << " clave " << mod.key << "\n";
            }
        }
        return 0;
    }
    if (cmd == "launch") {
        auto adapter = make_adapter(*manifest);
        if (!adapter) return fail("este juego no se puede lanzar");
        InstallStatus s = adapter->probe(root);
        if (!s.ready()) return fail(s.problem);
        LaunchSpec spec = adapter->offline(root);
        LaunchMods info;
        const char* exp = std::getenv("WL_MODS_EXPERIMENTAL");
        if (!prepare_launch(game, root, spec, info, error, opt["profile"], exp && std::string(exp) == "1")) return fail(error);
        if (opt.count("seconds")) {
            if (spec.args.empty()) spec.args.push_back("games/" + game + "/extracted");
            spec.args.push_back(opt["seconds"]);
        }
        std::cout << "perfil " << info.profile << " (época " << info.epoch << "), " << info.mods << " mods, fingerprint "
                  << info.fingerprint << "\n";
        for (const auto& arg : spec.args) std::cout << "arg: " << arg << "\n";
        Spawned p = spawn(spec);
        if (!p.ok) return fail(p.error);
        int code = opt.count("seconds") ? wait_exit(p.pid, (std::atoi(opt["seconds"].c_str()) + 60) * 1000) : 0;
        std::cout << "pid " << p.pid << " salida " << code << "\n";
        return 0;
    }
    return fail("orden desconocida");
}
