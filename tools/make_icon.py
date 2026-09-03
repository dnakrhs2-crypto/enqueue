"""Generates the Enqueue application icon (PNG + multi-size ICO) with Pillow.

    python tools/make_icon.py [variant]                 writes assets/icon_512.png + assets/Enqueue.ico
    python tools/make_icon.py --candidates <dir>        writes every design candidate as a 256 px PNG (+ a contact sheet)

Variants (see VARIANTS): the default is "go" (the original mark; gom kept it).
"""
import math
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
ASSETS = ROOT / "assets"
ASSETS.mkdir(exist_ok=True)

SIZE = 512
BG = (27, 27, 31, 255)        # Palette::background
GREEN = (46, 160, 67, 255)    # Palette::goButton
BLUE = (47, 111, 214, 255)    # Palette::selection
ORANGE = (224, 135, 42, 255)  # fade colour
WHITE = (255, 255, 255, 255)
DARK_MARK = (22, 23, 25, 255)

VARIANTS = [
    # (name, tile colour, mark colour, description)
    ("go",          GREEN, WHITE, "GO (기존 아이콘)"),
    ("playq",       GREEN, WHITE, "▶ + Q"),
    ("q",           GREEN, WHITE, "큰 Q"),
    ("qtail",       GREEN, WHITE, "Q의 꼬리가 재생 삼각형"),
    ("nq",          GREEN, WHITE, "NQ"),
    ("hangul",      GREEN, WHITE, "앤큐"),
    ("list",        GREEN, WHITE, "큐 리스트 세 줄 + 재생"),
    ("circle",      GREEN, WHITE, "원 안의 재생 삼각형"),
    ("playq_blue",  BLUE,  WHITE, "▶ + Q, 파란 타일"),
    ("playq_dark",  None,  GREEN, "어두운 타일에 초록 마크"),
    ("q_orange",    ORANGE, WHITE, "큰 Q, 주황 타일"),
]
DEFAULT_VARIANT = "go"   # gom keeps the GO mark (2026-09-03)


