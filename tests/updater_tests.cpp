// Tests del updater sin red: versiones, firma, verificador, lista de orígenes, feed con un HTTP falso,
// descarga a staging y recuperación de una instalación interrumpida. Ver docs/updater/matriz-resultados.md.
#include <windows.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>

#include "wl/adapter.h"
#include "wl/update.h"

using namespace wl::upd;

extern int failures, checks;
#define UCHECK(x)                                                                    \
    do {                                                                             \
        ++checks;                                                                    \
        if (!(x)) {                                                                  \
            ++failures;                                                              \
            std::cerr << "FALLO updater_tests.cpp:" << __LINE__ << ": " #x "\n";    \
        }                                                                            \
    } while (0)

namespace {

struct FakeHttp : HttpClient {
    struct Reply {
        int status = 200;
        std::string body;
        std::map<std::string, std::string> headers;
    };
    std::map<std::string, Reply> routes;
    std::vector<std::string> asked;
    std::vector<std::vector<std::pair<std::string, std::string>>> asked_headers;

    HttpResponse get(const HttpRequest& req) override {
        asked.push_back(req.url);
        asked_headers.push_back(req.headers);
        HttpResponse r;
        auto it = routes.find(req.url);
        if (it == routes.end()) {
            r.status = 0;
            r.error = "Sin conexión a Internet o DNS no disponible.";
            return r;
        }
        r.status = it->second.status;
        r.headers = it->second.headers;
        if (r.status == 200 && it->second.body.size() > req.max_bytes) {
            r.status = 0;
            r.error = "La respuesta supera el tamaño permitido.";
            return r;
        }
        if (r.status == 200 && !req.to_file.empty()) std::ofstream(req.to_file, std::ios::binary | std::ios::trunc) << it->second.body;
        else r.body = it->second.body;
        return r;
    }
    int count(const std::string& url) const {
        int n = 0;
        for (const auto& a : asked) n += a == url;
        return n;
    }
};

std::string g_priv, g_pub;

std::string manifest(const std::string& version, const std::string& channel, const std::string& files, const std::string& extra = "") {
    return "format = 1\napp = wii-launcher\nchannel = " + channel + "\nversion = " + version +
           "\nplatform = windows-x86_64\npublished = 2026-11-18T00:00:00Z\n" + extra + "note = Cambios de prueba.\n" + files;
}

std::string file_line(const std::string& content, const std::string& asset, const std::string& path) {
    return "file = " + sha256_hex(content) + " " + std::to_string(content.size()) + " " + asset + " " + path + "\n";
}

std::string sign(const std::string& text) {
    std::string s;
    ecdsa_sign(g_priv, text, s);
    return s;
}

VerifyPolicy policy(const std::string& installed = "1.0.0", const std::string& channel = "stable") {
    VerifyPolicy p;
    p.installed = installed;
    p.channel = channel;
    p.public_key = g_pub;
    return p;
}

const std::string kExeV2 = "MZ nuevo launcher 1.0.1";
const std::string kToml = "manifest_version = 1\n";

}  // namespace

