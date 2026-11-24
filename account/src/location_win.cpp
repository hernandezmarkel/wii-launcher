#include "wl/account/location.h"

#include <algorithm>
#include <fstream>

#include <windows.h>

#include "wl/account/cloud.h"
#include "wl/mods/safe_path.h"

namespace wl::account {
namespace {

bool safe_file(const fs::path& path, bool directory) {
    const auto attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) == 0 &&
           ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) == directory;
}

bool write_record(const fs::path& path, const std::string& text, bool replace) {
    const auto id = new_id();
    if (id.empty()) return false;
    const auto temporary = path.parent_path() / (".new-" + id);
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const auto count = static_cast<DWORD>(text.size());
    const bool wrote = WriteFile(file, text.data(), count, &written, nullptr) && written == count && FlushFileBuffers(file);
    CloseHandle(file);
    if (wrote && MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0)))
        return true;
    std::error_code ignored;
    fs::remove(temporary, ignored);
    return false;
}

bool read_record(const fs::path& file, std::string& out) {
    out.clear();
    std::error_code ec;
    if (!fs::exists(file, ec) && !ec) return true;
    const auto size = fs::file_size(file, ec);
    if (ec || size > 200 || !safe_file(file, false)) return false;
    std::string text(static_cast<size_t>(size), '\0');
    std::ifstream stream(file, std::ios::binary);
    if (!stream.read(text.data(), static_cast<std::streamsize>(size)) || stream.peek() != EOF) return false;
    out = std::move(text);
    return true;
}

bool valid_digest(const std::string& value) {
    return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == value.npos;
}

}  // namespace

std::optional<SaveLocation> save_location(const fs::path& root, const std::string& owner, const std::string& game,
                                          const fs::path& install_root, const InstallStatus& status) {
    SaveTarget target;
    std::string error;
    if (!valid_user_id(owner) || !game_save_target(game, install_root, status, target, error)) return std::nullopt;
    const auto work = root / "users" / owner / "saves" / game / target.slot_id;
    const auto nand = work / "nand";
    target.directory = nand / "title" / "00010000" / target.slot_id / "data";
    if (!mods::is_within(root, work) || !mods::is_within(work, target.directory)) return std::nullopt;
    return SaveLocation{owner, work, nand, std::move(target)};
}

bool save_location_enabled(const SaveLocation& location) {
    std::string text;
    return safe_file(location.work, true) && safe_file(location.nand, true) &&
           read_record(location.work / "enabled", text) && text == "v1";
}

bool enable_save_location(const SaveLocation& location) {
    if (save_location_enabled(location)) return true;
    std::error_code ec;
    fs::create_directories(location.nand, ec);
    return !ec && safe_file(location.work, true) && safe_file(location.nand, true) &&
           write_record(location.work / "enabled", "v1", false);
}

bool launch_with_save_location(LaunchSpec& spec, const SaveLocation& location, const GameIsRunning& running, std::string& error) {
    if (!save_location_enabled(location) || spec.args.size() > 1) {
        error = "No se pudo preparar la NAND separada de la cuenta.";
        return false;
    }
    if (!recover_save_restore(location.target, running, error))
        return false;
    auto path = location.nand;
    auto text = mods::to_utf8(path);
    const auto ascii = [](const std::string& value) {
        return std::all_of(value.begin(), value.end(), [](unsigned char c) { return c < 128; });
    };
    if (!ascii(text)) {
        wchar_t short_path[32768]{};
        const auto n = GetShortPathNameW(path.c_str(), short_path, static_cast<DWORD>(std::size(short_path)));
        if (n && n < std::size(short_path)) text = mods::to_utf8(short_path);
    }
    if (!ascii(text)) {
        error = "El port necesita una ruta de guardado compatible con Windows.";
        return false;
    }
    if (spec.args.empty()) spec.args.push_back("games/" + location.target.game_id + "/extracted");
    spec.args.push_back("0");
    spec.args.push_back(std::move(text));
    return true;
}

bool read_save_baseline(const SaveLocation& location, SaveBaseline& out) {
    out = {};
    std::string text;
    if (!read_record(location.work / "baseline", text)) return false;
    if (text.empty()) return true;
    if (text.size() != 101 || text[36] != '\n') return false;
    auto id = text.substr(0, 36);
    auto sha = text.substr(37);
    if (!valid_user_id(id) || !valid_digest(sha)) return false;
    out = {std::move(id), std::move(sha)};
    return true;
}

bool write_save_baseline(const SaveLocation& location, const SaveBaseline& data) {
    return valid_user_id(data.revision) && valid_digest(data.sha256) &&
           write_record(location.work / "baseline", data.revision + "\n" + data.sha256, true);
}

fs::path export_save_bundle(const SaveLocation& location, const SaveBundle& bundle) {
    if (!save_location_enabled(location) || bundle.bytes.empty() || bundle.bytes.size() > kMaxSaveBytes ||
        !valid_digest(bundle.sha256) || upd::sha256_hex(bundle.bytes) != bundle.sha256) return {};
    const auto dir = location.work / "exports";
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec || !safe_file(dir, true)) return {};
    const auto id = new_id();
    if (id.empty()) return {};
    const auto file = dir / (location.target.game_id + "-" + id + ".wlsave");
    return write_record(file, "WL-EXPORT-1\n" + bundle.sha256 + "\n" + bundle.bytes, false) ? file : fs::path{};
}

std::string load_device_id(const fs::path& account_root) {
    const auto file = account_root / "device.id";
    std::string value;
    if (!read_record(file, value)) return {};
    if (valid_user_id(value)) return value;
    if (!value.empty()) return {};
    value = new_id();
    if (valid_user_id(value) && write_record(file, value, false)) return value;
    return read_record(file, value) && valid_user_id(value) ? value : std::string{};
}

}  // namespace wl::account
