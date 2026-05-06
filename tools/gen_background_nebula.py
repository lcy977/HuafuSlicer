#!/usr/bin/env python3
"""Generate static OpenGL background: #121212 void, faint upper-mid nebula, subtle constellation."""
from __future__ import annotations

import numpy as np
from PIL import Image, ImageDraw, ImageFilter


def smoothstep(edge0: float, edge1: float, x: np.ndarray) -> np.ndarray:
    t = np.clip((x - edge0) / (edge1 - edge0 + 1e-9), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def main() -> None:
    w, h = 2560, 1440
    rng = np.random.default_rng(20260428)

    # Strict void color #121212
    base = np.full((h, w, 3), 18, dtype=np.float32)

    xv = np.linspace(0.0, 1.0, w, dtype=np.float32)
    yv = np.linspace(0.0, 1.0, h, dtype=np.float32)
    xv, yv = np.meshgrid(xv, yv)

    # Upper-middle concentration (screen: y=0 at top)
    cx, cy = 0.50, 0.30
    dx = (xv - cx) / 0.40
    dy = (yv - cy) / 0.36
    rad = np.sqrt(dx * dx + dy * dy)
    zone = (1.0 - smoothstep(0.12, 1.05, rad)) * (1.0 - smoothstep(0.62, 0.995, yv))
    zone = np.clip(zone, 0.0, 1.0)

    # Soft cloud / nebula (low-contrast)
    cloud = np.zeros((h, w), dtype=np.float32)
    for i in range(5):
        fx = 2.4 + float(i) * 0.85
        fy = 2.1 + float(i) * 0.7
        phx, phy = float(rng.uniform(0.0, 6.28)), float(rng.uniform(0.0, 6.28))
        cloud += np.sin((xv * 6.2 + yv * 3.8) * fx + phx) * 0.22
        cloud += np.sin((xv * 9.0 - yv * 5.5) * fy + phy) * 0.16
    cloud -= cloud.min()
    cloud /= cloud.max() + 1e-6
    neb = cloud * zone * zone

    # Very faint green-cyan mist (matches prior app accent)
    base[..., 0] += neb * 6.5
    base[..., 1] += neb * 12.0
    base[..., 2] += neb * 9.5
    base = np.clip(base, 0.0, 255.0).astype(np.uint8)

    base_rgba = Image.fromarray(base, mode="RGB").convert("RGBA")
    lines_layer = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    draw = ImageDraw.Draw(lines_layer)

    # Constellation: deterministic star field in the nebula region
    stars_xy = [
        (0.36, 0.16),
        (0.42, 0.20),
        (0.48, 0.17),
        (0.54, 0.21),
        (0.50, 0.24),
        (0.44, 0.28),
        (0.52, 0.30),
        (0.58, 0.26),
        (0.46, 0.14),
        (0.56, 0.18),
        (0.40, 0.23),
    ]
    pts: list[tuple[float, float]] = []
    for ux, uy in stars_xy:
        px = (ux + rng.normal(0.0, 0.004)) * w
        py = (uy + rng.normal(0.0, 0.004)) * h
        pts.append((px, py))

    line_rgba = (130, 148, 138, 32)
    edges = [
        (0, 1),
        (1, 2),
        (2, 3),
        (3, 4),
        (4, 5),
        (5, 6),
        (6, 7),
        (2, 4),
        (1, 10),
        (10, 5),
        (8, 2),
        (8, 9),
        (9, 3),
        (7, 9),
    ]
    for a, b in edges:
        draw.line([pts[a], pts[b]], fill=line_rgba, width=1)

    lines_layer = lines_layer.filter(ImageFilter.GaussianBlur(radius=0.45))

    stars_layer = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    sdraw = ImageDraw.Draw(stars_layer)
    for x, y in pts:
        r = 1.35
        sdraw.ellipse([x - r, y - r, x + r, y + r], fill=(155, 172, 162, 70))

    overlay = Image.alpha_composite(lines_layer, stars_layer)
    out = Image.alpha_composite(base_rgba, overlay).convert("RGB")

    out_path = r"d:\背景图1.png"
    out.save(out_path, format="PNG", compress_level=6)
    print("Wrote", out_path, out.size)


if __name__ == "__main__":
    main()
