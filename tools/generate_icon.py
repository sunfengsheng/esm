from __future__ import annotations

from pathlib import Path
from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "assets" / "icon"
SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)


def sc(v: float, size: int) -> int:
    return round(v * size)


def draw_icon(size: int) -> Image.Image:
    # Render at 4x and downsample for clean antialiasing at Windows icon sizes.
    scale = 4
    n = size * scale
    image = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    shadow = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    sd = ImageDraw.Draw(shadow)

    margin = sc(0.055, n)
    radius = sc(0.225, n)
    sd.rounded_rectangle(
        (margin, margin + sc(0.018, n), n - margin, n - margin + sc(0.018, n)),
        radius=radius,
        fill=(4, 18, 35, 105),
    )
    shadow = shadow.filter(ImageFilter.GaussianBlur(max(1, sc(0.028, n))))
    image.alpha_composite(shadow)

    d = ImageDraw.Draw(image)
    # Navy/teal tile deliberately differs from Everything's orange identity.
    tile = (margin, margin, n - margin, n - margin)
    d.rounded_rectangle(tile, radius=radius, fill=(8, 31, 56, 255))
    d.rounded_rectangle(
        (margin + sc(0.018, n), margin + sc(0.018, n),
         n - margin - sc(0.018, n), n - margin - sc(0.018, n)),
        radius=max(1, radius - sc(0.018, n)),
        outline=(24, 92, 116, 220), width=max(1, sc(0.018, n)),
    )

    # Abstract indexed-file stack in the upper-right background.
    line_w = max(1, sc(0.035, n))
    for y, width, alpha in ((0.285, 0.24, 180), (0.385, 0.19, 145), (0.485, 0.15, 110)):
        x1 = sc(0.64, n)
        x2 = min(n - margin - sc(0.075, n), x1 + sc(width, n))
        yy = sc(y, n)
        d.rounded_rectangle((x1, yy, x2, yy + line_w), radius=line_w // 2,
                            fill=(39, 207, 205, alpha))

    # Magnifier: thick high-contrast ring and a cyan handle.
    cx, cy = sc(0.43, n), sc(0.43, n)
    outer_r = sc(0.245, n)
    ring_w = max(2, sc(0.064, n))
    lens_box = (cx - outer_r, cy - outer_r, cx + outer_r, cy + outer_r)
    d.ellipse(lens_box, fill=(11, 50, 73, 255), outline=(235, 253, 255, 255), width=ring_w)

    # Three compact index rows inside the lens. Simplify at tiny sizes.
    rows = 2 if size <= 20 else 3
    row_y = [0.35, 0.43, 0.51][:rows]
    dot_r = max(1, sc(0.014 if size >= 32 else 0.018, n))
    inner_w = max(1, sc(0.025, n))
    for idx, y in enumerate(row_y):
        yy = sc(y, n)
        xdot = sc(0.33, n)
        d.ellipse((xdot - dot_r, yy - dot_r, xdot + dot_r, yy + dot_r),
                  fill=(47, 223, 218, 255))
        x1 = sc(0.37, n)
        x2 = sc(0.52 - idx * 0.018, n)
        d.rounded_rectangle((x1, yy - inner_w // 2, x2, yy + inner_w // 2),
                            radius=max(1, inner_w // 2), fill=(190, 249, 249, 255))

    # Handle is drawn last so its joint stays crisp.
    handle_w = max(2, sc(0.09, n))
    p1 = (sc(0.585, n), sc(0.585, n))
    p2 = (sc(0.79, n), sc(0.79, n))
    d.line((p1, p2), fill=(230, 253, 255, 255), width=handle_w)
    cyan_w = max(1, round(handle_w * 0.48))
    d.line((sc(0.62, n), sc(0.62, n), sc(0.79, n), sc(0.79, n)),
           fill=(35, 205, 202, 255), width=cyan_w)

    return image.resize((size, size), Image.Resampling.LANCZOS)


def write_svg(path: Path) -> None:
    path.write_text('''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1024 1024">
  <defs>
    <filter id="shadow" x="-20%" y="-20%" width="140%" height="150%">
      <feGaussianBlur stdDeviation="24"/>
    </filter>
  </defs>
  <rect x="72" y="92" width="880" height="880" rx="224" fill="#041223" opacity=".42" filter="url(#shadow)"/>
  <rect x="56" y="56" width="912" height="912" rx="230" fill="#081f38"/>
  <rect x="74" y="74" width="876" height="876" rx="212" fill="none" stroke="#185c74" stroke-width="18"/>
  <g fill="#27cfcd">
    <rect x="655" y="292" width="230" height="36" rx="18" opacity=".72"/>
    <rect x="655" y="394" width="182" height="36" rx="18" opacity=".58"/>
    <rect x="655" y="496" width="144" height="36" rx="18" opacity=".44"/>
  </g>
  <circle cx="440" cy="440" r="250" fill="#0b3249" stroke="#ebfdff" stroke-width="66"/>
  <g fill="#2fdfda">
    <circle cx="338" cy="358" r="15"/><circle cx="338" cy="440" r="15"/><circle cx="338" cy="522" r="15"/>
  </g>
  <g stroke="#bef9f9" stroke-width="26" stroke-linecap="round">
    <path d="M385 358h150"/><path d="M385 440h132"/><path d="M385 522h114"/>
  </g>
  <path d="M598 598 808 808" stroke="#e6fdff" stroke-width="92" stroke-linecap="round"/>
  <path d="M634 634 808 808" stroke="#23cdca" stroke-width="44" stroke-linecap="round"/>
</svg>\n''', encoding="utf-8")


def write_preview(frames: dict[int, Image.Image], path: Path) -> None:
    display_sizes = [max(size, 48) for size in SIZES]
    canvas_width = 28 + sum(display + 24 for display in display_sizes)
    canvas = Image.new("RGBA", (canvas_width, 300), (244, 247, 250, 255))
    d = ImageDraw.Draw(canvas)
    x = 28
    for size in SIZES:
        display = max(size, 48)
        icon = frames[size].resize((display, display), Image.Resampling.NEAREST if size <= 24 else Image.Resampling.LANCZOS)
        y = 45 + (170 - display) // 2
        canvas.alpha_composite(icon, (x, y))
        d.text((x + display // 2, 235), f"{size}", fill=(32, 45, 58, 255), anchor="mm")
        x += display + 24
    canvas.convert("RGB").save(path, quality=95)


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    frames = {size: draw_icon(size) for size in SIZES}
    source = draw_icon(1024)
    source.save(OUT / "everything_sm-icon-source.png")
    frames[256].save(
        OUT / "everything_sm.ico",
        format="ICO",
        sizes=[(s, s) for s in SIZES],
        append_images=[frames[s] for s in SIZES if s != 256],
    )
    for size in (16, 32, 48, 256):
        frames[size].save(OUT / f"everything_sm-{size}.png")
    write_svg(OUT / "everything_sm-icon-source.svg")
    write_preview(frames, OUT / "everything_sm-icon-preview.png")
    print(f"Wrote icon assets to {OUT}")


if __name__ == "__main__":
    main()
