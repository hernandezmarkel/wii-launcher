#include "wl/account/auth.h"

#include <atomic>
#include <fstream>
#include <string_view>

#include <windows.h>
#include <dpapi.h>

namespace wl::account {
namespace {

constexpr size_t kMaxRefresh = 8192;
constexpr size_t kMaxBlob = 24 * 1024;

DATA_BLOB blob(const std::string& value) {
    return {static_cast<DWORD>(value.size()), reinterpret_cast<BYTE*>(const_cast<char*>(value.data()))};
}

struct ProtectedBytes {
    DATA_BLOB data{};
    ~ProtectedBytes() {
        if (data.pbData) {
            SecureZeroMemory(data.pbData, data.cbData);
            LocalFree(data.pbData);
        }
    }
};

}  // namespace

SessionVault::SessionVault(fs::path file, std::string origin)
    : file_(std::move(file)), origin_("wii-launcher account session v1:" + std::move(origin)) {}

bool SessionVault::load(std::string& token, std::string& error, std::string* cached_user) const {
    token.clear();
    if (cached_user) cached_user->clear();
    error.clear();
    std::error_code ec;
    const auto present = fs::exists(file_, ec);
    if (!present && !ec) return true;
    const auto size = fs::file_size(file_, ec);
    if (ec || size == 0 || size > kMaxBlob) {
        error = "No se pudo leer la sesión protegida.";
        return false;
    }
    std::ifstream file(file_, std::ios::binary);
    std::string buffer(static_cast<size_t>(size), '\0');
    if (!file.read(buffer.data(), static_cast<std::streamsize>(size)) || file.peek() != EOF) {
        error = "No se pudo leer la sesión protegida.";
        return false;
    }
    auto input = blob(buffer), entropy = blob(origin_);
    ProtectedBytes plain;
    if (!CryptUnprotectData(&input, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &plain.data) ||
        plain.data.cbData == 0 || plain.data.cbData > kMaxRefresh + 40) {
        error = "Windows no pudo abrir la sesión protegida. Vuelve a iniciar sesión.";
        return false;
    }
    const std::string_view value(reinterpret_cast<const char*>(plain.data.pbData), plain.data.cbData);
    if (value.substr(0, 3) == "v2\n") {
        const auto id = std::string(value.substr(3, 36));
        if (value.size() <= 40 || value[39] != '\n' || value.size() - 40 > kMaxRefresh || !valid_user_id(id)) {
            error = "La sesión protegida no tiene una identidad válida.";
            return false;
        }
        token.assign(value.substr(40));
        if (cached_user) *cached_user = id;
    } else {
        if (value.size() > kMaxRefresh) return false;
        token.assign(value);
    }
    return true;
}

bool SessionVault::save(const std::string& token, std::string& error, const std::string& user_id) const {
    error.clear();
    if (token.empty() || token.size() > kMaxRefresh || (!user_id.empty() && !valid_user_id(user_id))) {
        error = "La sesión no es válida para guardarla.";
        return false;
    }
    std::string receipt = user_id.empty() ? token : "v2\n" + user_id + "\n" + token;
    auto input = blob(receipt), entropy = blob(origin_);
    ProtectedBytes encrypted;
    const bool protected_ok = CryptProtectData(&input, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &encrypted.data);
    SecureZeroMemory(receipt.data(), receipt.size());
    if (!protected_ok) {
        error = "Windows no pudo proteger la sesión.";
        return false;
    }
    std::error_code ec;
    fs::create_directories(file_.parent_path(), ec);
    if (ec) {
        error = "No se pudo crear la carpeta de sesión.";
        return false;
    }
    static std::atomic_uint sequence{0};
    fs::path temp = file_;
    temp += L"." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(++sequence) + L".tmp";
    HANDLE handle = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = "No se pudo escribir la sesión protegida.";
        return false;
    }
    DWORD written = 0;
    const bool saved = WriteFile(handle, encrypted.data.pbData, encrypted.data.cbData, &written, nullptr) &&
                       written == encrypted.data.cbData && FlushFileBuffers(handle);
    CloseHandle(handle);
    if (!saved || !MoveFileExW(temp.c_str(), file_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        fs::remove(temp, ec);
        error = "No se pudo confirmar la sesión protegida en disco.";
        return false;
    }
    return true;
}

bool SessionVault::clear(std::string& error) const {
    error.clear();
    std::error_code ec;
    fs::remove(file_, ec);
    if (!ec) return true;
    error = "No se pudo retirar la sesión protegida. Cierra el launcher y revisa los permisos.";
    return false;
}

}  // namespace wl::account
