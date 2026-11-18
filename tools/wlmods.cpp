// Herramienta de línea de comandos del gestor de mods (pruebas reproducibles y scripts). Usa exactamente el
// mismo núcleo que la interfaz. Uso:
//   wlmods <juego> preview <zip|carpeta>
//   wlmods <juego> install <zip|carpeta> [--name N] [--version V]
//   wlmods <juego> list
//   wlmods <juego> profile <perfil> show | add <clave> | remove <id> | enable <id> | disable <id> | activate
//   wlmods <juego> fingerprint [--profile P]
//   wlmods <juego> launch [--profile P] [--seconds N]
// Variables: WL_MODS_EXPERIMENTAL=1 permite lanzar un juego sin prueba A/B (solo para hacer esa prueba).
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "wl/catalog.h"
#include "wl/mods/launch.h"
#include "wl/mods/store.h"
#include "wl/process.h"

using namespace wl;
using namespace wl::mods;

static int fail(const std::string& why) {
    std::cerr << "error: " << why << "\n";
    return 1;
}

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
    Catalog catalog = load_catalog(WL_MANIFEST_DIR);
    const Manifest* manifest = nullptr;
    for (const auto& g : catalog.games)
        if (g.manifest.game_id == game) manifest = &g.manifest;
    if (!manifest) return fail("juego desconocido: " + game);
    fs::path root = install_root(*manifest);
    ModStore store(game, default_mods_root(game), extracted_dir(game, root));
    std::string error;
    if (!store.open(error)) return fail(error);

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
