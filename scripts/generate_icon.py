#!/usr/bin/env python3
"""Generates a Precision Bench Instrument macOS AppIcon.icns for SCOPE."""

import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib


def encode_png(width: int, height: int, rgba: bytes) -> bytes:
    def chunk(tag: bytes, data: bytes) -> bytes:
        crc = zlib.crc32(tag + data) & 0xFFFFFFFF
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", crc)

    raw = bytearray()
    stride = width * 4
    for y in range(height):
        raw.append(0)
        raw.extend(rgba[y * stride : (y + 1) * stride])

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", ihdr)
        + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + chunk(b"IEND", b"")
    )


def render_icon(size: int) -> bytes:
    buf = bytearray(size * size * 4)
    s = float(size)
    margin = s * 0.08
    inner_min = margin
    inner_max = s - margin
    radius = s * 0.18
    cx_min = inner_min + radius
    cx_max = inner_max - radius

    def set_px(x: int, y: int, r: int, g: int, b: int, a: int) -> None:
        if 0 <= x < size and 0 <= y < size:
            idx = (y * size + x) * 4
            buf[idx] = r
            buf[idx + 1] = g
            buf[idx + 2] = b
            buf[idx + 3] = a

    def blend_px(x: int, y: int, r: int, g: int, b: int, alpha: float) -> None:
        if 0 <= x < size and 0 <= y < size and alpha > 0.0:
            alpha = min(1.0, alpha)
            idx = (y * size + x) * 4
            buf[idx] = int(buf[idx] * (1.0 - alpha) + r * alpha)
            buf[idx + 1] = int(buf[idx + 1] * (1.0 - alpha) + g * alpha)
            buf[idx + 2] = int(buf[idx + 2] * (1.0 - alpha) + b * alpha)
            buf[idx + 3] = 255

    # 1) Rounded macOS squircle chassis in Anodized Matte Carbon
    for y in range(size):
        for x in range(size):
            px = x + 0.5
            py = y + 0.5
            qx = max(cx_min - px, 0.0, px - cx_max)
            qy = max(cx_min - py, 0.0, py - cx_max)
            dist = math.hypot(qx, qy) - radius
            if dist <= 0.5:
                cov = max(0.0, min(1.0, 0.5 - dist))
                # Subtle machined vertical gradient
                t = y / s
                base_r = int(20 + (1.0 - t) * 10)
                base_g = int(22 + (1.0 - t) * 10)
                base_b = int(26 + (1.0 - t) * 12)
                # Crisp bezel edge
                if dist > -max(1.5, s * 0.015):
                    base_r, base_g, base_b = 54, 60, 70
                set_px(x, y, base_r, base_g, base_b, int(cov * 255))

    # 2) Recessed oscilloscope screen well
    well_x0 = int(s * 0.17)
    well_x1 = int(s * 0.83)
    well_y0 = int(s * 0.22)
    well_y1 = int(s * 0.78)
    for y in range(well_y0, well_y1):
        for x in range(well_x0, well_x1):
            set_px(x, y, 11, 12, 14, 255)
            if x == well_x0 or x == well_x1 - 1 or y == well_y0 or y == well_y1 - 1:
                set_px(x, y, 48, 54, 64, 255)

    # 3) Subtle Tektronix graticule grid (4x4 divisions)
    well_w = well_x1 - well_x0
    well_h = well_y1 - well_y0
    for i in range(1, 4):
        gx = well_x0 + int(well_w * i / 4.0)
        gy = well_y0 + int(well_h * i / 4.0)
        for y in range(well_y0 + 1, well_y1 - 1):
            blend_px(gx, y, 52, 58, 70, 0.55)
        for x in range(well_x0 + 1, well_x1 - 1):
            blend_px(x, gy, 52, 58, 70, 0.55)

    # 4) Signal-Orange top LED strip + Phosphor-Green live dot
    led_h = max(2, int(s * 0.025))
    for y in range(well_y0, well_y0 + led_h):
        for x in range(well_x0, well_x1):
            set_px(x, y, 255, 87, 34, 255)

    # 5) Crisp Signal-Orange oscilloscope pulse trace
    trace_r = max(1.6, s * 0.024)
    glow_r = trace_r * 2.6
    mid_y = (well_y0 + well_y1) * 0.5
    amp = well_h * 0.30

    def wave_y(u: float) -> float:
        # Sharp precision instrument damped pulse
        env = math.exp(-((u - 0.48) ** 2) / 0.045)
        return mid_y - amp * env * math.sin((u - 0.15) * math.pi * 4.2)

    pad_x = int(well_w * 0.06)
    samples = []
    for x in range(well_x0 + pad_x, well_x1 - pad_x):
        u = (x - (well_x0 + pad_x)) / max(1.0, float(well_w - 2 * pad_x))
        samples.append((float(x), wave_y(u)))

    for i in range(1, len(samples)):
        x0, y0 = samples[i - 1]
        x1, y1 = samples[i]
        min_x = max(well_x0 + 1, int(min(x0, x1) - glow_r - 1))
        max_x = min(well_x1 - 2, int(max(x0, x1) + glow_r + 1))
        min_y = max(well_y0 + 1, int(min(y0, y1) - glow_r - 1))
        max_y = min(well_y1 - 2, int(max(y0, y1) + glow_r + 1))
        dx, dy = x1 - x0, y1 - y0
        seg_len2 = dx * dx + dy * dy + 1e-9
        for y in range(min_y, max_y + 1):
            for x in range(min_x, max_x + 1):
                t = max(0.0, min(1.0, ((x - x0) * dx + (y - y0) * dy) / seg_len2))
                d = math.hypot(x - (x0 + t * dx), y - (y0 + t * dy))
                if d < trace_r:
                    blend_px(x, y, 255, 110, 56, 1.0)
                elif d < glow_r:
                    a = (1.0 - (d - trace_r) / (glow_r - trace_r)) * 0.35
                    blend_px(x, y, 255, 87, 34, a)

    # 6) Phosphor-green status LED in bottom-right bezel
    led_cx = int(s * 0.78)
    led_cy = int(s * 0.84)
    led_rad = max(2.0, s * 0.028)
    for y in range(int(led_cy - led_rad * 2), int(led_cy + led_rad * 2) + 1):
        for x in range(int(led_cx - led_rad * 2), int(led_cx + led_rad * 2) + 1):
            d = math.hypot(x - led_cx, y - led_cy)
            if d <= led_rad:
                blend_px(x, y, 52, 211, 153, 1.0)
            elif d <= led_rad * 2.0:
                blend_px(x, y, 52, 211, 153, (1.0 - (d - led_rad) / led_rad) * 0.3)

    return bytes(buf)


