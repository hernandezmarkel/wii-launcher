#include "lobby.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <random>
#include <sstream>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
constexpr socket_t kNoSocket = INVALID_SOCKET;
static void close_socket(socket_t s) { closesocket(s); }
static bool would_block() {
    int error = WSAGetLastError();
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
}
static void set_nonblocking(socket_t s) {
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
}
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t kNoSocket = -1;
static void close_socket(socket_t s) { close(s); }
static bool would_block() { return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS; }
static void set_nonblocking(socket_t s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
#endif

void wake_main_loop();

namespace {

constexpr size_t kMaxLine = 512;
constexpr size_t kMaxName = 20;
constexpr auto kConnectTimeout = std::chrono::seconds(6);
constexpr auto kHelloTimeout = std::chrono::seconds(6);

void network_up() {
#ifdef _WIN32
    static bool started = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    (void)started;
#endif
}

std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> fields;
    size_t start = 0;
    while (true) {
        size_t tab = line.find('\t', start);
        fields.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
        if (tab == std::string::npos) {
            return fields;
        }
        start = tab + 1;
    }
}

bool parse_int(const std::string& text, long low, long high, long& value) {
    if (text.empty() || text.size() > 10) {
        return false;
    }
    char* end = nullptr;
    long parsed = std::strtol(text.c_str(), &end, 10);
    if (*end != 0 || parsed < low || parsed > high) {
        return false;
    }
    value = parsed;
    return true;
}

// A TCP stream split into lines. Lines longer than kMaxLine close the connection.
struct Stream {
    socket_t socket = kNoSocket;
    std::string in;
    std::string out;
    bool broken = false;

    void send(const std::string& line) { out += line + "\n"; }

    void flush() {
        while (!broken && !out.empty()) {
            int sent = ::send(socket, out.data(), static_cast<int>(std::min<size_t>(out.size(), 65536)), 0);
            if (sent > 0) {
                out.erase(0, static_cast<size_t>(sent));
            } else if (sent < 0 && would_block()) {
                return;
            } else {
                broken = true;
            }
        }
    }

    void receive() {
        char buffer[1024];
        while (!broken) {
            int got = ::recv(socket, buffer, sizeof(buffer), 0);
            if (got > 0) {
                in.append(buffer, static_cast<size_t>(got));
                if (in.size() > kMaxLine * 8) {
                    broken = true;
                }
            } else if (got < 0 && would_block()) {
                return;
            } else {
                broken = true;
            }
        }
    }

    bool next(std::string& line) {
        size_t end = in.find('\n');
        if (end == std::string::npos) {
            if (in.size() > kMaxLine) {
                broken = true;
            }
            return false;
        }
        line = in.substr(0, end);
        in.erase(0, end + 1);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.size() > kMaxLine) {
            broken = true;
            return false;
        }
        return true;
    }

    void close() {
        if (socket != kNoSocket) {
            // A peer dropped for a protocol reason still gets its last line, such as a REJECT.
            broken = false;
            flush();
            shutdown(socket, 1);
            close_socket(socket);
            socket = kNoSocket;
        }
    }
};

std::string roster_line(const std::vector<LobbyPlayer>& players) {
    std::string line = "ROSTER";
    for (const LobbyPlayer& player : players) {
        line += "\t" + std::to_string(player.id) + "\t" + std::to_string(player.seat) + "\t" + (player.ready ? "1" : "0") + "\t" + player.name;
    }
    return line;
}

bool parse_roster(const std::vector<std::string>& fields, std::vector<LobbyPlayer>& players) {
    if ((fields.size() - 1) % 4 != 0 || fields.size() > 1 + 4 * kLobbySeats) {
        return false;
    }
    std::vector<LobbyPlayer> parsed;
    for (size_t i = 1; i + 3 < fields.size(); i += 4) {
        LobbyPlayer player;
        long id = 0;
        long seat = 0;
        if (!parse_int(fields[i], 0, 1000000, id) || !parse_int(fields[i + 1], -1, kLobbySeats - 1, seat) || (fields[i + 2] != "0" && fields[i + 2] != "1")) {
            return false;
        }
        player.id = static_cast<int>(id);
        player.seat = static_cast<int>(seat);
        player.ready = fields[i + 2] == "1";
        player.name = sanitize_name(fields[i + 3]);
        parsed.push_back(player);
    }
    players = std::move(parsed);
    return true;
}

bool seat_taken(const std::vector<LobbyPlayer>& players, int seat, int except) {
    return seat >= 0 && std::any_of(players.begin(), players.end(), [&](const LobbyPlayer& p) { return p.seat == seat && p.id != except; });
}

int first_free_seat(const std::vector<LobbyPlayer>& players) {
    for (int seat = 0; seat < kLobbySeats; seat++) {
        if (!seat_taken(players, seat, -1)) {
            return seat;
        }
    }
    return -1;
}

}

