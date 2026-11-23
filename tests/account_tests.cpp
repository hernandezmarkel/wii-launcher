#include "wl/account/auth.h"
#include "wl/account/cloud.h"
#include "wl/account/save.h"

#include <chrono>
#include <fstream>
#include <future>
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

void save_tests() {
    const auto root = scratch();
    const auto a_root = root / "a", b_root = root / "b";
    fs::create_directories(a_root);
    fs::create_directories(b_root);
    InstallStatus install;
    install.folder = install.built = install.disc_ok = true;
    install.disc_id = "SSQE01";
    install.revision = "5a2b71d";
    SaveTarget a, b;
    std::string error;
    const auto closed = [] { return false; };
    const auto running = [] { return true; };
    CHECK(game_save_target("mp9", a_root, install, a, error));
    CHECK(game_save_target("mp9", b_root, install, b, error));
    CHECK(a.slot_id == "53535145");
    fs::create_directories(a.directory / "slots" / "empty");
    std::ofstream(a.directory / "slots" / "save.bin", std::ios::binary).write("a\0b", 3);
    SaveBundle source, copy;
    CHECK(snapshot_save(a, closed, source, error));
    CHECK(!snapshot_save(a, running, copy, error));
    CHECK(!restore_save(b, running, source, error) && !fs::exists(b.directory));
    CHECK(restore_save(b, closed, source, error));
    CHECK(fs::is_directory(b.directory / "slots" / "empty"));
    CHECK(snapshot_save(b, closed, copy, error) && source.bytes == copy.bytes && source.sha256 == copy.sha256);

    copy.bytes.back() ^= 1;
    CHECK(!restore_save(b, closed, copy, error));
    auto traversal = source;
    const auto entry = traversal.bytes.find("slots/save.bin");
    CHECK(entry != traversal.bytes.npos);
    if (entry != traversal.bytes.npos) {
        traversal.bytes.replace(entry, 14, "slots/../x.dat");
        traversal.sha256 = upd::sha256_hex(traversal.bytes);
        CHECK(!restore_save(b, closed, traversal, error));
    }
    CHECK(snapshot_save(b, closed, copy, error) && source.sha256 == copy.sha256);
    const auto pending = b.directory.parent_path() / ".wl-save-restore";
    fs::create_directories(pending);
    fs::rename(b.directory, pending / "previous"); // interrupción entre las dos operaciones de cambio
    CHECK(!recover_save_restore(b, running, error) && !fs::exists(b.directory));
    CHECK(recover_save_restore(b, closed, error));
    CHECK(snapshot_save(b, closed, copy, error) && source.sha256 == copy.sha256);
}


