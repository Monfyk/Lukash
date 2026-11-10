#!/usr/bin/env python3
"""Генерує rsrc_windows_amd64.syso (COFF-об'єкт із ресурсами Windows): іконка + маніфест.
Go-лінкер підхоплює *.syso автоматично, тож exe отримує іконку без сторонніх утиліт."""
import os, struct
from io import BytesIO
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ICON_SRC = os.path.join(HERE, "..", "icon", "icon-512.png")
SIZES = [16, 24, 32, 48, 64, 128, 256]
MANIFEST = b"""<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <assemblyIdentity type="win32" name="Akordnyk" version="1.0.0.0" processorArchitecture="*"/>
  <dependency><dependentAssembly>
    <assemblyIdentity type="win32" name="Microsoft.Windows.Common-Controls" version="6.0.0.0" processorArchitecture="*" publicKeyToken="6595b64144ccf1df" language="*"/>
  </dependentAssembly></dependency>
  <application xmlns="urn:schemas-microsoft-com:asm.v3"><windowsSettings>
    <dpiAware xmlns="http://schemas.microsoft.com/SMI/2005/WindowsSettings">true/pm</dpiAware>
    <dpiAwareness xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">PerMonitorV2,PerMonitor</dpiAwareness>
  </windowsSettings></application>
  <compatibility xmlns="urn:schemas-microsoft-com:compatibility.v1"><application>
    <supportedOS Id="{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}"/>
  </application></compatibility>
</assembly>
"""
RT_ICON, RT_GROUP_ICON, RT_MANIFEST = 3, 14, 24
LANG = 0x0409

def png_bytes(img, size):
    b = BytesIO(); img.resize((size, size), Image.LANCZOS).save(b, "PNG"); return b.getvalue()

def build():
    src = Image.open(ICON_SRC).convert("RGBA")
    icons = [(i + 1, s, png_bytes(src, s)) for i, s in enumerate(SIZES)]
    grp = struct.pack("<HHH", 0, 1, len(icons))
    for iid, s, data in icons:
        d = 0 if s >= 256 else s
        grp += struct.pack("<BBBBHHIH", d, d, 0, 0, 1, 32, len(data), iid)
    # (тип, [(id, дані)])
    tree = [(RT_ICON, [(iid, data) for iid, _, data in icons]),
            (RT_GROUP_ICON, [(1, grp)]),
            (RT_MANIFEST, [(1, MANIFEST)])]

    # --- розкладка директорій ---
    def dir_hdr(n): return struct.pack("<IIHHHH", 0, 0, 0, 0, 0, n)
    size_l1 = 16 + 8 * len(tree)
    l2_off, off = [], size_l1
    for _, items in tree:
        l2_off.append(off); off += 16 + 8 * len(items)
    l3_off = []
    for _, items in tree:
        row = []
        for _ in items:
            row.append(off); off += 16 + 8
        l3_off.append(row)
    de_off = []
    for _, items in tree:
        row = []
        for _ in items:
            row.append(off); off += 16
        de_off.append(row)
    data_off = []
    for _, items in tree:
        row = []
        for _, data in items:
            off = (off + 7) & ~7; row.append(off); off += len(data)
        data_off.append(row)
    total = (off + 7) & ~7

    buf = bytearray(total); relocs = []
    def put(o, b): buf[o:o + len(b)] = b
    put(0, dir_hdr(len(tree)))
    for i, (t, _) in enumerate(tree):
        put(16 + 8 * i, struct.pack("<II", t, 0x80000000 | l2_off[i]))
    for i, (_, items) in enumerate(tree):
        put(l2_off[i], dir_hdr(len(items)))
        for j, (rid, data) in enumerate(items):
            put(l2_off[i] + 16 + 8 * j, struct.pack("<II", rid, 0x80000000 | l3_off[i][j]))
            put(l3_off[i][j], dir_hdr(1))
            put(l3_off[i][j] + 16, struct.pack("<II", LANG, de_off[i][j]))
            put(de_off[i][j], struct.pack("<IIII", data_off[i][j], len(data), 0, 0))
            relocs.append(de_off[i][j])  # поле OffsetToData — RVA, потрібна релокація
            put(data_off[i][j], data)

    # --- COFF ---
    hdr_size, sec_size = 20, 40
    raw_ptr = hdr_size + sec_size
    rel_ptr = raw_ptr + len(buf)
    sym_ptr = rel_ptr + 10 * len(relocs)
    out = struct.pack("<HHIIIHH", 0x8664, 1, 0, sym_ptr, 1, 0, 0)
    out += struct.pack("<8sIIIIIIHHI", b".rsrc", 0, 0, len(buf), raw_ptr, rel_ptr, 0,
                       len(relocs), 0, 0x40000040)
    out += bytes(buf)
    for r in relocs:
        out += struct.pack("<IIH", r, 0, 3)  # IMAGE_REL_AMD64_ADDR32NB відносно символу .rsrc
    out += struct.pack("<8sIhHBB", b".rsrc", 0, 1, 0, 3, 0)  # символ секції
    out += struct.pack("<I", 4)  # порожня таблиця рядків
    dst = os.path.join(HERE, "rsrc_windows_amd64.syso")
    open(dst, "wb").write(out)
    print("ok", dst, len(out), "байт")

if __name__ == "__main__":
    build()
