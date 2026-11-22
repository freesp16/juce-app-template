#!/usr/bin/env python3
"""Generate the app-info ELF note: name, version, id and the app icon, for launchers on the unit.

  tools/gen-app-info.py ICON.png OUT.cpp NAME VERSION ID [--size 128]

Called by src/app/CMakeLists.txt with the values of cmake/ProjectIdentity.cmake. The output defines one object in
the section .note.appinfo, which the assembler marks SHT_NOTE and the linker maps into a PT_NOTE segment, so a
launcher finds it through the program headers alone (strip keeps it; the boot menu in toraizsp16/bootmenu reads it
to show the app's name, version and icon without running it).

Note layout (little-endian, 4-byte aligned; the standard ELF note header, then the "descriptor"):

  namesz u32 = 8, descsz u32, type u32 = 1, name "APPINFO\\0"
  desc +0    "AINF"          magic
       +4    u16 = 1         format version
       +6    u16 = 128       header bytes (the pixels follow)
       +8    char[48]        the name a launcher shows, UTF-8, NUL-padded
       +56   char[24]        version
       +80   char[40]        reverse-DNS id
       +120  u16 w, u16 h    icon size (a power of two, so a GLES2 driver can build mipmaps)
       +124  u32 = 1         pixel format: RGBA8, straight alpha, rows top to bottom
       +128  w*h*4 bytes     pixels

The PNG decoder is plain Python + zlib (8-bit RGB/RGBA, not interlaced), so the build needs no imaging library.
The icon is shrunk by an integer factor with an alpha-weighted box filter.
"""
import argparse
import struct
import sys
import zlib
from pathlib import Path


def read_png(path):
    data = Path(path).read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        sys.exit(f"{path}: not a PNG file")
    pos, idat, w = 8, bytearray(), 0
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        pos += 12 + n
        if kind == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8 or ctype not in (2, 6) or interlace:
                sys.exit(f"{path}: need an 8-bit RGB or RGBA PNG, not interlaced")
            bpp = 4 if ctype == 6 else 3
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
    raw = zlib.decompress(bytes(idat))
    stride = w * bpp
    out = bytearray(w * h * 4)
    prev = bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if f == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 255
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif f == 3:
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 255
        elif f == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        elif f != 0:
            sys.exit(f"{path}: bad PNG filter {f}")
        for x in range(w):
            s = x * bpp
            out[(y * w + x) * 4:(y * w + x) * 4 + 4] = line[s:s + 3] + (bytes([line[s + 3]]) if bpp == 4 else b"\xff")
        prev = line
    return w, h, out


def shrink(w, h, px, size):
    if w != h or w % size:
        sys.exit(f"the icon must be square with a side that is a multiple of {size} (it is {w} x {h})")
    k = w // size
    out = bytearray(size * size * 4)
    for oy in range(size):
        for ox in range(size):
            r = g = b = a = 0
            for y in range(oy * k, oy * k + k):
                for x in range(ox * k, ox * k + k):
                    i = (y * w + x) * 4
                    al = px[i + 3]
                    r += px[i] * al
                    g += px[i + 1] * al
                    b += px[i + 2] * al
                    a += al
            o = (oy * size + ox) * 4
            if a:
                out[o:o + 4] = bytes((round(r / a), round(g / a), round(b / a), round(a / (k * k))))
    return out


def c_string(value, n, what):
    raw = value.encode("utf-8")
    if len(raw) >= n:
        sys.exit(f"{what} '{value}' is longer than {n - 1} bytes")
    return "{" + ",".join(str(c) for c in raw.ljust(n, b"\0")) + "}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("icon")
    ap.add_argument("out")
    ap.add_argument("name")
    ap.add_argument("version")
    ap.add_argument("id")
    ap.add_argument("--size", type=int, default=128)
    a = ap.parse_args()
    if a.size & (a.size - 1) or not 16 <= a.size <= 256:
        sys.exit("--size must be a power of two between 16 and 256")
    w, h, px = read_png(a.icon)
    icon = shrink(w, h, px, a.size)
    rows = ",\n".join(",".join(str(v) for v in icon[i:i + 64]) for i in range(0, len(icon), 64))
    text = f"""// Generated by tools/gen-app-info.py from {Path(a.icon).name} and cmake/ProjectIdentity.cmake. Do not edit.
// The app-info ELF note (format: see the script): launchers read the name, version and icon from the binary.
#include <cstdint>

struct AppInfoNote
{{
    std::uint32_t namesz, descsz, type;
    char owner[8];
    char magic[4];
    std::uint16_t format, headerBytes;
    char name[48], version[24], id[40];
    std::uint16_t iconW, iconH;
    std::uint32_t pixelFormat;
    std::uint8_t pixels[{a.size} * {a.size} * 4];
}};
static_assert(sizeof(AppInfoNote) == 20 + 128 + {a.size} * {a.size} * 4, "the note layout has no padding");

extern const AppInfoNote appInfoNote;
__attribute__((section(".note.appinfo"), aligned(4), used)) const AppInfoNote appInfoNote = {{
    8, sizeof(AppInfoNote) - 20, 1, {{'A', 'P', 'P', 'I', 'N', 'F', 'O', 0}},
    {{'A', 'I', 'N', 'F'}}, 1, 128,
    {c_string(a.name, 48, "name")},
    {c_string(a.version, 24, "version")},
    {c_string(a.id, 40, "id")},
    {a.size}, {a.size}, 1,
    {{
{rows}
    }}}};
"""
    out = Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    if not out.exists() or out.read_text() != text:
        out.write_text(text)
    print(f"app-info: {a.name} {a.version} ({a.id}), icon {a.size} x {a.size} from {Path(a.icon).name}")


if __name__ == "__main__":
    main()
