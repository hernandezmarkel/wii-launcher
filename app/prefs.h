#pragma once
// Preferencias del launcher unificado: %LOCALAPPDATA%\WiiLauncher\prefs.ini (clave=valor).
// Separadas de las de cada juego: el launcher nunca escribe en las carpetas de los juegos.
#include <map>
#include <string>

namespace wl {

class Prefs {
public:
    void load();
    void save() const;
    std::string get(const std::string& key, const std::string& fallback = "") const;
    void set(const std::string& key, const std::string& value);

private:
    std::map<std::string, std::string> values_;
};

}