def upscale_2x(width: int, height: int, rgba: bytes) -> bytes:
    out = bytearray((width * 2) * (height * 2) * 4)
    out_stride = width * 2 * 4
    in_stride = width * 4
    for y in range(height):
        row = rgba[y * in_stride : (y + 1) * in_stride]
        dst0 = (y * 2) * out_stride
        dst1 = (y * 2 + 1) * out_stride
        for x in range(width):
            px = row[x * 4 : x * 4 + 4]
            o = x * 8
            out[dst0 + o : dst0 + o + 4] = px
            out[dst0 + o + 4 : dst0 + o + 8] = px
            out[dst1 + o : dst1 + o + 4] = px
            out[dst1 + o + 4 : dst1 + o + 8] = px
    return bytes(out)


def main() -> int:
    if len(sys.argv) != 2:
        print("Usage: generate_icon.py <output.icns>", file=sys.stderr)
        return 1

    out_icns = os.path.abspath(sys.argv[1])
    os.makedirs(os.path.dirname(out_icns), exist_ok=True)

    # Standard Apple ICNS PNG icon chunks (modern macOS 10.7+ / Apple Silicon)
    chunks = [
        (b"icp4", 16),
        (b"icp5", 32),
        (b"icp6", 64),
        (b"ic07", 128),
        (b"ic08", 256),
        (b"ic09", 512),
        (b"ic10", 1024),
    ]
    body = bytearray()
    rendered_512 = None
    for ostype, sz in chunks:
        if sz == 1024 and rendered_512 is not None:
            rgba = upscale_2x(512, 512, rendered_512)
        else:
            rgba = render_icon(sz)
            if sz == 512:
                rendered_512 = rgba
        png = encode_png(sz, sz, rgba)
        body.extend(ostype)
        body.extend(struct.pack(">I", 8 + len(png)))
        body.extend(png)

    with open(out_icns, "wb") as f:
        f.write(b"icns")
        f.write(struct.pack(">I", 8 + len(body)))
        f.write(body)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
