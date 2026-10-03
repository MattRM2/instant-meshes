"""make_imd_icon.py -- builds the .imd file type icon (resources/icons/imd.ico).

Same document sheet as the MattRM2 VFX Build .blend icon (File_Icon.svg:
#E6E6E6 sheet with a folded top-right corner in #545454, 1000x1000 layout),
carrying the Instant Meshes application icon instead of the hexagon mark.

    python resources/icons/make_imd_icon.py
"""
import os

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
APP_ICON = os.path.join(HERE, "..", "icon.png")
SIZES = [16, 24, 32, 48, 64, 96, 128, 256]
SUPER = 1024                       # drawing resolution (1000-unit layout scaled)

SHEET = (230, 230, 230, 255)
FOLD = (84, 84, 84, 255)


def master():
    s = SUPER / 1000.0
    img = Image.new("RGBA", (SUPER, SUPER), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.polygon([(220 * s, 110 * s), (600 * s, 110 * s), (780 * s, 290 * s), (780 * s, 890 * s), (220 * s, 890 * s)],
              fill=SHEET)
    d.polygon([(600 * s, 110 * s), (780 * s, 290 * s), (600 * s, 290 * s)], fill=FOLD)

    # Application icon, centred where the .blend icon has its mark
    app = Image.open(APP_ICON).convert("RGBA")
    width = int(440 * s)
    height = int(app.height * width / app.width)
    app = app.resize((width, height), Image.LANCZOS)
    cx, cy = 500 * s, 590 * s
    img.alpha_composite(app, (int(cx - width / 2), int(cy - height / 2)))
    return img


def main():
    big = master()
    images = [big.resize((n, n), Image.LANCZOS) for n in SIZES]
    images[-1].save(os.path.join(HERE, "imd_256.png"))
    images[-1].save(os.path.join(HERE, "imd.ico"), sizes=[(n, n) for n in SIZES],
                    append_images=images[:-1])
    print("imd.ico written:", ", ".join(str(n) for n in SIZES))


if __name__ == "__main__":
    main()