def find_font(size):
    for name in ("arialbd.ttf", "segoeuib.ttf", "malgunbd.ttf", "arial.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            continue
    return ImageFont.load_default()


def find_hangul_font(size):
    for name in ("malgunbd.ttf", "malgun.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            continue
    return find_font(size)


def draw_text_centred(draw, text, font, fill, size, dy=0):
    bbox = draw.textbbox((0, 0), text, font=font)
    w, h = bbox[2] - bbox[0], bbox[3] - bbox[1]
    draw.text(((size - w) / 2 - bbox[0], (size - h) / 2 - bbox[1] + dy), text, font=font, fill=fill)


def draw_play_and_q(draw, size, scale, fill):
    font = find_font(int(230 * scale))
    bbox = draw.textbbox((0, 0), "Q", font=font)
    qw, qh = bbox[2] - bbox[0], bbox[3] - bbox[1]
    tri_w, gap = int(120 * scale), int(28 * scale)
    total = tri_w + gap + qw
    x0 = (size - total) / 2
    cy = size / 2 - int(6 * scale)
    tri_h = int(150 * scale)
    draw.polygon([(x0, cy - tri_h / 2), (x0, cy + tri_h / 2), (x0 + tri_w, cy)], fill=fill)
    draw.text((x0 + tri_w + gap - bbox[0], cy - qh / 2 - bbox[1]), "Q", font=font, fill=fill)


def render(size, variant=DEFAULT_VARIANT):
    spec = next((v for v in VARIANTS if v[0] == variant), None)
    if spec is None:
        raise SystemExit("unknown variant " + variant)
    _, tile, mark, _ = spec

    scale = size / SIZE
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    radius = int(96 * scale)
    draw.rounded_rectangle((0, 0, size - 1, size - 1), radius=radius, fill=BG)

    inset = int(56 * scale)
    if tile is not None:
        draw.rounded_rectangle((inset, inset, size - 1 - inset, size - 1 - inset), radius=int(64 * scale), fill=tile)

    base = variant.split("_")[0]

    if base == "go":
        draw_text_centred(draw, "GO", find_font(int(210 * scale)), mark, size, dy=-int(8 * scale))
    elif base == "playq":
        draw_play_and_q(draw, size, scale, mark)
    elif base == "q":
        draw_text_centred(draw, "Q", find_font(int(300 * scale)), mark, size, dy=-int(10 * scale))
    elif base == "qtail":
        # a thick ring with a play triangle where the Q's tail would be
        cx, cy, r = size / 2, size / 2 - int(8 * scale), int(128 * scale)
        thick = int(52 * scale)
        draw.ellipse((cx - r, cy - r, cx + r, cy + r), fill=mark)
        draw.ellipse((cx - r + thick, cy - r + thick, cx + r - thick, cy + r - thick), fill=tile if tile else BG)
        tx, ty, th = cx + int(40 * scale), cy + int(40 * scale), int(150 * scale)
        draw.polygon([(tx, ty), (tx, ty + th), (tx + int(120 * scale), ty + th / 2)], fill=mark)
    elif base == "nq":
        draw_text_centred(draw, "NQ", find_font(int(210 * scale)), mark, size, dy=-int(8 * scale))
    elif base == "hangul":
        draw_text_centred(draw, "앤큐", find_hangul_font(int(190 * scale)), mark, size, dy=-int(8 * scale))
    elif base == "list":
        # three cue rows; the first is "next" and carries the play triangle
        x0, x1 = int(120 * scale), size - int(110 * scale)
        rows = [int(170 * scale), int(256 * scale), int(342 * scale)]
        bar = int(44 * scale)
        tri = int(64 * scale)
        for i, y in enumerate(rows):
            left = x0 + (tri + int(24 * scale) if i == 0 else 0)
            draw.rounded_rectangle((left, y - bar / 2, x1, y + bar / 2), radius=int(18 * scale), fill=mark)
        y = rows[0]
        draw.polygon([(x0 - int(10 * scale), y - tri / 2), (x0 - int(10 * scale), y + tri / 2), (x0 + tri - int(10 * scale), y)], fill=mark)
    elif base == "circle":
        cx, cy, r = size / 2, size / 2, int(150 * scale)
        draw.ellipse((cx - r, cy - r, cx + r, cy + r), fill=mark)
        tw, th = int(120 * scale), int(140 * scale)
        tx = cx - tw / 2 + int(14 * scale)
        draw.polygon([(tx, cy - th / 2), (tx, cy + th / 2), (tx + tw, cy)], fill=tile if tile else BG)
    return img


def contact_sheet(out_dir):
    cell, pad = 256, 24
    cols = 5
    rows = math.ceil(len(VARIANTS) / cols)
    font = find_hangul_font(20)
    sheet = Image.new("RGBA", (cols * (cell + pad) + pad, rows * (cell + pad + 40) + pad), (240, 240, 236, 255))
    draw = ImageDraw.Draw(sheet)
    for i, (name, _, _, desc) in enumerate(VARIANTS):
        x = pad + (i % cols) * (cell + pad)
        y = pad + (i // cols) * (cell + pad + 40)
        sheet.paste(render(cell, name), (x, y), render(cell, name))
        draw.text((x, y + cell + 8), "%02d %s" % (i + 1, desc), font=font, fill=(30, 30, 30, 255))
    sheet.save(out_dir / "00_전체보기.png")


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == "--candidates":
        out = Path(sys.argv[2])
        out.mkdir(parents=True, exist_ok=True)
        for i, (name, _, _, _) in enumerate(VARIANTS, 1):
            render(256, name).save(out / ("%02d_%s.png" % (i, name)))
        contact_sheet(out)
        print("candidates in", out)
        return

    variant = sys.argv[1] if len(sys.argv) >= 2 else DEFAULT_VARIANT
    big = render(SIZE, variant)
    big.save(ASSETS / "icon_512.png")
    sizes = [256, 128, 64, 48, 32, 16]
    frames = [render(s, variant) for s in sizes]
    frames[0].save(ASSETS / "Enqueue.ico", format="ICO", sizes=[(s, s) for s in sizes], append_images=frames[1:])
    print("wrote", ASSETS / "icon_512.png", "and", ASSETS / "Enqueue.ico", "(variant %s)" % variant)


if __name__ == "__main__":
    main()
