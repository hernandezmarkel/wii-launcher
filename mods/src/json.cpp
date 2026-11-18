#include "wl/mods/json.h"

#include <cstdio>

namespace wl::mods {

namespace {
const Json kNull;
}

const Json& Json::operator[](const std::string& key) const {
    if (!is_object()) return kNull;
    auto it = o_.find(key);
    return it == o_.end() ? kNull : it->second;
}

Json& Json::set(const std::string& key, Json value) {
    if (!is_object()) { *this = object(); }
    return o_[key] = std::move(value);
}

const Json& Json::operator[](size_t index) const {
    return is_array() && index < a_.size() ? a_[index] : kNull;
}

Json& Json::push(Json value) {
    if (!is_array()) { *this = array(); }
    a_.push_back(std::move(value));
    return a_.back();
}

static void dump_string(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
}

void Json::dump_to(std::string& out, int indent, int level) const {
    auto newline = [&](int l) {
        if (indent <= 0) return;
        out += '\n';
        out.append(static_cast<size_t>(indent * l), ' ');
    };
    switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += b_ ? "true" : "false"; break;
        case Type::Int: out += std::to_string(i_); break;
        case Type::String: dump_string(out, s_); break;
        case Type::Array:
            out += '[';
            for (size_t i = 0; i < a_.size(); ++i) {
                if (i) out += ',';
                newline(level + 1);
                a_[i].dump_to(out, indent, level + 1);
            }
            if (!a_.empty()) newline(level);
            out += ']';
            break;
        case Type::Object: {
            out += '{';
            bool first = true;
            for (const auto& [k, v] : o_) {
                if (!first) out += ',';
                first = false;
                newline(level + 1);
                dump_string(out, k);
                out += indent > 0 ? ": " : ":";
                v.dump_to(out, indent, level + 1);
            }
            if (!o_.empty()) newline(level);
            out += '}';
            break;
        }
    }
}

std::string Json::dump() const {
    std::string out;
    dump_to(out, 0, 0);
    return out;
}

std::string Json::pretty() const {
    std::string out;
    dump_to(out, 2, 0);
    out += '\n';
    return out;
}

namespace {

struct Parser {
    const std::string& t;
    const JsonLimits& lim;
    size_t p = 0;
    size_t elements = 0;
    std::string error;

