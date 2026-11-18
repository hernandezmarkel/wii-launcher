// Adaptador de Wii Party. Reproduce online_launch() y online_content() de launcher/src/app_online.cpp
// (launcher 2611-029 del sobremesa). La sala es lobby.cpp copiado tal cual en wiiparty/room/.
#include <fstream>

#include "common.h"
#include "wiiparty/room/lobby.h"
#include "wiiparty/room/sha256.h"

// lobby.cpp avisa al bucle principal con esta función. La UI ya despierta cada 33 ms con una sala
// abierta, así que no hace falta nada más.
void wake_main_loop() {}

namespace wl {

namespace {

// WP_LAUNCHER_BUILD del launcher del que se copió lobby.cpp. El HELLO exige que coincida: así este
// launcher puede jugar con quien use ese mismo launcher antiguo, y rechaza otras versiones.
constexpr const char* kWiiPartyLobbyBuild = "2611-029";

std::string error_text(const std::string& code) {
    if (code == "port") return "No se pudo abrir el puerto. ¿Hay otra sala abierta en este PC?";
    if (code == "socket") return "No se pudo usar la red de este PC.";
    if (code == "address") return "No se encuentra esa dirección.";
    if (code == "connect") return "No se pudo conectar. Revisa la dirección, el puerto y el cortafuegos del anfitrión.";
    if (code == "timeout") return "El anfitrión no responde.";
    if (code == "closed") return "Se cerró la conexión con la sala.";
    if (code == "reject_protocol" || code == "reject_build") return "El anfitrión usa otra versión del launcher.";
    if (code == "reject_content") return "El anfitrión tiene otra versión del juego. Instalad la misma.";
    if (code == "reject_started") return "La partida ya ha empezado.";
    if (code == "reject_full") return "La sala está llena.";
    return code;
}

// compat_token = "<build>\t<content>"; la sala lo separa para el HELLO.
LobbyIdentity identity(const std::string& name, const std::string& token) {
    size_t tab = token.find('\t');
    return LobbyIdentity{sanitize_name(name), token.substr(0, tab), tab == std::string::npos ? std::string() : token.substr(tab + 1)};
}

class WiiPartyRoom final : public RoomSession {
public:
    bool host(uint16_t port, const std::string& name, const std::string& token, std::string& error) override {
        launched_ = false;
        if (!lobby_.host(port, identity(name, token))) {
            error = error_text(lobby_.snapshot().message);
            return false;
        }
        // En la sala original el anfitrión también pulsa Listo. Aquí lo es al elegir puesto: lo
        // indica pulsando Empezar.
        lobby_.set_ready(true);
        return true;
    }
    bool join(const std::string& address, uint16_t port, const std::string& name, const std::string& token, std::string&) override {
        launched_ = false;
        return lobby_.join(address, port, identity(name, token));
    }
    void leave() override { lobby_.leave(); }
    bool poll(SessionStart& out) override {
        LobbySnapshot room = lobby_.snapshot();
        if (room.state != LobbyState::Started || launched_) return false;
        launched_ = true;
        // Como App::online_launch(). La sala sigue abierta (igual que en el original), así que el
        // START no compite con el cierre.
        out = SessionStart{};
        out.session = room.session;
        out.seed = room.seed;
        out.epoch = room.epoch;
        out.port = room.port;
        out.host = room.my_id == 0 ? std::string() : room.host;
        for (const LobbyPlayer& p : room.players) {
            if (p.id == room.my_id) out.seat = p.seat;
            if (p.seat >= 0) out.seat_mask |= 1u << p.seat;
        }
        return true;
    }
    void choose_seat(int seat) override {
        lobby_.choose_seat(seat);
        if (lobby_.snapshot().my_id == 0) lobby_.set_ready(true);
    }
    void set_ready(bool ready) override { lobby_.set_ready(ready); }
    bool can_start() const override { return lobby_.can_start(); }
    void start(int) override { lobby_.start(); }
    RoomMode mode() const override {
        switch (lobby_.snapshot().state) {
        case LobbyState::Hosting: return RoomMode::Hosting;
        case LobbyState::Connecting: return RoomMode::Joining;
        case LobbyState::Joined: return RoomMode::Joined;
        default: return RoomMode::Idle;  // Started: el juego ya se lanzó; Failed: se muestra status()
        }
    }
    int self() const override { return lobby_.snapshot().my_id; }
    std::vector<RoomMember> members() const override {
        std::vector<RoomMember> out;
        for (const LobbyPlayer& p : lobby_.snapshot().players) out.push_back({p.id, p.seat, p.ready, p.name});
        return out;
    }
    std::string status() const override {
        LobbySnapshot room = lobby_.snapshot();
        switch (room.state) {
        case LobbyState::Failed: return error_text(room.message);
        case LobbyState::Hosting: return "Sala abierta en el puerto " + std::to_string(room.port) + ". Tus amigos se unen con una de las direcciones de abajo.";
        case LobbyState::Connecting: return "Conectando con " + room.host + ":" + std::to_string(room.port) + "...";
        case LobbyState::Joined: return "Dentro de la sala de " + room.host + ". Elige jugador y pulsa Listo.";
        default: return "";
        }
    }
    int seats() const override { return kLobbySeats; }
    uint16_t default_port() const override { return kLobbyDefaultPort; }

private:
    mutable Lobby lobby_;  // snapshot() y can_start() no son const en el original
    bool launched_ = false;
};

}

class WiiPartyAdapter final : public RecompAdapter {
public:
    using RecompAdapter::RecompAdapter;
    // El HELLO compara build (WP_LAUNCHER_BUILD) y content = sha256(payload_revision + "\n" +
    // translation_inputs)[0..16]. translation_inputs es lo que el launcher guardó en
    // build/wiiparty/translation.stamp al compilar; payload_revision, lo de .launcher-revision.
    std::string compat_token(const InstallStatus& s) const override {
        std::string stamp = read_first_line(s.root / "build" / "wiiparty" / "translation.stamp");
        if (s.revision.empty() || stamp.empty()) return std::string(kWiiPartyLobbyBuild) + "\t";
        return std::string(kWiiPartyLobbyBuild) + "\t" + sha256_of_bytes(s.revision + "\n" + stamp).substr(0, 16);
    }

