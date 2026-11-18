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
#include "wl/mods/gamebanana.h"
#include "wl/mods/json.h"
#include "wl/mods/package.h"
#include "wl/mods/safe_path.h"
#include "wl/mods/session.h"
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

// ---- Proveedor con un servidor simulado (nunca se usa la red real en las pruebas)
struct MockHttp : wl::upd::HttpClient {
    std::map<std::string, wl::upd::HttpResponse> routes;
    std::vector<std::string> seen;
    wl::upd::HttpResponse get(const wl::upd::HttpRequest& req) override {
        seen.push_back(req.url);
        auto it = routes.find(req.url);
        wl::upd::HttpResponse r;
        if (it == routes.end()) { r.status = 404; return r; }
        r = it->second;
        if (r.status == 200 && r.body.size() > req.max_bytes) { wl::upd::HttpResponse big; big.error = "grande"; return big; }
        if (r.status == 200 && !req.to_file.empty()) { put(req.to_file, r.body); r.body.clear(); }
        return r;
    }
    void ok(const std::string& url, const std::string& body) { routes[url].status = 200; routes[url].body = body; }
    void redirect(const std::string& url, const std::string& to) { routes[url].status = 302; routes[url].headers["location"] = to; }
};

void test_gamebanana(const fs::path& tmp) {
    MCHECK(gamebanana_url_allowed("https://gamebanana.com/dl/1"));
    MCHECK(gamebanana_url_allowed("https://files.gamebanana.com/mods/x.zip"));
    for (const char* bad : {"http://gamebanana.com/dl/1", "https://gamebanana.com.evil.com/x", "https://evil.com/x", "file:///c:/x",
                            "https://user@gamebanana.com/x", "https://gamebanana.com:8443/x", "https://192.168.1.10/x"})
        MCHECK(!gamebanana_url_allowed(bad));
    MCHECK(gamebanana_game("wiiparty") == 6370 && gamebanana_game("brawl") == 5678 && gamebanana_game("x") == 0);
    MCHECK(strip_html("<b>Hola</b> &amp; <a href='x'>adiós</a>") == "Hola & adiós");

    MockHttp http;
    GameBananaProvider gb(http, tmp / "gbcache");
    gb.set_min_interval(std::chrono::milliseconds(0));
    std::string e;
    const std::string list_url = "https://gamebanana.com/apiv11/Mod/Index?_nPage=1&_nPerpage=20&_aFilters%5BGeneric_Game%5D=6370";
    // B12: registros incompletos y de otro juego se descartan; decimales en la respuesta no rompen nada.
    http.ok(list_url, R"j({"_aMetadata":{"_nRecordCount":3,"_bIsComplete":true,"_fRating":4.5},"_aRecords":[
        {"_idRow":1,"_sModelName":"Mod","_sName":"Bueno","_aGame":{"_idRow":6370},"_sProfileUrl":"https://gamebanana.com/mods/1"},
        {"_idRow":2,"_sModelName":"Mod","_sName":"Otro juego","_aGame":{"_idRow":5896}},
        {"_sModelName":"Mod","_aGame":{"_idRow":6370}},
        {"_idRow":4,"_sModelName":"Mod","_sName":"Url rara","_aGame":{"_idRow":6370},"_sProfileUrl":"javascript:alert(1)"}]})j");
    CatalogPage page;
    MCHECK(gb.list(6370, 1, "", page, e));
    MCHECK(page.items.size() == 2 && page.items[0].name == "Bueno" && page.total == 3);
    MCHECK(page.items[1].profile_url == "https://gamebanana.com/mods/4");
    // B05: proveedor caído → se usa la caché (modo sin conexión) y no falla el launcher.
    http.routes[list_url].status = 503;
    MockHttp down;
    GameBananaProvider gb2(down, tmp / "gbcache");
    gb2.set_min_interval(std::chrono::milliseconds(0));
    CatalogPage cached;
    MCHECK(gb2.list(6370, 1, "", cached, e) && cached.from_cache);
    GameBananaProvider gb3(down, tmp / "gbcache-vacia");
    gb3.set_min_interval(std::chrono::milliseconds(0));
    MCHECK(!gb3.list(6370, 1, "", cached, e) && !e.empty());
    MCHECK(!gb.list(6370, 1, "", cached, e) == false);   // la misma instancia sigue usando su caché válida
    // B12: JSON roto
    http.ok("https://gamebanana.com/apiv11/Mod/9/ProfilePage", "{roto");
    CatalogDetail d;
    MCHECK(!gb.detail(9, d, e));
    // Ficha: archivos sin MD5 válido o con URL ajena se descartan
    http.ok("https://gamebanana.com/apiv11/Mod/7/ProfilePage", R"({"_idRow":7,"_sName":"Mod 7","_sLicense":"<b>CC BY</b>","_aFiles":[
        {"_idRow":70,"_sFile":"bueno.zip","_nFilesize":4,"_sMd5Checksum":"5D41402ABC4B2A76B9719D911017C592","_sDownloadUrl":"https://gamebanana.com/dl/70"},
        {"_idRow":71,"_sFile":"malo.zip","_nFilesize":4,"_sMd5Checksum":"5d41402abc4b2a76b9719d911017c592","_sDownloadUrl":"https://evil.example/dl/71"},
        {"_idRow":72,"_sFile":"sinmd5.zip","_nFilesize":4,"_sDownloadUrl":"https://gamebanana.com/dl/72"}]})");
    MCHECK(gb.detail(7, d, e) && d.files.size() == 1 && d.license == "CC BY" && d.files[0].md5 == "5d41402abc4b2a76b9719d911017c592");
    // Descarga: redirección a files.gamebanana.com permitida; MD5 de "hello" correcto
    http.redirect("https://gamebanana.com/dl/70", "https://files.gamebanana.com/mods/bueno.zip");
    http.ok("https://files.gamebanana.com/mods/bueno.zip", "hello");
    CatalogFile f = d.files[0];
    f.size = 5;
    MCHECK(gb.download(f, tmp / "dl" / "a.zip", e) && get(tmp / "dl" / "a.zip") == "hello");
    // D08: MD5 distinto → se borra
    http.ok("https://files.gamebanana.com/mods/bueno.zip", "HELLO");
    MCHECK(!gb.download(f, tmp / "dl" / "b.zip", e) && !fs::exists(tmp / "dl" / "b.zip"));
    // D11: redirección a un dominio inesperado → rechazada sin pedir ese dominio
    http.redirect("https://gamebanana.com/dl/70", "https://evil.example/x.zip");
    size_t before = http.seen.size();
    MCHECK(!gb.download(f, tmp / "dl" / "c.zip", e) && e.find("no permitido") != std::string::npos);
    for (size_t i = before; i < http.seen.size(); ++i) MCHECK(http.seen[i].find("evil") == std::string::npos);
    // Tamaño anunciado enorme → ni se intenta
    CatalogFile huge = f;
    huge.size = kMaxModDownload + 1;
    MCHECK(!gb.download(huge, tmp / "dl" / "d.zip", e));
    // Más bytes de los anunciados → corte
    http.redirect("https://gamebanana.com/dl/70", "https://files.gamebanana.com/mods/bueno.zip");
    http.ok("https://files.gamebanana.com/mods/bueno.zip", std::string(3 << 20, 'x'));
    MCHECK(!gb.download(f, tmp / "dl" / "e.zip", e));
    // Árbol del archivo
    http.ok("https://api.gamebanana.com/Core/Item/Data?itemtype=File&itemid=70&fields=Metadata().aArchiveFilesList()",
            R"([["Mod/files/opening.bnr","Mod/README.txt"]])");
    std::vector<std::string> tree;
    MCHECK(gb.file_tree(70, tree, e) && tree.size() == 2);

    // Comprobación previa solo con nombres
    Fst fst;
    MCHECK(parse_fst(make_fst(kDisc), fst, e));
    const auto& sup = *game_support("wiiparty");
    MCHECK(quick_check({"Mod/files/opening.bnr", "Mod/LÉEME.txt"}, fst, sup, "SUPP01").verdict == QuickVerdict::Compatible);
    MCHECK(quick_check({"codes/SUPP01.gct"}, fst, sup, "SUPP01").verdict == QuickVerdict::Incompatible);
    MCHECK(quick_check({"x/rel/menu.rel.lz"}, fst, sup, "SUPP01").verdict == QuickVerdict::Incompatible);
    MCHECK(quick_check({"riivolution/a.xml", "a/opening.bnr"}, fst, sup, "SUPP01").verdict == QuickVerdict::NeedsDownload);
    MCHECK(quick_check({"riivolution/a.xml", "setup.exe"}, fst, sup, "SUPP01").verdict == QuickVerdict::Incompatible);
    MCHECK(quick_check({"riivolution/a.xml", "Pack/rel/board.rel.lz"}, fst, sup, "SUPP01").verdict == QuickVerdict::Incompatible);
    MCHECK(quick_check({"juego.iso"}, fst, sup, "SUPP01").verdict == QuickVerdict::Incompatible);
    MCHECK(quick_check({"../evil/opening.bnr"}, fst, sup, "SUPP01").verdict == QuickVerdict::Incompatible);
}

