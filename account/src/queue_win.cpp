#include "wl/account/queue.h"

#include <algorithm>
#include <fstream>
#include <string_view>

#include <windows.h>

#include "wl/mods/json.h"
#include "wl/mods/safe_path.h"

namespace wl::account {
namespace {

using mods::Json;
constexpr size_t kHeaderBytes = 4096;
constexpr size_t kMaxQueued = 16;

struct Pending {
    SaveRevision revision;
    SaveBundle bundle;
    int64_t attempts = 0;
    int64_t retry_at = 0;
    std::string terminal;
};

class QueueLock {
public:
    explicit QueueLock(const fs::path& directory) {
        std::error_code ec;
        const auto path = fs::weakly_canonical(directory, ec);
        if (ec) return;
        const auto id = "Local\\WiiPorts.Queue." + upd::sha256_hex(mods::fold_key(path.generic_u8string()));
        const std::wstring name(id.begin(), id.end());
        handle_ = CreateMutexW(nullptr, FALSE, name.c_str());
        if (handle_) {
            const auto wait = WaitForSingleObject(handle_, 0);
            leased_ = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
        }
    }
    QueueLock(const QueueLock&) = delete;
    QueueLock& operator=(const QueueLock&) = delete;
    ~QueueLock() {
        if (leased_) ReleaseMutex(handle_);
        if (handle_) CloseHandle(handle_);
    }
    explicit operator bool() const { return leased_; }
private:
    HANDLE handle_ = nullptr;
    bool leased_ = false;
};

bool failure(std::string& error, const char* message) {
    error = message;
    return false;
}

bool bounded_hex(const std::string& text, size_t length) {
    return text.size() == length && text.find_first_not_of("0123456789abcdef") == text.npos;
}

bool valid(const SaveRevision& rev, const SaveBundle& bundle) {
    return valid_user_id(rev.revision_id) && valid_user_id(rev.device_id) &&
           (rev.parent_id.empty() || valid_user_id(rev.parent_id)) && rev.parent_id != rev.revision_id &&
           (rev.game_id == "mp9" || rev.game_id == "wiiparty") && bounded_hex(rev.slot_id, 8) &&
           rev.compatibility.size() > 7 && rev.compatibility.size() <= 87 &&
           rev.compatibility.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789:._-") == rev.compatibility.npos &&
           !bundle.bytes.empty() && bundle.bytes.size() <= kMaxSaveBytes && rev.byte_size == bundle.bytes.size() &&
           bounded_hex(rev.sha256, 64) && bundle.sha256 == rev.sha256 && upd::sha256_hex(bundle.bytes) == rev.sha256;
}

bool regular_local(const fs::path& path, bool directory) {
    const auto attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) == 0 &&
           ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) == directory;
}

bool write_all(HANDLE file, std::string_view value) {
    DWORD written = 0;
    const auto size = static_cast<DWORD>(value.size());
    return WriteFile(file, value.data(), size, &written, nullptr) && written == size;
}

bool persist(const fs::path& path, const std::string& owner, const Pending& saved, bool replace, std::string& error) {
    Json header = Json::object();
    const auto& r = saved.revision;
    header.set("format", 1);
    header.set("owner", owner);
    header.set("id", r.revision_id);
    header.set("parent", r.parent_id);
    header.set("device", r.device_id);
    header.set("game", r.game_id);
    header.set("slot", r.slot_id);
    header.set("compatibility", r.compatibility);
    header.set("sha256", r.sha256);
    header.set("size", static_cast<uint64_t>(r.byte_size));
    header.set("attempts", saved.attempts);
    header.set("retry_at", saved.retry_at);
    header.set("terminal", saved.terminal);
    const auto suffix = new_id();
    if (suffix.empty()) return failure(error, "No se pudo reservar la copia pendiente.");
    const auto temp = path.parent_path() / ("." + suffix + ".tmp");
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) return failure(error, "No se pudo escribir la cola local.");
    const bool ok = write_all(file, header.dump() + "\n") && write_all(file, saved.bundle.bytes) && FlushFileBuffers(file);
    CloseHandle(file);
    const DWORD flags = MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0);
    if (ok && MoveFileExW(temp.c_str(), path.c_str(), flags)) return true;
    std::error_code ignored;
    fs::remove(temp, ignored);
    return failure(error, "No se pudo confirmar la copia pendiente en disco.");
}

