#pragma once

#include "wl/account/cloud.h"

namespace wl::account {

enum class QueueState { Empty, Deferred, Uploaded, Conflict, Rejected };

struct QueueResult {
    QueueState state = QueueState::Rejected;
    std::string revision_id;
};

class SaveQueue {
public:
    SaveQueue(fs::path base, std::string owner);
    bool enqueue(const SaveRevision& revision, const SaveBundle& bundle, std::string& error);
    QueueResult process_next(CloudClient& cloud, const Session& session, int64_t now_unix, std::string& error);

private:
    fs::path directory_;
    std::string owner_;
};

}  // namespace wl::account
