// Herramienta de línea de comandos del prototipo: valida el catálogo, comprueba instalaciones
// y lanza un juego sin ninguna UI. Uso:
//   wlaunch list | probe | dry-run <game_id> | play <game_id> [--manifests <carpeta>]
#include <chrono>
#include <cstring>
#include <iostream>

#include "wl/catalog.h"
#include "wl/process.h"

using namespace wl;

int main(int argc, char** argv) {
    std::string dir = WL_MANIFEST_DIR;
    std::vector<std::string> a;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--manifests") && i + 1 < argc) dir = argv[++i];
        else a.push_back(argv[i]);
    }
    std::string cmd = a.empty() ? "list" : a[0];
    auto t0 = std::chrono::steady_clock::now();
    Catalog c = load_catalog(dir);
    auto t1 = std::chrono::steady_clock::now();
    for (const auto& e : c.errors) std::cerr << "manifest rechazado: " << e << "\n";
    if (cmd == "list" || cmd == "probe") {
        for (const auto& g : c.games) {
            const Manifest& m = g.manifest;
            std::cout << m.game_id << "\t" << m.display_name << "\t" << status_name(m.status) << "\t[";
            for (size_t i = 0; i < m.capabilities.size(); ++i) std::cout << (i ? "," : "") << m.capabilities[i];
            std::cout << "]";
            if (cmd == "probe") {
                auto adapter = make_adapter(m);
                if (!adapter) { std::cout << "\tsin adaptador (previsto)\n"; continue; }
                auto p0 = std::chrono::steady_clock::now();
                InstallStatus s = adapter->probe(install_root(m));
                auto p1 = std::chrono::steady_clock::now();
                std::cout << "\t" << (s.ready() ? "LISTO" : "NO LISTO: " + s.problem) << "\tversión sala=" << adapter->compat_token(s)
                          << "\t(" << std::chrono::duration<double, std::milli>(p1 - p0).count() << " ms)";
            }
            std::cout << "\n";
        }
        std::cout << "catálogo cargado en " << std::chrono::duration<double, std::milli>(t1 - t0).count() << " ms\n";
        return c.errors.empty() ? 0 : 2;
    }
    if ((cmd == "dry-run" || cmd == "play") && a.size() >= 2) {
        for (const auto& g : c.games) {
            if (g.manifest.game_id != a[1]) continue;
            auto adapter = make_adapter(g.manifest);
            if (!adapter) { std::cerr << "Este juego está previsto: todavía no se puede lanzar.\n"; return 3; }
            fs::path root = install_root(g.manifest);
            InstallStatus s = adapter->probe(root);
            if (!s.ready()) { std::cerr << s.problem << "\n"; return 4; }
            LaunchSpec spec = adapter->offline(root);
            std::cout << "exe: " << spec.executable.u8string() << "\ncwd: " << spec.working_dir.u8string() << "\n";
            for (const auto& [k, v] : spec.env) std::cout << "env: " << k << "=" << v << "\n";
            if (cmd == "dry-run") return 0;
            fs::create_directories(root / "logs");
            Spawned p = spawn(spec);
            if (!p.ok) { std::cerr << p.error << "\n"; return 5; }
            std::cout << "iniciado, pid " << p.pid << "\n";
            return 0;
        }
        std::cerr << "juego desconocido\n";
        return 1;
    }
    std::cerr << "uso: wlaunch list | probe | dry-run <id> | play <id> [--manifests carpeta]\n";
    return 1;
}