bool read(const fs::path& file, const std::string& owner, Pending& saved, std::string& error) {
    std::error_code ec;
    if (!regular_local(file, false) || fs::file_size(file, ec) > kMaxSaveBytes + kHeaderBytes || ec)
        return failure(error, "La copia pendiente no es un archivo local válido.");
    std::ifstream stream(file, std::ios::binary);
    std::string header;
    for (size_t i = 0; i < kHeaderBytes; ++i) {
        const int ch = stream.get();
        if (ch == '\n' || ch == EOF) break;
        header.push_back(static_cast<char>(ch));
    }
    Json json;
    if (header.size() >= kHeaderBytes || !mods::parse_json(header, json, error) ||
        json["format"].num() != 1 || json["owner"].str() != owner)
        return failure(error, "La copia pendiente no pertenece a esta cuenta o está dañada.");
    const auto size = json["size"].num(-1);
    const auto retries = json["attempts"].num(-1);
    const auto when = json["retry_at"].num(-1);
    const auto terminal = json["terminal"].str();
    if (size <= 0 || size > static_cast<int64_t>(kMaxSaveBytes) || retries < 0 || retries > 20 || when < 0 ||
        (terminal != "" && terminal != "conflict" && terminal != "rejected"))
        return failure(error, "La copia pendiente tiene metadatos no válidos.");
    const auto id = json["id"].str();
    if (file.stem().u8string() != id) return failure(error, "La revisión pendiente no coincide con su archivo.");
    SaveBundle body{std::string(static_cast<size_t>(size), '\0'), json["sha256"].str()};
    if (!stream.read(body.bytes.data(), static_cast<std::streamsize>(size)) || stream.peek() != EOF)
        return failure(error, "La copia pendiente está incompleta.");
    SaveRevision rev{id, json["parent"].str(), json["device"].str(), json["game"].str(), json["slot"].str(),
                     json["compatibility"].str(), body.sha256, static_cast<size_t>(size)};
    if (!valid(rev, body)) return failure(error, "La copia pendiente tiene una huella incorrecta.");
    saved = {std::move(rev), std::move(body), retries, when, terminal};
    return true;
}

}  // namespace

SaveQueue::SaveQueue(fs::path base, std::string owner) : owner_(std::move(owner)) {
    if (valid_user_id(owner_) && !base.empty()) directory_ = std::move(base) / owner_;
}

bool SaveQueue::enqueue(const SaveRevision& revision, const SaveBundle& bundle, std::string& error) {
    error.clear();
    if (directory_.empty() || !valid(revision, bundle)) return failure(error, "No se puede encolar ese guardado.");
    std::error_code ec;
    fs::create_directories(directory_, ec);
    if (ec || !regular_local(directory_, true)) return failure(error, "La carpeta de sincronización no es segura.");
    const QueueLock lock(directory_);
    if (!lock) return failure(error, "Ya hay una operación de sincronización en curso.");
    size_t count = 0;
    for (fs::directory_iterator it(directory_, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() == ".pending") ++count;
    }
    if (ec || count >= kMaxQueued) return failure(error, "La cola local de guardados está llena o no se puede leer.");
    const auto file = directory_ / (revision.revision_id + ".pending");
    return persist(file, owner_, {revision, bundle, 0, 0, {}}, false, error);
}

QueueResult SaveQueue::process_next(CloudClient& cloud, const Session& session, int64_t now, std::string& error) {
    error.clear();
    if (directory_.empty() || session.user_id != owner_ || now < 0 || now > INT64_MAX - 600) {
        failure(error, "La cola pertenece a otra cuenta o el reloj no es válido.");
        return {};
    }
    std::error_code ec;
    if (!fs::exists(directory_, ec) && !ec) return {QueueState::Empty, {}};
    if (ec || !regular_local(directory_, true)) {
        failure(error, "La carpeta de sincronización no es segura.");
        return {};
    }
    const QueueLock lock(directory_);
    if (!lock) return {QueueState::Deferred, {}};

    bool deferred = false;
    QueueResult unresolved{QueueState::Empty, {}};
    for (fs::directory_iterator it(directory_, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() != ".pending") continue;
        Pending saved;
        if (!read(it->path(), owner_, saved, error)) return {};
        if (!saved.terminal.empty()) {
            const auto state = saved.terminal == "conflict" ? QueueState::Conflict : QueueState::Rejected;
            if (unresolved.state != QueueState::Conflict) unresolved = {state, saved.revision.revision_id};
            continue;
        }
        if (saved.retry_at > now) { deferred = true; continue; }

        const auto& r = saved.revision;
        const SaveTarget target{{}, r.game_id, r.slot_id, r.compatibility};
        auto state = cloud.head(session, target).state;
        if (state == SaveState::Missing || state == SaveState::Ok) state = cloud.push(session, r, saved.bundle).state;

        const auto id = r.revision_id;
        if (state == SaveState::Ok) {
            if (!fs::remove(it->path(), ec) || ec) {
                failure(error, "La revisión está subida. La cola se revalidará al reintentar.");
                return {QueueState::Deferred, id};
            }
            return {QueueState::Uploaded, id};
        }

        QueueState result = QueueState::Rejected;
        if (state == SaveState::Offline) {
            saved.attempts = std::min<int64_t>(saved.attempts + 1, 20);
            saved.retry_at = now + std::min<int64_t>(600, 5LL << std::min<int64_t>(saved.attempts - 1, 7));
            result = QueueState::Deferred;
        } else if (state == SaveState::Conflict) {
            saved.terminal = "conflict";
            result = QueueState::Conflict;
        } else {
            saved.terminal = "rejected";
        }
        if (!persist(it->path(), owner_, saved, true, error)) return {};
        return {result, id};
    }
    if (ec) {
        failure(error, "No se pudo leer la cola de guardados.");
        return {};
    }
    return deferred ? QueueResult{QueueState::Deferred, {}} : unresolved;
}

}  // namespace wl::account
