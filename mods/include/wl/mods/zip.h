#pragma once
// Lector ZIP defensivo (sin zip64, sin cifrado, solo store/deflate). Se lee el directorio central
// ANTES de extraer nada y se rechaza el archivo entero ante cualquier entrada peligrosa: rutas que
// escapan, enlaces simbólicos, duplicados por mayúsculas, bombas de compresión, entradas solapadas.
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace wl::mods {

namespace fs = std::filesystem;

struct ZipLimits {
    size_t max_entries = 20000;
    uint64_t max_entry_bytes = 256ull << 20;   // 256 MiB por archivo
    uint64_t max_total_bytes = 1024ull << 20;  // 1 GiB descomprimido en total
    uint64_t max_ratio = 200;                  // descomprimido / comprimido para entradas > 1 MiB
};

struct ZipEntry {
    std::string name;        // ruta normalizada ("a/b.bin")
    std::string raw_name;    // tal cual en el ZIP (para mensajes)
    uint64_t size = 0;       // descomprimido
    uint64_t compressed = 0;
    uint32_t crc = 0;
    uint16_t method = 0;
    uint64_t local_offset = 0;
    bool directory = false;
};

class ZipReader {
public:
    // Lee y valida el directorio central. false + error si algo no es aceptable.
    bool open(const fs::path& file, std::string& error, const ZipLimits& limits = {});
    const std::vector<ZipEntry>& entries() const { return entries_; }
    // Extrae una entrada a `dest` (archivo nuevo). Comprueba tamaño real y CRC-32. `cancel` se consulta
    // cada 64 KiB. En error borra el archivo parcial.
    bool extract(const ZipEntry& entry, const fs::path& dest, std::string& error,
                 const std::function<bool()>& cancel = {}) const;

private:
    fs::path file_;
    uint64_t file_size_ = 0;
    std::vector<ZipEntry> entries_;
};

}  // namespace wl::mods
