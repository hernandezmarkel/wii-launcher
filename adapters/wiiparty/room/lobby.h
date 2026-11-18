#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Private online room over a LAN or a virtual LAN such as Hamachi. One launcher hosts on a TCP port and
// the others join by address. There is no server: the host keeps the roster and relays it to everyone.

constexpr int kLobbyProtocol = 1;
constexpr uint16_t kLobbyDefaultPort = 47630;
constexpr int kLobbySeats = 4;

struct LobbyIdentity {
    std::string name;
    std::string build;
    std::string content;
};

struct LobbyPlayer {
    int id = 0;
    std::string name;
    int seat = -1;
    bool ready = false;
};

enum class LobbyState { Idle, Hosting, Connecting, Joined, Started, Failed };

struct LobbySnapshot {
    LobbyState state = LobbyState::Idle;
    std::string message;
    std::vector<LobbyPlayer> players;
    int my_id = -1;
    uint16_t port = 0;
    std::string host;
    uint32_t session = 0;
    uint32_t seed = 0;
    uint64_t epoch = 0;
};

class Lobby {
public:
    Lobby() = default;
    Lobby(const Lobby&) = delete;
    Lobby& operator=(const Lobby&) = delete;
    ~Lobby();

    bool host(uint16_t port, const LobbyIdentity& me);
    bool join(const std::string& address, uint16_t port, const LobbyIdentity& me);
    void leave();
    void choose_seat(int seat);
    void set_ready(bool ready);
    void start();
    LobbySnapshot snapshot();
    bool can_start();

private:
    struct Peer;
    void run_host(uintptr_t listener);
    void run_client(uintptr_t socket);
    void fail(const std::string& message);
    void stop_thread();

    std::mutex mutex_;
    LobbySnapshot shared_;
    LobbyIdentity me_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::vector<std::string> outbox_;
    int wanted_seat_ = -2;
    int wanted_ready_ = -1;
    bool wanted_start_ = false;
};

std::vector<std::string> local_addresses();
std::string sanitize_name(const std::string& name);
