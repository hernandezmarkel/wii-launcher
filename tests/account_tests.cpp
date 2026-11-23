#include "wl/account/auth.h"

#include <chrono>
#include <fstream>
#include <iostream>

using namespace wl;
using namespace wl::account;

namespace {
int failures = 0;
#define CHECK(expr) do { if (!(expr)) { ++failures; std::cerr << "FALLO " << __LINE__ << ": " #expr "\n"; } } while (0)

constexpr const char* kUser = "12345678-1234-4123-8123-12345678abcd";
constexpr const char* kPublicKey = "sb_publishable_synthetic";
constexpr const char* kOrigin = "http://127.0.0.1:54321";
constexpr const char* kPassword = "FixtureA2!CuentaDePrueba";

class FakeHttp final : public upd::HttpClient {
public:
    upd::HttpResponse next;
    upd::HttpRequest last;
    int calls = 0;
    upd::HttpResponse get(const upd::HttpRequest& r) override { return perform(r); }
    upd::HttpResponse perform(const upd::HttpRequest& r) override {
        ++calls;
        last = r;
        return next;
    }
};

fs::path scratch() {
    const auto id = std::chrono::steady_clock::now().time_since_epoch().count();
    auto path = fs::temp_directory_path() / ("wl-account-tests-" + std::to_string(id));
    fs::create_directories(path);
    return path;
}

void unit_tests() {
    CHECK((Config{kOrigin, kPublicKey}.valid()));
    CHECK((Config{"https://abcdefghijklmnopqrst.supabase.co", kPublicKey}.valid()));
    CHECK((!Config{"http://localhost:54321", kPublicKey}.valid()));
    CHECK((!Config{"http://127.0.0.1:54321/path", kPublicKey}.valid()));
    CHECK((!Config{"https://abcdefghijklmnopqrst.supabase.co.ejemplo.invalid", kPublicKey}.valid()));
    CHECK((!Config{kOrigin, "sb_secret_restricted"}.valid()));
    CHECK(valid_alias("Jugador_01") && valid_alias("Mi alias") && !valid_alias("correo@invalid") &&
          !valid_alias("Mal  Nombre") && !valid_alias("_Luis"));

    FakeHttp http;
    AuthClient client(http, {kOrigin, kPublicKey});
    http.next.status = 200;
    http.next.body = R"({"user":{"id":"12345678-1234-4123-8123-12345678abcd"},"access_token":"fixture.a","refresh_token":"refresh_fixture","expires_in":3600})";
    auto result = client.sign_in("test@example.com", kPassword);
    CHECK(result.ok() && result.session.user_id == kUser);
    CHECK(http.last.method == "POST" && http.last.url == std::string(kOrigin) + "/auth/v1/token?grant_type=password");
    CHECK(http.last.headers.size() == 2 && http.last.headers.front().second == kPublicKey);
    CHECK(http.last.body == R"({"email":"test@example.com","password":"FixtureA2!CuentaDePrueba"})");
    const int calls = http.calls;
    CHECK(client.refresh("bad\r\nHeader").state == AuthState::InvalidSession && calls == http.calls);

    http.next.body = R"({"id":"12345678-1234-4123-8123-12345678abcd"})";
    CHECK(client.sign_up("test@example.com", kPassword).state == AuthState::ConfirmEmail);
    http.next.status = 400;
    http.next.body = R"({"error_code":"refresh_token_not_found","message":"secret sentinel"})";
    auto expired = client.refresh("fixture");
    CHECK(expired.state == AuthState::InvalidSession && expired.message.find("sentinel") == expired.message.npos);
    http.next.status = 0;
    CHECK(client.refresh("fixture").state == AuthState::Offline);

    const auto root = scratch();
    const auto file = root / "A" / "session.dat";
    SessionVault a(file, kOrigin);
    SessionVault other_project(file, "http://127.0.0.1:50001");
    SessionVault other_install(root / "B" / "session.dat", kOrigin);
    std::string error, token;
    CHECK(a.save("refresh_fixture_only", error));
    {
        std::ifstream bytes(file, std::ios::binary);
        const std::string ciphertext{std::istreambuf_iterator<char>(bytes), {}};
        CHECK(ciphertext.find("refresh_fixture_only") == ciphertext.npos);
    }
    CHECK(a.load(token, error) && token == "refresh_fixture_only");
    CHECK(!other_project.load(token, error) && token.empty());
    CHECK(other_install.load(token, error) && token.empty());
    CHECK(a.save("refresh_rotated", error));
    CHECK(a.load(token, error) && token == "refresh_rotated");
    CHECK(a.clear(error) && a.load(token, error) && token.empty());
}

int live() {
    auto cfg = Config::from_environment();
    if (!cfg.valid() || cfg.origin.rfind("http://127.0.0.1:", 0) != 0) return 2;
    auto http_a = upd::make_winhttp_client();
    auto http_b = upd::make_winhttp_client();
    AuthClient a(*http_a, cfg), b(*http_b, cfg);
    const auto counter = std::chrono::system_clock::now().time_since_epoch().count();
    const auto suffix = std::to_string(counter).substr(4);
    const std::string email_a = "wl-a-" + suffix + "@example.com";
    const std::string email_b = "wl-b-" + suffix + "@example.com";
    auto a1 = a.sign_up(email_a, kPassword);
    auto b1 = b.sign_up(email_b, kPassword);
    if (!a1.ok() || !b1.ok()) {
        std::cerr << "El registro local requiere cuentas sintéticas sin verificación obligatoria. Estado A/B "
                  << static_cast<int>(a1.state) << "/" << static_cast<int>(b1.state) << '\n';
        return 2;
    }
    CHECK(a1.session.user_id != b1.session.user_id);
    CHECK(!a.sign_in(email_a, "password_incorrecta").ok());
    auto login = a.sign_in(email_a, kPassword);
    CHECK(login.ok() && login.session.user_id == a1.session.user_id);

    const auto root = scratch();
    SessionVault vault(root / "A" / "session.dat", cfg.origin);
    std::string error, persisted;
    CHECK(vault.save(login.session.refresh_token, error));
    CHECK(vault.load(persisted, error));
    AuthClient restarted(*http_a, cfg);
    auto renewed = restarted.refresh(persisted);
    CHECK(renewed.ok() && renewed.session.user_id == a1.session.user_id);
    const auto alias_a = "Piloto-" + suffix;
    const auto alias_b = "Nave-" + suffix;
    CHECK(a.set_alias(renewed.session, alias_a).ok());
    CHECK(b.set_alias(b1.session, alias_b).ok());
    CHECK(!b.set_alias(b1.session, alias_a).ok());
    CHECK(b.profile(b1.session).alias == alias_b && a.profile(renewed.session).alias == alias_a);

    upd::HttpRequest access;
    access.url = cfg.origin + "/rest/v1/account_profiles?select=display_name&user_id=eq." + b1.session.user_id;
    access.headers = {{"apikey", cfg.public_key},{"Authorization", "Bearer " + renewed.session.access_token}};
    auto denied = http_a->perform(access);
    CHECK(denied.status == 200 && denied.body == "[]");
    access.url = cfg.origin + "/rest/v1/account_profiles?user_id=eq." + b1.session.user_id;
    access.method = "PATCH";
    access.headers.emplace_back("Content-Type", "application/json");
    access.headers.emplace_back("Prefer", "return=representation");
    access.body = R"({"display_name":"NoAutorizado"})";
    auto update = http_a->perform(access);
    CHECK(update.status == 200 && update.body == "[]");
    access.method = "DELETE";
    access.body.clear();
    auto erase = http_a->perform(access);
    CHECK(erase.status == 200 && erase.body == "[]");
    CHECK(b.profile(b1.session).alias == alias_b);

    CHECK(a.sign_out(renewed.session.access_token).ok() && vault.clear(error));
    CHECK(!a.refresh(renewed.session.refresh_token).ok());
    CHECK(b.sign_out(b1.session.access_token).ok());
    if (!failures) std::cout << "Supabase real local: registro A/B, login, rotación, logout, DPAPI y RLS de perfil correctos.\n";
    return failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--live") return live();
    unit_tests();
    if (!failures) std::cout << "Contratos de cuenta y DPAPI correctos.\n";
    return failures ? 1 : 0;
}
