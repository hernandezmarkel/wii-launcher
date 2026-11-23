#pragma once

#include "wl/adapter.h"
#include "wl/update.h"

#include <chrono>

namespace wl::account {

struct Config {
    std::string origin;
    std::string public_key;
    bool valid() const;
    static Config from_environment();
};

struct Session {
    std::string user_id;
    std::string access_token;
    std::string refresh_token;
    std::chrono::system_clock::time_point expires_at;
};

enum class AuthState { Ok, ConfirmEmail, Offline, Rejected, InvalidSession, Unconfigured, InvalidResponse };

struct AuthResult {
    AuthState state = AuthState::InvalidResponse;
    Session session;
    std::string message;
    bool ok() const { return state == AuthState::Ok; }
};

struct ProfileResult {
    AuthState state = AuthState::InvalidResponse;
    std::string alias;
    std::string message;
    bool ok() const { return state == AuthState::Ok; }
};

bool valid_user_id(const std::string& id);
bool valid_alias(const std::string& alias);

class AuthClient {
public:
    AuthClient(upd::HttpClient& http, Config config);

    AuthResult sign_in(const std::string& email, const std::string& password);
    AuthResult sign_up(const std::string& email, const std::string& password);
    AuthResult refresh(const std::string& refresh_token);
    AuthResult sign_out(const std::string& access_token);
    AuthResult request_recovery(const std::string& email);
    AuthResult reset_password(const std::string& email, const std::string& code, const std::string& new_password);
    ProfileResult profile(const Session& session);
    ProfileResult set_alias(const Session& session, const std::string& alias);
    bool configured() const { return configured_; }

private:
    upd::HttpResponse send(const char* method, const std::string& path, const std::string& body,
                           const std::string& access_token = {}, const char* prefer = nullptr);

    upd::HttpClient& http_;
    const Config config_;
    const bool configured_;
};

// Persiste únicamente el refresh token protegido por DPAPI y ligado al origen.
class SessionVault {
public:
    SessionVault(fs::path file, std::string origin);
    bool load(std::string& token, std::string& error) const;
    bool save(const std::string& token, std::string& error) const;
    bool clear(std::string& error) const;

private:
    fs::path file_;
    std::string origin_;
};

}  // namespace wl::account
