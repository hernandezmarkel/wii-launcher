#pragma once

#include "wl/account/cloud.h"
#include "wl/account/location.h"

#include <condition_variable>
#include <mutex>
#include <thread>

namespace wl::account {

struct AccountView {
    bool configured = false;
    bool busy = false;
    bool signed_in = false;
    bool warning = false;
    bool save_locked = false;
    bool identity_ready = false;
    std::string local_owner;
    std::string alias;
    std::string message;
};

class AccountService {
public:
    AccountService(Config config, const fs::path& data_root, std::function<void()> wake);
    ~AccountService();
    AccountView view() const;

    void sign_in(std::string email, std::string password, bool register_account);
    void sign_out();
    void request_recovery(std::string email);
    void reset_password(std::string email, std::string code, std::string password);
    void set_alias(std::string alias);
    void sync_mod_catalog(std::string game_id, fs::path install_root, bool upload);
    std::optional<SaveLocation> local_save(const std::string& game, const fs::path& install_root,
                                           const InstallStatus& status) const;
    void enable_save(SaveLocation location);
    bool synchronize_save(SaveLocation location);
    void restore_cloud_save(SaveLocation location);

private:
    using Clock = std::chrono::steady_clock;
    bool post(std::function<void()> job, bool lock_save = false);
    void run();
    void resume();
    void renew();
    void accept(AuthResult result);
    void feedback(std::string message, bool warning);
    void account(bool signed_in, std::string alias = {});

    std::unique_ptr<upd::HttpClient> http_;
    AuthClient client_;
    CloudClient cloud_;
    const fs::path data_root_;
    SessionVault vault_;
    const std::function<void()> wake_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    AccountView view_;
    std::function<void()> job_;
    bool stop_ = false;

    // Solo se accede desde el hilo de cuentas.
    Session session_;
    Clock::time_point next_refresh_ = Clock::time_point::max();
    std::chrono::seconds backoff_{5};
    std::thread worker_;
};

}  // namespace wl::account