std::string sanitize_name(const std::string& name) {
    std::string clean;
    for (char c : name) {
        unsigned char u = static_cast<unsigned char>(c);
        if (u >= 0x20 && u != 0x7F) {
            clean += c;
        }
    }
    while (!clean.empty() && clean.back() == ' ') {
        clean.pop_back();
    }
    if (clean.size() > kMaxName) {
        clean.resize(kMaxName);
        while (!clean.empty() && (static_cast<unsigned char>(clean.back()) & 0xC0) == 0x80) {
            clean.pop_back();
        }
        if (!clean.empty() && static_cast<unsigned char>(clean.back()) >= 0xC0) {
            clean.pop_back();
        }
    }
    return clean.empty() ? "?" : clean;
}

std::vector<std::string> local_addresses() {
    network_up();
    std::vector<std::string> found;
    char name[256] = {};
    if (gethostname(name, sizeof(name) - 1) != 0) {
        return found;
    }
    addrinfo hints{};
    hints.ai_family = AF_INET;
    addrinfo* list = nullptr;
    if (getaddrinfo(name, nullptr, &hints, &list) != 0) {
        return found;
    }
    for (addrinfo* it = list; it; it = it->ai_next) {
        char text[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(it->ai_addr)->sin_addr, text, sizeof(text));
        std::string address = text;
        if (address.rfind("127.", 0) != 0 && std::find(found.begin(), found.end(), address) == found.end()) {
            found.push_back(address);
        }
    }
    freeaddrinfo(list);
    // Hamachi hands out 25.x.x.x addresses; show those first.
    std::stable_partition(found.begin(), found.end(), [](const std::string& a) { return a.rfind("25.", 0) == 0 || a.rfind("26.", 0) == 0; });
    return found;
}

Lobby::~Lobby() {
    stop_thread();
}

void Lobby::stop_thread() {
    stop_ = true;
    if (thread_.joinable()) {
        thread_.join();
    }
    stop_ = false;
}

void Lobby::fail(const std::string& message) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shared_.state = LobbyState::Failed;
        shared_.message = message;
    }
    wake_main_loop();
}

LobbySnapshot Lobby::snapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    return shared_;
}

void Lobby::leave() {
    stop_thread();
    std::lock_guard<std::mutex> lock(mutex_);
    shared_ = LobbySnapshot{};
}

void Lobby::choose_seat(int seat) {
    std::lock_guard<std::mutex> lock(mutex_);
    wanted_seat_ = seat;
}

void Lobby::set_ready(bool ready) {
    std::lock_guard<std::mutex> lock(mutex_);
    wanted_ready_ = ready ? 1 : 0;
}

void Lobby::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    wanted_start_ = true;
}

bool Lobby::can_start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shared_.state != LobbyState::Hosting || shared_.players.size() < 2) {
        return false;
    }
    return std::all_of(shared_.players.begin(), shared_.players.end(), [](const LobbyPlayer& p) { return p.seat >= 0 && p.ready; });
}

bool Lobby::host(uint16_t port, const LobbyIdentity& me) {
    leave();
    network_up();
    me_ = me;
    socket_t listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == kNoSocket) {
        fail("socket");
        return false;
    }
    int on = 1;
#ifdef _WIN32
    setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&on), sizeof(on));
#else
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(listener, 8) != 0) {
        close_socket(listener);
        fail("port");
        return false;
    }
    set_nonblocking(listener);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shared_ = LobbySnapshot{};
        shared_.state = LobbyState::Hosting;
        shared_.port = port;
        shared_.my_id = 0;
        shared_.players.push_back({0, sanitize_name(me.name), 0, false});
        wanted_seat_ = -2;
        wanted_ready_ = -1;
        wanted_start_ = false;
    }
    thread_ = std::thread([this, listener] { run_host(static_cast<uintptr_t>(listener)); });
    return true;
}

