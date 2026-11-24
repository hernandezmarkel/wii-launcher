#include "wl/account/auth.h"
#include "wl/account/cloud.h"
#include "wl/account/queue.h"
#include "wl/account/save.h"
#include "wl/account/service.h"
#include "wl/mods/safe_path.h"
#include "wl/process.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iostream>
#include <thread>
#include "wl/mods/json.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

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
    std::string cached;
    CHECK(a.save("refresh_cached", error, kUser));
    CHECK(a.load(token, error, &cached) && token == "refresh_cached" && cached == kUser);
    CHECK(a.clear(error));
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
    const auto owner_a = new_id(), owner_b = new_id();
    const auto local_a = save_location(root, owner_a, "mp9", a_root, install);
    const auto local_b = save_location(root, owner_b, "mp9", a_root, install);
    CHECK(local_a && local_b && local_a->nand != local_b->nand && !save_location_enabled(*local_b));
    if (local_a) {
        CHECK(enable_save_location(*local_a));
        LaunchSpec vanilla, modded;
        modded.args = {"disco-virtual"};
        CHECK(launch_with_save_location(vanilla, *local_a, closed, error) && vanilla.args.size() == 3 &&
              vanilla.args.front() == "games/mp9/extracted");
        CHECK(launch_with_save_location(modded, *local_a, closed, error) && modded.args.size() == 3 &&
              modded.args.front() == "disco-virtual" && modded.args.back() == vanilla.args.back());
        CHECK(!fs::exists(local_a->target.directory) && fs::exists(a.directory / "slots" / "save.bin"));
    }
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

    class DropCommit final : public upd::HttpClient {
    public:
        upd::HttpClient& real;
        bool dropped = false;
        explicit DropCommit(upd::HttpClient& transport) : real(transport) {}
        upd::HttpResponse get(const upd::HttpRequest& request) override { return real.get(request); }
        upd::HttpResponse perform(const upd::HttpRequest& request) override {
            if (!dropped && request.url.find("/rpc/account_commit_save") != request.url.npos) {
                dropped = true;
                return {};
            }
            return real.perform(request);
        }
    } interrupted(http_a);

    const auto queue_root = root / "pending";
    const auto queued_rev = make_revision(left, winner, device_a);
    {
        SaveQueue queue(queue_root, a.user_id);
        CloudClient connection(interrupted, cfg);
        CHECK(queue.enqueue(queued_rev, left, error));
        CHECK(queue.process_next(connection, other_user, 1000, error).state == QueueState::Rejected);
        CHECK(queue.process_next(connection, a, 1000, error).state == QueueState::Deferred && interrupted.dropped);
        CHECK(desktop.head(a, local_a).revision_id == winner);
    }
    SaveQueue restarted(queue_root, a.user_id);
    CHECK(restarted.process_next(desktop, a, 1003, error).state == QueueState::Deferred);
    CHECK(restarted.process_next(desktop, a, 1010, error).state == QueueState::Uploaded);
    CHECK(desktop.head(a, local_a).revision_id == queued_rev.revision_id);
    CHECK(laptop.pull(same_user_on_b, local_b, queued_rev.revision_id, incoming) == SaveState::Ok && incoming.bytes == left.bytes);
}


