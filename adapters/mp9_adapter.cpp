// Adaptador de Mario Party 9. Reproduce lo que hace mp9-recomp/launcher/src/app_online.cpp
// (App::launch_online) sin cambiar nada del protocolo ni del netcode.
#include <chrono>

#include "common.h"
#include "mp9/room/online.h"

namespace wl {

class Mp9Adapter final : public RecompAdapter {
public:
    using RecompAdapter::RecompAdapter;

    // online.cpp envía "HELLO <payload_revision()>/<disc_id>" y el anfitrión exige que coincida.
    // payload_revision() es lo que el launcher guarda en .launcher-revision al instalar.
    std::string compat_token(const InstallStatus& s) const override { return s.revision + "/" + s.disc_id; }

    std::optional<LaunchSpec> online(const fs::path& root, const SessionStart& st) const override {
        if (st.seat < 0 || st.seats == 0 || st.host.empty() || st.port == 0) return std::nullopt;
        LaunchSpec spec = offline(root);
        spec.env["WP_NET_SESSION"] = std::to_string(st.session);
        spec.env["WP_NET_SEAT"] = std::to_string(st.seat);
        spec.env["WP_NET_SEATS"] = std::to_string(st.seats);
        spec.env["WP_NET_HOST"] = st.host;
        spec.env["WP_NET_PORT"] = std::to_string(st.port);
        spec.env["WP_NET_EPOCH"] = std::to_string(st.epoch);
        spec.env["WP_NET_DELAY"] = std::to_string(st.delay);
        spec.env["WP_NET_MIN_DELAY"] = std::to_string(st.min_delay);
        return spec;
    }

    std::unique_ptr<RoomSession> make_room() const override;
};

// Envuelve online::Room (copia literal del launcher de MP9) en el contrato común.
class Mp9Room final : public RoomSession {
public:
    bool host(uint16_t port, const std::string& name, const std::string& token, std::string& error) override {
        bool ok = room_.host(port, name, token, error);
        if (!ok) room_.leave();
        return ok;
    }
    bool join(const std::string& address, uint16_t port, const std::string& name, const std::string& token, std::string& error) override {
        return room_.join(address, port, name, token, error);
    }
    void leave() override { room_.leave(); closing_ = false; }
    bool poll(SessionStart& out) override {
        online::StartInfo info;
        if (closing_) {
            // El anfitrión ya empezó: sigue enviando unos instantes y cierra después (ver abajo).
            room_.poll(info);
            if (std::chrono::steady_clock::now() >= close_at_) { room_.leave(); closing_ = false; }
            return false;
        }
        bool hosting = room_.mode() == online::Mode::Hosting;
        bool go = room_.poll(info) || room_.take_host_start(info);
        if (!go) return false;
        out = SessionStart{};
        out.session = info.session;
        out.seats = info.seats;
        out.seat = info.seat;
        out.epoch = info.epoch;
        out.host = info.host;
        out.port = info.port;
        out.delay = info.delay;
        out.min_delay = info.min_delay;
        if (hosting) {
            // El launcher original cierra la sala del anfitrión justo al empezar. Si el START y el cierre
            // llegan juntos al invitado, su poll() ve el cierre antes de leer el START y se queda fuera
            // (reproducido en tests por loopback). Aquí el anfitrión mantiene la sala 2 s más; el protocolo no cambia.
            closing_ = true;
            close_at_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        } else {
            room_.leave();  // como App::launch_online()
        }
        return true;
    }
    void choose_seat(int seat) override { room_.choose_seat(seat); }
    void set_ready(bool ready) override { room_.set_ready(ready); }
    bool can_start() const override { return room_.can_start(); }
    // Mismo mapeo que online_min_delay() de app_online.cpp: 0 -> 2, 1 -> 3, 2 -> 5 fotogramas.
    void start(int option) override { room_.start(option <= 0 ? 2 : option == 1 ? 3 : 5); }
    std::vector<std::string> start_options() const override { return {"Cable, poco ping", "Normal", "WiFi inestable"}; }
    RoomMode mode() const override {
        if (closing_) return RoomMode::Idle;
        switch (room_.mode()) {
        case online::Mode::Hosting: return RoomMode::Hosting;
        case online::Mode::Joining: return RoomMode::Joining;
        case online::Mode::Joined: return RoomMode::Joined;
        default: return RoomMode::Idle;
        }
    }
    int self() const override { return room_.self(); }
    std::vector<RoomMember> members() const override {
        std::vector<RoomMember> out;
        for (const auto& m : room_.members()) out.push_back({m.id, m.seat, m.ready, m.name});
        return out;
    }
    std::string status() const override { return room_.status(); }
    int seats() const override { return online::kSeats; }
    uint16_t default_port() const override { return online::kDefaultPort; }

private:
    online::Room room_;
    bool closing_ = false;
    std::chrono::steady_clock::time_point close_at_;
};

std::unique_ptr<RoomSession> Mp9Adapter::make_room() const { return std::make_unique<Mp9Room>(); }

std::vector<std::string> local_addresses() { return online::local_addresses(); }

std::unique_ptr<GameAdapter> make_mp9_adapter(const Manifest& m) { return std::make_unique<Mp9Adapter>(m, "mp9"); }

}
