from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter


ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "assets"
SIZE = 1024


def rounded_line(draw, points, color, width):
    radius = width // 2
    draw.line(points, fill=color, width=width, joint="curve")
    for x, y in points:
        draw.ellipse((x - radius, y - radius, x + radius, y + radius), fill=color)


def build_png():
    image = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))

    shadow = Image.new("RGBA", image.size, (0, 0, 0, 0))
    shadow_draw = ImageDraw.Draw(shadow)
    shadow_draw.rounded_rectangle((62, 72, 962, 972), radius=220, fill=(0, 0, 0, 150))
    shadow = shadow.filter(ImageFilter.GaussianBlur(28))
    image.alpha_composite(shadow)

    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((50, 50, 950, 950), radius=218, fill="#090C12")
    draw.rounded_rectangle(
        (76, 76, 924, 924), radius=194, outline="#232A37", width=5
    )

    draw.polygon(
        [(76, 610), (406, 280), (924, 280), (924, 924), (76, 924)],
        fill="#0E131C",
    )
    draw.polygon(
        [(76, 748), (530, 294), (924, 294), (924, 438), (438, 924), (76, 924)],
        fill="#121925",
    )

    glow = Image.new("RGBA", image.size, (0, 0, 0, 0))
    glow_draw = ImageDraw.Draw(glow)
    rounded_line(glow_draw, [(270, 720), (270, 338), (512, 580)], "#00E5FF", 142)
    rounded_line(glow_draw, [(512, 580), (754, 338), (754, 720)], "#FF2D9A", 142)
    glow = glow.filter(ImageFilter.GaussianBlur(34))
    glow.putalpha(glow.getchannel("A").point(lambda value: value * 90 // 255))
    image.alpha_composite(glow)

    draw = ImageDraw.Draw(image)
    rounded_line(draw, [(270, 720), (270, 338), (512, 580)], "#00DFF2", 104)
    rounded_line(draw, [(512, 580), (754, 338), (754, 720)], "#F43A9A", 104)

    draw.rounded_rectangle((188, 252, 352, 340), radius=30, fill="#DDFBFF")
    draw.rounded_rectangle((672, 252, 836, 340), radius=30, fill="#FFE2F1")
    draw.rounded_rectangle((212, 276, 328, 316), radius=14, fill="#00DFF2")
    draw.rounded_rectangle((696, 276, 812, 316), radius=14, fill="#F43A9A")

    draw.polygon([(512, 512), (580, 580), (512, 648), (444, 580)], fill="#D7FF45")
    draw.polygon([(512, 542), (550, 580), (512, 618), (474, 580)], fill="#0A0D12")

    return image


def write_svg():
    svg = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1024 1024">
  <rect x="50" y="50" width="900" height="900" rx="218" fill="#090c12"/>
  <rect x="76" y="76" width="848" height="848" rx="194" fill="none" stroke="#232a37" stroke-width="5"/>
  <path d="M76 610 406 280h518v644H76Z" fill="#0e131c"/>
  <path d="M76 748 530 294h394v144L438 924H76Z" fill="#121925"/>
  <path d="M270 720V338l242 242" fill="none" stroke="#00dff2" stroke-width="104" stroke-linecap="round" stroke-linejoin="round"/>
  <path d="m512 580 242-242v382" fill="none" stroke="#f43a9a" stroke-width="104" stroke-linecap="round" stroke-linejoin="round"/>
  <rect x="188" y="252" width="164" height="88" rx="30" fill="#ddfbff"/>
  <rect x="672" y="252" width="164" height="88" rx="30" fill="#ffe2f1"/>
  <rect x="212" y="276" width="116" height="40" rx="14" fill="#00dff2"/>
  <rect x="696" y="276" width="116" height="40" rx="14" fill="#f43a9a"/>
  <path d="m512 512 68 68-68 68-68-68Z" fill="#d7ff45"/>
  <path d="m512 542 38 38-38 38-38-38Z" fill="#0a0d12"/>
</svg>
"""
    (ASSETS / "mux-icon.svg").write_text(svg, encoding="utf-8")


def main():
    ASSETS.mkdir(parents=True, exist_ok=True)
    image = build_png()
    image.save(ASSETS / "mux-icon.png", optimize=True)
    image.save(
        ASSETS / "mux.ico",
        format="ICO",
        sizes=[
            (16, 16),
            (20, 20),
            (24, 24),
            (32, 32),
            (40, 40),
            (48, 48),
            (64, 64),
            (96, 96),
            (128, 128),
            (256, 256),
        ],
    )
    write_svg()


if __name__ == "__main__":
    main()
