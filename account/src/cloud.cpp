#include "wl/account/cloud.h"

#include <windows.h>
#include <bcrypt.h>

#include "wl/mods/json.h"

namespace wl::account {
namespace {
using mods::Json;
constexpr auto kObjects = "/storage/v1/object/";
constexpr auto kBucket = "wiiports-saves/";

bool is_hex(const std::string& s, size_t size) {
    return s.size() == size && s.find_first_not_of("0123456789abcdef") == s.npos;
}

bool valid_target(const SaveTarget& t) {
    return (t.game_id == "mp9" || t.game_id == "wiiparty") && is_hex(t.slot_id, 8) &&
           t.compatibility.size() > 7 && t.compatibility.size() <= 87 &&
           t.compatibility.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789:._-") == t.compatibility.npos;
}

bool valid_access(const Session& session) {
    const auto& token = session.access_token;
    return valid_user_id(session.user_id) && !token.empty() && token.size() <= 8192 &&
           token.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.") == token.npos;
}

SaveState classify(const upd::HttpResponse& result) {
    if (result.status >= 200 && result.status < 300) return SaveState::Ok;
    if (result.status == 404) return SaveState::Missing;
    if (result.status == 0 || result.status == 408 || result.status == 429 || result.status >= 500) return SaveState::Offline;
    return SaveState::Rejected;
}

bool parse(const upd::HttpResponse& response, Json& out) {
    std::string ignored;
    return mods::parse_json(response.body, out, ignored);
}

std::string object_key(const Session& session, const SaveTarget& target, const std::string& revision_id) {
    return session.user_id + "/" + target.game_id + "/" + target.slot_id + "/" + revision_id;
}

std::string filters(const Session& session, const SaveTarget& target) {
    return "&user_id=eq." + session.user_id + "&game_id=eq." + target.game_id + "&slot_id=eq." + target.slot_id;
}

std::string object_url(const std::string& key, bool authenticated_read) {
    return std::string(kObjects) + (authenticated_read ? "authenticated/" : "") + kBucket + key;
}

}  // namespace

std::string new_id() {
    unsigned char bytes[16]{};
    if (BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return {};
    bytes[6] = (bytes[6] & 15) | 64;
    bytes[8] = (bytes[8] & 63) | 128;
    const char* const hex = "0123456789abcdef";
    std::string out;
    out.reserve(36);
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out.push_back('-');
        out.push_back(hex[bytes[i] >> 4]);
        out.push_back(hex[bytes[i] & 15]);
    }
    return out;
}

CloudClient::CloudClient(upd::HttpClient& http, Config config) : http_(http), config_(std::move(config)) {}

upd::HttpResponse CloudClient::send(const Session& session, const char* method, const std::string& path,
                                   std::string body, const char* mime, size_t response_limit) {
    upd::HttpRequest req;
    req.url = config_.origin + path;
    req.method = method;
    req.body = std::move(body);
    req.max_bytes = response_limit;
    req.headers = {{"apikey", config_.public_key},{"Authorization", "Bearer " + session.access_token}};
    if (mime) req.headers.emplace_back("Content-Type", mime);
    return http_.perform(req);
}

HeadResult CloudClient::head(const Session& session, const SaveTarget& target) {
    if (!config_.valid() || !valid_access(session) || !valid_target(target)) return {};
    const auto response = send(session, "GET", "/rest/v1/account_save_heads?select=revision_id,version" + filters(session, target), {});
    const auto state = classify(response);
    if (state != SaveState::Ok) return {state, {}, 0};
    Json data;
    if (!parse(response, data) || !data.is_array() || data.size() > 1) return {};
    if (data.size() == 0) return {SaveState::Missing, {}, 0};
    const auto id = data[0]["revision_id"].str();
    const auto version = data[0]["version"].num();
    if (!valid_user_id(id) || version <= 0) return {};
    return {SaveState::Ok, id, version};
}

HeadResult CloudClient::push(const Session& session, const SaveRevision& rev, const SaveBundle& bundle) {
    const SaveTarget target{{}, rev.game_id, rev.slot_id, rev.compatibility};
    if (!config_.valid() || !valid_access(session) || !valid_target(target) ||
        !valid_user_id(rev.revision_id) || !valid_user_id(rev.device_id) ||
        (!rev.parent_id.empty() && !valid_user_id(rev.parent_id)) || rev.revision_id == rev.parent_id ||
        rev.byte_size == 0 || rev.byte_size > kMaxSaveBytes || bundle.bytes.size() != rev.byte_size ||
        !is_hex(rev.sha256, 64) || rev.sha256 != bundle.sha256 || upd::sha256_hex(bundle.bytes) != rev.sha256) return {};

    const auto key = object_key(session, target, rev.revision_id);
    const auto response = send(session, "POST", object_url(key, false), bundle.bytes, "application/octet-stream");
    auto state = classify(response);
    if (state == SaveState::Rejected) {
        // Si se perdió la respuesta de una subida anterior, solo se reutiliza el objeto idéntico.
        const auto prior = send(session, "GET", object_url(key, true), {}, nullptr, kMaxSaveBytes);
        if (classify(prior) != SaveState::Ok || prior.body.size() != rev.byte_size || upd::sha256_hex(prior.body) != rev.sha256)
            return {state, {}, 0};
        state = SaveState::Ok;
    }
    if (state != SaveState::Ok) return {state, {}, 0};

    Json rpc = Json::object();
    rpc.set("expected_head", rev.parent_id.empty() ? Json{} : Json(rev.parent_id));
    rpc.set("new_revision", rev.revision_id);
    rpc.set("save_game", rev.game_id);
    rpc.set("save_slot", rev.slot_id);
    rpc.set("content_hash", rev.sha256);
    rpc.set("content_size", static_cast<uint64_t>(rev.byte_size));
    rpc.set("port_compatibility", rev.compatibility);
    rpc.set("source_device", rev.device_id);
    const auto commit = send(session, "POST", "/rest/v1/rpc/account_commit_save", rpc.dump(), "application/json");
    state = classify(commit);
    if (state != SaveState::Ok) return {state, {}, 0};
    Json outcome;
    if (!parse(commit, outcome) || !outcome.is_object()) return {};
    const auto decision = outcome["status"].str();
    const auto current = outcome["head"].str();
    const auto version = outcome["version"].num(-1);
    if (decision == "committed" && current == rev.revision_id && version > 0) return {SaveState::Ok, current, version};
    if (decision == "conflict" && valid_user_id(current) && version > 0) return {SaveState::Conflict, current, version};
    return {};
}

SaveState CloudClient::pull(const Session& session, const SaveTarget& target, const std::string& revision_id, SaveBundle& out) {
    out = {};
    if (!config_.valid() || !valid_access(session) || !valid_target(target) || !valid_user_id(revision_id))
        return SaveState::Rejected;
    const auto key = object_key(session, target, revision_id);
    const auto response = send(session, "GET", "/rest/v1/account_save_revisions?select=sha256,byte_size,compatibility,storage_key" +
                               filters(session, target) + "&revision_id=eq." + revision_id, {});
    auto state = classify(response);
    if (state != SaveState::Ok) return state;
    Json data;
    if (!parse(response, data) || !data.is_array() || data.size() > 1) return SaveState::Rejected;
    if (!data.size()) return SaveState::Missing;
    const auto& first = data[0];
    const auto size = first["byte_size"].num();
    const auto hash = first["sha256"].str();
    if (size <= 0 || size > static_cast<int64_t>(kMaxSaveBytes) || !is_hex(hash, 64) ||
        first["compatibility"].str() != target.compatibility || first["storage_key"].str() != key)
        return SaveState::Rejected;
    auto download = send(session, "GET", object_url(key, true), {}, nullptr, static_cast<size_t>(size));
    state = classify(download);
    if (state != SaveState::Ok) return state;
    if (download.body.size() != static_cast<size_t>(size) || upd::sha256_hex(download.body) != hash)
        return SaveState::Rejected;
    out = {std::move(download.body), hash};
    return SaveState::Ok;
}

}  // namespace wl::account
