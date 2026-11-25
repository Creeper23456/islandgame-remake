#!/usr/bin/env python3
"""Replay the pre-composite RGB565 and signed 32-bit depth layers of a guest trace.

Kind 1 is FUN_00076ad4 (affine); kinds 0 and 6 are vtable slots
FUN_00083fa8 and FUN_00085bb0 (perspective). Kinds 4/5 are unimplemented.
Input layouts are defined by pc_trace.h. Triangle order, including depth ties,
is preserved separately for each guest colour/depth pointer pair.
"""

import argparse
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
import struct

import numpy as np


KINDS = (0, 1, 6)
COLOUR_KEY = 0xF81F
Q28 = 1 << 28
# Renderer + 0x44 table initialized by FUN_0006f720; idx = camera-space depth.
RECIP = np.array([1] + [Q28 // i for i in range(1, 70000)], dtype=np.int64)


def s32(x):
    """The ARM ALU keeps only the low word. Works on Python ints and int64 arrays."""
    return ((x + (1 << 31)) & 0xFFFFFFFF) - (1 << 31)


def trunc_div(n, d):
    return (-1 if n < 0 else 1) * (abs(n) // d)


def round_shift(n, bits):
    return (n + (1 << (bits - 1))) >> bits


@dataclass
class Texture:
    width: int
    height: int
    palette: np.ndarray
    texels: np.ndarray

    @classmethod
    def load(cls, path):
        raw = path.read_bytes()
        w, h = struct.unpack_from("<2I", raw)
        if not w or not h or w & (w - 1) or h & (h - 1) or len(raw) != 520 + w * h:
            raise ValueError(f"invalid texture: {path}")
        return cls(w, h, np.frombuffer(raw, "<u2", 256, 8), np.frombuffer(raw, "u1", offset=520))


@dataclass
class Target:
    colour: np.ndarray
    depth: np.ndarray
    owner: np.ndarray
    alpha: np.ndarray

    @classmethod
    def cleared(cls, width, height):
        shape = (height, width)
        return cls(np.zeros(shape, "<u2"), np.zeros(shape, "<i4"),
                   np.full(shape, -1, "i1"), np.zeros(shape, "u1"))


@dataclass
class Edge:
    y: int
    base: tuple
    step: tuple

    def at(self, y):
        return tuple(s32(base + (y - self.y) * step) for base, step in zip(self.base, self.step))


def vertex(words, affine):
    x, y, idx, u, v, _w5 = words  # w5 is not read by any of these kernels.
    if not 0 <= idx < len(RECIP):
        raise ValueError(f"depth index outside guest reciprocal table: {idx}")
    z = int(RECIP[idx])
    scale = (1 << 16) if affine else z >> 6
    return y, (s32(x << 16), s32(u * scale), s32(v * scale), z)


def edge(a, b, affine):
    y, base = a
    dy = b[0] - y
    d = tuple(s32(hi - lo) for lo, hi in zip(base, b[1]))
    k = Q28 // dy
    if affine:
        step = tuple(s32((n >> 16) * (k >> 12)) for n in d[:3]) + (trunc_div(d[3], dy),)
    else:
        step = tuple(s32(round_shift(n * k, 28)) for n in d)
    return Edge(y, base, step)


def set_edge_z(e, z, dz):
    return Edge(e.y, (*e.base[:3], z), (*e.step[:3], dz))


def scan_edges(top, mid, bot, affine):
    """Yield the row ranges and guest-designated left and right edges."""
    ty, my, by = top[0], mid[0], bot[0]
    long = edge(top, bot, affine)
    if ty == my:
        right = edge(mid, bot, affine)
        if affine:
            # FUN_00076ad4 flat-top path: left depth stays at the left top;
            # right starts at bottom depth and steps back toward the left top.
            long = set_edge_z(long, top[1][3], 0)
            right = set_edge_z(right, bot[1][3], trunc_div(top[1][3] - bot[1][3], by - ty))
        yield ty, by, long, right
    elif my == by:
        yield ty, by, edge(top, mid, affine), long
    else:
        lower = edge(mid, bot, affine)
        if my < 0:
            # Both upper edges already clipped away; compare lower-edge slopes.
            left, right = (lower, long) if lower.step[0] >= long.step[0] else (long, lower)
            yield 0, by, left, right
        else:
            upper = edge(top, mid, affine)
            short_left = upper.step[0] <= long.step[0]
            left, right = (upper, long) if short_left else (long, upper)
            # The guest switches AFTER emitting the middle row, not before it.
            yield ty, my + 1, left, right
            left, right = (lower, long) if short_left else (long, lower)
            yield my + 1, by, left, right


def draw_triangle(target, kind, words, tex, alpha):
    height, width = target.colour.shape
    (x0, y0, *_), (x1, y1, *_), (x2, y2, *_) = words
    area = s32((x0 - x2) * (y1 - y2) - (x1 - x2) * (y0 - y2))
    if area <= 0 or min(y0, y1, y2) >= height or max(y0, y1, y2) < 0:
        return
    if min(x0, x1, x2) > width or max(x0, x1, x2) < 0:
        return
    affine = kind == 1
    top, mid, bot = (vertex(v, affine) for v in sorted(words, key=lambda v: (v[1], v[0])))
    for start, end, left, right in scan_edges(top, mid, bot, affine):
        for y in range(max(0, start), min(height, end)):
            lx, lu, lv, lz = left.at(y)
            rx, ru, rv, rz = right.at(y)
            x0, x1 = s32(lx + 0x8000) >> 16, s32(rx + 0x8000) >> 16
            lo, hi = max(0, x0), min(width, x1)
            if lo >= hi:
                continue
            k = Q28 // (x1 - x0)  # UNCLIPPED rounded span width
            diff = tuple(s32(r - l) for l, r in zip((lu, lv, lz), (ru, rv, rz)))
            if affine:
                du, dv, dz = (s32((n >> 6) * (k >> 22)) for n in diff)
            else:
                du, dv, dz = (s32(round_shift(n * k, 28)) for n in diff)
            x = np.arange(lo, hi, dtype=np.int64)
            z = s32(lz + dz * (x - x0))
            passed = target.depth[y, lo:hi] < z
            if not passed.any():
                continue
            x, z = x[passed], z[passed]
            u, v = s32(lu + du * (x - x0)), s32(lv + dv * (x - x0))
            if affine:
                u, v = s32(u + 0x8000) >> 16, s32(v + 0x8000) >> 16
                # One mask after the linear address sum; U can carry into V.
                address = (u + v * tex.width) & (tex.width * tex.height - 1)
            else:
                k = RECIP[z >> 9]
                u, v = round_shift(u * k, 31), round_shift(v * k, 31)
                address = (u & (tex.width - 1)) + (v & (tex.height - 1)) * tex.width
            rgb = tex.palette[tex.texels[address]]
            opaque = rgb != COLOUR_KEY
            x = x[opaque]
            target.colour[y, x] = rgb[opaque]
            target.depth[y, x] = z[opaque]
            target.owner[y, x] = kind
            if kind == 6:
                target.alpha[y, x] = alpha & 0xFF


def rasterize(directory, records, width, height):
    """Return raw raster targets keyed by (guest colour pointer, guest depth pointer)."""
    textures, targets = {}, {}
    for kind, texarg, colour, depth, alpha, _, _, *words in records.tolist():
        if kind not in KINDS:
            raise ValueError(f"unimplemented raster kind {kind} (kinds 4/5 use other kernels)")
        tex_id = words[18] or texarg
        if tex_id not in textures:
            textures[tex_id] = Texture.load(directory / f"tex-{tex_id:08x}.bin")
        key = colour, depth
        if key not in targets:
            targets[key] = Target.cleared(width, height)
        draw_triangle(targets[key], kind, [list(map(s32, words[i:i + 6]))
                                          for i in (0, 6, 12)], textures[tex_id], alpha)
    return targets


def read_buffer(path, plane):
    raw = path.read_bytes()
    w, h = struct.unpack_from("<2I", raw)
    offset, dtype = 8, {"colour": "<u2", "depth": "<i4", "alpha": "u1"}[plane]
    if plane == "depth":
        if struct.unpack_from("<I", raw, 8)[0] != 4:
            raise ValueError(f"expected 4-byte depth entries in {path}")
        offset = 12
    return np.frombuffer(raw, dtype, offset=offset).reshape(h, w)


def projection_batches(directory, count):
    """When available, proj.bin identifies consecutive clears within one trace frame."""
    path = directory / "proj.bin"
    if not path.exists():
        return None, [slice(0, count)]
    data = path.read_bytes()
    at, start, batches = 0, 0, []
    cx = None
    while at < len(data):
        n, _, cx, _, *_ = struct.unpack_from("<8I", data, at)
        batches.append(slice(start, start + n))
        start += n
        at += 32 + 23 * 4 * n
    if at != len(data) or start != count:
        # Projection can include culled records not submitted to a rasterizer.
        batches = [slice(0, count)]
    return cx, batches


def compare(target, kinds, **references):
    """Attribute mismatches to the replay's last writer, or to the clear."""
    mismatches = {}
    for plane, expected in references.items():
        actual = getattr(target, plane)
        if actual.shape != expected.shape:
            raise ValueError(f"{plane} viewport {expected.shape} does not match {actual.shape}")
        mismatches[plane] = actual != expected
    any_mismatch = np.logical_or.reduce(list(mismatches.values()))

    def counts(mask):
        planes = ", ".join(f"{name}={np.count_nonzero(diff & mask)}" for name, diff in mismatches.items())
        return f"{planes}, any={np.count_nonzero(any_mismatch & mask)}"

    for kind in (*kinds, -1):
        owned = target.owner == kind
        name = f"kind {kind}" if kind >= 0 else "background/unattributed"
        print(f"  {name}: {counts(owned)} (of {np.count_nonzero(owned)})")
    print(f"  all: {counts(True)}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tracedir", type=Path)
    parser.add_argument("--size", nargs=2, type=int, metavar=("WIDTH", "HEIGHT"),
                        help="viewport size for traces without reference buffers (default 2*cx by 208)")
    parser.add_argument("--batch", type=int, default=-1, help="projection batch to compare (default: last)")
    args = parser.parse_args()
    directory = args.tracedir
    records = np.fromfile(directory / "tris.bin", "<u4").reshape(-1, 32)
    counts = Counter(map(int, records[:, 0]))
    print("Triangles:", ", ".join(f"kind {k}={n}" for k, n in sorted(counts.items())))
    cx, batches = projection_batches(directory, len(records))
    colour_file, depth_file = directory / "color.bin", directory / "depth.bin"
    captured = colour_file.is_file() and depth_file.is_file()
    if captured:
        colour = read_buffer(colour_file, "colour")
        depth = read_buffer(depth_file, "depth")
        height, width = colour.shape
    else:
        width, height = args.size or (2 * cx if cx else 176, 208)
    batch = records[batches[args.batch]]
    targets = rasterize(directory, batch, width, height)
    print(f"Viewport: {width}x{height}; batch {args.batch % len(batches) + 1}/{len(batches)}")
    for (cp, dp), target in targets.items():
        kinds = sorted({int(r[0]) for r in batch if (r[2], r[3]) == (cp, dp)})
        cover = ", ".join(f"kind {k}={np.count_nonzero(target.owner == k)} visible" for k in kinds)
        print(f"Target {cp:08x}/{dp:08x}: {cover}")
        if kinds != [6] and captured:
            compare(target, kinds, colour=colour, depth=depth)
        elif kinds == [6]:
            references = {}
            for plane, filename in (("colour", "kind6-color.bin"), ("depth", "kind6-depth.bin"),
                                    ("alpha", "kind6-alpha.bin")):
                path = directory / filename
                if path.is_file():
                    references[plane] = read_buffer(path, plane)
            if references:
                compare(target, kinds, **references)
            else:
                print("  No raw kind-6 reference planes; replay only.")
        else:
            print("  No color.bin/depth.bin pair; replay only.")


if __name__ == "__main__":
    main()
