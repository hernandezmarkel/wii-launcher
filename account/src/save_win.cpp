#include "wl/account/save.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <map>
#include <set>
#include <string_view>

#include <windows.h>
#include <tlhelp32.h>

#include "wl/mods/safe_path.h"
#include "wl/update.h"

namespace wl::account {
namespace {

constexpr std::string_view kMagic = "WL-SAVE-1\n";
constexpr size_t kMaxEntries = 2048;
constexpr const char* kJournal = ".wl-save-restore";
using Files = std::map<std::string, std::string>;
using Dirs = std::set<std::string>;

class SaveLock {
public:
    explicit SaveLock(const fs::path& path) {
        std::error_code ec;
        const auto canonical = fs::weakly_canonical(path, ec);
        if (ec || canonical.empty()) return;
        const auto key = "Local\\WiiPorts.Save." + upd::sha256_hex(mods::fold_key(canonical.generic_u8string()));
        const std::wstring name(key.begin(), key.end());
        handle_ = CreateMutexW(nullptr, FALSE, name.c_str());
        if (handle_) {
            const auto wait = WaitForSingleObject(handle_, 0);
            locked_ = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
        }
    }
    ~SaveLock() {
        if (locked_) ReleaseMutex(handle_);
        if (handle_) CloseHandle(handle_);
    }
    explicit operator bool() const { return locked_; }
    SaveLock(const SaveLock&) = delete;
    SaveLock& operator=(const SaveLock&) = delete;
private:
    HANDLE handle_ = nullptr;
    bool locked_ = false;
};

bool fail(std::string& error, const char* reason) {
    error = reason;
    return false;
}

bool is_closed(const GameIsRunning& running, std::string& error) {
    return (running && !running()) || fail(error, "Cierra el juego antes de operar sobre el guardado.");
}

bool plain_path(const fs::path& path) {
    const auto attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

bool exists_plain_directory(const fs::path& path, bool& exists, std::string& error) {
    std::error_code ec;
    exists = fs::exists(path, ec);
    if (ec || (exists && (!plain_path(path) || !fs::is_directory(path, ec))) || ec)
        return fail(error, "La carpeta de guardado no es un directorio local seguro.");
    return true;
}

void write_u32(std::string& bytes, uint32_t v) {
    for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<char>((v >> (i * 8)) & 0xff));
}

bool read_u32(std::string_view& bytes, uint32_t& out) {
    if (bytes.size() < 4) return false;
    out = 0;
    for (int i = 0; i < 4; ++i) out |= static_cast<uint32_t>(static_cast<unsigned char>(bytes[i])) << (i * 8);
    bytes.remove_prefix(4);
    return true;
}

bool read_tree(const fs::path& dir, SaveBundle& out, std::string& error) {
    bool exists = false;
    if (!exists_plain_directory(dir, exists, error)) return false;
    if (!exists) return fail(error, "Todavía no hay guardado para este juego.");
    std::error_code ec;
    std::map<std::string, std::pair<bool, uintmax_t>> paths;
    std::set<std::string> unique;
    size_t size = kMagic.size() + 4;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const auto path = it->path();
        if (!plain_path(path)) return fail(error, "El guardado contiene un enlace o un archivo inaccesible.");
        const auto type = it->symlink_status(ec);
        if (ec || (!fs::is_directory(type) && !fs::is_regular_file(type)))
            return fail(error, "El guardado contiene una entrada no compatible.");
        const bool is_dir = fs::is_directory(type);
        const auto file_size = is_dir ? 0 : it->file_size(ec);
        std::string name;
        if (ec || !mods::normalize_relative(path.lexically_relative(dir).generic_u8string(), name, error) ||
            !unique.insert(mods::fold_key(name)).second)
            return fail(error, "El guardado contiene una ruta inválida o duplicada.");
        size += name.size() + 8;
        if (file_size > kMaxSaveBytes || size > kMaxSaveBytes - file_size || paths.size() >= kMaxEntries)
            return fail(error, "El guardado supera el tamaño permitido.");
        size += static_cast<size_t>(file_size);
        paths.emplace(name, std::make_pair(is_dir, file_size));
    }
    if (ec) return fail(error, "No se pudo enumerar el guardado.");
    if (paths.empty()) return fail(error, "Todavía no hay archivos de guardado.");
    std::string bytes(kMagic);
    bytes.reserve(size);
    write_u32(bytes, static_cast<uint32_t>(paths.size()));
    bool found_file = false;
    for (const auto& [name, entry] : paths) {
        const auto [directory, count] = entry;
        write_u32(bytes, static_cast<uint32_t>(name.size()));
        write_u32(bytes, directory ? UINT32_MAX : static_cast<uint32_t>(count));
        bytes.append(name);
        if (directory) continue;
        const auto path = mods::join_inside(dir, name);
        if (path.empty() || !plain_path(path)) return fail(error, "Una ruta de guardado cambió durante la copia.");
        std::ifstream stream(path, std::ios::binary);
        const auto offset = bytes.size();
        bytes.resize(offset + static_cast<size_t>(count));
        if (!stream.read(bytes.data() + offset, static_cast<std::streamsize>(count)) || stream.peek() != EOF)
            return fail(error, "El guardado cambió durante la copia.");
        found_file = true;
    }
    if (!found_file) return fail(error, "Todavía no hay archivos de guardado.");
    auto sha = upd::sha256_hex(bytes);
    if (sha.size() != 64) return fail(error, "No se pudo calcular la huella del guardado.");
    out = {std::move(bytes), std::move(sha)};
    return true;
}

bool decode(const SaveBundle& bundle, Files& files, Dirs& dirs, std::string& error) {
    std::string_view bytes(bundle.bytes);
    if (bytes.size() > kMaxSaveBytes || bundle.sha256.size() != 64 || upd::sha256_hex(bundle.bytes) != bundle.sha256 ||
        bytes.substr(0, kMagic.size()) != kMagic)
        return fail(error, "El tamaño, formato o SHA-256 del guardado recibido no coincide.");
    bytes.remove_prefix(kMagic.size());
    uint32_t entries = 0;
    if (!read_u32(bytes, entries) || entries == 0 || entries > kMaxEntries)
        return fail(error, "El guardado recibido tiene un índice no válido.");
    std::set<std::string> unique;
    std::string previous;
    for (uint32_t i = 0; i < entries; ++i) {
        uint32_t path_size = 0, file_size = 0;
        if (!read_u32(bytes, path_size) || !read_u32(bytes, file_size) || path_size > mods::kMaxPathBytes || path_size > bytes.size())
            return fail(error, "El guardado está incompleto.");
        std::string raw(bytes.substr(0, path_size)), name;
        bytes.remove_prefix(path_size);
        if (!mods::normalize_relative(raw, name, error) || raw != name || name <= previous ||
            !unique.insert(mods::fold_key(name)).second)
            return fail(error, "El guardado contiene una ruta inválida o duplicada.");
        previous = name;
        const auto parent = fs::u8path(name).parent_path().generic_u8string();
        if (!parent.empty() && !dirs.count(parent))
            return fail(error, "El guardado contiene una carpeta padre ausente o inválida.");
        if (file_size == UINT32_MAX) dirs.insert(name);
        else {
            if (file_size > bytes.size()) return fail(error, "El guardado está incompleto.");
            files.emplace(name, bytes.substr(0, file_size));
            bytes.remove_prefix(file_size);
        }
    }
    return (!files.empty() && bytes.empty()) || fail(error, "El guardado recibido tiene un formato no válido.");
}

bool move_directory(const fs::path& from, const fs::path& to) {
    return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH) != 0;
}