void test_session(const fs::path& tmp) {
    fs::path disc = tmp / "s-juego" / "extracted";
    make_disc(disc, kDisc);
    ModStore host("wiiparty", tmp / "s-host", disc), guest("wiiparty", tmp / "s-guest", disc);
    std::string e;
    MCHECK(host.open(e) && guest.open(e));
    write_zip(tmp / "sa.zip", {{"files/opening.bnr", "A"}});
    write_zip(tmp / "sb.zip", {{"files/menu/title_bg.brres.lz", "B"}});
    write_zip(tmp / "sb2.zip", {{"files/menu/title_bg.brres.lz", "B-distinto"}});
    write_zip(tmp / "sx.zip", {{"files/locale/sp_EU/mess.bin", "X"}});
    ModMeta a, b, x;
    a.id = "gamebanana.111"; a.name = "A"; a.version = "1.0"; a.provider = "gamebanana"; a.source_id = "111";
    b.id = "local.b"; b.name = "B"; b.version = "1.0";
    x.id = "local.x"; x.name = "X"; x.version = "1.0";
    InstalledMod ha, hb, ga, gb, gb2, gx;
    MCHECK(host.install(tmp / "sa.zip", a, ha, e) && host.install(tmp / "sb.zip", b, hb, e));
    Profile hp;
    hp.mods = {{ha.id(), ha.key, true, {}}, {hb.id(), hb.key, true, {}}};
    Json mj = build_session_manifest(host, hp, "sala-1", 3);
    SessionManifest m;
    MCHECK(parse_session_manifest(mj.dump(), m, e));
    MCHECK(m.mods.size() == 2 && m.fingerprint == host.compute(hp).fingerprint && m.revision == 3);

    // C04/C05: al invitado le falta todo; A se puede pedir a GameBanana, B (importado a mano) no.
    Profile gp;
    SessionDiff d = diff_session(guest, gp, m);
    MCHECK(d.game_ok && !d.compatible && d.missing == 2 && d.items.size() == 2);
    MCHECK(d.items[0].state == ModDiff::Missing && d.items[0].downloadable);
    MCHECK(d.items[1].state == ModDiff::NoAuthorizedSource && !d.items[1].downloadable);
    MCHECK(!d.session_ready);
    // Con los dos instalados pero desactivados (C08) y uno extra activo (C07): el perfil de sesión lo arregla.
    MCHECK(guest.install(tmp / "sa.zip", a, ga, e) && guest.install(tmp / "sb.zip", b, gb, e) && guest.install(tmp / "sx.zip", x, gx, e));
    gp.mods = {{gx.id(), gx.key, true, {}}, {ga.id(), ga.key, false, {}}, {gb.id(), gb.key, false, {}}};
    d = diff_session(guest, gp, m);
    MCHECK(!d.compatible && d.disabled == 2 && d.extra == 1 && d.session_ready);
    MCHECK(guest.compute(d.session_profile).fingerprint == m.fingerprint);
    // C01: idéntico
    gp.mods = {{ga.id(), ga.key, true, {}}, {gb.id(), gb.key, true, {}}};
    d = diff_session(guest, gp, m);
    MCHECK(d.compatible && d.items[0].state == ModDiff::ExactMatch && d.items[1].state == ModDiff::ExactMatch);
    // C09: orden distinto
    gp.mods = {{gb.id(), gb.key, true, {}}, {ga.id(), ga.key, true, {}}};
    d = diff_session(guest, gp, m);
    MCHECK(!d.compatible && d.items[0].state == ModDiff::LoadOrder && d.session_ready);
    // C10: misma lista con otras opciones
    gp.mods = {{ga.id(), ga.key, true, {{"Pack/Texto", 2}}}, {gb.id(), gb.key, true, {}}};
    d = diff_session(guest, gp, m);
    MCHECK(!d.compatible && d.items[0].state == ModDiff::ConfigMismatch);
    // C06: mismo id y versión, contenido distinto
    ModStore guest2("wiiparty", tmp / "s-guest2", disc);
    MCHECK(guest2.open(e));
    InstalledMod g2a, g2b;
    MCHECK(guest2.install(tmp / "sa.zip", a, g2a, e) && guest2.install(tmp / "sb2.zip", b, g2b, e));
    Profile g2p;
    g2p.mods = {{g2a.id(), g2a.key, true, {}}, {g2b.id(), g2b.key, true, {}}};
    d = diff_session(guest2, g2p, m);
    MCHECK(!d.compatible && d.items[1].state == ModDiff::WrongHash && !d.session_ready);
    // C02: otro disco base
    SessionManifest other = m;
    other.base_content_id = "SUPE01:0000000000000000";
    d = diff_session(guest, gp, other);
    MCHECK(!d.game_ok && d.items.size() == 1 && d.items[0].state == ModDiff::IncompatibleGame);
    // Vanilla con vanilla
    Json vj = build_session_manifest(host, Profile{}, "sala-2", 1);
    SessionManifest vm;
    MCHECK(parse_session_manifest(vj.dump(), vm, e) && vm.fingerprint == kVanilla);
    MCHECK(diff_session(guest, Profile{}, vm).compatible);

    // D06/D07/D13: manifiestos hostiles
    auto hostile = [&](const std::string& field, Json value) {
        Json bad = mj;
        Json mods = Json::array();
        Json first = mj["mods"][0];
        first.set(field, value);
        mods.push(first);
        bad.set("mods", mods);
        SessionManifest out;
        std::string err;
        return parse_session_manifest(bad.dump(), out, err);
    };
    MCHECK(!hostile("id", Json("../../launcher")));
    MCHECK(!hostile("id", Json("A.Mayus")));
    MCHECK(!hostile("effective_content_sha256", Json("no-es-hex")));
    MCHECK(!hostile("load_order", Json(-1)));
    {
        // Una URL o ruta en el manifiesto no se usa para nada: source desconocido → no descargable.
        Json bad = mj;
        Json mods = Json::array();
        Json first = mj["mods"][0];
        first.set("source", "file:///C:/Windows/System32");
        first.set("download_url", "http://192.168.1.10/virus.zip");
        mods.push(first);
        bad.set("mods", mods);
        SessionManifest out;
        MCHECK(parse_session_manifest(bad.dump(), out, e) && out.mods[0].source == "desconocido");
        SessionDiff dd = diff_session(guest2, Profile{}, out);
        MCHECK(!dd.items[0].downloadable);
    }
    std::string huge = "{\"protocol\":\"launcher-lobby-mods\",\"x\":\"" + std::string(70000, 'a') + "\"}";
    SessionManifest hm;
    MCHECK(!parse_session_manifest(huge, hm, e));
    MCHECK(!parse_session_manifest("[[[[[[[[[[[[[[[[[[[[]]]]]]]]]]]]]]]]]]]]", hm, e));
    Json wrong = mj;
    wrong.set("protocol_version", 99);
    MCHECK(!parse_session_manifest(wrong.dump(), hm, e));   // C24: protocolo distinto → no se acepta
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
    test_gamebanana(tmp);
    test_session(tmp);
    if (failures == before) fs::remove_all(tmp, ec);
}
