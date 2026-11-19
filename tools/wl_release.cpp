// Herramienta de publicación del updater (la usan el workflow de release y las pruebas de staging).
//
//   wl_release keygen <archivo-clave-privada>          crea un par P-256; imprime la clave pública (hex)
//   wl_release pack <carpeta> <salida> <versión> <canal> [notas.txt]
//        copia los archivos permitidos de <carpeta> a <salida> con su nombre de asset y escribe update.txt
//   wl_release sign <update.txt>                         firma con la clave de WL_UPDATE_SIGNING_KEY (hex)
//        o de WL_UPDATE_SIGNING_KEY_FILE; escribe update.txt.sig
//   wl_release verify <update.txt> <update.txt.sig> <clave-pública-hex> <versión-instalada> [canal]
//
// La clave privada nunca se imprime ni se escribe en el repositorio.
#include <windows.h>

#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>

#include "wl/update.h"

using namespace wl::upd;

namespace {

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

std::string asset_for(const std::string& path) {
    std::string a = path;
    for (char& c : a)
        if (c == '/') c = '.';
    return a;
}

int usage() {
    std::cerr << "uso: wl_release keygen|pack|sign|verify ... (ver cabecera de tools/wl_release.cpp)\n";
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    std::string cmd = argv[1];
    if (cmd == "keygen" && argc == 3) {
        std::string priv, pub;
        if (!ecdsa_generate(priv, pub)) {
            std::cerr << "no se pudo generar la clave\n";
            return 1;
        }
        fs::path out = fs::u8path(argv[2]);
        std::error_code e;
        if (fs::exists(out, e)) {
            std::cerr << "ya existe " << argv[2] << ": no se sobrescribe una clave\n";
            return 1;
        }
        std::ofstream(out, std::ios::binary) << priv << "\n";
        SecureZeroMemory(priv.data(), priv.size());
        std::cout << pub << "\n";
        return 0;
    }
    if (cmd == "pack" && (argc == 6 || argc == 7)) {
        fs::path src = fs::u8path(argv[2]), out = fs::u8path(argv[3]);
        std::string version = argv[4], channel = argv[5];
        SemVer v;
        if (!parse_semver(version, v) || version[0] == 'v' || (channel != "stable" && channel != "beta") || (channel == "stable") != v.stable()) {
            std::cerr << "versión o canal inválidos (stable = x.y.z, beta = x.y.z-beta.N)\n";
            return 1;
        }
        std::error_code e;
        fs::create_directories(out, e);
        std::ostringstream m;
        char stamp[32];
        std::time_t t = std::time(nullptr);
        std::tm tmv{};
        gmtime_s(&tmv, &t);
        std::strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%SZ", &tmv);
        m << "format = 1\napp = " << kAppId << "\nchannel = " << channel << "\nversion = " << version << "\nplatform = " << kPlatform
          << "\npublished = " << stamp << "\n";
        if (argc == 7) {
            std::string raw = slurp(fs::u8path(argv[6]));
            if (raw.rfind("\xEF\xBB\xBF", 0) == 0) raw.erase(0, 3);  // BOM de PowerShell/Notepad
            std::istringstream notes(raw);
            std::string line;
            while (std::getline(notes, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.rfind("- ", 0) == 0) line.erase(0, 2);
                if (!line.empty() && line[0] != '#') m << "note = " << line << "\n";
            }
        }
        int files = 0;
        for (const auto& entry : fs::recursive_directory_iterator(src, e)) {
            if (!entry.is_regular_file()) continue;
            std::string rel = fs::relative(entry.path(), src, e).generic_u8string();
            if (!allowed_update_path(rel)) {
                std::cerr << "omitido (no es del launcher): " << rel << "\n";
                continue;
            }
            std::string asset = asset_for(rel);
            fs::copy_file(entry.path(), out / fs::u8path(asset), fs::copy_options::overwrite_existing, e);
            if (e) {
                std::cerr << "no se pudo copiar " << rel << "\n";
                return 1;
            }
            m << "file = " << sha256_file_hex(entry.path()) << " " << fs::file_size(entry.path()) << " " << asset << " " << rel << "\n";
            ++files;
        }
        std::string text = m.str();
        UpdateManifest check;
        std::string err;
        if (!parse_manifest_text(text, check, err)) {
            std::cerr << "manifiesto inválido: " << err << "\n";
            return 1;
        }
        std::ofstream(out / "update.txt", std::ios::binary) << text;
        std::cout << "update.txt: " << version << " (" << channel << "), " << files << " archivos, " << check.total_size() << " bytes\n";
        return 0;
    }
    if (cmd == "pack-installers" && argc == 6) {
        // wl_release pack-installers <carpeta con instaladores/> <salida> <versión> <canal>: instaladores.txt (sin firmar)
        fs::path src = fs::u8path(argv[2]), out = fs::u8path(argv[3]);
        std::string version = argv[4], channel = argv[5];
        std::error_code e;
        fs::create_directories(out, e);
        std::ostringstream m;
        m << "format = 1\napp = " << kInstallersAppId << "\nchannel = " << channel << "\nversion = " << version << "\nplatform = " << kPlatform << "\n";
        int files = 0;
        for (const auto& entry : fs::directory_iterator(src / "instaladores", e)) {
            if (!entry.is_regular_file()) continue;
            std::string rel = "instaladores/" + entry.path().filename().u8string();
            if (!allowed_installer_path(rel)) continue;
            std::string asset = asset_for(rel);
            fs::copy_file(entry.path(), out / fs::u8path(asset), fs::copy_options::overwrite_existing, e);
            if (e) {
                std::cerr << "no se pudo copiar " << rel << "\n";
                return 1;
            }
            m << "file = " << sha256_file_hex(entry.path()) << " " << fs::file_size(entry.path()) << " " << asset << " " << rel << "\n";
            ++files;
        }
        std::string text = m.str();
        UpdateManifest check;
        std::string err;
        if (files == 0 || !parse_manifest_text(text, check, err, true)) {
            std::cerr << "manifiesto de instaladores inválido: " << (files ? err : "no hay instaladores .exe") << "\n";
            return 1;
        }
        std::ofstream(out / "instaladores.txt", std::ios::binary) << text;
        std::cout << "instaladores.txt: " << files << " instaladores, " << check.total_size() << " bytes\n";
        return 0;
    }
    if (cmd == "verify-installers" && argc == 6) {
        std::string sig = slurp(fs::u8path(argv[3]));
        while (!sig.empty() && (sig.back() == '\n' || sig.back() == '\r')) sig.pop_back();
        VerifyResult r = verify_installers_manifest(slurp(fs::u8path(argv[2])), sig, argv[4], argv[5]);
        std::cout << (r.ok ? "OK instaladores " + r.manifest.version : "RECHAZADO: " + r.error) << "\n";
        return r.ok ? 0 : 1;
    }
    if (cmd == "sign" && argc == 3) {
        std::string key;
        if (const char* k = std::getenv("WL_UPDATE_SIGNING_KEY")) key = k;
        else if (const char* f = std::getenv("WL_UPDATE_SIGNING_KEY_FILE")) key = slurp(fs::u8path(f));
        if (key.empty()) {
            std::cerr << "falta WL_UPDATE_SIGNING_KEY o WL_UPDATE_SIGNING_KEY_FILE\n";
            return 1;
        }
        std::string text = slurp(fs::u8path(argv[2])), sig;
        bool ok = ecdsa_sign(key, text, sig);
        SecureZeroMemory(key.data(), key.size());
        if (!ok) {
            std::cerr << "no se pudo firmar (¿clave inválida?)\n";
            return 1;
        }
        fs::path out = fs::u8path(std::string(argv[2]) + ".sig");
        std::ofstream(out, std::ios::binary) << sig << "\n";
        std::cout << "firmado: " << out.u8string() << "\n";
        return 0;
    }
    if (cmd == "verify" && (argc == 6 || argc == 7)) {
        VerifyPolicy p;
        p.public_key = argv[4];
        p.installed = argv[5];
        p.channel = argc == 7 ? argv[6] : "beta";
        std::string sig = slurp(fs::u8path(argv[3]));
        while (!sig.empty() && (sig.back() == '\n' || sig.back() == '\r')) sig.pop_back();
        VerifyResult r = verify_manifest(slurp(fs::u8path(argv[2])), sig, p);
        std::cout << (r.ok ? "OK " + r.manifest.version : "RECHAZADO: " + r.error) << "\n";
        return r.ok ? 0 : 1;
    }
    return usage();
}
