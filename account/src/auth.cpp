#include "wl/account/auth.h"

#include <algorithm>
#include <cstdlib>
#include <string_view>

#include "wl/mods/json.h"

namespace wl::account {
namespace {

using mods::Json;
namespace clock = std::chrono;

bool ascii_word(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

bool wire_token(const std::string& s) {
    return !s.empty() && s.size() <= 8192 && std::all_of(s.begin(), s.end(), [](char c) {
        return ascii_word(c) || c == '.' || c == '-' || c == '_';
    });
}

std::string decode_url64(std::string_view encoded) {
    const std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    uint32_t bits = 0;
    int available = 0;
    std::string out;
    for (char c : encoded) {
        const size_t index = alphabet.find(c);
        if (index == alphabet.npos) return {};
        bits = (bits << 6) | static_cast<uint32_t>(index);
        available += 6;
        if (available >= 8) {
            available -= 8;
            out.push_back(static_cast<char>((bits >> available) & 0xff));
        }
    }
    return out;
}

bool public_api_key(const std::string& key) {
    if (!wire_token(key)) return false;
    if (key.rfind("sb_publishable_", 0) == 0) return key.size() > 15;
    const size_t first = key.find('.');
    const size_t last = key.rfind('.');
    if (first == key.npos || first == 0 || last <= first + 1 || last + 1 == key.size()) return false;
    Json payload;
    std::string error;
    return mods::parse_json(decode_url64(std::string_view(key).substr(first + 1, last - first - 1)), payload, error) &&
           payload["role"].str() == "anon";
}

bool valid_origin(const std::string& origin) {
    const std::string_view url(origin);
    const std::string_view local = "http://127.0.0.1:";
    const std::string_view tls = "https://";
    const std::string_view suffix = ".supabase.co";
    if (url.substr(0, local.size()) == local) {
        const auto port = url.substr(local.size());
        return !port.empty() && port.size() <= 5 && port.find_first_not_of("0123456789") == port.npos &&
               std::stoi(std::string(port)) > 0 && std::stoi(std::string(port)) <= 65535;
    }
    if (url.size() != tls.size() + 20 + suffix.size() || url.substr(0, tls.size()) != tls ||
        url.substr(url.size() - suffix.size()) != suffix) return false;
    const auto ref = url.substr(tls.size(), 20);
    return ref.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789") == ref.npos;
}

bool email_input(const std::string& email) {
    return email.size() <= 254 && email.find('@') != email.npos &&
           email.find_first_of(" \r\n\t") == email.npos && email.find('\0') == email.npos;
}

AuthResult failure(AuthState state, const char* message) {
    return {state, {}, message};
}

AuthResult unconfigured() {
    return failure(AuthState::Unconfigured, "Configura el proyecto Supabase de cuentas.");
}

AuthResult rejected(const upd::HttpResponse& res, bool refreshing = false) {
    if (res.status == 0 || res.status >= 500 || res.status == 408)
        return failure(AuthState::Offline, "No se pudo conectar con el servicio de cuentas.");
    if (res.status == 429) return failure(AuthState::Offline, "Demasiados intentos. Vuelve a probar más tarde.");
    Json error;
    std::string ignored;
    mods::parse_json(res.body, error, ignored);
    std::string code = error["error_code"].str();
    if (code.empty()) code = error["code"].str();
    if (code == "email_not_confirmed")
        return failure(AuthState::ConfirmEmail, "Confirma tu correo antes de iniciar sesión.");
    if (refreshing && (res.status == 400 || res.status == 401 || res.status == 403))
        return failure(AuthState::InvalidSession, "Tu sesión caducó. Vuelve a iniciar sesión.");
    if (code == "weak_password")
        return failure(AuthState::Rejected, "Elige una contraseña más larga y segura.");
    return failure(AuthState::Rejected, "El servicio rechazó la solicitud. Revisa los datos y vuelve a intentarlo.");
}

bool success(const upd::HttpResponse& response) {
    return response.status >= 200 && response.status < 300;
}

AuthResult session_result(const upd::HttpResponse& res, bool signup = false, bool refreshing = false) {
    if (!success(res)) return rejected(res, refreshing);
    Json root;
    std::string error;
    mods::JsonLimits limits;
    limits.allow_fractions = true;
    if (!mods::parse_json(res.body, root, error, limits) || !root.is_object())
        return failure(AuthState::InvalidResponse, "Respuesta de sesión no válida.");
    const auto uid = root["user"]["id"].str();
    if (signup && !root.has("access_token") && (valid_user_id(root["id"].str()) || valid_user_id(uid)))
        return failure(AuthState::ConfirmEmail, "Revisa tu correo para confirmar la cuenta y después inicia sesión.");
    const auto access = root["access_token"].str();
    const auto refresh = root["refresh_token"].str();
    const auto lifetime = root["expires_in"].num();
    if (!valid_user_id(uid) || !wire_token(access) || !wire_token(refresh) || lifetime <= 0 || lifetime > 604800)
        return failure(AuthState::InvalidResponse, "Respuesta de sesión no válida.");
    return {AuthState::Ok, {uid, access, refresh, clock::system_clock::now() + clock::seconds(lifetime)}, {}};
}

Json credentials(const std::string& email, const std::string& password) {
    Json data = Json::object();
    data.set("email", email);
    data.set("password", password);
    return data;
}

ProfileResult profile_result(const upd::HttpResponse& response) {
    if (!success(response)) {
        Json error;
        std::string ignored;
        if (response.status == 409 && mods::parse_json(response.body, error, ignored) && error["code"].str() == "23505")
            return {AuthState::Rejected, {}, "Ese alias ya está en uso."};
        auto failure = rejected(response);
        return {failure.state, {}, failure.message};
    }
    Json data;
    std::string error;
    if (!mods::parse_json(response.body, data, error) || !data.is_array() || data.size() > 1 ||
        (data.size() == 1 && !valid_alias(data[0]["display_name"].str())))
        return {AuthState::InvalidResponse, {}, "Respuesta de perfil no válida."};
    return {AuthState::Ok, data[0]["display_name"].str(), {}};
}

}  // namespace

bool valid_user_id(const std::string& id) {
    if (id.size() != 36) return false;
    for (size_t i = 0; i < id.size(); ++i) {
        const char c = id[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

bool valid_alias(const std::string& alias) {
    return alias.size() >= 3 && alias.size() <= 24 && ascii_word(alias.front()) && ascii_word(alias.back()) &&
           std::all_of(alias.begin(), alias.end(), [](char c) { return ascii_word(c) || c == ' ' || c == '-' || c == '_'; }) &&
           alias.find("  ") == alias.npos;
}

bool Config::valid() const { return valid_origin(origin) && public_api_key(public_key); }

Config Config::from_environment() {
    const auto url = std::getenv("WL_SUPABASE_URL");
    const auto key = std::getenv("WL_SUPABASE_PUBLISHABLE_KEY");
    return {url ? url : "", key ? key : ""};
}

AuthClient::AuthClient(upd::HttpClient& http, Config config)
    : http_(http), config_(std::move(config)), configured_(config_.valid()) {}

upd::HttpResponse AuthClient::send(const char* method, const std::string& path, const std::string& body,
                                  const std::string& access, const char* prefer) {
    if (!configured_ || (!access.empty() && !wire_token(access))) return {};
    upd::HttpRequest req;
    req.url = config_.origin + path;
    req.method = method;
    req.body = body;
    req.max_bytes = 128 * 1024;
    req.headers.emplace_back("apikey", config_.public_key);
    if (!access.empty()) req.headers.emplace_back("Authorization", "Bearer " + access);
    if (!body.empty()) req.headers.emplace_back("Content-Type", "application/json");
    if (prefer) req.headers.emplace_back("Prefer", prefer);
    return http_.perform(req);
}

AuthResult AuthClient::sign_in(const std::string& email, const std::string& password) {
    if (!configured()) return unconfigured();
    if (!email_input(email) || password.empty() || password.size() > 1024)
        return failure(AuthState::Rejected, "Introduce un correo y una contraseña válidos.");
    return session_result(send("POST", "/auth/v1/token?grant_type=password", credentials(email, password).dump()));
}

AuthResult AuthClient::sign_up(const std::string& email, const std::string& password) {
    if (!configured()) return unconfigured();
    if (!email_input(email) || password.size() < 8 || password.size() > 1024)
        return failure(AuthState::Rejected, "Introduce un correo válido y una contraseña de al menos ocho caracteres.");
    return session_result(send("POST", "/auth/v1/signup", credentials(email, password).dump()), true);
}

AuthResult AuthClient::refresh(const std::string& token) {
    if (!configured()) return unconfigured();
    if (!wire_token(token)) return failure(AuthState::InvalidSession, "Tu sesión caducó.");
    Json data = Json::object();
    data.set("refresh_token", token);
    return session_result(send("POST", "/auth/v1/token?grant_type=refresh_token", data.dump()), false, true);
}

AuthResult AuthClient::sign_out(const std::string& access) {
    if (!configured()) return unconfigured();
    if (!wire_token(access)) return failure(AuthState::InvalidSession, "Tu sesión caducó.");
    const auto response = send("POST", "/auth/v1/logout?scope=local", {}, access);
    return success(response) ? AuthResult{AuthState::Ok, {}, {}} : rejected(response);
}

AuthResult AuthClient::request_recovery(const std::string& email) {
    if (!configured()) return unconfigured();
    if (!email_input(email)) return failure(AuthState::Rejected, "Introduce un correo válido.");
    Json data = Json::object();
    data.set("email", email);
    const auto response = send("POST", "/auth/v1/recover", data.dump());
    return success(response) ? AuthResult{AuthState::Ok, {}, "Si la cuenta existe, recibirás el código de recuperación."}
                             : rejected(response);
}

AuthResult AuthClient::reset_password(const std::string& email, const std::string& code, const std::string& password) {
    if (!configured()) return unconfigured();
    if (!email_input(email) || code.size() != 6 || code.find_first_not_of("0123456789") != code.npos ||
        password.size() < 8 || password.size() > 1024)
        return failure(AuthState::Rejected, "Revisa el correo, el código de seis dígitos y la nueva contraseña.");
    Json data = Json::object();
    data.set("email", email);
    data.set("token", code);
    data.set("type", "recovery");
    const auto recovery = session_result(send("POST", "/auth/v1/verify", data.dump()));
    if (!recovery.ok()) return recovery;
    data = Json::object();
    data.set("password", password);
    const auto response = send("PUT", "/auth/v1/user", data.dump(), recovery.session.access_token);
    sign_out(recovery.session.access_token);
    return success(response) ? AuthResult{AuthState::Ok, {}, "Contraseña cambiada. Ya puedes iniciar sesión."}
                             : rejected(response);
}

ProfileResult AuthClient::profile(const Session& session) {
    if (!valid_user_id(session.user_id) || !wire_token(session.access_token))
        return {AuthState::InvalidSession, {}, "Inicia sesión para consultar tu perfil."};
    return profile_result(send("GET", "/rest/v1/account_profiles?select=display_name&user_id=eq." + session.user_id,
                               {}, session.access_token));
}

ProfileResult AuthClient::set_alias(const Session& session, const std::string& alias) {
    if (!valid_user_id(session.user_id) || !wire_token(session.access_token))
        return {AuthState::InvalidSession, {}, "Inicia sesión para editar tu perfil."};
    if (!valid_alias(alias)) return {AuthState::Rejected, {}, "Usa de 3 a 24 letras sin tildes, números, espacios o guiones."};
    Json data = Json::object();
    data.set("display_name", alias);
    data.set("user_id", session.user_id);
    return profile_result(send("POST", "/rest/v1/account_profiles?on_conflict=user_id&select=display_name",
                               data.dump(), session.access_token, "resolution=merge-duplicates,return=representation"));
}

}  // namespace wl::account
