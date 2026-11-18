#include "online.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <sstream>

#include "texts.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
constexpr socket_t kNoSocket = INVALID_SOCKET;
int close_socket(socket_t s) {
    return closesocket(s);
}
bool would_block() {
    int error = WSAGetLastError();
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS || error == WSAEALREADY;
}
void set_nonblocking(socket_t s) {
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
}
void start_sockets() {
    static bool started = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    (void)started;
}
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t kNoSocket = -1;
int close_socket(socket_t s) {
    return close(s);
}
bool would_block() {
    return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS;
}
void set_nonblocking(socket_t s) {
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
}
void start_sockets() {}
#endif

namespace online {

namespace {

bool spanish() {
    return language_is("es");
}

const char* say(const char* english, const char* spanish_text) {
    return spanish() ? spanish_text : english;
}

std::string clean_name(std::string name) {
    for (char& c : name) {
        if (c == ';' || c == ',' || c == '\t' || c == '\n' || c == '\r') {
            c = ' ';
        }
    }
    if (name.size() > 24) {
        name.resize(24);
    }
    if (name.empty()) {
        name = "?";
    }
    return name;
}

socket_t as_socket(intptr_t value) {
    return static_cast<socket_t>(value);
}

void no_delay(socket_t s) {
    int on = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof on);
}

}

Room::~Room() {
    leave();
}

bool Room::host(uint16_t port, const std::string& name, const std::string& version, std::string& error) {
    leave();
    start_sockets();
    socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kNoSocket) {
        error = say("Could not open a network socket.", "No se pudo abrir un socket de red.");
        return false;
    }
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on), sizeof on);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (bind(s, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0 || listen(s, 8) != 0) {
        close_socket(s);
        char text[160];
        std::snprintf(text, sizeof text, say("Port %u is busy: close other rooms or choose another port.", "El puerto %u está ocupado: cierra otras salas o elige otro puerto."),
                      port);
        error = text;
        return false;
    }
    set_nonblocking(s);
    listener_ = static_cast<intptr_t>(s);
    mode_ = Mode::Hosting;
    port_ = port;
    version_ = version;
    name_ = clean_name(name);
    self_ = 0;
    next_id_ = 1;
    started_ = false;
    members_.clear();
    members_.push_back({0, 0, true, name_});
    status_ = say("Room open. Your friends join with one of the addresses below.", "Sala abierta. Tus amigos se unen con una de las direcciones de abajo.");
    return true;
}

bool Room::join(const std::string& address, uint16_t port, const std::string& name, const std::string& version, std::string& error) {
    leave();
    start_sockets();
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    std::string port_text = std::to_string(port);
    if (getaddrinfo(address.c_str(), port_text.c_str(), &hints, &found) != 0 || !found) {
        error = say("That address is not valid.", "Esa dirección no es válida.");
        return false;
    }
    socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kNoSocket) {
        freeaddrinfo(found);
        error = say("Could not open a network socket.", "No se pudo abrir un socket de red.");
        return false;
    }
    set_nonblocking(s);
    no_delay(s);
    int result = connect(s, found->ai_addr, static_cast<int>(found->ai_addrlen));
    freeaddrinfo(found);
    if (result != 0 && !would_block()) {
        close_socket(s);
        error = say("Could not reach that address.", "No se pudo conectar con esa dirección.");
        return false;
    }
    Peer peer;
    peer.socket = static_cast<intptr_t>(s);
    peers_.push_back(peer);
    mode_ = Mode::Joining;
    address_ = address;
    port_ = port;
    version_ = version;
    name_ = clean_name(name);
    started_ = false;
    members_.clear();
    send_line(peers_[0], "HELLO " + version_ + "\t" + name_);
    status_ = say("Connecting...", "Conectando...");
    return true;
}

