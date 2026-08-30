"""Generates the GoCue application icon (PNG + multi-size ICO) with Pillow.

    python tools/make_icon.py

Output: assets/icon_512.png and assets/GoCue.ico (used by CMake ICON_BIG and the installer).
"""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
ASSETS = ROOT / "assets"
ASSETS.mkdir(exist_ok=True)

SIZE = 512
BG = (27, 27, 31, 255)        # Palette::background
GREEN = (46, 160, 67, 255)    # Palette::goButton
WHITE = (255, 255, 255, 255)


def find_font(size):
    for name in ("arialbd.ttf", "segoeuib.ttf", "malgunbd.ttf", "arial.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            continue
    return ImageFont.load_default()


def render(size):
    scale = size / SIZE
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    radius = int(96 * scale)
    draw.rounded_rectangle((0, 0, size - 1, size - 1), radius=radius, fill=BG)

    inset = int(56 * scale)
    draw.rounded_rectangle((inset, inset, size - 1 - inset, size - 1 - inset), radius=int(64 * scale), fill=GREEN)

    font = find_font(int(210 * scale))
    text = "GO"
    bbox = draw.textbbox((0, 0), text, font=font)
    w, h = bbox[2] - bbox[0], bbox[3] - bbox[1]
    x = (size - w) / 2 - bbox[0]
    y = (size - h) / 2 - bbox[1] - int(8 * scale)
    draw.text((x, y), text, font=font, fill=WHITE)
    return img


def main():
    big = render(SIZE)
    big.save(ASSETS / "icon_512.png")
    sizes = [256, 128, 64, 48, 32, 16]
    frames = [render(s) for s in sizes]
    frames[0].save(ASSETS / "GoCue.ico", format="ICO", sizes=[(s, s) for s in sizes], append_images=frames[1:])
    print("wrote", ASSETS / "icon_512.png", "and", ASSETS / "GoCue.ico")


if __name__ == "__main__":
    main()