void live_mods(upd::HttpClient& http_a, upd::HttpClient& http_b, const Config& cfg,
               const Session& a, const Session& same_user_on_b, const Session& other_user) {
    using mods::Json;
    CloudClient desktop(http_a, cfg), laptop(http_b, cfg);
    Json catalog = Json::object(), refs = Json::array();
    catalog.set("schema", "wii-launcher-mod-catalog");
    catalog.set("version", 1);
    catalog.set("game_id", "mp9");
    catalog.set("profile", "fixture");
    catalog.set("base_content_id", "fixture-disc");
    catalog.set("loader", "fixture-loader");
    for (int i = 0; i < 3; ++i) {
        mods::ModReference ref;
        ref.id = i == 0 ? "manual.fixture" : "gamebanana." + std::to_string(i);
        ref.source = i == 0 ? "local" : "gamebanana";
        ref.source_id = i == 0 ? "" : std::to_string(i);
        ref.name = "Fixture " + std::to_string(i);
        ref.version = "1";
        ref.content_sha256 = upd::sha256_hex(ref.id);
        ref.config_sha256 = upd::sha256_hex("{}");
        ref.load_order = i;
        ref.enabled = i != 1;
        refs.push(mods::serialize_mod_reference(ref));
    }
    catalog.set("mods", refs);
    CHECK(desktop.mod_catalog(a, "mp9", "fixture").state == SaveState::Missing);
    auto created = desktop.push_mod_catalog(a, catalog.dump(), 0);
    CHECK(created.state == SaveState::Ok && created.version == 1);
    const auto fetched = laptop.mod_catalog(same_user_on_b, "mp9", "fixture");
    CHECK(fetched.state == SaveState::Ok && fetched.version == 1 && fetched.catalog.mods.size() == 3);
    if (fetched.catalog.mods.size() == 3) {
        CHECK(!fetched.catalog.mods[0].downloadable() && fetched.catalog.mods[1].downloadable());
        CHECK(!fetched.catalog.mods[1].enabled && fetched.catalog.mods[2].load_order == 2);
    }
    const auto duplicate = laptop.push_mod_catalog(same_user_on_b, catalog.dump(), 0);
    CHECK(duplicate.state == SaveState::Conflict && duplicate.version == 1);
    CHECK(laptop.push_mod_catalog(same_user_on_b, catalog.dump(), 1).version == 2);
    CHECK(desktop.mod_catalog(a, "mp9", "fixture").version == 2);
    auto foreign = other_user;
    foreign.user_id = a.user_id;
    CHECK(laptop.mod_catalog(foreign, "mp9", "fixture").state == SaveState::Missing);
}


