#include "wl/mods/xml.h"

#include <cctype>
#include <cstdlib>

namespace wl::mods {

namespace {

bool decode(const std::string& in, std::string& out) {
    out.clear();
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '&') { out += in[i]; continue; }
        size_t semi = in.find(';', i);
        if (semi == std::string::npos || semi - i > 10) return false;
        std::string ent = in.substr(i + 1, semi - i - 1);
        if (ent == "amp") out += '&';
        else if (ent == "lt") out += '<';
        else if (ent == "gt") out += '>';
        else if (ent == "quot") out += '"';
        else if (ent == "apos") out += '\'';
        else if (ent.size() > 1 && ent[0] == '#') {
            long v = ent[1] == 'x' ? std::strtol(ent.c_str() + 2, nullptr, 16) : std::strtol(ent.c_str() + 1, nullptr, 10);
            if (v <= 0 || v > 0x7E) return false;  // solo ASCII imprimible
            out += static_cast<char>(v);
        } else {
            return false;
        }
        i = semi;
    }
    return true;
}

}  // namespace

bool parse_xml(const std::string& t, XmlNode& root, std::string& error, size_t max_bytes) {
    if (t.size() > max_bytes) { error = "XML demasiado grande"; return false; }
    if (t.find("<!DOCTYPE") != std::string::npos || t.find("<!ENTITY") != std::string::npos) {
        error = "XML con DTD o entidades: no admitido";
        return false;
    }
    root = XmlNode{};
    root.name = "#document";
    std::vector<XmlNode*> stack{&root};
    size_t p = 0, elements = 0;
    while (true) {
        size_t lt = t.find('<', p);
        if (lt == std::string::npos) break;
        p = lt + 1;
        if (t.compare(lt, 4, "<!--") == 0) {
            size_t end = t.find("-->", lt + 4);
            if (end == std::string::npos) { error = "comentario sin cerrar"; return false; }
            p = end + 3;
            continue;
        }
        if (t.compare(lt, 2, "<?") == 0) {
            size_t end = t.find("?>", lt + 2);
            if (end == std::string::npos) { error = "declaración sin cerrar"; return false; }
            p = end + 2;
            continue;
        }
        if (t.compare(lt, 9, "<![CDATA[") == 0) {
            size_t end = t.find("]]>", lt);
            if (end == std::string::npos) { error = "CDATA sin cerrar"; return false; }
            p = end + 3;
            continue;
        }
        size_t gt = t.find('>', p);
        if (gt == std::string::npos) { error = "etiqueta sin cerrar"; return false; }
        std::string tag = t.substr(p, gt - p);
        p = gt + 1;
        if (!tag.empty() && tag[0] == '/') {
            std::string name = tag.substr(1);
            while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) name.pop_back();
            if (stack.size() < 2 || stack.back()->name != name) { error = "etiqueta de cierre inesperada: " + name; return false; }
            stack.pop_back();
            continue;
        }
        bool self_close = !tag.empty() && tag.back() == '/';
        if (self_close) tag.pop_back();
        size_t i = 0;
        while (i < tag.size() && !std::isspace(static_cast<unsigned char>(tag[i]))) ++i;
        XmlNode node;
        node.name = tag.substr(0, i);
        if (node.name.empty()) { error = "etiqueta vacía"; return false; }
        while (i < tag.size()) {
            while (i < tag.size() && std::isspace(static_cast<unsigned char>(tag[i]))) ++i;
            if (i >= tag.size()) break;
            size_t eq = tag.find('=', i);
            if (eq == std::string::npos) { error = "atributo sin valor en <" + node.name + ">"; return false; }
            std::string key = tag.substr(i, eq - i);
            while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back()))) key.pop_back();
            size_t q = eq + 1;
            while (q < tag.size() && std::isspace(static_cast<unsigned char>(tag[q]))) ++q;
            if (q >= tag.size() || (tag[q] != '"' && tag[q] != '\'')) { error = "atributo sin comillas en <" + node.name + ">"; return false; }
            char quote = tag[q];
            size_t end = tag.find(quote, q + 1);
            if (end == std::string::npos) { error = "atributo sin cerrar en <" + node.name + ">"; return false; }
            std::string value;
            if (!decode(tag.substr(q + 1, end - q - 1), value)) { error = "entidad no admitida en <" + node.name + ">"; return false; }
            if (node.attrs.count(key)) { error = "atributo repetido en <" + node.name + ">"; return false; }
            node.attrs[key] = value;
            i = end + 1;
        }
        if (++elements > 20000 || stack.size() > 32) { error = "XML demasiado grande o profundo"; return false; }
        stack.back()->children.push_back(std::move(node));
        if (!self_close) stack.push_back(&stack.back()->children.back());
    }
    if (stack.size() != 1) { error = "falta cerrar <" + stack.back()->name + ">"; return false; }
    return true;
}

}  // namespace wl::mods
