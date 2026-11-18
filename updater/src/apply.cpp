// UpdateInstaller: aplicador fuera del proceso, diario, copia de seguridad, comprobación de salud y rollback.
//
// Diario (updates\journal.txt), una línea por evento, solo se añade:
//   from = 1.0.0 / to = 1.0.1 / target = <carpeta> / backup = <carpeta>
//   state = applying | applied | done | rolled_back
//   moved = <ruta>    el archivo viejo está en backup\<ruta>
//   placed = <ruta>   el archivo nuevo está en su sitio
//   health = ok       el exe nuevo pintó su primer fotograma
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

#include "wl/adapter.h"
#include "wl/update.h"

namespace wl::upd {

namespace {

constexpr const char* kExe = "wii-launcher.exe";

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::wstring quote(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        out.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        out += c;
    }
    out.append(slashes * 2, L'\\');
    return out + L"\"";
}

HANDLE launch(const fs::path& exe, const std::vector<std::wstring>& args, DWORD* pid = nullptr) {
    std::wstring cmd = quote(exe.wstring());
    for (const auto& a : args) cmd += L" " + quote(a);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    std::wstring dir = exe.parent_path().wstring();
    if (!CreateProcessW(exe.wstring().c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) return nullptr;
    CloseHandle(pi.hThread);
    if (pid) *pid = pi.dwProcessId;
    return pi.hProcess;
}

fs::path self_exe() {
    std::wstring buf(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
    buf.resize(n);
    return fs::path(buf);
}

bool fault(const char* name) {
    if (!testing_build()) return false;
    const char* f = std::getenv("WL_UPDATER_FAULT");
    return f && std::string(f) == name;
}

struct Journal {
    std::string from, to, state;
    fs::path target, backup;
    std::vector<std::string> moved, placed;
    bool health = false;
};

Journal read_journal(const fs::path& file) {
    Journal j;
    std::ifstream in(file, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t eq = line.find(" = ");
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 3);
        if (k == "from") j.from = v;
        else if (k == "to") j.to = v;
        else if (k == "target") j.target = fs::u8path(v);
        else if (k == "backup") j.backup = fs::u8path(v);
        else if (k == "state") j.state = v;
        else if (k == "moved") j.moved.push_back(v);
        else if (k == "placed") j.placed.push_back(v);
        else if (k == "health" && v == "ok") j.health = true;
    }
    return j;
}

void journal_add(const fs::path& file, const std::string& key, const std::string& value) {
    std::ofstream out(file, std::ios::binary | std::ios::app);
    out << key << " = " << value << "\n";
    out.flush();
}

bool move_replace(const fs::path& from, const fs::path& to) {
    std::error_code e;
    fs::create_directories(to.parent_path(), e);
    // Varios intentos: el antivirus o el proceso que acaba de cerrarse pueden retener el archivo un momento.
    for (int i = 0; i < 20; ++i) {
        if (MoveFileExW(from.wstring().c_str(), to.wstring().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return false;
}

// Deshace a partir del diario: aparta lo nuevo y devuelve lo viejo. Idempotente (se puede repetir tras otro corte).
bool rollback(const fs::path& journal_file, const Journal& j) {
    log("rollback de " + j.to + " a " + j.from);
    bool ok = true;
    std::error_code e;
    fs::path discard = j.backup.parent_path() / ("descartado-" + j.to);
    for (auto it = j.placed.rbegin(); it != j.placed.rend(); ++it) {
        fs::path cur = j.target / fs::u8path(*it);
        if (fs::exists(cur, e) && !move_replace(cur, discard / fs::u8path(*it))) ok = false;
    }
    for (const auto& rel : j.moved) {
        fs::path saved = j.backup / fs::u8path(rel);
        if (fs::exists(saved, e) && !move_replace(saved, j.target / fs::u8path(rel))) ok = false;
    }
    // Restos de copias a medias.
    for (const auto& rel : j.placed) fs::remove(j.target / fs::u8path(rel + ".wl-new"), e);
    journal_add(journal_file, "state", ok ? "rolled_back" : "rollback_incompleto");
    log(ok ? "rollback completo" : "rollback incompleto: revisar .wl-backup a mano");
    return ok;
}

std::string lower(std::wstring w) {
    std::transform(w.begin(), w.end(), w.begin(), ::towlower);
    return std::string(w.begin(), w.end());
}

}  // namespace

fs::path updates_dir() { return local_app_data() / "WiiLauncher" / "updates"; }

bool UpdateLock::acquire(int wait_ms) {
    if (handle_) return true;
    std::error_code e;
    fs::create_directories(updates_dir(), e);
    fs::path file = updates_dir() / ".lock";
    auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
    for (;;) {
        HANDLE h = CreateFileW(file.wstring().c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            handle_ = h;
            return true;
        }
        if (std::chrono::steady_clock::now() >= until) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void UpdateLock::release() {
    if (handle_) CloseHandle((HANDLE)handle_);
    handle_ = nullptr;
}

bool folder_writable(const fs::path& dir) {
    fs::path probe = dir / ".wl-escritura";
    HANDLE h = CreateFileW(probe.wstring().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

int other_launcher_instances(const fs::path& exe, unsigned long ignore_pid) {
    std::error_code e;
    std::string want = lower(fs::weakly_canonical(exe, e).wstring());
    int count = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    for (BOOL more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe)) {
        if (pe.th32ProcessID == GetCurrentProcessId() || pe.th32ProcessID == ignore_pid) continue;
        if (_wcsicmp(pe.szExeFile, L"wii-launcher.exe") != 0) continue;
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!p) {
            ++count;  // no se puede comprobar la ruta: mejor suponer que es esta copia
            continue;
        }
        wchar_t path[MAX_PATH * 4];
        DWORD n = MAX_PATH * 4;
        if (QueryFullProcessImageNameW(p, 0, path, &n) && lower(fs::weakly_canonical(fs::path(std::wstring(path, n)), e).wstring()) == want) ++count;
        CloseHandle(p);
    }
    CloseHandle(snap);
    return count;
}

void report_health(const fs::path& journal) {
    if (fault("no-health")) {
        log("prueba: el exe nuevo no informa de salud");
        return;
    }
    journal_add(journal, "health", "ok");
}

int run_apply(const ApplyArgs& a) {
    log("aplicador: " + current_version() + " prepara " + a.staging.filename().u8string());
    UpdateLock lock;
    if (!lock.acquire(15000)) {
        log("aplicador: otra instalación en curso");
        return 3;
    }
    fs::path old_exe = a.target / kExe;
    auto relaunch_old = [&](const char* why) {
        HANDLE h = launch(old_exe, {L"--update-failed", widen(why)});
        if (h) CloseHandle(h);
    };
    if (a.wait_pid) {
        HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, a.wait_pid);
        if (p) {
            DWORD w = WaitForSingleObject(p, 60000);
            CloseHandle(p);
            if (w != WAIT_OBJECT_0) {
                log("aplicador: el launcher no se cerró; no se cambia nada");
                return 4;
            }
        }
    }
    if (other_launcher_instances(old_exe, 0) > 0) {
        log("aplicador: hay otra ventana del launcher abierta; no se cambia nada");
        relaunch_old("otra-ventana");
        return 5;
    }
    std::string err;
    VerifyPolicy policy;
    policy.channel = "beta";  // el canal ya se comprobó al descargar; aquí se revalidan firma, versión y hashes
    policy.installed = current_version();
    policy.public_key = public_key_hex();
    VerifyResult v = verify_staging(a.staging, policy, err);
    if (!v.ok) {
        log("aplicador: staging rechazado: " + err);
        relaunch_old("verificacion");
        return 6;
    }
    const UpdateManifest& m = v.manifest;
    if (!folder_writable(a.target)) {
        log("aplicador: carpeta sin permiso de escritura");
        relaunch_old("permisos");
        return 7;
    }
    // Plan: qué se coloca, qué se aparta. Los archivos sin descargar deben coincidir ya con la firma.
    std::error_code e;
    std::vector<const UpdateFile*> place;
    uint64_t needed = 1 << 20;
    std::set<std::string> listed;
    for (const auto& f : m.files) {
        listed.insert(f.path);
        fs::path staged = a.staging / "files" / fs::u8path(f.asset);
        if (fs::is_regular_file(staged, e)) {
            place.push_back(&f);
            needed += f.size;
        } else if (sha256_file_hex(a.target / fs::u8path(f.path)) != f.sha256) {
            log("aplicador: falta " + f.path + " en staging");
            relaunch_old("incompleta");
            return 8;
        }
    }
    std::vector<std::string> remove;  // manifests de juegos retirados en la versión nueva
    for (const auto& entry : fs::directory_iterator(a.target / "manifests", e)) {
        std::string rel = "manifests/" + entry.path().filename().u8string();
        if (entry.path().extension() == ".toml" && !listed.count(rel)) remove.push_back(rel);
    }
    ULARGE_INTEGER free_bytes{};
    GetDiskFreeSpaceExW(a.target.wstring().c_str(), &free_bytes, nullptr, nullptr);
    if (fault("disk-full") || free_bytes.QuadPart < needed) {
        log("aplicador: espacio insuficiente (" + std::to_string(needed) + " bytes necesarios)");
        relaunch_old("espacio");
        return 9;
    }

    fs::path backup = a.target / ".wl-backup" / current_version();
    if (fs::exists(backup, e)) fs::rename(backup, backup.parent_path() / (current_version() + "-" + std::to_string(GetTickCount64())), e);
    fs::create_directories(backup, e);
    std::ofstream(a.journal, std::ios::binary | std::ios::trunc);  // diario nuevo
    journal_add(a.journal, "from", current_version());
    journal_add(a.journal, "to", m.version);
    journal_add(a.journal, "target", a.target.u8string());
    journal_add(a.journal, "backup", backup.u8string());
    journal_add(a.journal, "state", "applying");

    bool ok = true;
    int placed_count = 0;
    for (const UpdateFile* f : place) {
        fs::path dst = a.target / fs::u8path(f->path);
        fs::path tmp = dst;
        tmp += ".wl-new";
        fs::create_directories(dst.parent_path(), e);
        if (!CopyFileW((a.staging / "files" / fs::u8path(f->asset)).wstring().c_str(), tmp.wstring().c_str(), FALSE) ||
            sha256_file_hex(tmp) != f->sha256) {
            log("aplicador: no se pudo copiar " + f->path + " (error " + std::to_string(GetLastError()) + ")");
            fs::remove(tmp, e);
            ok = false;
            break;
        }
        if (fs::exists(dst, e)) {
            if (!move_replace(dst, backup / fs::u8path(f->path))) {
                log("aplicador: no se pudo apartar " + f->path);
                fs::remove(tmp, e);
                ok = false;
                break;
            }
            journal_add(a.journal, "moved", f->path);
        }
        if (!move_replace(tmp, dst)) {
            log("aplicador: no se pudo colocar " + f->path);
            ok = false;
            break;
        }
        journal_add(a.journal, "placed", f->path);
        if (++placed_count == 1 && fault("crash-after-1")) {
            log("prueba: cierre forzado del aplicador a mitad");
            TerminateProcess(GetCurrentProcess(), 99);
        }
    }
    for (const auto& rel : remove) {
        if (!ok) break;
        if (move_replace(a.target / fs::u8path(rel), backup / fs::u8path(rel))) journal_add(a.journal, "moved", rel);
    }
    if (!ok) {
        rollback(a.journal, read_journal(a.journal));
        relaunch_old("instalacion");
        return 10;
    }
    journal_add(a.journal, "state", "applied");

    // Arranca la versión nueva y espera su señal de salud.
    DWORD new_pid = 0;
    HANDLE proc = launch(old_exe, {L"--post-update", a.journal.wstring()}, &new_pid);
    bool healthy = false;
    auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(a.health_timeout_ms);
    while (proc && std::chrono::steady_clock::now() < until) {
        if (read_journal(a.journal).health) {
            healthy = true;
            break;
        }
        if (WaitForSingleObject(proc, 250) == WAIT_OBJECT_0) {
            healthy = read_journal(a.journal).health;
            break;
        }
    }
    if (!healthy) {
        log(proc ? "aplicador: la versión nueva no arrancó bien; se vuelve atrás" : "aplicador: no se pudo arrancar la versión nueva");
        if (proc) {
            TerminateProcess(proc, 1);
            WaitForSingleObject(proc, 5000);
        }
        if (proc) CloseHandle(proc);
        rollback(a.journal, read_journal(a.journal));
        relaunch_old("arranque");
        return 11;
    }
    CloseHandle(proc);
    journal_add(a.journal, "state", "done");
    log("aplicador: actualizado de " + current_version() + " a " + m.version);
    // Limpieza: staging de esta versión y copias de seguridad anteriores (se conserva la última).
    fs::remove_all(a.staging, e);
    for (const auto& entry : fs::directory_iterator(a.target / ".wl-backup", e))
        if (entry.path() != backup) fs::remove_all(entry.path(), e);
    return 0;
}

std::string recover_interrupted_update() {
    fs::path journal = updates_dir() / "journal.txt";
    std::error_code e;
    if (!fs::exists(journal, e)) return {};
    Journal j = read_journal(journal);
    if (j.state != "applying" && j.state != "applied") return {};
    UpdateLock lock;
    if (!lock.acquire(0)) return {};  // el aplicador sigue trabajando: no es una interrupción
    if (j.state == "applied" && j.health) {
        journal_add(journal, "state", "done");
        return {};
    }
    log("recuperación: actualización interrumpida a " + j.to + " (estado " + j.state + ")");
    bool ok = rollback(journal, j);
    return ok ? "La actualización a la versión " + j.to + " se interrumpió. Se ha restaurado la versión " + j.from + "."
              : "La actualización se interrumpió y no se pudo restaurar todo. Mira la guía: docs/updater/guia-usuario.md.";
}

bool start_apply(const fs::path& staging, const fs::path& install_dir, std::string& error) {
    std::error_code e;
    fs::create_directories(updates_dir(), e);
    fs::path applier = updates_dir() / "aplicador.exe";
    if (!CopyFileW(self_exe().wstring().c_str(), applier.wstring().c_str(), FALSE)) {
        error = "No se pudo preparar el instalador de la actualización (error " + std::to_string(GetLastError()) + ").";
        return false;
    }
    std::vector<std::wstring> args = {L"--apply-update", staging.wstring(), L"--target", install_dir.wstring(),
                                      L"--journal", (updates_dir() / "journal.txt").wstring(), L"--wait-pid", std::to_wstring(GetCurrentProcessId())};
    HANDLE h = launch(applier, args);
    if (!h) {
        error = "No se pudo iniciar el instalador de la actualización (¿antivirus?).";
        return false;
    }
    CloseHandle(h);
    log("aplicador lanzado");
    return true;
}

}  // namespace wl::upd