    std::unique_ptr<RoomSession> make_room() const override { return std::make_unique<WiiPartyRoom>(); }

    std::optional<LaunchSpec> online(const fs::path& root, const SessionStart& st) const override {
        if (st.seat < 0 || st.seat > 3 || !(st.seat_mask & (1u << st.seat)) || st.port == 0) return std::nullopt;
        LaunchSpec spec = offline(root);
        spec.env["WP_NET_SESSION"] = std::to_string(st.session);
        spec.env["WP_NET_SEED"] = std::to_string(st.seed);
        spec.env["WP_NET_EPOCH"] = std::to_string(st.epoch);
        spec.env["WP_NET_SEAT"] = std::to_string(st.seat);
        spec.env["WP_NET_SEATS"] = std::to_string(st.seat_mask);
        spec.env["WP_NET_HOST"] = st.host;  // vacío en el anfitrión
        spec.env["WP_NET_PORT"] = std::to_string(st.port);
        return spec;
    }
};

std::unique_ptr<GameAdapter> make_wiiparty_adapter(const Manifest& m) { return std::make_unique<WiiPartyAdapter>(m, "wiiparty"); }

std::unique_ptr<GameAdapter> make_mp9_adapter(const Manifest& m);
std::unique_ptr<GameAdapter> make_mkwii_adapter(const Manifest& m);

std::unique_ptr<GameAdapter> make_adapter(const Manifest& m) {
    if (m.status == GameStatus::Planned) return nullptr;
    if (m.adapter == "mp9") return make_mp9_adapter(m);
    if (m.adapter == "wiiparty") return make_wiiparty_adapter(m);
    if (m.adapter == "mkwii") return make_mkwii_adapter(m);
    return nullptr;
}

}
