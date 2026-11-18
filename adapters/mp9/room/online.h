#pragma once

#include <cstdint>
#include <string>
#include <vector>

// The launcher's online room, a LAN game over Hamachi or any network: one PC creates the room and the
// others join it by IP. The room only agrees who plays which seat; when the host starts, every launcher
// opens the game, and the games connect to the host's game on the next port and play in lockstep.
namespace online {

constexpr uint16_t kDefaultPort = 47630;
constexpr int kSeats = 4;

struct Member {
    int id = 0;
    int seat = -1;
    bool ready = false;
    std::string name;
};

struct StartInfo {
    uint32_t session = 0;
    uint32_t seats = 0;
    uint64_t epoch = 0;
    int delay = 4;
    int min_delay = 3;
    int seat = -1;
    std::string host;
    uint16_t port = kDefaultPort;
};

enum class Mode { Idle, Hosting, Joining, Joined };

class Room {
public:
    ~Room();

    bool host(uint16_t port, const std::string& name, const std::string& version, std::string& error);
    bool join(const std::string& address, uint16_t port, const std::string& name, const std::string& version, std::string& error);
    void leave();
    // Call every frame: accepts players, reads messages. Returns true when the game should start now.
    bool poll(StartInfo& start);

    void choose_seat(int seat);
    void set_ready(bool ready);
    // Host only: true when everyone has a seat of their own and is ready.
    bool can_start() const;
    void start(int min_delay);
    // Host only: after start(), the host's own seat and session.
    bool take_host_start(StartInfo& start);

    Mode mode() const { return mode_; }
    int self() const { return self_; }
    const std::vector<Member>& members() const { return members_; }
    const std::string& status() const { return status_; }

private:
    struct Peer {
        intptr_t socket = -1;
        int id = 0;
        std::string in;
        std::string out;
        bool greeted = false;
    };

    void send_line(Peer& peer, const std::string& line);
    void broadcast(const std::string& line);
    void broadcast_state();
    void handle_host_line(Peer& peer, const std::string& line);
    bool handle_client_line(const std::string& line, StartInfo& start);
    bool pump(Peer& peer);
    void drop(size_t index, const char* why);
    Member* member(int id);

    Mode mode_ = Mode::Idle;
    intptr_t listener_ = -1;
    std::vector<Peer> peers_;
    std::vector<Member> members_;
    std::string version_;
    std::string name_;
    std::string address_;
    uint16_t port_ = kDefaultPort;
    int self_ = 0;
    int next_id_ = 1;
    std::string status_;
    bool started_ = false;
    StartInfo pending_start_;
    bool host_start_ready_ = false;
};

// This PC's IPv4 addresses, Hamachi's (25.x.x.x) first.
std::vector<std::string> local_addresses();

}
