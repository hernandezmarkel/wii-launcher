#pragma once

#include "wl/account/auth.h"
#include "wl/account/save.h"

namespace wl::account {

enum class SaveState { Ok, Missing, Conflict, Offline, Rejected };

struct SaveRevision {
    std::string revision_id, parent_id, device_id;
    std::string game_id, slot_id, compatibility, sha256;
    size_t byte_size = 0;
};

struct HeadResult {
    SaveState state = SaveState::Rejected;
    std::string revision_id;
    int64_t version = 0;
};

std::string new_id();

class CloudClient {
public:
    CloudClient(upd::HttpClient& http, Config config);
    HeadResult head(const Session& session, const SaveTarget& target);
    HeadResult push(const Session& session, const SaveRevision& revision, const SaveBundle& bundle);
    SaveState pull(const Session& session, const SaveTarget& target, const std::string& revision_id, SaveBundle& out);

private:
    upd::HttpResponse send(const Session& session, const char* method, const std::string& path,
                           std::string body, const char* mime = nullptr, size_t response_limit = 128 * 1024);

    upd::HttpClient& http_;
    const Config config_;
};

}  // namespace wl::account