fs::path journal(const SaveTarget& target) { return target.directory.parent_path() / kJournal; }

bool retain_journal(const fs::path& work, std::string& error) {
    static std::atomic_uint serial{0};
    const auto count = std::chrono::system_clock::now().time_since_epoch().count();
    const auto name = ".wl-save-backup-" + std::to_string(count) + "-" + std::to_string(++serial);
    return move_directory(work, work.parent_path() / name) ||
           fail(error, "El guardado actual está intacto. Queda un respaldo pendiente en el journal.");
}

bool write_file(const fs::path& path, const std::string& bytes) {
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const auto size = static_cast<DWORD>(bytes.size());
    const bool result = WriteFile(handle, bytes.data(), size, &written, nullptr) && written == size && FlushFileBuffers(handle);
    CloseHandle(handle);
    return result;
}

}  // namespace

bool game_save_target(const std::string& game_id, const fs::path& root, const InstallStatus& status,
                      SaveTarget& out, std::string& error) {
    out = {};
    error.clear();
    if (!status.ready() || root.empty() ||
        !((game_id == "mp9" && status.disc_id == "SSQE01") ||
          (game_id == "wiiparty" && (status.disc_id == "SUPE01" || status.disc_id == "SUPP01"))))
        return fail(error, "Esta instalación aún no tiene un adaptador de guardado verificado.");
    if (status.revision.empty() || status.revision.size() > 80 ||
        status.revision.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-") != std::string::npos)
        return fail(error, "La revisión instalada no permite comparar guardados con otro equipo.");
    const char* hex = "0123456789abcdef";
    std::string title;
    for (unsigned char c : status.disc_id.substr(0, 4)) {
        title.push_back(hex[c >> 4]);
        title.push_back(hex[c & 15]);
    }
    const auto path = root / "games" / game_id / "nand" / "title" / "00010000" / title / "data";
    if (!mods::is_within(root, path)) return fail(error, "La carpeta de guardado sale de la instalación.");
    out = {path, game_id, title, status.disc_id + ":" + status.revision};
    return true;
}

bool game_process_running(const std::string& game_id) {
    const wchar_t* exe = game_id == "mp9" ? L"mp9.exe" : game_id == "wiiparty" ? L"wiiparty.exe" : nullptr;
    if (!exe) return true;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return true;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool running = !Process32FirstW(snapshot, &entry);
    if (!running) {
        do {
            if (_wcsicmp(exe, entry.szExeFile) == 0) { running = true; break; }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return running;
}

bool snapshot_save(const SaveTarget& target, const GameIsRunning& running, SaveBundle& out, std::string& error) {
    out = {};
    error.clear();
    const SaveLock lock(target.directory);
    if (!lock) return fail(error, "Otra operación de guardado está en curso.");
    if (!is_closed(running, error)) return false;
    SaveBundle first, second;
    if (!read_tree(target.directory, first, error) || !read_tree(target.directory, second, error) || !is_closed(running, error))
        return false;
    if (first.sha256 != second.sha256) return fail(error, "El guardado cambió durante la copia.");
    out = std::move(first);
    return true;
}

bool recover_save_restore(const SaveTarget& target, const GameIsRunning& running, std::string& error) {
    error.clear();
    const SaveLock lock(target.directory);
    if (!lock) return fail(error, "Otra operación de guardado está en curso.");
    if (!is_closed(running, error)) return false;
    const auto work = journal(target);
    const auto previous = work / "previous";
    bool work_exists = false, live_exists = false, previous_exists = false;
    if (!exists_plain_directory(work, work_exists, error) || !exists_plain_directory(target.directory, live_exists, error)) return false;
    if (!work_exists) return true;
    if (!exists_plain_directory(previous, previous_exists, error)) return false;
    if (!live_exists && previous_exists && !move_directory(previous, target.directory))
        return fail(error, "El guardado original sigue en el journal. No se pudo devolver a su carpeta.");
    return retain_journal(work, error);
}

bool restore_save(const SaveTarget& target, const GameIsRunning& running, const SaveBundle& incoming, std::string& error) {
    error.clear();
    const SaveLock lock(target.directory);
    if (!lock) return fail(error, "Otra operación de guardado está en curso.");
    if (!is_closed(running, error)) return false;
    Files files;
    Dirs dirs;
    if (!decode(incoming, files, dirs, error) || !recover_save_restore(target, running, error)) return false;
    std::error_code ec;
    fs::create_directories(target.directory.parent_path(), ec);
    const auto work = journal(target);
    if (ec || !plain_path(target.directory.parent_path()) || !fs::create_directory(work, ec) || ec)
        return fail(error, "No se pudo reservar una carpeta aislada para la restauración.");
    const auto stage = work / "incoming";
    if (!fs::create_directory(stage, ec) || ec) return fail(error, "No se pudo crear la copia temporal.");
    for (const auto& name : dirs)
        if (!fs::create_directory(stage / fs::u8path(name), ec) || ec)
            return fail(error, "No se pudo escribir el guardado temporal.");
    for (const auto& [name, bytes] : files)
        if (!write_file(stage / fs::u8path(name), bytes)) return fail(error, "No se pudo escribir el guardado temporal.");
    SaveBundle check;
    if (!read_tree(stage, check, error) || check.sha256 != incoming.sha256)
        return fail(error, "La copia temporal no pasó la verificación de integridad.");
    if (!is_closed(running, error)) return false;
    bool live_exists = false;
    if (!exists_plain_directory(target.directory, live_exists, error)) return false;
    const auto backup = work / "previous";
    if (live_exists) {
        SaveBundle before, backed_up;
        if (!read_tree(target.directory, before, error) || !is_closed(running, error)) return false;
        if (!move_directory(target.directory, backup))
            return fail(error, "No se pudo respaldar el guardado instalado.");
        if (!read_tree(backup, backed_up, error) || before.sha256 != backed_up.sha256) {
            if (!move_directory(backup, target.directory))
                return fail(error, "El original sigue en el journal. Usa la recuperación antes de jugar.");
            return fail(error, "No se pudo verificar el respaldo. Se conservó el guardado anterior.");
        }
    }
    if (!is_closed(running, error)) {
        if (live_exists && !move_directory(backup, target.directory))
            return fail(error, "El original sigue en el journal. Usa la recuperación antes de jugar.");
        return false;
    }
    if (!move_directory(stage, target.directory)) {
        if (live_exists && !move_directory(backup, target.directory))
            return fail(error, "El original está intacto en el journal. Usa la recuperación antes de jugar.");
        return fail(error, "No se pudo activar el guardado nuevo. Se conservó el anterior.");
    }
    return retain_journal(work, error);
}

}  // namespace wl::account
