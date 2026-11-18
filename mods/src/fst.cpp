#include "wl/mods/fst.h"

#include <algorithm>

#include "wl/mods/safe_path.h"

namespace wl::mods {

namespace {
constexpr size_t kEntry = 12;
constexpr uint64_t kAlign = 0x8000;

uint32_t be32(const std::vector<uint8_t>& d, size_t o) {
    return (static_cast<uint32_t>(d[o]) << 24) | (static_cast<uint32_t>(d[o + 1]) << 16) | (static_cast<uint32_t>(d[o + 2]) << 8) | d[o + 3];
}
void put32(std::vector<uint8_t>& d, size_t o, uint32_t v) {
    d[o] = static_cast<uint8_t>(v >> 24);
    d[o + 1] = static_cast<uint8_t>(v >> 16);
    d[o + 2] = static_cast<uint8_t>(v >> 8);
    d[o + 3] = static_cast<uint8_t>(v);
}
}  // namespace

const FstFile* Fst::find(const std::string& path) const {
    auto it = by_key.find(fold_key(path));
    return it == by_key.end() ? nullptr : &files[it->second];
}

bool parse_fst(const std::vector<uint8_t>& raw, Fst& out, std::string& error) {
    out = Fst{};
    out.raw = raw;
    if (raw.size() < kEntry || raw[0] != 1) { error = "fst.bin no válida"; return false; }
    uint32_t count = be32(raw, 8);
    size_t names = static_cast<size_t>(count) * kEntry;
    if (count == 0 || names > raw.size()) { error = "fst.bin no válida (recuento)"; return false; }
    // Pila de carpetas: (nombre con '/', índice de fin)
    std::vector<std::pair<std::string, uint32_t>> dirs{{"", count}};
    for (uint32_t i = 1; i < count; ++i) {
        while (dirs.size() > 1 && i >= dirs.back().second) dirs.pop_back();
        size_t e = static_cast<size_t>(i) * kEntry;
        bool is_dir = raw[e] != 0;
        size_t name_off = names + (be32(raw, e) & 0x00FFFFFF);
        if (name_off >= raw.size()) { error = "fst.bin no válida (nombre)"; return false; }
        size_t end = name_off;
        while (end < raw.size() && raw[end]) ++end;
        std::string name(reinterpret_cast<const char*>(&raw[name_off]), end - name_off);
        std::string parent;
        for (const auto& d : dirs) parent += d.first;
        if (is_dir) {
            uint32_t next = be32(raw, e + 8);
            if (next <= i || next > count) { error = "fst.bin no válida (carpeta)"; return false; }
            dirs.push_back({name + "/", next});
        } else {
            FstFile f;
            f.path = parent + name;
            f.index = i;
            f.offset = static_cast<uint64_t>(be32(raw, e + 4)) << 2;
            f.size = be32(raw, e + 8);
            out.data_end = std::max(out.data_end, f.offset + f.size);
            out.by_key[fold_key(f.path)] = out.files.size();
            out.files.push_back(std::move(f));
        }
    }
    return true;
}

std::vector<uint8_t> rebuild_fst(const Fst& fst, const std::map<uint32_t, uint32_t>& new_sizes, uint64_t min_end) {
    std::vector<uint8_t> raw = fst.raw;
    // Hueco disponible de cada archivo: hasta el siguiente desplazamiento ocupado.
    std::vector<uint64_t> starts;
    starts.reserve(fst.files.size());
    for (const auto& f : fst.files) starts.push_back(f.offset);
    std::sort(starts.begin(), starts.end());
    uint64_t tail = (std::max(min_end, fst.data_end) + kAlign - 1) / kAlign * kAlign;
    for (const auto& f : fst.files) {
        auto it = new_sizes.find(f.index);
        if (it == new_sizes.end()) continue;
        uint32_t size = it->second;
        size_t e = static_cast<size_t>(f.index) * kEntry;
        auto next = std::upper_bound(starts.begin(), starts.end(), f.offset);
        uint64_t room = next == starts.end() ? UINT64_MAX : *next - f.offset;
        if (size <= room) {
            put32(raw, e + 8, size);
        } else {
            put32(raw, e + 4, static_cast<uint32_t>(tail >> 2));
            put32(raw, e + 8, size);
            tail = (tail + size + kAlign - 1) / kAlign * kAlign;
        }
    }
    return raw;
}

}  // namespace wl::mods
