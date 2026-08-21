# Generates src\toolbox.ico from vector-style shapes.
# Design language mirrors the launcher canvas: near-black rounded square,
# three tool tiles in the ICC/MUX/IYX accent colors plus one empty slot.
# Run from repository root:  python src\toolbox_icon.py
from PIL import Image, ImageDraw

BG = (0x10, 0x11, 0x0F, 0xFF)
ICC = (0xFF, 0x65, 0x4F, 0xFF)
MUX = (0x28, 0x64, 0xFF, 0xFF)
IYX = (0xC8, 0xF4, 0x3D, 0xFF)
SLOT = (0xFF, 0xFF, 0xFF, 0x2E)

# Draw oversized, downsample with Lanczos for crisp small sizes.
SS = 4096


def render(size: int) -> Image.Image:
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    u = size / 100.0

    inset, bg_radius = 4.0 * u, 21.0 * u
    d.rounded_rectangle([inset, inset, size - inset, size - inset],
                        radius=bg_radius, fill=BG)

    pad, gap = 19.0 * u, 8.0 * u
    tile = (size - 2.0 * pad - gap) / 2.0
    tile_radius = tile * 0.30
    xs = (pad, pad + tile + gap)
    ys = (pad, pad + tile + gap)

    for x, y, color in ((xs[0], ys[0], ICC), (xs[1], ys[0], MUX),
                        (xs[0], ys[1], IYX)):
        d.rounded_rectangle([x, y, x + tile, y + tile],
                            radius=tile_radius, fill=color)

    stroke = max(2, round(4.2 * u))
    d.rounded_rectangle([xs[1], ys[1], xs[1] + tile, ys[1] + tile],
                        radius=tile_radius, outline=SLOT, width=stroke)
    return img


def main() -> None:
    master = render(SS)
    sizes = [256, 128, 96, 64, 48, 32, 24, 16]
    frames = [master.resize((s, s), Image.LANCZOS) for s in sizes]
    frames[0].save("src\\toolbox.ico", format="ICO",
                   sizes=[(s, s) for s in sizes], append_images=frames[1:])
    master.resize((512, 512), Image.LANCZOS).save("build\\icon-preview.png")
    print(f"written: src\\toolbox.ico ({', '.join(map(str, sizes))})")


if __name__ == "__main__":
    main()
