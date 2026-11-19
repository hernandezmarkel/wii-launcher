#include "wl/mods/lobby_mods.h"

#include <algorithm>
#include <chrono>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include "wl/adapter.h"
#include "wl/mods/session.h"

namespace wl::mods {

#ifdef _WIN32
namespace {

struct WsaInit {
    WsaInit() {
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
    }
};

void wsa() { static WsaInit init; }

bool wait_socket(SOCKET s, bool write, int ms) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    return select(0, write ? nullptr : &set, write ? &set : nullptr, nullptr, &tv) > 0;
}

}  // namespace

ManifestServer::~ManifestServer() { stop(); }

bool ManifestServer::start(uint16_t port, std::function<std::string()> manifest, std::string& error) {
    stop();
    wsa();
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { error = "no se puede abrir el puerto de mods"; return false; }
    BOOL excl = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof excl);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(s, 8) != 0) {
        closesocket(s);
        error = "el puerto " + std::to_string(port) + " (mods de la sala) está ocupado";
        return false;
    }
    socket_ = static_cast<uintptr_t>(s);
    manifest_ = std::move(manifest);
    running_ = true;
    thread_ = std::thread([this] { loop(); });
    return true;
}

void ManifestServer::stop() {
    if (!running_) return;
    running_ = false;
    closesocket(static_cast<SOCKET>(socket_));
    if (thread_.joinable()) thread_.join();
    socket_ = ~uintptr_t(0);
}

void ManifestServer::loop() {
    SOCKET ls = static_cast<SOCKET>(socket_);
    while (running_) {
        if (!wait_socket(ls, false, 200)) continue;
        SOCKET c = accept(ls, nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;
        // Una petición corta con plazo; cualquier otra cosa se cierra sin responder.
        std::string req;
        auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
        char buf[64];
        while (req.size() < 32 && req.find('\n') == std::string::npos && std::chrono::steady_clock::now() < until) {
            if (!wait_socket(c, false, 100)) continue;
            int n = recv(c, buf, sizeof buf, 0);
            if (n <= 0) break;
            req.append(buf, static_cast<size_t>(n));
        }
        if (req.rfind("MODS 1\n", 0) == 0 || req.rfind("MODS 1\r\n", 0) == 0) {
            std::string body = manifest_ ? manifest_() : std::string();
            if (body.size() <= kMaxSessionManifest) {
                body += "\n";
                size_t sent = 0;
                while (sent < body.size() && wait_socket(c, true, 1000)) {
                    int n = send(c, body.data() + sent, static_cast<int>(body.size() - sent), 0);
                    if (n <= 0) break;
                    sent += static_cast<size_t>(n);
                }
            }
        }
        shutdown(c, SD_BOTH);
        closesocket(c);
    }
}

bool fetch_manifest(const std::string& host, uint16_t port, std::string& out, std::string& error, int timeout_ms) {
    out.clear();
    wsa();
    // Solo IPv4 literal (como la sala): no se resuelven nombres que pudiera colar nadie.
    if (!valid_ipv4(host)) { error = "dirección no válida"; return false; }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { error = "sin red"; return false; }
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &a.sin_addr);
    auto start = std::chrono::steady_clock::now();
    auto left = [&] { return timeout_ms - static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count()); };
    connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a);
    if (!wait_socket(s, true, std::max(1, left()))) {
        closesocket(s);
        error = "el anfitrión no publica sus mods (launcher sin gestor de mods, o puerto " + std::to_string(port) + " cerrado)";
        return false;
    }
    int soerr = 0, len = sizeof soerr;
    getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soerr), &len);
    if (soerr != 0) {
        closesocket(s);
        error = "el anfitrión no publica sus mods (launcher sin gestor de mods, o puerto " + std::to_string(port) + " cerrado)";
        return false;
    }
    const char req[] = "MODS 1\n";
    send(s, req, sizeof req - 1, 0);
    char buf[4096];
    while (left() > 0) {
        if (!wait_socket(s, false, std::max(1, left()))) break;
        int n = recv(s, buf, sizeof buf, 0);
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
        if (out.size() > kMaxSessionManifest + 2) { closesocket(s); error = "manifiesto demasiado grande"; return false; }
    }
    closesocket(s);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    if (out.empty()) { error = "el anfitrión no respondió a tiempo"; return false; }
    return true;
}

#else
ManifestServer::~ManifestServer() {}
bool ManifestServer::start(uint16_t, std::function<std::string()>, std::string& error) { error = "solo Windows"; return false; }
void ManifestServer::stop() {}
void ManifestServer::loop() {}
bool fetch_manifest(const std::string&, uint16_t, std::string&, std::string& error, int) { error = "solo Windows"; return false; }
#endif

}  // namespace wl::mods
