from pathlib import Path
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "assets" / "icon"
SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)


def rounded_rectangle(draw, box, radius, fill):
    draw.rounded_rectangle(box, radius=radius, fill=fill)


def draw_icon(size: int) -> Image.Image:
    scale = 4 if size <= 48 else 2
    n = size * scale
    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    p = n / 256.0

    # Indigo content-search tile, deliberately distinct from the filename-search icon.
    rounded_rectangle(d, (10*p, 10*p, 246*p, 246*p), 52*p, (67, 56, 202, 255))
    rounded_rectangle(d, (22*p, 22*p, 234*p, 234*p), 42*p, (82, 70, 222, 255))

    # Document and folded corner.
    d.rounded_rectangle((55*p, 36*p, 174*p, 211*p), radius=16*p,
                        fill=(250, 251, 255, 255))
    d.polygon([(139*p, 36*p), (174*p, 71*p), (139*p, 71*p)],
              fill=(202, 207, 247, 255))
    d.line((139*p, 36*p, 139*p, 71*p, 174*p, 71*p),
           fill=(153, 159, 224, 255), width=max(1, round(3*p)))

    # Text lines with one amber match line.
    line_width = max(1, round(8*p))
    d.line((78*p, 94*p, 148*p, 94*p), fill=(133, 140, 178, 255), width=line_width)
    d.line((78*p, 123*p, 151*p, 123*p), fill=(249, 176, 52, 255), width=line_width)
    d.line((78*p, 152*p, 132*p, 152*p), fill=(133, 140, 178, 255), width=line_width)

    # Cyan magnifier over the lower-right corner.
    cyan = (50, 224, 219, 255)
    navy = (31, 35, 91, 255)
    d.ellipse((132*p, 126*p, 220*p, 214*p), fill=navy, outline=cyan,
              width=max(2, round(14*p)))
    d.ellipse((151*p, 145*p, 201*p, 195*p), fill=(82, 70, 222, 255))
    d.line((201*p, 197*p, 232*p, 228*p), fill=navy, width=max(2, round(25*p)))
    d.line((201*p, 197*p, 232*p, 228*p), fill=cyan, width=max(2, round(14*p)))

    return img.resize((size, size), Image.Resampling.LANCZOS)


def write_svg(path: Path) -> None:
    path.write_text('''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256">
  <rect x="10" y="10" width="236" height="236" rx="52" fill="#4338ca"/>
  <rect x="22" y="22" width="212" height="212" rx="42" fill="#5246de"/>
  <rect x="55" y="36" width="119" height="175" rx="16" fill="#fafbff"/>
  <path d="M139 36v35h35z" fill="#cacfF7"/>
  <path d="M139 36v35h35" fill="none" stroke="#999fe0" stroke-width="3"/>
  <path d="M78 94h70M78 152h54" stroke="#858cb2" stroke-width="8" stroke-linecap="round"/>
  <path d="M78 123h73" stroke="#f9b034" stroke-width="8" stroke-linecap="round"/>
  <circle cx="176" cy="170" r="44" fill="#1f235b" stroke="#32e0db" stroke-width="14"/>
  <circle cx="176" cy="170" r="25" fill="#5246de"/>
  <path d="M201 197l31 31" stroke="#1f235b" stroke-width="25" stroke-linecap="round"/>
  <path d="M201 197l31 31" stroke="#32e0db" stroke-width="14" stroke-linecap="round"/>
</svg>\n''', encoding="utf-8")


def write_preview(frames: dict[int, Image.Image], path: Path) -> None:
    shown = (16, 24, 32, 48, 128, 256)
    widths = [max(48, size) for size in shown]
    canvas = Image.new("RGBA", (40 + sum(w + 28 for w in widths), 340), (244, 246, 252, 255))
    d = ImageDraw.Draw(canvas)
    x = 30
    for size, display in zip(shown, widths):
        icon = frames[size].resize((display, display), Image.Resampling.NEAREST if size <= 24 else Image.Resampling.LANCZOS)
        y = 36 + (230 - display) // 2
        canvas.alpha_composite(icon, (x, y))
        d.text((x + display // 2, 292), str(size), fill=(43, 48, 83, 255), anchor="mm")
        x += display + 28
    canvas.convert("RGB").save(path, quality=95)


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    frames = {size: draw_icon(size) for size in SIZES}
    source = draw_icon(1024)
    source.save(OUT / "content_search-icon-source.png")
    source.save(OUT / "content_search.ico", format="ICO", sizes=[(s, s) for s in SIZES])
    for size in (16, 32, 48, 256):
        frames[size].save(OUT / f"content_search-{size}.png")
    write_svg(OUT / "content_search-icon-source.svg")
    write_preview(frames, OUT / "content_search-icon-preview.png")
    print(f"Wrote content-search icon assets to {OUT}")


if __name__ == "__main__":
    main()
