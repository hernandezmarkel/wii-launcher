#pragma once
// Tabla FST de un disco de Wii extraído (sys/fst.bin) y su regeneración para el disco virtual.
// Formato: entradas de 12 bytes big-endian [tipo|nombre:24][desplazamiento>>2 o padre][tamaño o fin],
// seguidas de la tabla de nombres. El motor wii_recomp (engine/src/ios/disc.cpp) mapea cada lectura de
// disco a `files/<ruta>` con estos desplazamientos y tamaños, y el juego ve la misma tabla en MEM1.
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace wl::mods {

struct FstFile {
    std::string path;     // relativa a files/, con '/', tal cual en el disco
    uint32_t index = 0;   // entrada en la FST
    uint64_t offset = 0;  // en bytes
    uint32_t size = 0;
};

struct Fst {
    std::vector<uint8_t> raw;
    std::vector<FstFile> files;
    std::map<std::string, size_t> by_key;  // fold_key(path) -> índice en files
    uint64_t data_end = 0;                 // fin del último archivo (bytes)

    const FstFile* find(const std::string& path) const;
};

bool parse_fst(const std::vector<uint8_t>& raw, Fst& out, std::string& error);

// Devuelve una FST nueva con los tamaños cambiados (índice de entrada -> tamaño nuevo). Un archivo que
// cabe en su hueco conserva su desplazamiento; si no, se recoloca al final del disco (alineado a 32 KiB)
// a partir de `min_end` (fin de todo lo que ya ocupa el disco, incluida la propia FST).
std::vector<uint8_t> rebuild_fst(const Fst& fst, const std::map<uint32_t, uint32_t>& new_sizes, uint64_t min_end);

}  // namespace wl::mods
