"""Generate assets/textures/atlas.png — the 16x16-tile (256x256) block atlas.

Engine-made placeholder art (no external assets). Tiles:
  0 debug magenta/black checker | 1 stone | 2 dirt | 3 grass top | 4 grass side
  5 lamp (emissive glow — warm yellow)
Deterministic hash noise so the output is reproducible.
"""
from pathlib import Path
import struct, zlib

SIZE, TILE = 256, 16


def noise(x, y, seed):
    h = (x * 374761393 + y * 668265263 + seed * 2246822519) & 0xFFFFFFFF
    h = (h ^ (h >> 13)) * 1274126177 & 0xFFFFFFFF
    return ((h ^ (h >> 16)) & 0xFF) / 255.0


def shade(base, n, amount):
    return tuple(max(0, min(255, int(c * (1.0 - amount + 2 * amount * n)))) for c in base)


def tile_pixel(tile, px, py):
    if tile == 1:  # stone
        return shade((128, 128, 132), noise(px, py, 1), 0.18)
    if tile == 2:  # dirt
        return shade((115, 82, 58), noise(px, py, 2), 0.22)
    if tile == 3:  # grass top
        return shade((88, 140, 62), noise(px, py, 3), 0.20)
    if tile == 4:  # grass side: dirt with a grass lip
        if py < 3 + int(noise(px, 0, 4) * 2):
            return shade((88, 140, 62), noise(px, py, 3), 0.20)
        return shade((115, 82, 58), noise(px, py, 2), 0.22)
    if tile == 5:  # lamp: bright warm core with a darker frame, reads as a light
        edge = px == 0 or py == 0 or px == TILE - 1 or py == TILE - 1
        if edge:
            return (120, 78, 20)
        return shade((255, 224, 130), noise(px, py, 5), 0.12)
    return (255, 0, 255) if (px // 4 + py // 4) % 2 == 0 else (16, 16, 16)  # debug


def build_rows():
    rows = []
    for y in range(SIZE):
        row = bytearray([0])  # filter: none
        for x in range(SIZE):
            tile = (y // TILE) * 16 + (x // TILE)
            # PNG row 0 is top; engine flips on load, so tile 0 sits at uv-top-left.
            r, g, b = tile_pixel(tile, x % TILE, y % TILE)
            row += bytes((r, g, b, 255))
        rows.append(bytes(row))
    return b"".join(rows)


def chunk(tag, data):
    return struct.pack(">I", len(data)) + tag + data + struct.pack(
        ">I", zlib.crc32(tag + data) & 0xFFFFFFFF)


out = Path(__file__).resolve().parent.parent / "assets" / "textures" / "atlas.png"
out.parent.mkdir(parents=True, exist_ok=True)
ihdr = struct.pack(">IIBBBBB", SIZE, SIZE, 8, 6, 0, 0, 0)
png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
       chunk(b"IDAT", zlib.compress(build_rows(), 9)) + chunk(b"IEND", b""))
out.write_bytes(png)
print(f"wrote {out} ({len(png)} bytes)")
