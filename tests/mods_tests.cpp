// Pruebas del núcleo de mods (docs/mods/06-pruebas_resultados.md): JSON acotado, rutas seguras, ZIP
// hostil, FST, análisis de paquetes, almacén transaccional, perfiles, fingerprint y disco virtual.
// Todo en carpetas temporales: nunca toca el disco extraído, la ROM ni los guardados reales.
#include <zlib.h>

#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <set>

#include "wl/mods/fst.h"
#include "wl/mods/json.h"
#include "wl/mods/package.h"
#include "wl/mods/safe_path.h"
#include "wl/mods/store.h"
#include "wl/mods/zip.h"
#include "wl/update.h"

extern int failures, checks;
#define MCHECK(x)                                                                     \
    do {                                                                              \
        ++checks;                                                                     \
        if (!(x)) { ++failures; std::cerr << "FALLO mods " << __LINE__ << ": " #x "\n"; } \
    } while (0)

using namespace wl::mods;
namespace fs = std::filesystem;

namespace {

void put(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}
std::string get(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

// ---- ZIP de prueba (escritor mínimo; permite fabricar archivos hostiles a propósito)
struct ZEntry {
    std::string name, data;
    bool deflate = false;
    uint32_t ext_attr = 0;
    uint16_t made_by = 20;
    uint16_t flags = 0;
    int64_t fake_size = -1;   // tamaño declarado distinto del real
};
void le16(std::string& o, uint16_t v) { o += char(v & 0xFF); o += char(v >> 8); }
void le32(std::string& o, uint32_t v) { for (int i = 0; i < 4; ++i) o += char((v >> (8 * i)) & 0xFF); }
std::string deflate_raw(const std::string& in) {
    z_stream zs{};
    deflateInit2(&zs, 9, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
    std::string out(deflateBound(&zs, in.size()) + 16, '\0');
    zs.next_in = (Bytef*)in.data();
    zs.avail_in = (uInt)in.size();
    zs.next_out = (Bytef*)out.data();
    zs.avail_out = (uInt)out.size();
    deflate(&zs, Z_FINISH);
    out.resize(zs.total_out);
    deflateEnd(&zs);
    return out;
}
void write_zip(const fs::path& p, const std::vector<ZEntry>& entries, bool overlap_second = false) {
    std::string out, cd;
    std::vector<uint32_t> offsets;
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        std::string body = e.deflate ? deflate_raw(e.data) : e.data;
        uint32_t crc = crc32(0, (const Bytef*)e.data.data(), (uInt)e.data.size());
        uint32_t usize = e.fake_size >= 0 ? (uint32_t)e.fake_size : (uint32_t)e.data.size();
        uint32_t off = overlap_second && i == 1 ? offsets[0] : (uint32_t)out.size();
        offsets.push_back(off);
        if (!(overlap_second && i == 1)) {
            le32(out, 0x04034b50); le16(out, 20); le16(out, e.flags); le16(out, e.deflate ? 8 : 0); le16(out, 0); le16(out, 0);
            le32(out, crc); le32(out, (uint32_t)body.size()); le32(out, usize); le16(out, (uint16_t)e.name.size()); le16(out, 0);
            out += e.name;
            out += body;
        }
        le32(cd, 0x02014b50); le16(cd, e.made_by); le16(cd, 20); le16(cd, e.flags); le16(cd, e.deflate ? 8 : 0); le16(cd, 0); le16(cd, 0);
        le32(cd, crc); le32(cd, (uint32_t)body.size()); le32(cd, usize); le16(cd, (uint16_t)e.name.size()); le16(cd, 0); le16(cd, 0);
        le16(cd, 0); le16(cd, 0); le32(cd, e.ext_attr); le32(cd, off);
        cd += e.name;
    }
    uint32_t cd_off = (uint32_t)out.size();
    out += cd;
    le32(out, 0x06054b50); le16(out, 0); le16(out, 0); le16(out, (uint16_t)entries.size()); le16(out, (uint16_t)entries.size());
    le32(out, (uint32_t)cd.size()); le32(out, cd_off); le16(out, 0);
    put(p, out);
}

// ---- Disco de prueba: FST real (formato de Wii) con carpetas anidadas
struct Node {
    std::map<std::string, Node> dirs;
    std::map<std::string, uint32_t> files;
};
void be32(std::vector<uint8_t>& d, size_t o, uint32_t v) { d[o] = v >> 24; d[o + 1] = v >> 16; d[o + 2] = v >> 8; d[o + 3] = (uint8_t)v; }
std::vector<uint8_t> make_fst(const std::vector<std::pair<std::string, std::string>>& files) {
    Node root;
    for (const auto& [path, data] : files) {
        Node* n = &root;
        size_t s = 0, slash;
        while ((slash = path.find('/', s)) != std::string::npos) { n = &n->dirs[path.substr(s, slash - s)]; s = slash + 1; }
        n->files[path.substr(s)] = (uint32_t)data.size();
    }
    struct E { bool dir; uint32_t name, a, b; };
    std::vector<E> entries{{true, 0, 0, 0}};
    std::string names;
    uint32_t offset = 0x100000;
    std::function<void(const Node&, uint32_t)> walk = [&](const Node& n, uint32_t parent) {
        for (const auto& [name, size] : n.files) {
            entries.push_back({false, (uint32_t)names.size(), offset >> 2, size});
            names += name; names += '\0';
            offset += (size + 0x7FFF) / 0x8000 * 0x8000 + 0x8000;
        }
        for (const auto& [name, child] : n.dirs) {
            size_t idx = entries.size();
            entries.push_back({true, (uint32_t)names.size(), parent, 0});
            names += name; names += '\0';
            walk(child, (uint32_t)idx);
            entries[idx].b = (uint32_t)entries.size();
        }
    };
    walk(root, 0);
    entries[0].b = (uint32_t)entries.size();
    std::vector<uint8_t> raw(entries.size() * 12 + names.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        be32(raw, i * 12, (entries[i].dir ? 0x01000000u : 0) | entries[i].name);
        be32(raw, i * 12 + 4, entries[i].a);
        be32(raw, i * 12 + 8, entries[i].b);
    }
    std::memcpy(raw.data() + entries.size() * 12, names.data(), names.size());
    return raw;
}
void make_disc(const fs::path& extracted, const std::vector<std::pair<std::string, std::string>>& files, const std::string& id = "SUPP01") {
    auto fst = make_fst(files);
    std::string boot(0x440, '\0');
    std::memcpy(boot.data(), id.data(), 6);
    std::vector<uint8_t> b(boot.begin(), boot.end());
    be32(b, 0x424, 0x80000 >> 2);
    be32(b, 0x428, (uint32_t)((fst.size() + 3) / 4));
    put(extracted / "sys" / "boot.bin", std::string(b.begin(), b.end()));
    put(extracted / "sys" / "bi2.bin", std::string(0x2000, '\0'));
    put(extracted / "sys" / "main.dol", "DOL");
    put(extracted / "sys" / "fst.bin", std::string(fst.begin(), fst.end()));
    for (const auto& [path, data] : files) put(extracted / "files" / path, data);
}

const std::vector<std::pair<std::string, std::string>> kDisc = {
    {"locale/sp_EU/mess.bin", "TEXTO ORIGINAL ES"},
    {"locale/en_EU/mess.bin", "ORIGINAL TEXT EN"},
    {"menu/title_bg.brres.lz", std::string(5000, 'T')},
    {"sound/stream/WP_91_32.brstm", std::string(3000, 'S')},
    {"rel/menu.rel.lz", "RELCODE"},
    {"opening.bnr", "BANNER"},
};

void test_json() {
    Json j;
    std::string e;
    MCHECK(parse_json(R"({"b":1,"a":[true,null,"x\u00e9"],"c":{"z":-5}})", j, e));
    MCHECK(j.dump() == R"({"a":[true,null,"xé"],"b":1,"c":{"z":-5}})");   // canónico: claves ordenadas
    MCHECK(!parse_json(R"({"a":1,"a":2})", j, e));              // clave repetida
    MCHECK(!parse_json("[1.5]", j, e));                           // solo enteros
    MCHECK(!parse_json("{\"a\":1} x", j, e));                     // basura al final
    MCHECK(!parse_json(std::string(30, '[') + std::string(30, ']'), j, e));   // profundidad (D07)
    JsonLimits small;
    small.max_bytes = 100;
    MCHECK(!parse_json("[" + std::string(200, ' ') + "]", j, e, small));   // tamaño (D07)
    small.max_bytes = 1 << 20;
    small.max_elements = 50;
    std::string many = "[";
    for (int i = 0; i < 100; ++i) many += (i ? ",1" : "1");
    MCHECK(!parse_json(many + "]", j, e, small));
    MCHECK(!parse_json("\"\\ud800\"", j, e));                     // sustituto suelto
    MCHECK(parse_json("\"\\ud83d\\ude00\"", j, e) && j.str() == "\xF0\x9F\x98\x80");
}

void test_paths() {
    std::string out, why;
    MCHECK(normalize_relative("files\\locale/./sp_EU//mess.bin", out, why) && out == "files/locale/sp_EU/mess.bin");
    for (const char* bad : {"../x", "a/../../x", "/etc/passwd", "C:/Windows/x", "C:x", "a/b:stream", "con.txt", "a/NUL",
                            "a/COM1.bin", "a/b./c", "a/b ", "a\x01", "", "a/\xC3\xA9", "a/b?c", "lpt9"}) {
        MCHECK(!normalize_relative(bad, out, why));
    }
    MCHECK(fold_key("Locale/SP_eu") == "locale/sp_eu");
    fs::path root = fs::temp_directory_path() / "wl-mods-paths";
    MCHECK(join_inside(root, "a/b").string().size() > 0);
    MCHECK(!is_within(root, root / ".." / "fuera"));
}

void test_zip(const fs::path& tmp) {
    ZipReader z;
    std::string e;
    // Válido: store + deflate, extracción con CRC.
    write_zip(tmp / "ok.zip", {{"Mod/files/a.bin", "hola"}, {"Mod/files/b.bin", std::string(10000, 'x'), true}, {"Mod/", ""}});
    MCHECK(z.open(tmp / "ok.zip", e));
    MCHECK(z.entries().size() == 3);
    for (const auto& en : z.entries()) {
        if (en.name == "Mod/files/b.bin") {
            MCHECK(z.extract(en, tmp / "out" / "b.bin", e));
            MCHECK(get(tmp / "out" / "b.bin") == std::string(10000, 'x'));
        }
    }
    // D01 traversal y rutas absolutas
    for (const char* bad : {"../evil.txt", "..\\evil.txt", "/abs.txt", "C:/abs.txt", "a/../../evil"}) {
        write_zip(tmp / "bad.zip", {{bad, "x"}});
        MCHECK(!z.open(tmp / "bad.zip", e));
    }
    // D02 enlace simbólico (Unix) y punto de reanálisis (Windows)
    ZEntry link{"link", "/etc/passwd"};
    link.made_by = (3 << 8) | 20;
    link.ext_attr = 0120777u << 16;
    write_zip(tmp / "link.zip", {link});
    MCHECK(!z.open(tmp / "link.zip", e));
    ZEntry rp{"rp", "x"};
    rp.ext_attr = 0x400;
    write_zip(tmp / "rp.zip", {rp});
    MCHECK(!z.open(tmp / "rp.zip", e));
    // D04 duplicados y colisión por mayúsculas
    write_zip(tmp / "dup.zip", {{"a/X.bin", "1"}, {"A/x.BIN", "2"}});
    MCHECK(!z.open(tmp / "dup.zip", e));
    // D03 bomba: relación de compresión exagerada
    write_zip(tmp / "bomb.zip", {{"bomb.bin", std::string(50 << 20, '\0'), true}});
    MCHECK(!z.open(tmp / "bomb.zip", e));
    // Declara menos de lo que descomprime: se corta al pasar del tamaño declarado
    ZEntry liar{"liar.bin", std::string(4096, 'a'), true};
    liar.fake_size = 10;
    write_zip(tmp / "liar.zip", {liar});
    MCHECK(z.open(tmp / "liar.zip", e));
    MCHECK(!z.extract(z.entries()[0], tmp / "out" / "liar.bin", e));
    MCHECK(!fs::exists(tmp / "out" / "liar.bin"));
    // Entradas solapadas (bomba «overlapping»)
    write_zip(tmp / "overlap.zip", {{"a.bin", "abc"}, {"b.bin", "abc"}}, true);
    MCHECK(!z.open(tmp / "overlap.zip", e));
    // Cifrado
    ZEntry enc{"enc.bin", "x"};
    enc.flags = 1;
    write_zip(tmp / "enc.zip", {enc});
    MCHECK(!z.open(tmp / "enc.zip", e));
    // Truncado: cortar el archivo por la mitad
    std::string whole = get(tmp / "ok.zip");
    put(tmp / "trunc.zip", whole.substr(0, whole.size() / 2));
    MCHECK(!z.open(tmp / "trunc.zip", e));
    // Nombres reservados de Windows
    write_zip(tmp / "con.zip", {{"files/CON", "x"}});
    MCHECK(!z.open(tmp / "con.zip", e));
}

void test_fst() {
    auto raw = make_fst(kDisc);
    Fst f;
    std::string e;
    MCHECK(parse_fst(raw, f, e));
    MCHECK(f.files.size() == kDisc.size());
    const FstFile* mess = f.find("LOCALE/sp_eu/MESS.bin");
    MCHECK(mess && mess->path == "locale/sp_EU/mess.bin" && mess->size == 17);
    // Más pequeño: mismo sitio. Más grande que su hueco: se recoloca al final, alineado.
    const FstFile* big = f.find("opening.bnr");
    auto rebuilt = rebuild_fst(f, {{mess->index, 10}, {big->index, 0x200000}}, 0);
    Fst g;
    MCHECK(parse_fst(rebuilt, g, e));
    MCHECK(g.find("locale/sp_EU/mess.bin")->offset == mess->offset && g.find("locale/sp_EU/mess.bin")->size == 10);
    MCHECK(g.find("opening.bnr")->size == 0x200000);
    MCHECK(g.find("opening.bnr")->offset >= f.data_end && g.find("opening.bnr")->offset % 0x8000 == 0);
    MCHECK(g.files.size() == f.files.size() && rebuilt.size() == raw.size());
    std::vector<uint8_t> junk(raw.begin(), raw.begin() + 20);
    MCHECK(!parse_fst(junk, g, e));
}

PackagePlan plan_for(const fs::path& zip, const Fst& fst, const std::string& disc = "SUPP01") {
    std::string e;
    auto src = open_source(zip, e);
    if (!src) { PackagePlan p; p.blocking.push_back(e); return p; }
    return analyze_package(*src, fst, *game_support("wiiparty"), disc);
}

bool blocked_with(const PackagePlan& p, const std::string& needle) {
    for (const auto& b : p.blocking)
        if (b.find(needle) != std::string::npos) return true;
    return false;
}

void test_package(const fs::path& tmp) {
    Fst fst;
    std::string e;
    MCHECK(parse_fst(make_fst(kDisc), fst, e));
    // Formato files con carpeta envolvente y prefijo DATA/files; README ignorado; otra región ignorada.
    write_zip(tmp / "p1.zip", {{"MiMod/DATA/files/locale/sp_EU/mess.bin", "NUEVO"},
                               {"MiMod/DATA/files/locale/en_US/mess.bin", "US"},
                               {"MiMod/README.txt", "léeme"}});
    auto p = plan_for(tmp / "p1.zip", fst);
    MCHECK(p.ok() && p.format == ModFormat::Files);
    MCHECK(p.patches["main"].size() == 1 && p.patches["main"][0].disc == "locale/sp_EU/mess.bin");
    MCHECK(p.ignored.size() == 2);
    // Estilo MP9: rutas desde una subcarpeta del disco sin «files/».
    write_zip(tmp / "p2.zip", {{"Skin/menu/title_bg.brres.lz", "X"}});
    p = plan_for(tmp / "p2.zip", fst);
    MCHECK(p.ok() && p.patches["main"][0].disc == "menu/title_bg.brres.lz");
    // A06/A05: nada coincide con el disco
    write_zip(tmp / "p3.zip", {{"otro/juego/archivo.szs", "X"}});
    MCHECK(blocked_with(plan_for(tmp / "p3.zip", fst), "ningún archivo coincide"));
    // A04: código
    write_zip(tmp / "p4.zip", {{"files/rel/menu.rel.lz", "X"}});
    MCHECK(blocked_with(plan_for(tmp / "p4.zip", fst), "requiere recompilar"));
    write_zip(tmp / "p5.zip", {{"codes/SUPP01.gct", "X"}, {"files/opening.bnr", "B"}});
    MCHECK(blocked_with(plan_for(tmp / "p5.zip", fst), "Gecko"));
    // D05: ejecutables y scripts
    write_zip(tmp / "p6.zip", {{"files/opening.bnr", "B"}, {"instalar.bat", "del *"}});
    MCHECK(blocked_with(plan_for(tmp / "p6.zip", fst), "programas o scripts"));
    write_zip(tmp / "p7.zip", {{"juego.wbfs", "X"}});
    MCHECK(blocked_with(plan_for(tmp / "p7.zip", fst), "imagen de disco"));
    write_zip(tmp / "p8.zip", {{"SUPP01/tex1_64x64_abc_5.png", "X"}});
    MCHECK(blocked_with(plan_for(tmp / "p8.zip", fst), "Dolphin"));
    // A03: Riivolution con opciones y solo archivos → admitido
    const char* xml = R"(<wiidisc version="1"><id game="SUP"><region type="P"/><region type="E"/></id>
      <options><section name="Pack"><option name="Texto" default="1"><choice name="Sí"><patch id="txt"/></choice></option>
      <option name="Fondo"><choice name="Rojo"><patch id="bg"/></choice></option></section></options>
      <patch id="txt" root="/Pack"><file disc="/locale/sp_EU/mess.bin" external="mess.bin"/></patch>
      <patch id="bg"><folder disc="/menu" external="/Pack/menu"/></patch></wiidisc>)";
    write_zip(tmp / "r1.zip", {{"riivolution/Pack.xml", xml}, {"Pack/mess.bin", "RIIVO"}, {"Pack/menu/title_bg.brres.lz", "BG"},
                               {"riivolution/config/SUPP.xml", "<x/>"}});
    p = plan_for(tmp / "r1.zip", fst);
    MCHECK(p.ok() && p.format == ModFormat::Riivolution);
    MCHECK(p.options.size() == 2 && p.options[1].default_choice == 1 && !p.warnings.empty());
    MCHECK(p.patches["bg"].size() == 1 && p.patches["bg"][0].disc == "menu/title_bg.brres.lz");
    // Riivolution con <memory>: código → rechazado; otra región → rechazado
    write_zip(tmp / "r2.zip", {{"riivolution/a.xml", R"(<wiidisc><patch id="m"><memory offset="0x80001000" value="60000000"/></patch></wiidisc>)"}});
    MCHECK(blocked_with(plan_for(tmp / "r2.zip", fst), "memoria"));
    write_zip(tmp / "r3.zip", {{"riivolution/a.xml", R"(<wiidisc><id game="SUP"><region type="E"/></id><patch id="t"><file disc="/opening.bnr" external="/b.bnr"/></patch></wiidisc>)"},
                               {"b.bnr", "B"}});
    MCHECK(blocked_with(plan_for(tmp / "r3.zip", fst), "otra región"));
    write_zip(tmp / "r4.zip", {{"riivolution/a.xml", R"(<wiidisc><id game="RMC"/><patch id="t"><file disc="/opening.bnr" external="/b.bnr"/></patch></wiidisc>)"},
                               {"b.bnr", "B"}});
    MCHECK(blocked_with(plan_for(tmp / "r4.zip", fst), "otro juego"));
    write_zip(tmp / "r5.zip", {{"riivolution/a.xml", R"(<!DOCTYPE x [<!ENTITY e SYSTEM "file:///c:/x">]><wiidisc/>)"}});
    MCHECK(!plan_for(tmp / "r5.zip", fst).ok());
    // .7z y .rar: se pide descomprimir y elegir la carpeta (no hay extractor seguro integrado)
    put(tmp / "m.7z", "7z");
    std::string err;
    MCHECK(!open_source(tmp / "m.7z", err) && err.find("carpeta") != std::string::npos);
    // Importar carpeta
    put(tmp / "carpeta" / "files" / "opening.bnr", "BANNER-MOD");
    p = plan_for(tmp / "carpeta", fst);
    MCHECK(p.ok() && p.patches["main"][0].disc == "opening.bnr");
}

void test_store(const fs::path& tmp) {
    fs::path disc = tmp / "juego" / "extracted";
    make_disc(disc, kDisc);
    std::string original_mess = get(disc / "files" / "locale" / "sp_EU" / "mess.bin");
    ModStore store("wiiparty", tmp / "datos", disc);
    std::string e;
    MCHECK(store.open(e));
    MCHECK(store.base().disc_id == "SUPP01" && store.base().base_content_id.size() == 6 + 1 + 16);
    MCHECK(ModStore("brawl", tmp / "b", disc).open(e) == false);   // juego sin soporte

    // Instalar A (texto) y B (texto + banner más grande que su hueco)
    write_zip(tmp / "a.zip", {{"A/files/locale/sp_EU/mess.bin", "TEXTO DEL MOD A!"}});
    write_zip(tmp / "b.zip", {{"B/files/locale/sp_EU/mess.bin", "TEXTO B"}, {"B/files/opening.bnr", std::string(0x30000, 'N')}});
    InstalledMod a, b, a2;
    ModMeta ma;
    ma.name = "Mod A";
    ma.version = "1.0.0";
    MCHECK(store.install(tmp / "a.zip", ma, a, e));
    MCHECK(a.id() == "local.mod-a" && a.json["kind"].str() == "data");
    MCHECK(store.install(tmp / "a.zip", ma, a2, e) && a2.key == a.key);   // B11: sin duplicar
    ModMeta mb;
    mb.name = "Mod B";
    MCHECK(store.install(tmp / "b.zip", mb, b, e));
    MCHECK(store.installed().size() == 2);
    MCHECK(store.verify(a, e));
    // Cancelación a mitad: no queda nada a medias (B14/D10)
    ModMeta mc;
    mc.name = "Mod C";
    InstalledMod c;
    write_zip(tmp / "c.zip", {{"files/opening.bnr", "C"}});
    MCHECK(!store.install(tmp / "c.zip", mc, c, e, {}, [] { return true; }));
    MCHECK(store.installed().size() == 2 && fs::is_empty(tmp / "datos" / "staging"));
    // Paquete rechazado: no se instala
    write_zip(tmp / "mal.zip", {{"files/opening.bnr", "B"}, {"x.exe", "MZ"}});
    MCHECK(!store.install(tmp / "mal.zip", mc, c, e));

    // Perfiles: default vacío = vanilla
    Profile def;
    MCHECK(store.load_profile("default", def, e));
    Effective v = store.compute(def);
    MCHECK(v.vanilla() && v.fingerprint == kVanilla);
    fs::path vd;
    MCHECK(store.build_vdisc(v, vd, e) && vd == disc);

    Profile custom;
    custom.name = "custom-1";
    custom.mods = {{a.id(), a.key, true, {}}, {b.id(), b.key, true, {}}};
    MCHECK(store.save_profile(custom, e));
    MCHECK(store.load_profile("custom-1", custom, e) && custom.epoch == 1);
    Effective eff = store.compute(custom);
    MCHECK(eff.problems.empty() && eff.files.size() == 2);
    MCHECK(eff.conflicts.size() == 1);   // A07: B tapa a A en mess.bin
    std::string fp1 = eff.fingerprint;
    MCHECK(fp1.size() == 64 && store.compute(custom).fingerprint == fp1);   // determinista
    // Orden distinto → fingerprint distinto (C09)
    Profile swapped = custom;
    std::swap(swapped.mods[0], swapped.mods[1]);
    MCHECK(store.compute(swapped).fingerprint != fp1);
    // Desactivado → otro fingerprint (C08)
    Profile only_a = custom;
    only_a.mods[1].enabled = false;
    Effective ea = store.compute(only_a);
    MCHECK(ea.fingerprint != fp1 && ea.conflicts.empty());

    // Disco virtual: enlaces duros + FST regenerada; el original intacto
    MCHECK(store.build_vdisc(eff, vd, e));
    MCHECK(get(vd / "files" / "locale" / "sp_EU" / "mess.bin") == "TEXTO B");
    MCHECK(get(vd / "files" / "menu" / "title_bg.brres.lz") == std::string(5000, 'T'));
    MCHECK(get(disc / "files" / "locale" / "sp_EU" / "mess.bin") == original_mess);
    Fst vf;
    auto raw = get(vd / "sys" / "fst.bin");
    MCHECK(parse_fst(std::vector<uint8_t>(raw.begin(), raw.end()), vf, e));
    MCHECK(vf.find("opening.bnr")->size == 0x30000 && vf.find("locale/sp_EU/mess.bin")->size == 7);
    MCHECK(get(vd / "sys" / "boot.bin") == get(disc / "sys" / "boot.bin"));
    MCHECK(get(vd / ".complete") == fp1);
    // Reutilización y detección de manipulación del disco virtual
    fs::path vd2;
    MCHECK(store.build_vdisc(eff, vd2, e) && vd2 == vd);
    // Romper el enlace y escribir otra cosa: se reconstruye en lugar de lanzar algo distinto
    fs::remove(vd / "files" / "opening.bnr");
    put(vd / "files" / "opening.bnr", "MANIPULADO");
    MCHECK(store.build_vdisc(eff, vd2, e) && get(vd2 / "files" / "opening.bnr") == std::string(0x30000, 'N'));
    // Paquete alterado tras instalar: verify lo detecta y compute+vdisc no lo aplican a ciegas
    fs::path a_file = tmp / "datos" / "packages" / a.key / "content" / "A" / "files" / "locale" / "sp_EU" / "mess.bin";
    put(a_file, "ALTERADO");
    MCHECK(!store.verify(a, e));
    fs::path vd3;
    MCHECK(!store.build_vdisc(store.compute(only_a), vd3, e));

    // Desinstalar: bloqueado si un perfil lo usa
    MCHECK(!store.uninstall(b.key, e));
    MCHECK(store.delete_profile("custom-1", e));
    MCHECK(store.uninstall(b.key, e) && store.installed().size() == 1);
    // Perfil activo
    MCHECK(store.active_profile() == "default");
    MCHECK(!store.set_active_profile("../x", e));
    // Recuperación al abrir: staging a medias y vdisc sin .complete desaparecen (D10)
    put(tmp / "datos" / "staging" / "123-0" / "content" / "x", "a medias");
    fs::create_directories(tmp / "datos" / "vdisc" / "roto" / "files");
    ModStore again("wiiparty", tmp / "datos", disc);
    MCHECK(again.open(e));
    MCHECK(fs::is_empty(tmp / "datos" / "staging") && !fs::exists(tmp / "datos" / "vdisc" / "roto"));
    // Bloqueo entre procesos: con el candado tomado, no se instala
    {
        StoreLock held(tmp / "datos" / "locks" / "store.lock");
        MCHECK(held.held());
        MCHECK(!again.install(tmp / "a.zip", ma, a2, e) && e.find("otra operación") != std::string::npos);
    }
    // Perfil corrupto o malicioso: error, no excepción
    put(tmp / "datos" / "profiles" / "roto.json", "{\"schema\":1,\"mods\":[{\"id\":\"x\",\"package\":\"../../etc\"}]}");
    Profile bad;
    MCHECK(!again.load_profile("roto", bad, e));
}

}  // namespace

void run_mods_tests() {
    fs::path tmp = fs::temp_directory_path() / "wl-mods-tests";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp);
    int before = failures;
    test_json();
    test_paths();
    test_zip(tmp);
    test_fst();
    test_package(tmp);
    test_store(tmp);
    if (failures == before) fs::remove_all(tmp, ec);
}
