#include "wl/mods/safe_path.h"

#include <algorithm>
#include <cctype>
#include <vector>

namespace wl::mods {

namespace {

bool reserved_device(std::string seg) {
    // "CON", "con.txt", "COM1.bin"... Windows ignora la extensión y los espacios finales.
    size_t dot = seg.find('.');
    if (dot != std::string::npos) seg = seg.substr(0, dot);
    for (auto& c : seg) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    static const char* fixed[] = {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$", "CLOCK$"};
    for (const char* f : fixed)
        if (seg == f) return true;
    if (seg.size() == 4 && (seg.compare(0, 3, "COM") == 0 || seg.compare(0, 3, "LPT") == 0) &&
        std::isdigit(static_cast<unsigned char>(seg[3])))
        return true;
    return false;
}

}  // namespace

bool normalize_relative(const std::string& raw, std::string& out, std::string& why) {
    out.clear();
    if (raw.empty()) { why = "ruta vacía"; return false; }
    if (raw.size() > kMaxPathBytes) { why = "ruta demasiado larga"; return false; }
    std::string s = raw;
    std::replace(s.begin(), s.end(), '\\', '/');
    if (s[0] == '/') { why = "ruta absoluta"; return false; }
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= s.size()) {
        size_t slash = s.find('/', start);
        std::string seg = s.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        start = slash == std::string::npos ? s.size() + 1 : slash + 1;
        if (seg.empty() || seg == ".") continue;
        if (seg == "..") { why = "ruta con '..'"; return false; }
        if (seg.size() > kMaxSegmentBytes) { why = "nombre demasiado largo"; return false; }
        for (unsigned char c : seg) {
            if (c < 0x20 || c == 0x7F) { why = "carácter de control en la ruta"; return false; }
            if (c >= 0x80) { why = "nombre con caracteres no ASCII"; return false; }
            if (c == ':') { why = "ruta con ':' (unidad o flujo alternativo)"; return false; }
            if (c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*') {
                why = "carácter no permitido en Windows";
                return false;
            }
        }
        if (seg.back() == '.' || seg.back() == ' ') { why = "nombre que acaba en punto o espacio"; return false; }
        if (reserved_device(seg)) { why = "nombre reservado de Windows (" + seg + ")"; return false; }
        parts.push_back(seg);
    }
    if (parts.empty()) { why = "ruta vacía"; return false; }
    if (static_cast<int>(parts.size()) > kMaxPathDepth) { why = "árbol de carpetas demasiado profundo"; return false; }
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += '/';
        out += parts[i];
    }
    return true;
}

std::string fold_key(const std::string& normalized) {
    std::string k = normalized;
    for (auto& c : k) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return k;
}

std::string to_utf8(const fs::path& p) {
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

fs::path from_utf8(const std::string& s) {
#if defined(__cpp_char8_t)
    return fs::path(std::u8string(s.begin(), s.end()));
#else
    return fs::u8path(s);
#endif
}

bool is_within(const fs::path& root, const fs::path& child) {
    std::error_code e1, e2;
    fs::path r = fs::weakly_canonical(root, e1);
    fs::path c = fs::weakly_canonical(child, e2);
    if (e1 || e2) return false;
    auto rs = fold_key(to_utf8(r.lexically_normal()));
    auto cs = fold_key(to_utf8(c.lexically_normal()));
    std::replace(rs.begin(), rs.end(), '\\', '/');
    std::replace(cs.begin(), cs.end(), '\\', '/');
    while (!rs.empty() && rs.back() == '/') rs.pop_back();
    return cs.size() > rs.size() && cs.compare(0, rs.size(), rs) == 0 && cs[rs.size()] == '/';
}

fs::path join_inside(const fs::path& root, const std::string& normalized) {
    fs::path p = root / from_utf8(normalized);
    return is_within(root, p) ? p : fs::path();
}

}  // namespace wl::mods
