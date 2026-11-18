// Tests del contrato: manifests válidos y maliciosos, catálogo, adaptadores y procesos.
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <chrono>
#include <thread>
#ifndef _WIN32
#include <signal.h>
#endif

#include "wl/catalog.h"
#include "wl/process.h"

using namespace wl;

int failures = 0, checks = 0;  // compartidos con updater_tests.cpp
void run_updater_tests();
#define CHECK(x)                                                                 \
    do {                                                                         \
        ++checks;                                                                \
        if (!(x)) { ++failures; std::cerr << "FALLO " << __LINE__ << ": " #x "\n"; } \
    } while (0)

static const char* kGood = R"(manifest_version = 1
game_id = "mp9"
display_name = "Mario Party 9"
status = "in_development"
adapter = "mp9"
adapter_version = 1
install_folder = "MarioParty9Online"
executable = "bin/mp9.exe"
disc_ids = ["SSQE01"]
max_players = 4
capabilities = ["local", "lan_host", "lan_join"]
)";

static std::string with(const std::string& key, const std::string& line) {
    std::string out, l;
    std::stringstream in(kGood);
    bool replaced = false;
    while (std::getline(in, l)) {
        if (l.rfind(key + " =", 0) == 0) { if (!line.empty()) out += line + "\n"; replaced = true; }
        else out += l + "\n";
    }
    if (!replaced) out += line + "\n";
    return out;
}

static void write(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}