void live_saves(upd::HttpClient& http_a, upd::HttpClient& http_b, const Config& cfg,
                const Session& a, const Session& same_user_on_b, const Session& other_user) {
    CloudClient desktop(http_a, cfg), laptop(http_b, cfg);
    const auto root = scratch();
    const SaveTarget local_a{root / "desktop" / "data", "mp9", "53535145", "SSQE01:fixture"};
    const SaveTarget local_b{root / "laptop" / "data", "mp9", "53535145", "SSQE01:fixture"};
    const auto closed = [] { return false; };
    std::string error;
    fs::create_directories(local_a.directory);
    std::ofstream(local_a.directory / "progress.bin", std::ios::binary) << "progreso inicial";

    auto make_revision = [&](const SaveBundle& b, const std::string& parent, const std::string& device) {
        return SaveRevision{new_id(), parent, device, local_a.game_id, local_a.slot_id, local_a.compatibility, b.sha256, b.bytes.size()};
    };
    const auto device_a = new_id(), device_b = new_id();
    CHECK(valid_user_id(device_a) && valid_user_id(device_b) && device_a != device_b);
    SaveBundle first, incoming;
    CHECK(snapshot_save(local_a, closed, first, error));
    CHECK(desktop.head(a, local_a).state == SaveState::Missing);
    const auto first_rev = make_revision(first, {}, device_a);
    CHECK(desktop.push(a, first_rev, first).state == SaveState::Ok);
    CHECK(laptop.head(same_user_on_b, local_b).revision_id == first_rev.revision_id);
    CHECK(laptop.pull(same_user_on_b, local_b, first_rev.revision_id, incoming) == SaveState::Ok);
    CHECK(restore_save(local_b, closed, incoming, error));
    CHECK(snapshot_save(local_b, closed, incoming, error) && incoming.bytes == first.bytes);

    std::ofstream(local_a.directory / "progress.bin", std::ios::binary | std::ios::app) << " + escritorio";
    std::ofstream(local_b.directory / "progress.bin", std::ios::binary | std::ios::app) << " + portátil";
    SaveBundle left, right;
    CHECK(snapshot_save(local_a, closed, left, error) && snapshot_save(local_b, closed, right, error));
    const auto left_rev = make_revision(left, first_rev.revision_id, device_a);
    const auto right_rev = make_revision(right, first_rev.revision_id, device_b);
    auto uploading = std::async(std::launch::async, [&] { return desktop.push(a, left_rev, left); });
    const auto b_result = laptop.push(same_user_on_b, right_rev, right);
    const auto a_result = uploading.get();
    CHECK((a_result.state == SaveState::Ok && b_result.state == SaveState::Conflict) ||
          (b_result.state == SaveState::Ok && a_result.state == SaveState::Conflict));
    const auto head = desktop.head(a, local_a);
    const auto winner = a_result.state == SaveState::Ok ? left_rev.revision_id : right_rev.revision_id;
    CHECK(head.state == SaveState::Ok && head.revision_id == winner && head.version == 2);
    CHECK(laptop.pull(same_user_on_b, local_b, left_rev.revision_id, incoming) == SaveState::Ok && incoming.bytes == left.bytes);
    CHECK(desktop.pull(a, local_a, right_rev.revision_id, incoming) == SaveState::Ok && incoming.bytes == right.bytes);
    CHECK(desktop.push(a, left_rev, left).state == a_result.state);

    CHECK(laptop.head(other_user, local_b).state == SaveState::Missing);
    CHECK(laptop.pull(other_user, local_b, first_rev.revision_id, incoming) == SaveState::Missing);
    const auto key = a.user_id + "/mp9/53535145/" + first_rev.revision_id;
    upd::HttpRequest request;
    request.headers = {{"apikey", cfg.public_key},{"Authorization", "Bearer " + other_user.access_token}};
    request.url = cfg.origin + "/storage/v1/object/authenticated/wiiports-saves/" + key;
    auto result = http_b.perform(request);
    CHECK(result.status >= 400 && result.status < 500);
    request.method = "POST";
    request.url = cfg.origin + "/storage/v1/object/wiiports-saves/" + a.user_id + "/mp9/53535145/" + new_id();
    request.headers.emplace_back("Content-Type", "application/octet-stream");
    request.body = "ajeno";
    result = http_b.perform(request);
    CHECK(result.status >= 400 && result.status < 500);
    request.method = "DELETE";
    request.url = cfg.origin + "/storage/v1/object/wiiports-saves/" + key;
    request.body.clear();
    const auto denied = http_b.perform(request);
    CHECK(denied.status >= 400 && denied.status < 500);
    request.method = "POST";
    request.url = cfg.origin + "/storage/v1/object/list/wiiports-saves";
    request.headers.back().second = "application/json";
    request.body = "{\"prefix\":\"" + a.user_id + "\",\"limit\":10}";
    result = http_b.perform(request);
    CHECK(result.status == 200 && result.body == "[]");
    CHECK(desktop.pull(a, local_a, first_rev.revision_id, incoming) == SaveState::Ok && incoming.bytes == first.bytes);
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
    auto second_device = b.sign_in(email_a, kPassword);
    CHECK(second_device.ok() && second_device.session.user_id == renewed.session.user_id);
    if (second_device.ok()) live_saves(*http_a, *http_b, cfg, renewed.session, second_device.session, b1.session);
    if (second_device.ok()) CHECK(b.sign_out(second_device.session.access_token).ok());

    CHECK(a.sign_out(renewed.session.access_token).ok() && vault.clear(error));
    CHECK(!a.refresh(renewed.session.refresh_token).ok());
    CHECK(b.sign_out(b1.session.access_token).ok());
    if (!failures) std::cout << "Supabase real local: cuentas A/B, sesión, RLS, Storage y conflicto de guardados correctos.\n";
    return failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--live") return live();
    unit_tests();
    save_tests();
    if (!failures) std::cout << "Contratos de cuenta y DPAPI correctos.\n";
    return failures ? 1 : 0;
}
