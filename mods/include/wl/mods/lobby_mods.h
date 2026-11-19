#pragma once
// Canal aparte del launcher para publicar el manifiesto de mods de una sala (docs/mods/05-protocolo_salas.md).
// No toca el netcode de las salas: es otro puerto TCP (el de la sala + kManifestPortOffset) con un único
// mensaje. El anfitrión solo RESPONDE con su manifiesto; nunca recibe ni ejecuta nada del cliente.
//   cliente → «MODS 1\n»
//   anfitrión → JSON del manifiesto + «\n» y cierra
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace wl::mods {

constexpr uint16_t kManifestPortOffset = 10;   // MP9 juega en sala + 1: se deja hueco

class ManifestServer {
public:
    ~ManifestServer();
    // `manifest` se llama en cada petición (así refleja la revisión actual del perfil).
    bool start(uint16_t port, std::function<std::string()> manifest, std::string& error);
    void stop();
    bool running() const { return running_; }

private:
    void loop();
    std::function<std::string()> manifest_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    uintptr_t socket_ = ~uintptr_t(0);
};

// Pide el manifiesto. Límite de 64 KiB y `timeout_ms` en total. false + error en español.
bool fetch_manifest(const std::string& host, uint16_t port, std::string& out, std::string& error, int timeout_ms = 3000);

}  // namespace wl::mods