void Room::leave() {
    for (Peer& peer : peers_) {
        if (peer.socket != -1) {
            close_socket(as_socket(peer.socket));
        }
    }
    peers_.clear();
    if (listener_ != -1) {
        close_socket(as_socket(listener_));
        listener_ = -1;
    }
    members_.clear();
    mode_ = Mode::Idle;
    status_.clear();
}

void Room::send_line(Peer& peer, const std::string& line) {
    peer.out += line;
    peer.out += '\n';
    pump(peer);
}

void Room::broadcast(const std::string& line) {
    for (Peer& peer : peers_) {
        if (peer.greeted) {
            send_line(peer, line);
        }
    }
}

void Room::broadcast_state() {
    std::string line = "STATE ";
    for (size_t i = 0; i < members_.size(); i++) {
        const Member& m = members_[i];
        line += (i ? ";" : "") + std::to_string(m.id) + "," + std::to_string(m.seat) + "," + (m.ready ? "1" : "0") + "," + m.name;
    }
    broadcast(line);
}

Member* Room::member(int id) {
    for (Member& m : members_) {
        if (m.id == id) {
            return &m;
        }
    }
    return nullptr;
}

// Sends what is queued and reads what arrived; false when the connection is gone.
bool Room::pump(Peer& peer) {
    socket_t s = as_socket(peer.socket);
    while (!peer.out.empty()) {
        int sent = send(s, peer.out.data(), static_cast<int>(peer.out.size()), 0);
        if (sent > 0) {
            peer.out.erase(0, static_cast<size_t>(sent));
        } else if (sent < 0 && would_block()) {
            break;
        } else {
            return false;
        }
    }
    char buffer[2048];
    while (true) {
        int got = recv(s, buffer, sizeof buffer, 0);
        if (got > 0) {
            peer.in.append(buffer, static_cast<size_t>(got));
            if (peer.in.size() > 65536) {
                return false;
            }
        } else if (got < 0 && would_block()) {
            return true;
        } else {
            return false;
        }
    }
}

void Room::drop(size_t index, const char* why) {
    Peer& peer = peers_[index];
    close_socket(as_socket(peer.socket));
    int id = peer.id;
    bool greeted = peer.greeted;
    peers_.erase(peers_.begin() + static_cast<std::ptrdiff_t>(index));
    if (mode_ == Mode::Hosting) {
        Member* gone = greeted ? member(id) : nullptr;
        if (gone) {
            status_ = std::string(gone->name) + say(" left the room.", " ha salido de la sala.");
            members_.erase(std::remove_if(members_.begin(), members_.end(), [id](const Member& m) { return m.id == id; }), members_.end());
            broadcast_state();
        }
    } else {
        mode_ = Mode::Idle;
        members_.clear();
        status_ = why;
    }
}

void Room::handle_host_line(Peer& peer, const std::string& line) {
    if (!peer.greeted) {
        if (line.rfind("HELLO ", 0) != 0) {
            return;
        }
        size_t tab = line.find('\t');
        std::string version = line.substr(6, tab == std::string::npos ? std::string::npos : tab - 6);
        std::string name = clean_name(tab == std::string::npos ? "" : line.substr(tab + 1));
        if (version != version_) {
            send_line(peer, std::string("REJECT ") + say("Your launcher or disc is a different version from the host's. Use the same launcher zip.",
                                                         "Tu launcher o tu disco son de otra versión que los del anfitrión. Usad el mismo zip del launcher."));
            return;
        }
        if (started_) {
            send_line(peer, std::string("REJECT ") + say("The game has already started.", "La partida ya ha empezado."));
            return;
        }
        if (members_.size() >= kSeats) {
            send_line(peer, std::string("REJECT ") + say("The room is full.", "La sala está llena."));
            return;
        }
        peer.greeted = true;
        peer.id = next_id_++;
        int seat = -1;
        for (int s = 0; s < kSeats && seat < 0; s++) {
            if (std::none_of(members_.begin(), members_.end(), [s](const Member& m) { return m.seat == s; })) {
                seat = s;
            }
        }
        members_.push_back({peer.id, seat, false, name});
        send_line(peer, "WELCOME " + std::to_string(peer.id));
        status_ = name + say(" joined.", " se ha unido.");
        broadcast_state();
        return;
    }
    Member* m = member(peer.id);
    if (!m) {
        return;
    }
    if (line.rfind("SEAT ", 0) == 0) {
        int seat = std::atoi(line.c_str() + 5);
        bool taken = std::any_of(members_.begin(), members_.end(), [&](const Member& other) { return other.id != m->id && other.seat == seat; });
        if (seat >= -1 && seat < kSeats && (!taken || seat < 0)) {
            m->seat = seat;
            m->ready = false;
        }
        broadcast_state();
    } else if (line.rfind("READY ", 0) == 0) {
        m->ready = line[6] == '1' && m->seat >= 0;
        broadcast_state();
    }
}

