# Generates the toolbox icon family from vector-style shapes.
# Design language mirrors the launcher canvas: near-black rounded square with
# a 2x2 tile grid. The master icon (src\toolbox.ico) fills three tiles in the
# ICC/MUX/IYX accents plus one empty slot; each component icon fills only its
# own tile (same position as its card in the launcher) and outlines the rest.
# Run from repository root:  python src\toolbox_icon.py
from PIL import Image, ImageDraw

BG = (0x10, 0x11, 0x0F, 0xFF)
ICC = (0xFF, 0x65, 0x4F, 0xFF)
MUX = (0x28, 0x64, 0xFF, 0xFF)
IYX = (0xC8, 0xF4, 0x3D, 0xFF)
SLOT = (0xFF, 0xFF, 0xFF, 0x2E)
SLOT_FAMILY = (0xFF, 0xFF, 0xFF, 0x3C)  # slightly stronger so 16px stays legible

# Draw oversized, downsample with Lanczos for crisp small sizes.
SS = 4096
SIZES = [256, 128, 96, 64, 48, 32, 24, 16]

# Tile order matches the launcher card order: 0=ICC, 1=MUX, 2=IYX.
ACCENTS = (ICC, MUX, IYX)


def render(size: int, filled=(-1, -1), slot_alpha=SLOT[3]) -> Image.Image:
    """filled: indices of the 2x2 grid (0..2) drawn as solid accent tiles."""
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

    stroke = max(2, round(4.2 * u))
    slot = (0xFF, 0xFF, 0xFF, slot_alpha)
    for index, (x, y) in enumerate(((xs[0], ys[0]), (xs[1], ys[0]),
                                    (xs[0], ys[1]))):
        if index in filled:
            d.rounded_rectangle([x, y, x + tile, y + tile],
                                radius=tile_radius, fill=ACCENTS[index])
        else:
            d.rounded_rectangle([x, y, x + tile, y + tile],
                                radius=tile_radius, outline=slot, width=stroke)
    return img


def write_ico(path: str, master: Image.Image) -> None:
    frames = [master.resize((s, s), Image.LANCZOS) for s in SIZES]
    frames[0].save(path, format="ICO", sizes=[(s, s) for s in SIZES],
                   append_images=frames[1:])
    print(f"written: {path} ({', '.join(map(str, SIZES))})")


def main() -> None:
    write_ico("src\\toolbox.ico", render(SS, filled=(0, 1, 2)))

    targets = [
        ("components\\icc-switch\\src\\icc-switch.ico", 0),
        ("components\\mux-display-switcher\\assets\\mux.ico", 1),
        ("components\\iyx-fast-launcher\\IYX.ico", 2),
    ]
    family = []
    for path, index in targets:
        write_ico(path, render(SS, filled=(index,), slot_alpha=SLOT_FAMILY[3]))
        family.append((index, Image.open(path).resize((128, 128), Image.LANCZOS)))

    # Contact sheet for quick visual review.
    sheet = Image.new("RGBA", (128 * 4 + 50, 178), (0xF3, 0xF4, 0xF1, 0xFF))
    sheet.paste(Image.open("src\\toolbox.ico").resize((128, 128), Image.LANCZOS),
                (10, 25), Image.open("src\\toolbox.ico").resize((128, 128),
                                                                Image.LANCZOS))
    for column, (index, frame) in enumerate(family):
        sheet.paste(frame, (10 + (column + 1) * 138, 25), frame)
    sheet.save("build\\icon-family-preview.png")
    print("written: build\\icon-family-preview.png")


if __name__ == "__main__":
    main()