bool Lobby::join(const std::string& address, uint16_t port, const LobbyIdentity& me) {
    leave();
    network_up();
    me_ = me;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shared_ = LobbySnapshot{};
        shared_.state = LobbyState::Connecting;
        shared_.host = address;
        shared_.port = port;
        wanted_seat_ = -2;
        wanted_ready_ = -1;
        wanted_start_ = false;
    }
    std::string host = address;
    thread_ = std::thread([this, host, port] {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* list = nullptr;
        if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &list) != 0 || !list) {
            fail("address");
            return;
        }
        socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == kNoSocket) {
            freeaddrinfo(list);
            fail("socket");
            return;
        }
        set_nonblocking(s);
        int result = connect(s, list->ai_addr, static_cast<int>(list->ai_addrlen));
        freeaddrinfo(list);
        if (result != 0 && !would_block()) {
            close_socket(s);
            fail("connect");
            return;
        }
        auto deadline = std::chrono::steady_clock::now() + kConnectTimeout;
        while (!stop_) {
            fd_set writable;
            fd_set failed;
            FD_ZERO(&writable);
            FD_ZERO(&failed);
            FD_SET(s, &writable);
            FD_SET(s, &failed);
            timeval wait{0, 100000};
            int ready = select(static_cast<int>(s) + 1, nullptr, &writable, &failed, &wait);
            if (ready > 0) {
                int error = 0;
                socklen_t size = sizeof(error);
                getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &size);
                if (error != 0 || FD_ISSET(s, &failed)) {
                    close_socket(s);
                    fail("connect");
                    return;
                }
                run_client(static_cast<uintptr_t>(s));
                return;
            }
            if (std::chrono::steady_clock::now() > deadline) {
                close_socket(s);
                fail("timeout");
                return;
            }
        }
        close_socket(s);
    });
    return true;
}

struct Lobby::Peer {
    Stream stream;
    int id = -1;
    std::chrono::steady_clock::time_point since;
};

void Lobby::run_host(uintptr_t raw_listener) {
    socket_t listener = static_cast<socket_t>(raw_listener);
    std::vector<Peer> peers;
    int next_id = 1;
    std::mt19937 random(std::random_device{}());
    while (!stop_) {
        bool changed = false;
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(listener, &readable);
        socket_t highest = listener;
        for (Peer& peer : peers) {
            FD_SET(peer.stream.socket, &readable);
            highest = std::max(highest, peer.stream.socket);
        }
        timeval wait{0, 50000};
        select(static_cast<int>(highest) + 1, &readable, nullptr, nullptr, &wait);
        if (FD_ISSET(listener, &readable)) {
            socket_t incoming = accept(listener, nullptr, nullptr);
            if (incoming != kNoSocket) {
                set_nonblocking(incoming);
                int on = 1;
                setsockopt(incoming, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));
                Peer peer;
                peer.stream.socket = incoming;
                peer.since = std::chrono::steady_clock::now();
                peers.push_back(std::move(peer));
            }
        }
        std::unique_lock<std::mutex> lock(mutex_);
        std::vector<LobbyPlayer>& players = shared_.players;
        bool started = shared_.state == LobbyState::Started;
        auto find = [&](int id) { return std::find_if(players.begin(), players.end(), [id](const LobbyPlayer& p) { return p.id == id; }); };
        for (Peer& peer : peers) {
            peer.stream.receive();
            std::string line;
            while (!peer.stream.broken && peer.stream.next(line)) {
                std::vector<std::string> f = split(line);
                if (peer.id < 0) {
                    long protocol = 0;
                    if (f.size() != 5 || f[0] != "HELLO" || !parse_int(f[1], 0, 1000000, protocol)) {
                        peer.stream.broken = true;
                    } else if (protocol != kLobbyProtocol) {
                        peer.stream.send("REJECT\tprotocol");
                        peer.stream.broken = true;
                    } else if (f[2] != me_.build) {
                        peer.stream.send("REJECT\tbuild");
                        peer.stream.broken = true;
                    } else if (f[3] != me_.content) {
                        peer.stream.send("REJECT\tcontent");
                        peer.stream.broken = true;
                    } else if (started) {
                        peer.stream.send("REJECT\tstarted");
                        peer.stream.broken = true;
                    } else if (players.size() >= kLobbySeats) {
                        peer.stream.send("REJECT\tfull");
                        peer.stream.broken = true;
                    } else {
                        peer.id = next_id++;
                        players.push_back({peer.id, sanitize_name(f[4]), first_free_seat(players), false});
                        peer.stream.send("WELCOME\t" + std::to_string(peer.id));
                        changed = true;
                    }
                    continue;
                }
                auto player = find(peer.id);
                long value = 0;
                if (player == players.end() || started) {
                    continue;
                }
                if (f.size() == 2 && f[0] == "SEAT" && parse_int(f[1], -1, kLobbySeats - 1, value)) {
                    if (!seat_taken(players, static_cast<int>(value), peer.id)) {
                        player->seat = static_cast<int>(value);
                        player->ready = false;
                    }
                    changed = true;
                } else if (f.size() == 2 && f[0] == "READY" && (f[1] == "0" || f[1] == "1")) {
                    player->ready = f[1] == "1" && player->seat >= 0;
                    changed = true;
                } else if (f.size() == 1 && f[0] == "BYE") {
                    peer.stream.broken = true;
                } else {
                    peer.stream.broken = true;
                }
            }
            if (peer.id < 0 && std::chrono::steady_clock::now() - peer.since > kHelloTimeout) {
                peer.stream.broken = true;
            }
        }
        for (Peer& peer : peers) {
            if (peer.stream.broken) {
                auto player = find(peer.id);
                if (player != players.end()) {
                    players.erase(player);
                    changed = true;
                }
                peer.stream.close();
            }
        }
        peers.erase(std::remove_if(peers.begin(), peers.end(), [](const Peer& p) { return p.stream.socket == kNoSocket; }), peers.end());
        auto self = find(0);
        if (!started && wanted_seat_ != -2 && self != players.end()) {
            if (!seat_taken(players, wanted_seat_, 0)) {
                self->seat = wanted_seat_;
                self->ready = false;
            }
            wanted_seat_ = -2;
            changed = true;
        }
        if (!started && wanted_ready_ >= 0 && self != players.end()) {
            self->ready = wanted_ready_ == 1 && self->seat >= 0;
            wanted_ready_ = -1;
            changed = true;
        }
        std::string start_line;
        if (wanted_start_) {
            wanted_start_ = false;
            bool ok = !started && players.size() >= 2 && std::all_of(players.begin(), players.end(), [](const LobbyPlayer& p) { return p.seat >= 0 && p.ready; });
            if (ok) {
                shared_.session = random() | 1u;
                shared_.seed = random();
                shared_.epoch = static_cast<uint64_t>(std::time(nullptr));
                shared_.state = LobbyState::Started;
                start_line = "START\t" + std::to_string(shared_.session) + "\t" + std::to_string(shared_.seed) + "\t" + std::to_string(shared_.epoch);
            }
        }
        std::string roster = changed ? roster_line(players) : std::string();
        lock.unlock();
        if (changed || !start_line.empty()) {
            wake_main_loop();
        }
        for (Peer& peer : peers) {
            if (peer.id >= 0) {
                if (!roster.empty()) {
                    peer.stream.send(roster);
                }
                if (!start_line.empty()) {
                    peer.stream.send(start_line);
                }
            }
            peer.stream.flush();
        }
    }
    for (Peer& peer : peers) {
        peer.stream.close();
    }
    close_socket(listener);
}