bool Room::handle_client_line(const std::string& line, StartInfo& start) {
    if (line.rfind("WELCOME ", 0) == 0) {
        self_ = std::atoi(line.c_str() + 8);
        mode_ = Mode::Joined;
        status_ = say("In the room. Choose your player and press Ready.", "Dentro de la sala. Elige tu jugador y pulsa Listo.");
    } else if (line.rfind("REJECT ", 0) == 0) {
        std::string why = line.substr(7);
        leave();
        status_ = why;
    } else if (line.rfind("STATE ", 0) == 0) {
        members_.clear();
        std::stringstream items(line.substr(6));
        std::string item;
        while (std::getline(items, item, ';')) {
            std::stringstream fields(item);
            std::string id;
            std::string seat;
            std::string ready;
            std::string name;
            std::getline(fields, id, ',');
            std::getline(fields, seat, ',');
            std::getline(fields, ready, ',');
            std::getline(fields, name);
            members_.push_back({std::atoi(id.c_str()), std::atoi(seat.c_str()), ready == "1", name});
        }
    } else if (line.rfind("START ", 0) == 0) {
        unsigned long session = 0;
        unsigned long seats = 0;
        unsigned long long epoch = 0;
        int delay = 4;
        int min_delay = 3;
        if (std::sscanf(line.c_str() + 6, "%lu %lu %llu %d %d", &session, &seats, &epoch, &delay, &min_delay) >= 4) {
            start.min_delay = min_delay;
            Member* me = member(self_);
            start.session = static_cast<uint32_t>(session);
            start.seats = static_cast<uint32_t>(seats);
            start.epoch = epoch;
            start.delay = delay;
            start.seat = me ? me->seat : -1;
            start.host = address_;
            start.port = port_;
            started_ = true;
            return start.seat >= 0;
        }
    }
    return false;
}

