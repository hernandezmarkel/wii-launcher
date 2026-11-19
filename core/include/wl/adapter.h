#pragma once
// Contrato launcher <-> adaptador, versión 1. El launcher solo conoce esta interfaz.
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "wl/manifest.h"

namespace wl {

namespace fs = std::filesystem;

constexpr int kAdapterContractVersion = 1;

struct InstallStatus {
    bool folder = false;     // existe la carpeta de instalación
    bool built = false;      // existe el ejecutable del juego
    bool disc_ok = false;    // el disco extraído es uno de los admitidos
    std::string revision;    // revisión de la build instalada (.launcher-revision)
    std::string disc_id;     // ID del disco extraído
    fs::path root;           // carpeta de instalación comprobada
    std::string problem;     // primer problema encontrado, en español; vacío si todo está bien
    bool ready() const { return folder && built && disc_ok; }
};

struct LaunchSpec {
    fs::path executable;
    fs::path working_dir;
    std::vector<std::string> args;
    std::map<std::string, std::string> env;  // se añaden al entorno heredado
};

// Lo que la sala acordó y el juego necesita para conectarse. Cada adaptador usa lo suyo.
struct SessionStart {
    uint32_t session = 0;
    uint32_t seats = 0;    // MP9: máscara de asientos ocupados (1 << asiento), igual que Room::start()
    int seat = -1;
    uint64_t epoch = 0;
    std::string host;
    uint16_t port = 0;
    int delay = 0;         // MP9
    int min_delay = 0;     // MP9
    uint32_t seed = 0;     // Wii Party
    uint32_t seat_mask = 0;  // Wii Party: WP_NET_SEATS es una máscara (1 << asiento) de asientos ocupados
};

struct RoomMember {
    int id = 0;
    int seat = -1;
    bool ready = false;
    std::string name;
};

enum class RoomMode { Idle, Hosting, Joining, Joined };

// Sala de un juego. El protocolo es del adaptador (copiado del launcher original); la UI es común.
// poll() no bloquea: se llama en cada fotograma mientras la sala está abierta.
class RoomSession {
public:
    virtual ~RoomSession() = default;
    virtual bool host(uint16_t port, const std::string& name, const std::string& token, std::string& error) = 0;
    virtual bool join(const std::string& address, uint16_t port, const std::string& name, const std::string& token, std::string& error) = 0;
    virtual void leave() = 0;
    virtual bool poll(SessionStart& start) = 0;  // true: hay que lanzar el juego con los datos de start
    virtual void choose_seat(int seat) = 0;
    virtual void set_ready(bool ready) = 0;
    virtual bool can_start() const = 0;
    virtual void start(int option) = 0;  // option: índice de start_options()
    virtual std::vector<std::string> start_options() const { return {}; }
    virtual RoomMode mode() const = 0;
    virtual int self() const = 0;
    virtual std::vector<RoomMember> members() const = 0;
    virtual std::string status() const = 0;
    virtual int seats() const = 0;
    virtual uint16_t default_port() const = 0;
};

std::vector<std::string> local_addresses();

class GameAdapter {
public:
    virtual ~GameAdapter() = default;
    virtual int contract_version() const { return kAdapterContractVersion; }
    // Solo lectura. Puede tardar (disco): llamar fuera del hilo de la UI.
    virtual InstallStatus probe(const fs::path& install_root) const = 0;
    // Cadena que la sala compara entre jugadores. Debe ser idéntica a la del launcher original.
    virtual std::string compat_token(const InstallStatus& status) const = 0;
    virtual LaunchSpec offline(const fs::path& install_root) const = 0;
    // nullopt si el adaptador todavía no sabe lanzar este juego online.
    virtual std::optional<LaunchSpec> online(const fs::path& install_root, const SessionStart& start) const = 0;
    // nullptr si este launcher todavía no tiene la sala de este juego.
    virtual std::unique_ptr<RoomSession> make_room() const { return nullptr; }
    // Online con servidor propio en la LAN (capacidad lan_wfc): IP del servidor guardada en la configuración del juego.
    virtual std::string lan_server(const fs::path&) const { return {}; }
    virtual bool set_lan_server(const fs::path&, const std::string&, std::string& error) const { error = "No disponible."; return false; }
};

// Comprueba una IPv4 escrita a mano ("192.168.1.20"). Nada más: sin nombres ni puertos.
bool valid_ipv4(const std::string& text);

std::unique_ptr<GameAdapter> make_adapter(const Manifest& manifest);

// Utilidades comunes para adaptadores (install.cpp).
fs::path local_app_data();  // %LOCALAPPDATA% (o WL_LOCALAPPDATA para pruebas)
fs::path roaming_app_data();  // %APPDATA% (o WL_APPDATA para pruebas)
fs::path install_root(const Manifest& manifest);  // root= del launcher individual si lo hay; si no, %LOCALAPPDATA%\<install_folder>
std::string read_first_line(const fs::path& path);
std::string read_disc_id(const fs::path& boot_bin);
// Resuelve una ruta relativa del manifest y comprueba que queda dentro de la raíz.
std::optional<fs::path> resolve_inside(const fs::path& root, const std::string& relative);

}