void live_recovery(upd::HttpClient& http, AuthClient& auth, const std::string& email) {
    const auto mail = std::getenv("WL_AUTH_TEST_MAIL");
    if (!mail) return;
    const std::string origin(mail);
    CHECK((Config{origin, kPublicKey}.valid() && origin.rfind("http://127.0.0.1:", 0) == 0));
    if (origin.rfind("http://127.0.0.1:", 0) != 0 || !Config{origin, kPublicKey}.valid()) return;

    CHECK(auth.request_recovery(email).ok());
    const std::string anchor = "Wii Ports recovery code: ";
    std::string code, ignored;
    for (int i = 0; i < 25 && code.empty(); ++i) {
        upd::HttpRequest request;
        request.url = origin + "/api/v1/messages?limit=50";
        const auto listed = http.get(request);
        mods::Json messages;
        if (listed.status == 200 && mods::parse_json(listed.body, messages, ignored) && messages["messages"].is_array()) {
            for (const auto& message : messages["messages"].elements()) {
                bool mine = false;
                for (const auto& to : message["To"].elements()) mine = mine || to["Address"].str() == email;
                if (!mine) continue;
                const auto id = message["ID"].str();
                if (id.empty() || id.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.") != id.npos)
                    continue;
                request.url = origin + "/api/v1/message/" + id;
                const auto fetched = http.get(request);
                mods::Json full;
                if (fetched.status != 200 || !mods::parse_json(fetched.body, full, ignored)) continue;
                auto body = full["HTML"].str();
                if (body.empty()) body = full["Text"].str();
                const auto offset = body.find(anchor);
                if (offset != body.npos) code = body.substr(offset + anchor.size(), 6);
            }
        }
        if (code.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(120));
    }
    CHECK(code.size() == 6 && code.find_first_not_of("0123456789") == code.npos);
    if (code.size() != 6) return;
    const std::string updated = "FixtureRecuperada52!ConCorreo";
    CHECK(auth.reset_password(email, code, updated).ok());
    CHECK(!auth.sign_in(email, kPassword).ok());
    const auto recovered = auth.sign_in(email, updated);
    CHECK(recovered.ok());
    if (recovered.ok()) CHECK(auth.sign_out(recovered.session.access_token).ok());
    if (!failures) std::cout << "Recuperación real por código SMTP local verificada.\n";
}


void live_service(const Config& cfg, const std::string& email, const std::string& owner) {
    const auto root = scratch();
    const auto settled = [](const AccountService& service) {
        for (int i = 0; i < 280; ++i) {
            if (!service.view().busy) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        return false;
    };
    AccountService a(cfg, root / "desktop", {}), b(cfg, root / "laptop", {});
    if (!settled(a) || !settled(b)) { CHECK(false); return; }
    a.sign_in(email, kPassword, false);
    b.sign_in(email, kPassword, false);
    if (!settled(a) || !settled(b)) { CHECK(false); return; }
    CHECK(a.view().local_owner == owner && b.view().local_owner == owner);
    InstallStatus status;
    status.folder = status.built = status.disc_ok = true;
    status.disc_id = "SUPP01";
    status.revision = "fixture";
    const auto install = root / "game";
    fs::create_directories(install);
    const auto local_a = a.local_save("wiiparty", install, status);
    const auto local_b = b.local_save("wiiparty", install, status);
    CHECK(local_a && local_b && local_a->work != local_b->work);
    if (!local_a || !local_b) return;
    a.enable_save(*local_a);
    b.enable_save(*local_b);
    CHECK(settled(a) && settled(b) && save_location_enabled(*local_a) && save_location_enabled(*local_b));
    fs::create_directories(local_a->target.directory);
    {
        std::ofstream fixture(local_a->target.directory / "service-fixture.bin");
        fixture << "progreso del servicio de cuenta";
        CHECK(fixture.good());
    }
    CHECK(a.synchronize_save(*local_a) && settled(a) && !a.view().warning);
    if (a.view().warning) std::cerr << a.view().message << '\n';
    b.restore_cloud_save(*local_b);
    CHECK(settled(b) && !b.view().warning);
    if (b.view().warning) std::cerr << b.view().message << '\n';
    std::ifstream restored(local_b->target.directory / "service-fixture.bin");
    const std::string data{std::istreambuf_iterator<char>(restored), {}};
    CHECK(data == "progreso del servicio de cuenta");
    b.sign_out();
    CHECK(settled(b) && b.view().local_owner.empty() && fs::exists(local_b->target.directory / "service-fixture.bin"));
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
    if (second_device.ok()) live_mods(*http_a, *http_b, cfg, renewed.session, second_device.session, b1.session);
    live_service(cfg, email_a, renewed.session.user_id);
    if (second_device.ok()) CHECK(b.sign_out(second_device.session.access_token).ok());

    CHECK(a.sign_out(renewed.session.access_token).ok() && vault.clear(error));
    CHECK(!a.refresh(renewed.session.refresh_token).ok());
    CHECK(b.sign_out(b1.session.access_token).ok());
    live_recovery(*http_b, b, email_b);
    if (!failures) std::cout << "Supabase real local: cuentas A/B, sesión, RLS, Storage y conflicto de guardados correctos.\n";
    return failures ? 1 : 0;
}


int port_smoke(const fs::path& root) {
    if (!mods::is_within(fs::temp_directory_path(), root) ||
        !fs::is_regular_file(root / ".account-port-fixture") || game_process_running("mp9")) return 2;

    const auto manifest = load_manifest((fs::u8path(WL_MANIFEST_DIR) / "mp9.toml").u8string());
    if (!manifest.ok) return 2;
    const auto adapter = make_adapter(manifest.manifest);
    const auto install = adapter->probe(root);
    const auto location = save_location(root / "account-data", new_id(), "mp9", root, install);
    if (!location || !enable_save_location(*location)) return 2;
    auto spec = adapter->offline(root);
    std::string error;
    if (!launch_with_save_location(spec, *location, [] { return game_process_running("mp9"); }, error)) return 2;
    spec.env["WP_HEADLESS"] = "1";
    spec.env["WP_LOG_FILE"] = (root / "port-smoke.log").u8string();
    const auto process = spawn(spec);
    if (!process.ok) return 2;
    HANDLE handle = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, static_cast<DWORD>(process.pid));
    if (!handle) return 2;
    bool mounted = false;
    for (int i = 0; i < 120 && WaitForSingleObject(handle, 100) == WAIT_TIMEOUT; ++i) {
        if (fs::is_directory(location->target.directory) &&
            fs::is_regular_file(location->nand / "shared2" / "sys" / "SYSCONF")) {
            mounted = true;
            break;
        }
    }
    if (WaitForSingleObject(handle, 0) == WAIT_TIMEOUT) {
        TerminateProcess(handle, 0);
        WaitForSingleObject(handle, 3000);
    }
    CloseHandle(handle);
    CHECK(mounted && !fs::exists(root / "games" / "mp9" / "nand"));
    if (!failures) std::cout << "MP9 real arrancó y montó la NAND aislada de la cuenta.\n";
    return failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--port-smoke") return port_smoke(fs::u8path(argv[2]));
    if (argc == 2 && std::string(argv[1]) == "--live") return live();
    unit_tests();
    save_tests();
    if (!failures) std::cout << "Contratos de cuenta y DPAPI correctos.\n";
    return failures ? 1 : 0;
}