bool Room::poll(StartInfo& start) {
    if (mode_ == Mode::Idle) {
        return false;
    }
    if (mode_ == Mode::Hosting) {
        while (true) {
            sockaddr_in from{};
            socklen_t length = sizeof from;
            socket_t s = accept(as_socket(listener_), reinterpret_cast<sockaddr*>(&from), &length);
            if (s == kNoSocket) {
                break;
            }
            set_nonblocking(s);
            no_delay(s);
            Peer peer;
            peer.socket = static_cast<intptr_t>(s);
            peers_.push_back(peer);
        }
    }
    for (size_t i = 0; i < peers_.size();) {
        if (!pump(peers_[i])) {
            drop(i, say("The connection to the room was lost.", "Se ha perdido la conexión con la sala."));
            continue;
        }
        bool removed = false;
        size_t end;
        while (!removed && (end = peers_[i].in.find('\n')) != std::string::npos) {
            std::string line = peers_[i].in.substr(0, end);
            peers_[i].in.erase(0, end + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (mode_ == Mode::Hosting) {
                handle_host_line(peers_[i], line);
                if (!peers_[i].greeted && line.rfind("HELLO ", 0) == 0) {
                    pump(peers_[i]);
                    drop(i, "");
                    removed = true;
                }
            } else if (handle_client_line(line, start)) {
                return true;
            } else if (mode_ == Mode::Idle) {
                return false;
            }
        }
        if (!removed) {
            i++;
        }
    }
    return false;
}

void Room::choose_seat(int seat) {
    if (mode_ == Mode::Hosting) {
        bool taken = std::any_of(members_.begin(), members_.end(), [&](const Member& m) { return m.id != 0 && m.seat == seat; });
        if (!taken && seat >= 0 && seat < kSeats) {
            members_[0].seat = seat;
            broadcast_state();
        }
    } else if (mode_ == Mode::Joined && !peers_.empty()) {
        send_line(peers_[0], "SEAT " + std::to_string(seat));
    }
}

void Room::set_ready(bool ready) {
    if (mode_ == Mode::Joined && !peers_.empty()) {
        send_line(peers_[0], std::string("READY ") + (ready ? "1" : "0"));
    }
}

bool Room::can_start() const {
    if (mode_ != Mode::Hosting) {
        return false;
    }
    uint32_t used = 0;
    for (const Member& m : members_) {
        if (m.seat < 0 || (m.id != 0 && !m.ready) || (used >> m.seat & 1)) {
            return false;
        }
        used |= 1u << m.seat;
    }
    return true;
}

void Room::start(int min_delay) {
    if (!can_start()) {
        return;
    }
    std::random_device random;
    uint32_t session = (random() & 0x7FFFFFFF) | 1;
    uint32_t seats = 0;
    for (const Member& m : members_) {
        seats |= 1u << m.seat;
    }
    uint64_t epoch = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    char line[128];
    std::snprintf(line, sizeof line, "START %u %u %llu %d %d", session, seats, static_cast<unsigned long long>(epoch), 4, min_delay);
    broadcast(line);
    for (Peer& peer : peers_) {
        for (int tries = 0; tries < 50 && !peer.out.empty(); tries++) {
            pump(peer);
        }
    }
    started_ = true;
    pending_start_.session = session;
    pending_start_.seats = seats;
    pending_start_.epoch = epoch;
    pending_start_.delay = 4;
    pending_start_.min_delay = min_delay;
    pending_start_.seat = members_[0].seat;
    pending_start_.host.clear();
    pending_start_.port = port_;
    host_start_ready_ = true;
}

bool Room::take_host_start(StartInfo& start) {
    if (!host_start_ready_) {
        return false;
    }
    host_start_ready_ = false;
    start = pending_start_;
    return true;
}

std::vector<std::string> local_addresses() {
    start_sockets();
    std::vector<std::string> result;
#ifdef _WIN32
    char name[256] = {};
    if (gethostname(name, sizeof name) == 0) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        addrinfo* found = nullptr;
        if (getaddrinfo(name, nullptr, &hints, &found) == 0) {
            for (addrinfo* a = found; a; a = a->ai_next) {
                char text[INET_ADDRSTRLEN] = {};
                inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(a->ai_addr)->sin_addr, text, sizeof text);
                result.push_back(text);
            }
            freeaddrinfo(found);
        }
    }
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) == 0) {
        for (ifaddrs* a = list; a; a = a->ifa_next) {
            if (a->ifa_addr && a->ifa_addr->sa_family == AF_INET) {
                char text[INET_ADDRSTRLEN] = {};
                inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(a->ifa_addr)->sin_addr, text, sizeof text);
                result.push_back(text);
            }
        }
        freeifaddrs(list);
    }
#endif
    result.erase(std::remove_if(result.begin(), result.end(), [](const std::string& a) { return a.rfind("127.", 0) == 0 || a.rfind("169.254.", 0) == 0; }),
                 result.end());
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    std::stable_partition(result.begin(), result.end(), [](const std::string& a) { return a.rfind("25.", 0) == 0; });
    return result;
}

}