int main() {
    // 1. Manifest válido
    auto r = parse_manifest(kGood);
    CHECK(r.ok);
    CHECK(r.manifest.has("lan_join"));
    CHECK(r.manifest.max_players == 4);

    // 2. Rutas peligrosas en executable
    for (const char* bad : {"../x.exe", "bin/../../x.exe", "C:/Windows/notepad.exe", "/usr/bin/x.exe", "\\\\server\\share\\x.exe",
                            "%TEMP%/x.exe", "$HOME/x.exe", "~/x.exe", "bin\\..\\..\\x.exe", "bin/x.exe:stream", "bin/x.exe ", "bin/ok.bat"}) {
        CHECK(!parse_manifest(with("executable", std::string("executable = \"") + bad + "\"")).ok);
    }
    // 3. install_folder solo un nombre
    for (const char* bad : {"..", "a/b", "C:x", "", "."}) CHECK(!parse_manifest(with("install_folder", std::string("install_folder = \"") + bad + "\"")).ok);
    // 4. Capacidades: internet no existe
    CHECK(!parse_manifest(with("capabilities", "capabilities = [\"local\", \"internet\"]")).ok);
    CHECK(!parse_manifest(with("capabilities", "capabilities = [\"lan_host\"]")).ok);  // sin local
    // 5. Claves desconocidas, repetidas, tipos
    CHECK(!parse_manifest(std::string(kGood) + "command = \"rm -rf\"\n").ok);
    CHECK(!parse_manifest(std::string(kGood) + "game_id = \"otro\"\n").ok);
    CHECK(!parse_manifest(with("max_players", "max_players = \"4\"")).ok);
    CHECK(!parse_manifest(with("max_players", "max_players = 99")).ok);
    CHECK(!parse_manifest(with("game_id", "game_id = \"MP9!\"")).ok);
    CHECK(!parse_manifest(with("adapter", "adapter = \"dll:evil\"")).ok);
    CHECK(!parse_manifest(with("manifest_version", "manifest_version = 2")).ok);
    CHECK(!parse_manifest(with("disc_ids", "disc_ids = [\"ssqe\"]")).ok);
    CHECK(!parse_manifest("basura sin igual\n").ok);
    // 6. Previsto: sin ejecutable ni capacidades
    CHECK(parse_manifest("manifest_version = 1\ngame_id = \"mkwii\"\ndisplay_name = \"MKW\"\nstatus = \"planned\"\nadapter = \"none\"\nadapter_version = 1\nmax_players = 1\n").ok);
    CHECK(!parse_manifest("manifest_version = 1\ngame_id = \"mkwii\"\ndisplay_name = \"MKW\"\nstatus = \"planned\"\nadapter = \"none\"\nadapter_version = 1\nmax_players = 1\ncapabilities = [\"local\"]\n").ok);

    // 7. Catálogo real del repo
    Catalog c = load_catalog("manifests");
    CHECK(c.errors.empty());
    for (const auto& e : c.errors) std::cerr << "  " << e << "\n";
    CHECK(c.games.size() == 4);

    // 8. resolve_inside
    fs::path root = fs::temp_directory_path() / "wl_tests_root";
    CHECK(resolve_inside(root, "bin/mp9.exe").has_value());
    CHECK(!resolve_inside(root, "../x.exe").has_value());

    // 9. Adaptador MP9 contra una instalación falsa
    fs::path lad = fs::temp_directory_path() / "wl_tests_lad";
    fs::remove_all(lad);
#ifdef _WIN32
    _putenv_s("WL_LOCALAPPDATA", lad.string().c_str());
#else
    setenv("WL_LOCALAPPDATA", lad.c_str(), 1);
#endif
    Manifest m = r.manifest;
    auto a = make_adapter(m);
    CHECK(a != nullptr);
    InstallStatus s = a->probe(install_root(m));
    CHECK(!s.ready() && !s.folder && !s.problem.empty());
    fs::path inst = lad / "MarioParty9Online";
    write(inst / "bin" / "mp9.exe", "x");
    write(inst / ".launcher-revision", "d755eb8\n");
    write(inst / "games" / "mp9" / "extracted" / "sys" / "boot.bin", "SSQE01\x00\x00");
    s = a->probe(install_root(m));
    CHECK(s.ready());
    CHECK(a->compat_token(s) == "d755eb8/SSQE01");  // misma cadena que HELLO del launcher original
    write(inst / "games" / "mp9" / "extracted" / "sys" / "boot.bin", "SSQP01");
    s = a->probe(install_root(m));
    CHECK(!s.ready() && s.problem.find("SSQP01") != std::string::npos);
    LaunchSpec off = a->offline(inst);
    CHECK(off.executable.filename() == "mp9.exe");
    CHECK(off.env.count("WP_LOG_FILE") && !off.env.count("WP_NET_SEAT"));
    SessionStart st;
    st.session = 7; st.seats = 3; st.seat = 1; st.epoch = 99; st.host = "25.1.2.3"; st.port = 47631; st.delay = 4; st.min_delay = 3;
    auto on = a->online(inst, st);
    CHECK(on.has_value());
    CHECK(on && on->env.size() == 9);
    CHECK(on && on->env.at("WP_NET_HOST") == "25.1.2.3" && on->env.at("WP_NET_MIN_DELAY") == "3" && on->env.at("WP_NET_SEATS") == "3");
    st.seat = -1;
    CHECK(!a->online(inst, st).has_value());

    // 10. Wii Party: variables de entorno como App::online_launch()
    auto wp = load_manifest("manifests/wiiparty.toml");
    CHECK(wp.ok);
    auto wa = make_adapter(wp.manifest);
    CHECK(wa && wa->make_room() && !wa->online(inst, SessionStart{}).has_value());
    SessionStart ws;
    ws.session = 5; ws.seed = 1234; ws.epoch = 1700000000; ws.seat = 0; ws.seat_mask = 0b0011; ws.port = 47630;  // anfitrión
    auto won = wa->online(inst, ws);
    CHECK(won && won->env.size() == 8 && won->env.at("WP_NET_SEATS") == "3" && won->env.at("WP_NET_HOST").empty() && won->env.at("WP_NET_SEED") == "1234");
    ws.seat = 2;  // asiento no ocupado según la máscara
    CHECK(!wa->online(inst, ws).has_value());
    // 11. Previstos: sin adaptador
    CHECK(make_adapter(load_manifest("manifests/zz_brawl.toml").manifest) == nullptr);

    // 12. Sala de MP9 (código original) por loopback: anfitrión + invitado, versión distinta rechazada
    {
        auto pump = [](RoomSession& a, RoomSession& b, SessionStart& sa, SessionStart& sb, bool& ga, bool& gb, int ms) {
            auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
            while (std::chrono::steady_clock::now() < until) {
                if (!ga) ga = a.poll(sa);
                if (!gb) gb = b.poll(sb);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        };
        auto host = a->make_room();
        auto guest = a->make_room();
        auto stranger = a->make_room();
        CHECK(host && guest && stranger);
        std::string err;
        uint16_t port = 47712;
        CHECK(host->host(port, "Ana", "d755eb8/SSQE01", err));
        CHECK(guest->join("127.0.0.1", port, "Beñat", "d755eb8/SSQE01", err));
        SessionStart sh, sg;
        bool gh = false, gg = false;
        pump(*host, *guest, sh, sg, gh, gg, 300);
        CHECK(guest->mode() == RoomMode::Joined);
        CHECK(host->members().size() == 2);
        CHECK(stranger->join("127.0.0.1", port, "Otro", "1234567/SSQE01", err));
        SessionStart sx;
        bool gx = false;
        pump(*host, *stranger, sh, sx, gh, gx, 300);
        CHECK(stranger->mode() == RoomMode::Idle);  // rechazado por versión
        CHECK(host->members().size() == 2);
        guest->choose_seat(1);
        pump(*host, *guest, sh, sg, gh, gg, 200);
        guest->set_ready(true);
        pump(*host, *guest, sh, sg, gh, gg, 200);
        CHECK(host->can_start());
        host->start(1);
        pump(*host, *guest, sh, sg, gh, gg, 400);
        CHECK(gh && gg);
        CHECK(sh.session == sg.session && sh.session != 0);
        CHECK(sh.seats == 0b11 && sg.seats == 0b11);
        CHECK(sh.seat == 0 && sg.seat == 1);
        CHECK(sh.min_delay == 3 && sg.min_delay == 3);
        CHECK(sh.host.empty() && sg.host == "127.0.0.1");
        auto spec = a->online(inst, sg);
        CHECK(spec && spec->env.at("WP_NET_SEATS") == "3" && spec->env.at("WP_NET_SEAT") == "1");
    }

    // 12b. Sala de Wii Party (lobby.cpp original) por loopback, y su compat_token
    {
        fs::path wpi = lad / "WiiPartyOnline";
        write(wpi / "bin" / "wiiparty.exe", "x");
        write(wpi / ".launcher-revision", "c055018\n");
        write(wpi / "games" / "wiiparty" / "extracted" / "sys" / "boot.bin", "SUPP01");
        write(wpi / "build" / "wiiparty" / "translation.stamp", "abc\n");
        InstallStatus ws2 = wa->probe(wpi);
        CHECK(ws2.ready());
        std::string tok = wa->compat_token(ws2);
        // sha256("c055018\nabc")[0..16], calculado aparte con sha256sum
        CHECK(tok == "2611-029\t284c730c121526b6");
        auto pump = [](RoomSession& a, RoomSession& b, SessionStart& sa, SessionStart& sb, bool& ga, bool& gb, int ms) {
            auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
            while (std::chrono::steady_clock::now() < until) {
                if (!ga) ga = a.poll(sa);
                if (!gb) gb = b.poll(sb);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        };
        auto host = wa->make_room();
        auto guest = wa->make_room();
        auto stranger = wa->make_room();
        CHECK(host && guest && stranger);
        std::string err;
        uint16_t port = 47714;
        CHECK(host->host(port, "Ana", tok, err));
        CHECK(guest->join("127.0.0.1", port, "Beñat", tok, err));
        SessionStart sh, sg;
        bool gh = false, gg = false;
        pump(*host, *guest, sh, sg, gh, gg, 300);
        CHECK(guest->mode() == RoomMode::Joined);
        CHECK(host->members().size() == 2);
        CHECK(stranger->join("127.0.0.1", port, "Otro", "2611-029\t0000000000000000", err));
        SessionStart sx;
        bool gx = false;
        pump(*host, *stranger, sh, sx, gh, gx, 300);
        CHECK(stranger->mode() == RoomMode::Idle);
        CHECK(stranger->status().find("otra versión del juego") != std::string::npos);
        CHECK(host->members().size() == 2);
        CHECK(!host->can_start());  // el invitado aún no está listo
        guest->choose_seat(2);
        pump(*host, *guest, sh, sg, gh, gg, 200);
        guest->set_ready(true);
        pump(*host, *guest, sh, sg, gh, gg, 200);
        CHECK(host->can_start());
        host->start(0);
        pump(*host, *guest, sh, sg, gh, gg, 400);
        CHECK(gh && gg);
        CHECK(sh.session == sg.session && sh.session != 0 && sh.seed == sg.seed && sh.epoch == sg.epoch);
        CHECK(sh.seat_mask == 0b101 && sg.seat_mask == 0b101);
        CHECK(sh.seat == 0 && sg.seat == 2);
        CHECK(sh.host.empty() && sg.host == "127.0.0.1" && sg.port == port);
        auto hs = wa->online(wpi, sh);
        auto gs = wa->online(wpi, sg);
        CHECK(hs && gs && hs->env.at("WP_NET_SEATS") == "5" && gs->env.at("WP_NET_SEAT") == "2" && gs->env.at("WP_NET_HOST") == "127.0.0.1");
        CHECK(host->mode() == RoomMode::Idle && guest->mode() == RoomMode::Idle);  // ya lanzados
    }

    // 12c. installer y status beta
    {
        auto b = parse_manifest(with("status", "status = \"beta\"") + "installer = \"instaladores/x.ps1\"\n");
        CHECK(b.ok && b.manifest.status == GameStatus::Beta && b.manifest.installer == "instaladores/x.ps1");
        CHECK(!parse_manifest(std::string(kGood) + "installer = \"../x.exe\"\n").ok);
        CHECK(!parse_manifest(std::string(kGood) + "installer = \"x.bat\"\n").ok);
        CHECK(load_manifest("manifests/mp9.toml").manifest.installer == "instaladores/mp9online-launcher.exe");
    }

    // 12d. Mario Kart Wii (beta): probe y edición de [network] sin tocar el resto de Config.toml
    {
        auto mk = load_manifest("manifests/zz_mkwii.toml");
        CHECK(mk.ok && mk.manifest.status == GameStatus::Beta && mk.manifest.has("lan_wfc"));
        auto ka = make_adapter(mk.manifest);
        CHECK(ka != nullptr && ka->make_room() == nullptr);
        fs::path mki = lad / "MKWiiLAN";
        CHECK(!ka->probe(mki).ready());
        write(mki / "juego" / "WiiCompiled.exe", "x");
        write(lad / "dump" / "sys" / "boot.bin", "RMCE01");
        std::string cfg = "# prueba\n[video]\nvsync = false\n\n[paths]\ndvd_root = '" + (lad / "dump").generic_u8string() + "'\n";
        write(mki / "juego" / "UserData" / "Config.toml", cfg);
        InstallStatus ks = ka->probe(mki);
        CHECK(ks.ready() && ks.disc_id == "RMCE01");
        std::string err;
        CHECK(!ka->set_lan_server(mki, "192.168.1.300", err) && !err.empty());
        CHECK(!ka->set_lan_server(mki, "1.2.3.4; rm", err));
        CHECK(ka->set_lan_server(mki, "192.168.1.135", err));
        CHECK(ka->lan_server(mki) == "192.168.1.135");
        CHECK(ka->set_lan_server(mki, "10.0.0.7", err));
        CHECK(ka->lan_server(mki) == "10.0.0.7");
        std::ifstream f(mki / "juego" / "UserData" / "Config.toml");
        std::stringstream all; all << f.rdbuf();
        std::string out = all.str();
        f.close();
        CHECK(out.find("vsync = false") != std::string::npos && out.find("dvd_root") != std::string::npos);
        CHECK(out.find("192.168.1.135") == std::string::npos);  // reemplazada, no duplicada
        CHECK(out.find("lan_nas_port = 8080") != std::string::npos && out.find("[network]") == out.rfind("[network]"));
        CHECK(ka->set_lan_server(mki, "", err) && ka->lan_server(mki).empty());  // vacío: offline
        CHECK(ka->probe(mki).ready());
        CHECK(valid_ipv4("25.1.2.3") && !valid_ipv4("1.2.3") && !valid_ipv4("a.b.c.d"));
    }

    // 13. Procesos: entorno añadido y espera sin bloquear
#ifndef _WIN32
    LaunchSpec sh;
    sh.executable = "/bin/sh";
    sh.args = {"-c", "test \"$WL_PROBE\" = 'hola mundo' && exit 7"};
    sh.env["WL_PROBE"] = "hola mundo";
    Spawned p = spawn(sh);
    CHECK(p.ok);
    CHECK(wait_exit(p.pid, 5000) == 7);
    LaunchSpec slow;
    slow.executable = "/bin/sleep";
    slow.args = {"2"};
    Spawned q = spawn(slow);
    auto t0 = std::chrono::steady_clock::now();
    CHECK(q.ok && wait_exit(q.pid, 50) == -1);  // no bloquea
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(500));
    kill((pid_t)q.pid, SIGKILL);
    wait_exit(q.pid, 2000);
    LaunchSpec missing;
    missing.executable = "/no/existe.exe";
    CHECK(!spawn(missing).ok);
#else
    LaunchSpec cmd;
    cmd.executable = "C:\\Windows\\System32\\cmd.exe";  // cmd.exe confunde "/" en argv[0] con opciones
    cmd.args = {"/c", "exit %WL_PROBE%"};
    cmd.env["WL_PROBE"] = "7";
    Spawned p = spawn(cmd);
    CHECK(p.ok);
    CHECK(wait_exit(p.pid, 5000) == 7);
    LaunchSpec missing;
    missing.executable = "C:/no/existe.exe";
    CHECK(!spawn(missing).ok);
#endif
    fs::remove_all(lad);
#ifdef _WIN32
    run_updater_tests();
#endif
    std::cout << (checks - failures) << "/" << checks << " comprobaciones correctas\n";
    return failures ? 1 : 0;
}
