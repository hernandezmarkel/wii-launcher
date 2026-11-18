#include "wl/mods/zip.h"

#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <set>

#include "wl/mods/safe_path.h"

namespace wl::mods {

namespace {

uint16_t le16(const unsigned char* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t le32(const unsigned char* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

bool read_at(std::ifstream& in, uint64_t offset, unsigned char* out, size_t n) {
    in.clear();
    in.seekg(static_cast<std::streamoff>(offset));
    in.read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(n));
    return static_cast<size_t>(in.gcount()) == n;
}

constexpr uint32_t kEocdSig = 0x06054b50;
constexpr uint32_t kCentralSig = 0x02014b50;
constexpr uint32_t kLocalSig = 0x04034b50;
constexpr uint32_t kUnixTypeMask = 0170000;
constexpr uint32_t kUnixSymlink = 0120000;
constexpr uint32_t kDosReparse = 0x400;

}  // namespace

bool ZipReader::open(const fs::path& file, std::string& error, const ZipLimits& lim) {
    entries_.clear();
    file_ = file;
    std::error_code ec;
    file_size_ = fs::file_size(file, ec);
    if (ec || file_size_ < 22) { error = "no es un ZIP válido (demasiado pequeño)"; return false; }
    std::ifstream in(file, std::ios::binary);
    if (!in) { error = "no se puede abrir el archivo"; return false; }

    // Fin del directorio central: últimos 22 + 65535 bytes como mucho.
    uint64_t tail = std::min<uint64_t>(file_size_, 22 + 65535);
    std::vector<unsigned char> buf(static_cast<size_t>(tail));
    if (!read_at(in, file_size_ - tail, buf.data(), buf.size())) { error = "no se puede leer el ZIP"; return false; }
    long eocd = -1;
    for (long i = static_cast<long>(buf.size()) - 22; i >= 0; --i) {
        if (le32(&buf[static_cast<size_t>(i)]) == kEocdSig) { eocd = i; break; }
    }
    if (eocd < 0) { error = "no es un ZIP válido (sin directorio central)"; return false; }
    const unsigned char* e = &buf[static_cast<size_t>(eocd)];
    uint16_t disk = le16(e + 4), cd_disk = le16(e + 6), here = le16(e + 8), total = le16(e + 10);
    uint32_t cd_size = le32(e + 12), cd_offset = le32(e + 16);
    if (disk != 0 || cd_disk != 0 || here != total) { error = "ZIP partido en varios volúmenes: no admitido"; return false; }
    if (total == 0xFFFF || cd_size == 0xFFFFFFFF || cd_offset == 0xFFFFFFFF) { error = "ZIP64: no admitido"; return false; }
    if (total > lim.max_entries) { error = "demasiadas entradas en el ZIP (" + std::to_string(total) + ")"; return false; }
    if (static_cast<uint64_t>(cd_offset) + cd_size > file_size_) { error = "directorio central fuera del archivo"; return false; }

    std::vector<unsigned char> cd(cd_size);
    if (cd_size && !read_at(in, cd_offset, cd.data(), cd.size())) { error = "no se puede leer el directorio central"; return false; }
    size_t p = 0;
    uint64_t sum = 0;
    std::set<std::string> seen;
    for (uint16_t i = 0; i < total; ++i) {
        if (p + 46 > cd.size() || le32(&cd[p]) != kCentralSig) { error = "directorio central corrupto"; return false; }
        const unsigned char* c = &cd[p];
        uint16_t made_by = le16(c + 4), flags = le16(c + 8), method = le16(c + 10);
        uint32_t crc = le32(c + 16), csize = le32(c + 20), usize = le32(c + 24);
        uint16_t nlen = le16(c + 28), xlen = le16(c + 30), klen = le16(c + 32), dstart = le16(c + 34);
        uint32_t ext = le32(c + 38), loff = le32(c + 42);
        if (p + 46 + nlen + xlen + klen > cd.size()) { error = "directorio central corrupto"; return false; }
        std::string raw(reinterpret_cast<const char*>(c + 46), nlen);
        p += 46 + nlen + xlen + klen;

        if (dstart != 0) { error = "ZIP multivolumen: no admitido"; return false; }
        if (flags & 0x1) { error = "entrada cifrada: " + raw; return false; }
        if (csize == 0xFFFFFFFF || usize == 0xFFFFFFFF || loff == 0xFFFFFFFF) { error = "ZIP64: no admitido"; return false; }
        if (raw.find('\0') != std::string::npos) { error = "nombre con byte nulo"; return false; }
        // Enlaces simbólicos (Unix) y puntos de reanálisis (Windows): nunca.
        uint8_t host = static_cast<uint8_t>(made_by >> 8);
        if (host == 3 && ((ext >> 16) & kUnixTypeMask) == kUnixSymlink) { error = "enlace simbólico en el ZIP: " + raw; return false; }
        if ((ext & kDosReparse) != 0) { error = "punto de reanálisis en el ZIP: " + raw; return false; }

        ZipEntry z;
        z.raw_name = raw;
        z.directory = !raw.empty() && (raw.back() == '/' || raw.back() == '\\');
        std::string why;
        if (!normalize_relative(raw, z.name, why)) {
            if (z.directory && (raw == "/" || raw == "./")) continue;
            error = "ruta no segura en el ZIP («" + raw.substr(0, 80) + "»): " + why;
            return false;
        }
        if (!seen.insert(fold_key(z.name) + (z.directory ? "/" : "")).second) {
            if (z.directory) continue;
            error = "entrada repetida (o igual salvo mayúsculas): " + z.name;
            return false;
        }
        z.method = method;
        z.crc = crc;
        z.size = usize;
        z.compressed = csize;
        z.local_offset = loff;
        if (!z.directory) {
            if (method != 0 && method != 8) { error = "compresión no admitida (" + std::to_string(method) + "): " + z.name; return false; }
            if (method == 0 && csize != usize) { error = "entrada sin comprimir con tamaños distintos: " + z.name; return false; }
            if (usize > lim.max_entry_bytes) { error = "archivo demasiado grande: " + z.name; return false; }
            if (usize > (1u << 20) && (csize == 0 || usize / std::max<uint32_t>(csize, 1) > lim.max_ratio)) {
                error = "relación de compresión sospechosa (posible bomba ZIP): " + z.name;
                return false;
            }
            sum += usize;
            if (sum > lim.max_total_bytes) { error = "el contenido descomprimido supera el límite"; return false; }
        }
        entries_.push_back(std::move(z));
    }
    // Un directorio no puede llamarse como un archivo (a y a/b).
    for (const auto& z : entries_) {
        if (z.directory) continue;
        if (seen.count(fold_key(z.name) + "/")) { error = "archivo y carpeta con el mismo nombre: " + z.name; return false; }
    }
    // Datos dentro del archivo y sin solapes (bombas que reutilizan los mismos bytes).
    std::vector<const ZipEntry*> files;
    for (const auto& z : entries_)
        if (!z.directory) files.push_back(&z);
    std::sort(files.begin(), files.end(), [](const ZipEntry* a, const ZipEntry* b) { return a->local_offset < b->local_offset; });
    for (size_t i = 0; i < files.size(); ++i) {
        unsigned char lh[30];
        if (!read_at(in, files[i]->local_offset, lh, 30) || le32(lh) != kLocalSig) { error = "cabecera local corrupta: " + files[i]->name; return false; }
        uint64_t data = files[i]->local_offset + 30 + le16(lh + 26) + le16(lh + 28);
        uint64_t end = data + files[i]->compressed;
        if (end > cd_offset) { error = "datos fuera de lugar: " + files[i]->name; return false; }
        if (i + 1 < files.size() && end > files[i + 1]->local_offset) { error = "entradas solapadas (posible bomba ZIP)"; return false; }
    }
    return true;
}

bool ZipReader::extract(const ZipEntry& z, const fs::path& dest, std::string& error, const std::function<bool()>& cancel) const {
    if (z.directory) { error = "es una carpeta"; return false; }
    std::ifstream in(file_, std::ios::binary);
    unsigned char lh[30];
    if (!in || !read_at(in, z.local_offset, lh, 30) || le32(lh) != kLocalSig) { error = "cabecera local corrupta"; return false; }
    if (le16(lh + 6) & 0x1) { error = "entrada cifrada"; return false; }
    uint64_t data = z.local_offset + 30 + le16(lh + 26) + le16(lh + 28);
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    std::ofstream out(dest, std::ios::binary | std::ios::trunc);
    if (!out) { error = "no se puede escribir en la carpeta temporal"; return false; }
    auto fail = [&](const std::string& why) {
        out.close();
        std::error_code rm;
        fs::remove(dest, rm);
        error = why + ": " + z.name;
        return false;
    };
    in.clear();
    in.seekg(static_cast<std::streamoff>(data));
    std::vector<unsigned char> inbuf(64 * 1024), outbuf(64 * 1024);
    uint64_t left = z.compressed, written = 0;
    uLong crc = crc32(0L, Z_NULL, 0);
    if (z.method == 0) {
        while (left) {
            if (cancel && cancel()) return fail("cancelado");
            size_t n = static_cast<size_t>(std::min<uint64_t>(left, inbuf.size()));
            in.read(reinterpret_cast<char*>(inbuf.data()), static_cast<std::streamsize>(n));
            if (static_cast<size_t>(in.gcount()) != n) return fail("ZIP truncado");
            crc = crc32(crc, inbuf.data(), static_cast<uInt>(n));
            out.write(reinterpret_cast<const char*>(inbuf.data()), static_cast<std::streamsize>(n));
            left -= n;
            written += n;
        }
    } else {
        z_stream zs{};
        if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) return fail("zlib no disponible");
        int rc = Z_OK;
        while (rc != Z_STREAM_END) {
            if (cancel && cancel()) { inflateEnd(&zs); return fail("cancelado"); }
            if (zs.avail_in == 0) {
                if (left == 0) { inflateEnd(&zs); return fail("ZIP truncado"); }
                size_t n = static_cast<size_t>(std::min<uint64_t>(left, inbuf.size()));
                in.read(reinterpret_cast<char*>(inbuf.data()), static_cast<std::streamsize>(n));
                if (static_cast<size_t>(in.gcount()) != n) { inflateEnd(&zs); return fail("ZIP truncado"); }
                left -= n;
                zs.next_in = inbuf.data();
                zs.avail_in = static_cast<uInt>(n);
            }
            zs.next_out = outbuf.data();
            zs.avail_out = static_cast<uInt>(outbuf.size());
            rc = inflate(&zs, Z_NO_FLUSH);
            if (rc != Z_OK && rc != Z_STREAM_END) { inflateEnd(&zs); return fail("datos comprimidos corruptos"); }
            size_t got = outbuf.size() - zs.avail_out;
            written += got;
            // Nunca más de lo declarado: una bomba no puede crecer más allá del límite ya comprobado.
            if (written > z.size) { inflateEnd(&zs); return fail("el archivo descomprime más de lo declarado"); }
            crc = crc32(crc, outbuf.data(), static_cast<uInt>(got));
            out.write(reinterpret_cast<const char*>(outbuf.data()), static_cast<std::streamsize>(got));
        }
        inflateEnd(&zs);
    }
    out.close();
    if (!out) return fail("error de escritura (¿disco lleno?)");
    if (written != z.size) return fail("tamaño distinto del declarado");
    if (static_cast<uint32_t>(crc) != z.crc) return fail("CRC-32 incorrecto");
    return true;
}

}  // namespace wl::mods
