#pragma once
// Análisis de un paquete de mod (ZIP o carpeta) contra el disco REAL del usuario, antes de instalar nada.
// Formatos admitidos (ver docs/mods/04-integracion_juegos.md):
//   - «files»: archivos con la estructura del disco (files/..., DATA/files/..., o una subcarpeta reconocible).
//   - «riivolution»: XML de Riivolution con parches <file>/<folder> solamente.
// Todo lo demás (Gecko, <memory>, REL/DOL, ejecutables, imágenes de disco, texturas de Dolphin) se
// clasifica y se rechaza con un motivo preciso; nunca se «instala» algo que el juego no vaya a cargar.
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "wl/mods/fst.h"
#include "wl/mods/json.h"

namespace wl::mods {

namespace fs = std::filesystem;

// Lo que admite un juego concreto. Solo se declara `proven` tras la prueba A/B en el port real.
struct GameModSupport {
    std::string game_id;
    std::string loader = "wl-vdisc-1";   // disco virtual: argv[1] del motor wii_recomp
    bool proven = false;                 // A/B superada (docs/mods/06-pruebas_resultados.md)
    std::vector<std::string> blocked_prefixes;   // rutas del disco que son código (REL)
    std::vector<std::string> blocked_suffixes;   // .rel, .rel.lz, .dol...
    uint64_t max_file_bytes = 256ull << 20;
};

// Soporte conocido por juego (nullptr si el juego no admite mods en este launcher).
const GameModSupport* game_support(const std::string& game_id);

// Entrada de un paquete (archivo dentro del ZIP o de la carpeta).
struct SourceEntry {
    std::string name;   // ruta normalizada
    uint64_t size = 0;
};

// Origen abstracto: ZIP o carpeta.
class PackageSource {
public:
    virtual ~PackageSource() = default;
    virtual const std::vector<SourceEntry>& entries() const = 0;
    virtual bool read_small(const std::string& name, std::string& out, size_t max_bytes, std::string& error) const = 0;
    virtual bool extract(const std::string& name, const fs::path& dest, std::string& error,
                         const std::function<bool()>& cancel) const = 0;
};
std::unique_ptr<PackageSource> open_source(const fs::path& path, std::string& error);

enum class ModFormat { Unknown, Files, Riivolution };

struct DiscOp {
    std::string disc;   // ruta en files/ del disco del usuario (como en su FST)
    std::string src;    // ruta dentro del paquete
};

struct RiivoChoice {
    std::string name;
    std::vector<std::string> patches;
};
struct RiivoOption {
    std::string id;     // "sección/opción"
    std::string name;
    int default_choice = 0;   // 1-based; 0 = desactivada
    std::vector<RiivoChoice> choices;
};

struct PackagePlan {
    ModFormat format = ModFormat::Unknown;
    std::string region_hint;                       // de <id game> / carpeta SSQE01... si existe
    std::map<std::string, std::vector<DiscOp>> patches;   // id -> operaciones ya resueltas contra la FST
    std::vector<std::string> always;               // parches que se aplican siempre (formato files)
    std::vector<RiivoOption> options;
    std::vector<std::string> ignored;              // "ruta: motivo" (no existe en tu disco, README...)
    std::vector<std::string> blocking;             // motivos que impiden instalar
    std::vector<std::string> warnings;
    uint64_t payload_bytes = 0;
    bool ok() const { return blocking.empty() && !patches.empty(); }
    std::vector<std::string> referenced_sources() const;
};

// Analiza el paquete. `fst` y `disc_id` (p. ej. SUPP01) son los del disco extraído del usuario.
PackagePlan analyze_package(const PackageSource& source, const Fst& fst, const GameModSupport& support,
                            const std::string& disc_id);

// Operaciones efectivas de un mod con una selección de opciones (id de opción -> elección 1-based, 0 = no).
std::vector<DiscOp> effective_ops(const Json& mod_json, const std::map<std::string, int>& choices);

std::string format_name(ModFormat f);

}  // namespace wl::mods
