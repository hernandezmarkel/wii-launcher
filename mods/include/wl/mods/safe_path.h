#pragma once
// Rutas relativas seguras para contenido no confiable (ZIP, XML de Riivolution, manifiestos del anfitrión).
// Defensas de OWASP «Path Traversal» + particularidades de Windows: unidades, ADS (':'), nombres
// reservados (CON, NUL, COM1...), puntos/espacios finales, separadores mezclados y mayúsculas.
#include <filesystem>
#include <string>

namespace wl::mods {

namespace fs = std::filesystem;

constexpr size_t kMaxPathBytes = 400;
constexpr size_t kMaxSegmentBytes = 120;
constexpr int kMaxPathDepth = 24;

// Convierte una ruta relativa a la forma canónica "a/b/c" (separador '/', sin '.', sin segmentos vacíos).
// Rechaza: vacía, absoluta, unidad, '..', ':', caracteres de control o prohibidos en Windows, nombres
// reservados, segmentos que acaban en '.' o ' ', no-ASCII, demasiado larga o profunda.
bool normalize_relative(const std::string& raw, std::string& out, std::string& why);

// Clave para detectar colisiones en NTFS (insensible a mayúsculas). Solo ASCII tras normalize_relative.
std::string fold_key(const std::string& normalized);

// true si `child` (ya existente o no) queda dentro de `root` tras resolver '..' y enlaces existentes.
bool is_within(const fs::path& root, const fs::path& child);

// Une root + ruta normalizada y comprueba is_within. Devuelve ruta vacía si no.
fs::path join_inside(const fs::path& root, const std::string& normalized);

// Ruta en UTF-8 para mensajes y JSON.
std::string to_utf8(const fs::path& p);
fs::path from_utf8(const std::string& s);

}  // namespace wl::mods