    bool fail(const std::string& why) {
        if (error.empty()) error = why + " (posición " + std::to_string(p) + ")";
        return false;
    }
    void ws() {
        while (p < t.size() && (t[p] == ' ' || t[p] == '\n' || t[p] == '\r' || t[p] == '\t')) ++p;
    }
    static void utf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    bool hex4(uint32_t& v) {
        if (p + 4 > t.size()) return fail("escape \\u incompleto");
        v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = t[p++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return fail("escape \\u inválido");
        }
        return true;
    }
    bool string(std::string& out) {
        if (p >= t.size() || t[p] != '"') return fail("se esperaba una cadena");
        ++p;
        while (true) {
            if (p >= t.size()) return fail("cadena sin cerrar");
            unsigned char c = static_cast<unsigned char>(t[p++]);
            if (c == '"') break;
            if (c < 0x20) return fail("carácter de control en una cadena");
            if (c == '\\') {
                if (p >= t.size()) return fail("escape incompleto");
                char e = t[p++];
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u': {
                        uint32_t cp;
                        if (!hex4(cp)) return false;
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            uint32_t lo;
                            if (p + 2 > t.size() || t[p] != '\\' || t[p + 1] != 'u') return fail("sustituto UTF-16 sin pareja");
                            p += 2;
                            if (!hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) return fail("sustituto UTF-16 inválido");
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                            return fail("sustituto UTF-16 suelto");
                        }
                        if (cp == 0) return fail("carácter nulo en una cadena");
                        utf8(out, cp);
                        break;
                    }
                    default: return fail("escape desconocido");
                }
            } else {
                out += static_cast<char>(c);
            }
            if (out.size() > lim.max_string) return fail("cadena demasiado larga");
        }
        return true;
    }
    bool value(Json& out, int depth) {
        if (depth > lim.max_depth) return fail("anidamiento excesivo");
        if (++elements > lim.max_elements) return fail("demasiados elementos");
        ws();
        if (p >= t.size()) return fail("fin inesperado");
        char c = t[p];
        if (c == '{') {
            ++p;
            out = Json::object();
            ws();
            if (p < t.size() && t[p] == '}') { ++p; return true; }
            while (true) {
                ws();
                std::string key;
                if (!string(key)) return false;
                if (out.has(key)) return fail("clave repetida: " + key.substr(0, 40));
                ws();
                if (p >= t.size() || t[p] != ':') return fail("se esperaba ':'");
                ++p;
                Json v;
                if (!value(v, depth + 1)) return false;
                out.set(key, std::move(v));
                ws();
                if (p < t.size() && t[p] == ',') { ++p; continue; }
                if (p < t.size() && t[p] == '}') { ++p; return true; }
                return fail("se esperaba ',' o '}'");
            }
        }
        if (c == '[') {
            ++p;
            out = Json::array();
            ws();
            if (p < t.size() && t[p] == ']') { ++p; return true; }
            while (true) {
                Json v;
                if (!value(v, depth + 1)) return false;
                out.push(std::move(v));
                ws();
                if (p < t.size() && t[p] == ',') { ++p; continue; }
                if (p < t.size() && t[p] == ']') { ++p; return true; }
                return fail("se esperaba ',' o ']'");
            }
        }
        if (c == '"') {
            std::string s;
            if (!string(s)) return false;
            out = Json(std::move(s));
            return true;
        }
        if (t.compare(p, 4, "true") == 0) { p += 4; out = Json(true); return true; }
        if (t.compare(p, 5, "false") == 0) { p += 5; out = Json(false); return true; }
        if (t.compare(p, 4, "null") == 0) { p += 4; out = Json(); return true; }
        if (c == '-' || (c >= '0' && c <= '9')) {
            bool neg = c == '-';
            if (neg) ++p;
            if (p >= t.size() || t[p] < '0' || t[p] > '9') return fail("número inválido");
            if (t[p] == '0' && p + 1 < t.size() && t[p + 1] >= '0' && t[p + 1] <= '9') return fail("número con cero a la izquierda");
            uint64_t v = 0;
            int digits = 0;
            while (p < t.size() && t[p] >= '0' && t[p] <= '9') {
                if (++digits > 18) return fail("número demasiado grande");
                v = v * 10 + static_cast<uint64_t>(t[p++] - '0');
            }
            if (p < t.size() && (t[p] == '.' || t[p] == 'e' || t[p] == 'E')) {
                if (!lim.allow_fractions) return fail("solo se admiten enteros");
                if (t[p] == '.') {
                    ++p;
                    if (p >= t.size() || t[p] < '0' || t[p] > '9') return fail("número inválido");
                    while (p < t.size() && t[p] >= '0' && t[p] <= '9') ++p;
                }
                if (p < t.size() && (t[p] == 'e' || t[p] == 'E')) {
                    ++p;
                    if (p < t.size() && (t[p] == '+' || t[p] == '-')) ++p;
                    if (p >= t.size() || t[p] < '0' || t[p] > '9') return fail("número inválido");
                    while (p < t.size() && t[p] >= '0' && t[p] <= '9') ++p;
                }
            }
            out = Json(neg ? -static_cast<int64_t>(v) : static_cast<int64_t>(v));
            return true;
        }
        return fail("valor inesperado");
    }
};

}  // namespace

bool parse_json(const std::string& text, Json& out, std::string& error, const JsonLimits& limits) {
    if (text.size() > limits.max_bytes) {
        error = "JSON demasiado grande (" + std::to_string(text.size()) + " bytes)";
        return false;
    }
    Parser parser{text, limits, 0, 0, {}};
    // BOM UTF-8 tolerado (lo dejan algunos editores de Windows).
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) parser.p = 3;
    Json v;
    if (!parser.value(v, 0)) {
        error = parser.error;
        return false;
    }
    parser.ws();
    if (parser.p != text.size()) {
        error = "datos sobrantes tras el JSON";
        return false;
    }
    out = std::move(v);
    return true;
}

}  // namespace wl::mods
