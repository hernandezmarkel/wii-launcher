"""Crea en local un mod de prueba de TEXTURA para Wii Party (prueba A/B de docs/mods/06-pruebas_resultados.md).

Parte del disco extraído DEL PROPIO USUARIO: abre locale/<idioma>/menu/common.arc.lz (LZ11 + archivo U8),
invierte la intensidad de una textura TPL (por defecto copyright.tpl, el «©2010 Nintendo» de la pantalla de
título) y vuelve a comprimir en LZ11 sin referencias (solo literales, ~12 % más grande: así la prueba también
ejercita la recolocación del archivo en la FST). Escribe un ZIP con la estructura files/... que importa el
launcher. El resultado contiene datos del juego: no se sube al repositorio ni se distribuye.

Uso: python -I crear-mod-textura.py <extracted> <salida.zip> [--textura copyright.tpl] [--idioma sp_EU]
"""
import argparse
import io
import os
import struct
import zipfile


def lz11_decode(d):
    if d[0] != 0x11:
        raise ValueError("no es LZ11")
    size = d[1] | (d[2] << 8) | (d[3] << 16)
    p = 4
    if size == 0:
        size = struct.unpack("<I", d[4:8])[0]
        p = 8
    out = bytearray()
    while len(out) < size:
        flags = d[p]
        p += 1
        for bit in range(8):
            if len(out) >= size:
                break
            if flags & (0x80 >> bit):
                ind = d[p] >> 4
                if ind == 0:
                    n = (((d[p] & 0xF) << 4) | (d[p + 1] >> 4)) + 0x11
                    disp = (((d[p + 1] & 0xF) << 8) | d[p + 2]) + 1
                    p += 3
                elif ind == 1:
                    n = (((d[p] & 0xF) << 12) | (d[p + 1] << 4) | (d[p + 2] >> 4)) + 0x111
                    disp = (((d[p + 2] & 0xF) << 8) | d[p + 3]) + 1
                    p += 4
                else:
                    n = ind + 1
                    disp = (((d[p] & 0xF) << 8) | d[p + 1]) + 1
                    p += 2
                for _ in range(n):
                    out.append(out[-disp])
            else:
                out.append(d[p])
                p += 1
    return bytes(out)


def lz11_encode_literal(raw):
    size = len(raw)
    out = bytearray([0x11, size & 0xFF, (size >> 8) & 0xFF, (size >> 16) & 0xFF])
    if size >= 1 << 24:
        out[1:4] = b"\0\0\0"
        out += struct.pack("<I", size)
    for i in range(0, size, 8):
        out.append(0)  # 8 literales
        out += raw[i:i + 8]
    while len(out) % 4:
        out.append(0)
    return bytes(out)


def u8_files(raw):
    if raw[:4] != b"\x55\xaa\x38\x2d":
        raise ValueError("no es un archivo U8")
    root, _, _ = struct.unpack(">III", raw[4:16])
    count = struct.unpack(">I", raw[root + 8:root + 12])[0]
    names = root + count * 12
    stack = [(count, "")]
    for i in range(1, count):
        while stack and i >= stack[-1][0]:
            stack.pop()
        e = root + i * 12
        name_off = struct.unpack(">I", b"\0" + raw[e + 1:e + 4])[0]
        end = raw.index(b"\0", names + name_off)
        name = raw[names + name_off:end].decode()
        a, b = struct.unpack(">II", raw[e + 4:e + 12])
        if raw[e]:
            stack.append((b, name + "/"))
        else:
            yield "".join(s[1] for s in stack) + name, a, b


def invert_tpl(buf, start, length):
    """Invierte la intensidad de las imágenes I4/I8/IA4/IA8 de un TPL; deja el alfa como está."""
    tpl = buf[start:start + length]
    if struct.unpack(">I", tpl[:4])[0] != 0x0020AF30:
        raise ValueError("no es un TPL")
    images, table = struct.unpack(">II", tpl[4:12])
    changed = 0
    for k in range(images):
        img_hdr = struct.unpack(">I", tpl[table + k * 8:table + k * 8 + 4])[0]
        h, w, fmt, data = struct.unpack(">HHII", tpl[img_hdr:img_hdr + 12])
        bpp = {0: 4, 1: 8, 2: 8, 3: 16}.get(fmt)
        if bpp is None:
            raise ValueError("formato TPL %d no admitido por esta prueba" % fmt)
        bw, bh = {0: (8, 8), 1: (8, 4), 2: (8, 4), 3: (4, 4)}[fmt]
        size = ((w + bw - 1) // bw * bw) * ((h + bh - 1) // bh * bh) * bpp // 8
        o = start + data
        for j in range(o, o + size):
            if fmt in (0, 1):      # I4/I8: todo es intensidad
                buf[j] ^= 0xFF
            elif fmt == 2:         # IA4: nibble alto alfa, bajo intensidad
                buf[j] ^= 0x0F
            elif (j - o) % 2 == 1:  # IA8: byte alfa, byte intensidad
                buf[j] ^= 0xFF
        changed += 1
    return changed


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("extracted")
    ap.add_argument("salida")
    ap.add_argument("--textura", default="copyright.tpl")
    ap.add_argument("--idioma", default="sp_EU")
    a = ap.parse_args()
    rel = "locale/%s/menu/common.arc.lz" % a.idioma
    src = os.path.join(a.extracted, "files", *rel.split("/"))
    raw = bytearray(lz11_decode(open(src, "rb").read()))
    found = 0
    for path, off, size in u8_files(raw):
        if path.endswith("/" + a.textura):
            found += invert_tpl(raw, off, size)
    if not found:
        raise SystemExit("no se encuentra %s en %s" % (a.textura, rel))
    packed = lz11_encode_literal(bytes(raw))
    with zipfile.ZipFile(a.salida, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("PruebaTextura/files/" + rel, packed)
        z.writestr("PruebaTextura/LEEME.txt", "Mod de prueba del launcher: invierte %s.\n" % a.textura)
    print("Creado %s: %s invertida, %d -> %d bytes" % (a.salida, a.textura, os.path.getsize(src), len(packed)))


if __name__ == "__main__":
    main()
