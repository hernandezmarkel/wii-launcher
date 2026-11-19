// Instaladores pequeños (MP9 y Wii Party) con su propio manifiesto firmado. Ver update.h.
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <thread>

#include "wl/update.h"

namespace wl::upd {

namespace {

std::wstring lower(std::wstring w) {
    std::transform(w.begin(), w.end(), w.begin(), ::towlower);
    return w;
}

// ¿Algún proceso ejecuta ese archivo ahora mismo? (p. ej. el usuario está instalando Mario Party 9)
bool running(const fs::path& exe) {
    std::error_code e;
    std::wstring want = lower(fs::weakly_canonical(exe, e).wstring());
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return true;  // ante la duda, no se toca
    bool found = false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    for (BOOL more = Process32FirstW(snap, &pe); more && !found; more = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, exe.filename().wstring().c_str()) != 0) continue;
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!p) {
            found = true;
            break;
        }
        wchar_t path[MAX_PATH * 4];
        DWORD n = MAX_PATH * 4;
        if (QueryFullProcessImageNameW(p, 0, path, &n) && lower(fs::weakly_canonical(fs::path(std::wstring(path, n)), e).wstring()) == want) found = true;
        CloseHandle(p);
    }
    CloseHandle(snap);
    return found;
}

bool replace_file(const fs::path& from, const fs::path& to) {
    for (int i = 0; i < 20; ++i) {
        if (MoveFileExW(from.wstring().c_str(), to.wstring().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return false;
}

}  // namespace

InstallersResult update_installers(HttpClient& http, const FeedConfig& feed, const fs::path& install_dir,
                                   const std::string& installed_version, const std::string& public_key) {
    InstallersResult r;
    HttpRequest req;
    req.url = asset_url(feed, installed_version, "instaladores.txt");
    HttpResponse m = fetch(http, req);
    if (m.status == 404) return r;  // Release anterior a la beta.4 o sin instaladores: nada que hacer
    if (m.status != 200) {
        r.ok = false;
        r.error = m.status ? "El servidor respondió " + std::to_string(m.status) + " al pedir los instaladores." : m.error;
        return r;
    }
    req.url = asset_url(feed, installed_version, "instaladores.txt.sig");
    req.max_bytes = 1024;
    HttpResponse s = fetch(http, req);
    if (s.status != 200) {
        r.ok = false;
        r.error = "No se pudo descargar la firma de los instaladores.";
        return r;
    }
    std::string sig = s.body;
    while (!sig.empty() && (sig.back() == '\n' || sig.back() == '\r' || sig.back() == ' ')) sig.pop_back();
    VerifyResult v = verify_installers_manifest(m.body, sig, public_key, installed_version);
    if (!v.ok) {
        r.ok = false;
        r.error = v.error;
        log("instaladores rechazados: " + v.error);
        return r;
    }
    std::error_code e;
    fs::path staging = updates_dir() / ("instaladores-" + installed_version);
    fs::create_directories(staging, e);
    for (const auto& f : v.manifest.files) {
        fs::path target = install_dir / fs::u8path(f.path);
        if (fs::is_regular_file(target, e) && fs::file_size(target, e) == f.size && sha256_file_hex(target) == f.sha256) continue;
        if (running(target)) {
            r.busy.push_back(f.path);
            continue;
        }
        fs::path part = staging / fs::u8path(f.asset + ".part");
        HttpRequest d;
        d.url = asset_url(feed, installed_version, f.asset);
        d.max_bytes = f.size;
        d.to_file = part;
        HttpResponse got = fetch(http, d);
        if (got.status != 200 || fs::file_size(part, e) != f.size || sha256_file_hex(part) != f.sha256) {
            fs::remove(part, e);
            r.ok = false;
            r.error = "No se pudo descargar o verificar " + f.path + ".";
            log("instalador no actualizado: " + r.error);
            return r;
        }
        fs::create_directories(target.parent_path(), e);
        fs::path fresh = target;
        fresh += ".wl-new";
        if (!CopyFileW(part.wstring().c_str(), fresh.wstring().c_str(), FALSE) || sha256_file_hex(fresh) != f.sha256) {
            fs::remove(fresh, e);
            r.ok = false;
            r.error = "No se pudo escribir " + f.path + " (¿permisos o antivirus?).";
            return r;
        }
        fs::path backup = install_dir / ".wl-backup" / "instaladores" / target.filename();
        bool had_old = fs::exists(target, e);
        if (had_old) {
            fs::create_directories(backup.parent_path(), e);
            if (!replace_file(target, backup)) {
                fs::remove(fresh, e);
                r.ok = false;
                r.error = "No se pudo apartar el instalador anterior de " + f.path + ".";
                return r;
            }
        }
        if (!replace_file(fresh, target)) {
            if (had_old) replace_file(backup, target);  // se deja el anterior en su sitio
            fs::remove(fresh, e);
            r.ok = false;
            r.error = "No se pudo colocar " + f.path + ".";
            return r;
        }
        fs::remove(part, e);
        r.updated.push_back(f.path);
        log("instalador actualizado: " + f.path + " (" + installed_version + ")");
    }
    fs::remove_all(staging, e);
    return r;
}

}  // namespace wl::upd