void Lobby::run_client(uintptr_t raw) {
    Stream stream;
    stream.socket = static_cast<socket_t>(raw);
    int on = 1;
    setsockopt(stream.socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));
    stream.send("HELLO\t" + std::to_string(kLobbyProtocol) + "\t" + me_.build + "\t" + me_.content + "\t" + sanitize_name(me_.name));
    bool welcomed = false;
    auto since = std::chrono::steady_clock::now();
    while (!stop_) {
        stream.flush();
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(stream.socket, &readable);
        timeval wait{0, 50000};
        select(static_cast<int>(stream.socket) + 1, &readable, nullptr, nullptr, &wait);
        stream.receive();
        std::string line;
        bool heard = false;
        while (stream.next(line)) {
            heard = true;
            std::vector<std::string> f = split(line);
            std::lock_guard<std::mutex> lock(mutex_);
            long value = 0;
            if (f.size() == 2 && f[0] == "WELCOME" && parse_int(f[1], 1, 1000000, value)) {
                welcomed = true;
                shared_.state = LobbyState::Joined;
                shared_.my_id = static_cast<int>(value);
            } else if (f.size() == 2 && f[0] == "REJECT") {
                shared_.state = LobbyState::Failed;
                shared_.message = "reject_" + f[1];
                stream.close();
                wake_main_loop();
                return;
            } else if (welcomed && f[0] == "ROSTER") {
                if (!parse_roster(f, shared_.players)) {
                    stream.broken = true;
                }
            } else if (welcomed && f.size() == 4 && f[0] == "START") {
                shared_.session = static_cast<uint32_t>(std::strtoul(f[1].c_str(), nullptr, 10));
                shared_.seed = static_cast<uint32_t>(std::strtoul(f[2].c_str(), nullptr, 10));
                shared_.epoch = std::strtoull(f[3].c_str(), nullptr, 10);
                shared_.state = LobbyState::Started;
            } else {
                stream.broken = true;
            }
        }
        if (heard) {
            wake_main_loop();
        }
        if (!welcomed && std::chrono::steady_clock::now() - since > kHelloTimeout) {
            stream.close();
            fail("timeout");
            return;
        }
        if (stream.broken) {
            stream.close();
            std::lock_guard<std::mutex> lock(mutex_);
            if (shared_.state != LobbyState::Started) {
                shared_.state = LobbyState::Failed;
                shared_.message = "closed";
            }
            wake_main_loop();
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (wanted_seat_ != -2) {
            stream.send("SEAT\t" + std::to_string(wanted_seat_));
            wanted_seat_ = -2;
        }
        if (wanted_ready_ >= 0) {
            stream.send(std::string("READY\t") + (wanted_ready_ ? "1" : "0"));
            wanted_ready_ = -1;
        }
    }
    stream.send("BYE");
    stream.close();
}
