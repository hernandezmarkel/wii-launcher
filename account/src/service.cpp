#include "wl/account/service.h"

#include <algorithm>
#include <utility>

namespace wl::account {

AccountService::AccountService(Config config, const fs::path& data_root, std::function<void()> wake)
    : http_(upd::make_winhttp_client()), client_(*http_, config),
      vault_(data_root / upd::sha256_hex(config.origin) / "session.dat", config.origin), wake_(std::move(wake)) {
    view_.configured = client_.configured();
    if (view_.configured) {
        view_.busy = true;
        job_ = [this] { resume(); };
        worker_ = std::thread([this] { run(); });
    }
}

AccountService::~AccountService() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
        job_ = {};
    }
    changed_.notify_one();
    if (worker_.joinable()) worker_.join();
}

AccountView AccountService::view() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return view_;
}

void AccountService::post(std::function<void()> job) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!view_.configured || view_.busy || stop_) return;
        view_.busy = true;
        view_.message.clear();
        view_.warning = false;
        job_ = std::move(job);
    }
    changed_.notify_one();
}

void AccountService::run() {
    for (;;) {
        std::function<void()> work;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            const auto ready = [this] { return stop_ || static_cast<bool>(job_); };
            if (next_refresh_ == Clock::time_point::max()) changed_.wait(lock, ready);
            else changed_.wait_until(lock, next_refresh_, ready);
            if (stop_) return;
            work = std::exchange(job_, {});
            view_.busy = true;
        }
        if (wake_) wake_();
        if (work) work();
        else renew();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            view_.busy = false;
        }
        if (wake_) wake_();
    }
}

void AccountService::feedback(std::string message, bool warning) {
    std::lock_guard<std::mutex> lock(mutex_);
    view_.message = std::move(message);
    view_.warning = warning;
}

void AccountService::account(bool signed_in, std::string alias) {
    std::lock_guard<std::mutex> lock(mutex_);
    view_.signed_in = signed_in;
    view_.alias = std::move(alias);
}

void AccountService::resume() {
    std::string refresh, error;
    if (!vault_.load(refresh, error)) {
        feedback(error, true);
        return;
    }
    if (refresh.empty()) return;
    session_.refresh_token = std::move(refresh);
    renew();
}

void AccountService::renew() {
    auto result = client_.refresh(session_.refresh_token);
    if (result.ok()) {
        if (!session_.user_id.empty() && result.session.user_id != session_.user_id) {
            next_refresh_ = Clock::time_point::max();
            session_ = {};
            account(false);
            std::string error;
            if (!vault_.clear(error)) feedback(error, true);
            else feedback("La sesión cambió de cuenta. Vuelve a iniciar sesión.", true);
            return;
        }
        accept(std::move(result));
        return;
    }
    if (result.state == AuthState::InvalidSession) {
        next_refresh_ = Clock::time_point::max();
        session_ = {};
        account(false);
        std::string error;
        if (!vault_.clear(error)) feedback(error, true);
        else feedback(result.message, true);
        return;
    }
    next_refresh_ = Clock::now() + backoff_;
    backoff_ = std::min(backoff_ * 2, std::chrono::seconds(120));
    feedback(result.message, true);
}

void AccountService::accept(AuthResult result) {
    if (!result.ok()) {
        feedback(result.message, result.state != AuthState::ConfirmEmail);
        return;
    }
    const bool same_user = session_.user_id == result.session.user_id;
    session_ = std::move(result.session);
    const auto delay = std::chrono::duration_cast<std::chrono::seconds>(
        session_.expires_at - std::chrono::system_clock::now());
    next_refresh_ = Clock::now() + std::max(delay - std::chrono::seconds(60), std::chrono::seconds(1));
    backoff_ = std::chrono::seconds(5);
    std::string error;
    const bool persisted = vault_.save(session_.refresh_token, error);
    const auto current = client_.profile(session_);
    const auto previous = view();
    account(true, current.ok() ? current.alias : same_user ? previous.alias : std::string{});
    if (!persisted) feedback(error, true);
    else feedback(current.message, !current.ok());
}

void AccountService::sign_in(std::string email, std::string password, bool register_account) {
    post([this, email = std::move(email), password = std::move(password), register_account] {
        accept(register_account ? client_.sign_up(email, password) : client_.sign_in(email, password));
    });
}

void AccountService::sign_out() {
    post([this] {
        next_refresh_ = Clock::time_point::max();
        const auto access = std::move(session_.access_token);
        session_ = {};
        account(false);
        std::string error;
        const bool cleared = vault_.clear(error);
        const auto result = access.empty() ? AuthResult{AuthState::Ok, {}, {}} : client_.sign_out(access);
        if (!cleared) feedback(error, true);
        else if (!result.ok()) feedback("Sesión cerrada en este equipo. Revocación remota pendiente.", true);
        else feedback("Sesión cerrada.", false);
    });
}

void AccountService::request_recovery(std::string email) {
    post([this, email = std::move(email)] {
        const auto result = client_.request_recovery(email);
        feedback(result.message, !result.ok());
    });
}

void AccountService::reset_password(std::string email, std::string code, std::string password) {
    post([this, email = std::move(email), code = std::move(code), password = std::move(password)] {
        const auto result = client_.reset_password(email, code, password);
        feedback(result.message, !result.ok());
    });
}

void AccountService::set_alias(std::string alias) {
    post([this, alias = std::move(alias)] {
        const auto result = client_.set_alias(session_, alias);
        if (result.ok()) account(true, result.alias);
        feedback(result.ok() ? "Alias guardado." : result.message, !result.ok());
    });
}

}  // namespace wl::account
