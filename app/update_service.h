#pragma once
// Estado de la autoactualización para la UI. El trabajo de red va en hilos de fondo; la UI solo lee snapshot().
// Las preferencias solo se tocan desde el hilo de la UI (pump()), porque Prefs no es thread-safe.
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "prefs.h"

namespace wl {

enum class UpdState { Idle, Checking, UpToDate, Available, Downloading, Verifying, Ready, Installing, Error };

struct UpdateSnapshot {
    UpdState state = UpdState::Idle;
    std::string installed, channel, available, published, error, info, last_check;
    std::vector<std::string> notes;
    uint64_t download_size = 0;  // lo que hay que bajar (solo archivos cambiados), si se sabe
    uint64_t update_size = 0;    // tamaño total de la versión
    uint64_t done = 0, total = 0;
    bool auto_check = true;
    bool enabled = true;         // false si esta copia no tiene repo o clave
    bool quiet_error = false;    // error de una comprobación automática: no se muestra fuera de Ajustes
    std::string installers_note; // instaladores de juegos actualizados en esta sesión (qué hacer a continuación)
};

class UpdateService {
public:
    UpdateService(Prefs& prefs, std::filesystem::path install_dir, std::function<void()> wake);
    ~UpdateService();

    void pump();                 // cada fotograma: guarda en prefs lo que hayan dejado los hilos
    void start_auto();           // al abrir: como mucho una vez cada 6 h y respetando límites de GitHub
    void check_now();            // «Buscar actualizaciones»
    void download();             // «Actualizar»
    void cancel();
    // «Reiniciar y actualizar». true = el aplicador arrancó y el launcher debe cerrarse ya.
    bool apply(bool game_running, std::string& why_not);
    void set_channel(bool beta);
    void set_auto(bool on);
    UpdateSnapshot snapshot() const;

private:
    struct Shared;
    std::shared_ptr<Shared> s_;
    Prefs& prefs_;
    std::filesystem::path install_dir_;
    long long last_manual_ = 0;
    void run_check(bool manual);
};

}