void run_updater_tests() {
    // ---- versiones (VersionProvider)
    SemVer v;
    UCHECK(parse_semver("v1.2.3", v) && v.major == 1 && v.minor == 2 && v.patch == 3 && v.stable());
    UCHECK(parse_semver("1.0.0-beta.2", v) && v.pre == "beta.2" && !v.stable());
    UCHECK(!parse_semver("1.0", v) && !parse_semver("01.0.0", v) && !parse_semver("1.0.0-", v) && !parse_semver("1.0.0-be/ta", v));
    UCHECK(compare_versions("1.0.1", "1.0.0") > 0 && compare_versions("1.0.0", "1.0.0-beta.9") > 0);
    UCHECK(compare_versions("1.0.0-beta.10", "1.0.0-beta.2") > 0 && compare_versions("1.10.0", "1.9.9") > 0);
    UCHECK(compare_versions("basura", "1.0.0") < 0);
    UCHECK(parse_semver(current_version(), v));

    // ---- criptografía
    UCHECK(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    UCHECK(ecdsa_generate(g_priv, g_pub) && g_pub.size() == 128 && g_priv.size() == 192);
    std::string sig;
    UCHECK(ecdsa_sign(g_priv, "hola", sig) && sig.size() == 128);
    UCHECK(ecdsa_verify(g_pub, "hola", sig));
    UCHECK(!ecdsa_verify(g_pub, "hola!", sig));                      // SEC-01: un byte cambiado
    std::string other_priv, other_pub;
    ecdsa_generate(other_priv, other_pub);
    UCHECK(!ecdsa_verify(other_pub, "hola", sig));                   // SEC-08: firmado con otra clave
    UCHECK(!ecdsa_verify(g_pub, "hola", "zz") && !ecdsa_verify("", "hola", sig));

    // ---- manifiesto y verificador (UpdateVerifier)
    std::string files = file_line(kExeV2, "wii-launcher.exe", "wii-launcher.exe") + file_line(kToml, "manifests.mp9.toml", "manifests/mp9.toml");
    std::string good = manifest("1.0.1", "stable", files);
    VerifyResult r = verify_manifest(good, sign(good), policy());
    UCHECK(r.ok && r.manifest.files.size() == 2 && r.manifest.notes.size() == 1);
    std::string tampered = good;
    tampered.replace(tampered.find("1.0.1"), 5, "1.0.9");
    UCHECK(!verify_manifest(tampered, sign(good), policy()).ok);     // SEC-02: manifiesto falsificado
    UCHECK(!verify_manifest(good, sign(good), [] { auto p = policy(); p.public_key = ""; return p; }()).ok);
    UCHECK(!verify_manifest(good, sign(good), policy("1.0.1")).ok);  // UPD-06: misma versión, sin bucle
    UCHECK(!verify_manifest(good, sign(good), policy("1.2.0")).ok);  // UPD-15: no baja de versión
    {
        auto p = policy();
        p.highest_seen = "1.0.4";
        UCHECK(!verify_manifest(good, sign(good), p).ok);            // SEC-03: replay de una versión vieja firmada
        p.highest_seen = "1.0.1";
        UCHECK(verify_manifest(good, sign(good), p).ok);
    }
    std::string beta = manifest("1.1.0-beta.1", "beta", files);
    UCHECK(!verify_manifest(beta, sign(beta), policy()).ok);                      // UPD-14: stable no recibe beta
    UCHECK(verify_manifest(beta, sign(beta), policy("1.0.0", "beta")).ok);
    std::string lie = manifest("1.1.0", "beta", files);
    UCHECK(!verify_manifest(lie, sign(lie), policy("1.0.0", "beta")).ok);         // canal y versión incoherentes
    std::string arm = good;
    arm.replace(arm.find("windows-x86_64"), 14, "windows-arm64");
    UCHECK(!verify_manifest(arm, sign(arm), policy()).ok);                        // SEC-04: otra arquitectura
    std::string otherapp = good;
    otherapp.replace(otherapp.find("app = wii-launcher"), 18, "app = mp9-recomp");
    UCHECK(!verify_manifest(otherapp, sign(otherapp), policy()).ok);
    std::string minfrom = manifest("1.0.1", "stable", files, "min_from = 1.0.1\n");
    UCHECK(!verify_manifest(minfrom, sign(minfrom), policy()).ok);
    // SEC-05: rutas peligrosas o fuera de la lista del launcher, aunque estén firmadas
    for (const char* bad : {"../wii-launcher.exe", "C:/Windows/x.exe", "/etc/x", "manifests/../../x.toml", "manifests\\x.toml",
                            "instaladores/mp9online-launcher.exe", "servidor-mkw/Iniciar-ServidorLAN.ps1", "manifests/x.exe",
                            "fonts/sub/x.ttf", "prefs.ini", "manifests/.toml"}) {
        std::string t = manifest("1.0.1", "stable", files + "file = " + sha256_hex("x") + " 1 x.bin " + bad + "\n");
        UCHECK(!verify_manifest(t, sign(t), policy()).ok);
        UCHECK(!allowed_update_path(bad));
    }
    UCHECK(allowed_update_path("wii-launcher.exe") && allowed_update_path("manifests/zz_mkwii.toml") && allowed_update_path("fonts/Inter-Regular.ttf"));
    UCHECK(!allowed_asset_name("../x") && !allowed_asset_name("a/b") && !allowed_asset_name(".x") && allowed_asset_name("manifests.mp9.toml"));
    {
        UpdateManifest m;
        std::string err;
        UCHECK(!parse_manifest_text(manifest("1.0.1", "stable", file_line(kToml, "a.toml", "manifests/a.toml")), m, err));  // sin exe
        UCHECK(!parse_manifest_text(manifest("1.0.1", "stable", files + files), m, err));                                    // repetidos
        UCHECK(!parse_manifest_text(manifest("1.0.1", "stable", "file = " + sha256_hex("x") + " 999999999 a.exe wii-launcher.exe\n"), m, err));  // SEC-06
        UCHECK(!parse_manifest_text(manifest("1.0.1", "stable", files, "postinstall = cmd /c calc\n"), m, err));             // SEC-07
        UCHECK(!parse_manifest_text(std::string(70000, '#'), m, err));
    }

    // ---- orígenes permitidos (NET-09)
    UCHECK(allowed_url("https://github.com/a/b/releases/latest/download/update.txt"));
    UCHECK(allowed_url("https://release-assets.githubusercontent.com/x?y=z"));
    UCHECK(!allowed_url("http://github.com/a/b"));                 // HTTP plano
    UCHECK(!allowed_url("https://github.com.evil.example/a"));
    UCHECK(!allowed_url("https://evil.example/github.com"));
    UCHECK(!allowed_url("https://github.com:8443/a"));
    UCHECK(!allowed_url("https://user@github.com/a"));
    UCHECK(!allowed_url("file:///C:/x") && !allowed_url("ftp://github.com/x"));
    UCHECK(allowed_url("http://127.0.0.1:8765/x") == testing_build());
    UCHECK(!allowed_url("http://localhost:8765/x") && !allowed_url("http://192.168.1.2/x"));
    {
        FakeHttp http;
        http.routes["https://github.com/r"] = {302, "", {{"location", "https://evil.example/payload"}}};
        HttpRequest q;
        q.url = "https://github.com/r";
        HttpResponse resp = fetch(http, q);
        UCHECK(resp.status == 0 && http.count("https://evil.example/payload") == 0);  // NET-09: no se llega a pedir
        http.routes["https://github.com/s"] = {302, "", {{"location", "https://objects.githubusercontent.com/ok"}}};
        http.routes["https://objects.githubusercontent.com/ok"] = {200, "datos", {}};
        q.url = "https://github.com/s";
        q.headers = {{"If-None-Match", "\"e\""}};
        resp = fetch(http, q);
        UCHECK(resp.status == 200 && resp.body == "datos" && http.asked_headers.back().empty());  // sin cabeceras de API en la CDN
        http.routes["https://github.com/loop"] = {302, "", {{"location", "/loop"}}};
        q.url = "https://github.com/loop";
        UCHECK(fetch(http, q).status == 0 && http.count("https://github.com/loop") == 6);
    }

    // ---- feed (ReleaseFeed)
    FeedConfig feed;
    feed.repo = "dueno/wii-launcher";
    const std::string latest = "https://github.com/dueno/wii-launcher/releases/latest/download/update.txt";
    const std::string sigurl = "https://github.com/dueno/wii-launcher/releases/download/v1.0.1/update.txt.sig";
    {
        FakeHttp http;  // NET-01: sin red
        CheckResult c = check_for_update(http, feed, policy());
        UCHECK(c.kind == CheckKind::Error && !c.error.empty());
        http.routes[latest] = {404, "", {}};
        UCHECK(check_for_update(http, feed, policy()).kind == CheckKind::NoReleases);
        http.routes[latest] = {403, "", {}};
        c = check_for_update(http, feed, policy());
        UCHECK(c.kind == CheckKind::Error && c.http_status == 403);       // NET-03
        http.routes[latest] = {429, "", {{"retry-after", "120"}}};
        c = check_for_update(http, feed, policy());
        UCHECK(c.kind == CheckKind::RateLimited && c.retry_after_s >= 15 * 60);  // NET-04: nunca menos de 15 min
        http.routes[latest] = {403, "", {{"x-ratelimit-remaining", "0"}}};
        UCHECK(check_for_update(http, feed, policy()).kind == CheckKind::RateLimited);
        http.routes[latest] = {500, "", {}};
        UCHECK(check_for_update(http, feed, policy()).kind == CheckKind::Error);  // NET-05
        http.routes[latest] = {200, good, {}};
        c = check_for_update(http, feed, policy("1.0.1"));
        UCHECK(c.kind == CheckKind::UpToDate && http.count(sigurl) == 0);  // UPD-04: al día, sin pedir la firma
        http.routes[sigurl] = {200, sign(good) + "\n", {}};
        c = check_for_update(http, feed, policy());
        UCHECK(c.kind == CheckKind::Available && c.manifest.version == "1.0.1");  // UPD-01
        http.routes[sigurl] = {200, sign(tampered), {}};
        UCHECK(check_for_update(http, feed, policy()).kind == CheckKind::Error);
    }
    {
        FakeHttp http;  // beta con ETag
        FeedConfig bf = feed;
        bf.channel = "beta";
        const std::string api = "https://api.github.com/repos/dueno/wii-launcher/releases?per_page=10";
        std::string bm = manifest("1.1.0-beta.2", "beta", files);
        http.routes[api] = {200, R"([{"tag_name":"v1.1.0-beta.1"},{"tag_name": "v1.1.0-beta.2"},{"tag_name":"v1.0.1"}])", {{"etag", "W/\"abc\""}}};
        http.routes["https://github.com/dueno/wii-launcher/releases/download/v1.1.0-beta.2/update.txt"] = {200, bm, {}};
        http.routes["https://github.com/dueno/wii-launcher/releases/download/v1.1.0-beta.2/update.txt.sig"] = {200, sign(bm), {}};
        CheckResult c = check_for_update(http, bf, policy("1.0.0", "beta"));
        UCHECK(c.kind == CheckKind::Available && c.manifest.version == "1.1.0-beta.2" && c.beta_etag == "W/\"abc\"");
        bf.beta_etag = c.beta_etag;
        bf.beta_tag = c.beta_tag;
        http.routes[api] = {304, "", {}};
        c = check_for_update(http, bf, policy("1.0.0", "beta"));
        UCHECK(c.kind == CheckKind::Available && c.manifest.version == "1.1.0-beta.2");
        bool sent_etag = false;
        for (const auto& h : http.asked_headers[http.asked_headers.size() - 3])
            sent_etag |= h.first == "If-None-Match" && h.second == "W/\"abc\"";
        UCHECK(sent_etag);
    }

    // ---- descarga a staging (UpdateDownloader) y recuperación (UpdateInstaller)
    // Carpeta única por ejecución: std::rand() sin semilla repetía el nombre y una ejecución anterior a medias interfería.
    fs::path root = fs::temp_directory_path() /
                    ("wl-upd-test-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    fs::path install = root / "WiiPorts", staging = root / "staging";
    std::error_code e;
    fs::create_directories(install / "manifests", e);
    std::ofstream(install / "wii-launcher.exe", std::ios::binary) << "MZ viejo";
    std::ofstream(install / "manifests" / "mp9.toml", std::ios::binary) << kToml;  // igual que en 1.0.1: no se descarga
    {
        FakeHttp http;
        http.routes[latest] = {200, good, {}};
        http.routes[sigurl] = {200, sign(good), {}};
        const std::string exe_url = "https://github.com/dueno/wii-launcher/releases/download/v1.0.1/wii-launcher.exe";
        CheckResult c = check_for_update(http, feed, policy());
        DownloadProgress prog;
        std::string err;
        http.routes[exe_url] = {404, "", {}};
        UCHECK(!download_update(http, feed, c, install, staging, prog, err) && !err.empty());  // NET-03 en descarga
        http.routes[exe_url] = {200, kExeV2.substr(0, 5), {}};  // NET-06: truncado
        UCHECK(!download_update(http, feed, c, install, staging, prog, err));
        UCHECK(!fs::exists(staging / "files" / "wii-launcher.exe", e) && !fs::exists(staging / "files" / "wii-launcher.exe.part", e));
        http.routes[exe_url] = {200, kExeV2, {}};
        UCHECK(download_update(http, feed, c, install, staging, prog, err));
        UCHECK(prog.total == kExeV2.size() && prog.done == kExeV2.size());
        UCHECK(http.count("https://github.com/dueno/wii-launcher/releases/download/v1.0.1/manifests.mp9.toml") == 0);  // PERF-04: diferencial
        std::string verr;
        UCHECK(verify_staging(staging, policy(), verr).ok);
        std::ofstream(staging / "files" / "wii-launcher.exe", std::ios::binary | std::ios::app) << "x";  // staging manipulado
        UCHECK(!verify_staging(staging, policy(), verr).ok);
        DownloadProgress cancelled;
        cancelled.cancel = true;
        fs::remove_all(staging, e);
        UCHECK(!download_update(http, feed, c, install, staging, cancelled, err));  // INST-05: cancelada
    }
    // INST-03/04: diario a medias tras un cierre forzado -> se restaura lo viejo
    {
        const char* old_lad = std::getenv("WL_LOCALAPPDATA");
        std::string saved = old_lad ? old_lad : "";
        _putenv_s("WL_LOCALAPPDATA", (root / "lad").u8string().c_str());
        fs::path backup = install / ".wl-backup" / "1.0.0";
        fs::create_directories(backup, e);
        fs::create_directories(updates_dir(), e);
        fs::rename(install / "wii-launcher.exe", backup / "wii-launcher.exe", e);
        std::ofstream(install / "wii-launcher.exe", std::ios::binary) << kExeV2;
        std::ofstream(updates_dir() / "journal.txt", std::ios::binary)
            << "from = 1.0.0\nto = 1.0.1\ntarget = " << install.u8string() << "\nbackup = " << backup.u8string()
            << "\nstate = applying\nmoved = wii-launcher.exe\nplaced = wii-launcher.exe\n";
        std::string msg = recover_interrupted_update();
        std::ifstream in(install / "wii-launcher.exe", std::ios::binary);
        std::string now((std::istreambuf_iterator<char>(in)), {});
        UCHECK(!msg.empty() && now == "MZ viejo");
        UCHECK(recover_interrupted_update().empty());  // ya resuelto: no repite
        UpdateLock a, b;
        UCHECK(a.acquire() && !b.acquire(0));          // UPD-10: una sola instalación a la vez
        a.release();
        UCHECK(b.acquire(0));
        b.release();
        _putenv_s("WL_LOCALAPPDATA", saved.c_str());
    }
    // ---- instaladores pequeños (manifiesto aparte, desde beta.4)
    UCHECK(allowed_installer_path("instaladores/mp9online-launcher.exe") && allowed_installer_path("instaladores/wiipartyrecomp-launcher.exe"));
    UCHECK(!allowed_installer_path("instaladores/mkwii/Instalar-MKWii-LAN.ps1") && !allowed_installer_path("instaladores/../wii-launcher.exe") &&
           !allowed_installer_path("wii-launcher.exe") && !allowed_installer_path("instaladores/x.ps1") && !allowed_update_path("instaladores/mp9online-launcher.exe"));
    {
        const char* old_lad = std::getenv("WL_LOCALAPPDATA");
        std::string saved = old_lad ? old_lad : "";
        _putenv_s("WL_LOCALAPPDATA", (root / "lad2").u8string().c_str());
        const std::string mp9v1 = "MZ instalador mp9 viejo", mp9v2 = "MZ instalador mp9 con parche online", wp = "MZ instalador wiiparty";
        auto inst_manifest = [&](const std::string& ver, const std::string& app) {
            return "format = 1\napp = " + app + "\nchannel = beta\nversion = " + ver + "\nplatform = windows-x86_64\n" +
                   file_line(mp9v2, "instaladores.mp9online-launcher.exe", "instaladores/mp9online-launcher.exe") +
                   file_line(wp, "instaladores.wiipartyrecomp-launcher.exe", "instaladores/wiipartyrecomp-launcher.exe");
        };
        std::string im = inst_manifest("1.0.0-beta.4", kInstallersAppId);
        UCHECK(verify_installers_manifest(im, sign(im), g_pub, "1.0.0-beta.4").ok);
        UCHECK(!verify_installers_manifest(im, sign(im), g_pub, "1.0.0-beta.5").ok);   // ligado a su versión (anti-replay)
        UCHECK(!verify_installers_manifest(im, sign(good), g_pub, "1.0.0-beta.4").ok); // firma de otro texto
        std::string wrong_app = inst_manifest("1.0.0-beta.4", "wii-launcher");
        UCHECK(!verify_installers_manifest(wrong_app, sign(wrong_app), g_pub, "1.0.0-beta.4").ok);
        std::string sneaky = im + file_line("x", "wii-launcher.exe", "wii-launcher.exe");  // no puede colar el exe del launcher
        UCHECK(!verify_installers_manifest(sneaky, sign(sneaky), g_pub, "1.0.0-beta.4").ok);
        UpdateManifest um;
        std::string uerr;
        UCHECK(!parse_manifest_text(im, um, uerr));  // un launcher viejo lo rechazaría: por eso va en otro archivo

        fs::create_directories(install / "instaladores", e);
        std::ofstream(install / "instaladores" / "mp9online-launcher.exe", std::ios::binary) << mp9v1;
        std::ofstream(install / "instaladores" / "wiipartyrecomp-launcher.exe", std::ios::binary) << wp;  // igual: no se baja
        const std::string base = "https://github.com/dueno/wii-launcher/releases/download/v1.0.0-beta.4/";
        FakeHttp http;
        http.routes[base + "instaladores.txt"] = {404, "", {}};
        InstallersResult ir = update_installers(http, feed, install, "1.0.0-beta.4", g_pub);
        UCHECK(ir.ok && ir.updated.empty());  // Release sin instaladores.txt: nada que hacer
        http.routes[base + "instaladores.txt"] = {200, im, {}};
        http.routes[base + "instaladores.txt.sig"] = {200, sign(im), {}};
        http.routes[base + "instaladores.mp9online-launcher.exe"] = {200, mp9v2.substr(0, 10), {}};  // truncado
        ir = update_installers(http, feed, install, "1.0.0-beta.4", g_pub);
        std::ifstream a(install / "instaladores" / "mp9online-launcher.exe", std::ios::binary);
        UCHECK(!ir.ok && std::string((std::istreambuf_iterator<char>(a)), {}) == mp9v1);  // el viejo sigue intacto
        a.close();
        http.routes[base + "instaladores.mp9online-launcher.exe"] = {200, mp9v2, {}};
        ir = update_installers(http, feed, install, "1.0.0-beta.4", g_pub);
        std::ifstream b(install / "instaladores" / "mp9online-launcher.exe", std::ios::binary);
        UCHECK(ir.ok && ir.updated.size() == 1 && ir.updated[0] == "instaladores/mp9online-launcher.exe" &&
               std::string((std::istreambuf_iterator<char>(b)), {}) == mp9v2);
        b.close();
        UCHECK(http.count(base + "instaladores.wiipartyrecomp-launcher.exe") == 0);  // sin cambios: no se descarga
        UCHECK(fs::exists(install / ".wl-backup" / "instaladores" / "mp9online-launcher.exe", e));
        ir = update_installers(http, feed, install, "1.0.0-beta.4", g_pub);
        UCHECK(ir.ok && ir.updated.empty());  // segunda vez: nada
        _putenv_s("WL_LOCALAPPDATA", saved.c_str());
    }

    UCHECK(folder_writable(install));
    UCHECK(redact(std::string(std::getenv("USERPROFILE") ? std::getenv("USERPROFILE") : "C:/x") + "\\a").find("%USERPROFILE%") != std::string::npos ||
           !std::getenv("USERPROFILE"));  // SEC-09
    fs::remove_all(root, e);
}
