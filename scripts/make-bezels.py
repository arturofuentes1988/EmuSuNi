#!/usr/bin/env python3
# EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
#
# Makes the built-in bezels (all original): frames that fill the sides a 4:3 picture leaves on a
# 16:9 TV, see-through where the game goes (the emulator finds that window and fits the picture
# in it). Three for each console, and an old TV for all of them:
#   assets/xmb/bezels/snes-console.png   dark grey, the purple line, the D-pad and face buttons
#   assets/xmb/bezels/snes-sfc.png       the Super Famicom's light grey and four colours
#   assets/xmb/bezels/snes-purple.png    deep purple with pixel stars
#   assets/xmb/bezels/nes-console.png    the NES's light grey and black, POWER / RESET
#   assets/xmb/bezels/nes-famicom.png    the Famicom's cream, dark red and gold
#   assets/xmb/bezels/nes-black.png      black with red lines, the pad
#   assets/xmb/bezels/n64-console.png    charcoal, the four colours, the stick
#   assets/xmb/bezels/n64-colors.png     the logo's four colours as bands
#   assets/xmb/bezels/n64-atomic.png     see-through purple, as the Atomic Purple console
#   assets/xmb/bezels/gb-console.png     the first Game Boy: grey, its screen frame, D-pad, B and A
#   assets/xmb/bezels/gb-pocket.png      the Game Boy Pocket's silver
#   assets/xmb/bezels/gb-green.png       the green of the Game Boy's screen, with its dots
#   assets/xmb/bezels/gbc-console.png    the Game Boy Color in berry
#   assets/xmb/bezels/gbc-teal.png       the Game Boy Color in teal
#   assets/xmb/bezels/gbc-atomic.png     the Game Boy Color in see-through purple
#   assets/xmb/bezels/gba-console.png    the Game Boy Advance in indigo, with L and R
#   assets/xmb/bezels/gba-glacier.png    the Game Boy Advance in see-through ice blue
#   assets/xmb/bezels/gba-black.png      a black Game Boy Advance with silver lines
#   assets/xmb/bezels/tv.png             a wooden tube TV around the picture
# The handhelds' windows have their screen's shape at a whole multiple: 960x864 (6x 160x144) for
# the Game Boy and Color, 1200x800 (5x 240x160) for the Advance.
#
#   python3 scripts/make-bezels.py [preview.png]   (needs Pillow; with a game picture, also writes
#                                                   previews to /tmp/bezel-previews)

import os
import random
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "assets/xmb/bezels")
FONT = os.path.join(ROOT, "assets/xmb/fonts/Michroma-Regular.ttf")
S = 2  # drawn at twice the size, then made smaller (smooth edges)
W, H = 1920 * S, 1080 * S
# The 4:3 window at full height.
X0, X1 = 240 * S, 1680 * S
LEFT, RIGHT = 120 * S, 1800 * S  # the middle of each side panel
RED, YELLOW, GREEN, BLUE = (229, 72, 77), (242, 194, 48), (61, 180, 109), (63, 120, 216)
N64_BLUE, N64_RED, N64_GREEN, N64_YELLOW = (0, 86, 167), (225, 30, 37), (0, 158, 73), (254, 203, 0)


def vertical_gradient(size, top, bottom):
    width, height = size
    column = Image.new("RGBA", (1, height))
    for y in range(height):
        t = y / max(1, height - 1)
        column.putpixel((0, y), tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)) + (255,))
    return column.resize((width, height))


def vertical_text(image, text, centre, size, fill, angle=90, spacing=0.18, shadow=False):
    """Text along a side panel (rotated), centred on `centre`."""
    font = ImageFont.truetype(FONT, size)
    widths = [font.getbbox(c)[2] - font.getbbox(c)[0] for c in text]
    gap = int(size * spacing)
    total = sum(widths) + gap * (len(text) - 1)
    strip = Image.new("RGBA", (total + size, size * 2), (0, 0, 0, 0))
    draw = ImageDraw.Draw(strip)
    x = size // 2
    for c, w in zip(text, widths):
        if shadow:
            draw.text((x - font.getbbox(c)[0] + 3 * S, size // 2 + 3 * S), c, font=font, fill=(0, 0, 0, 120))
        draw.text((x - font.getbbox(c)[0], size // 2), c, font=font, fill=fill)
        x += w + gap
    strip = strip.rotate(angle, expand=True, resample=Image.BICUBIC)
    image.alpha_composite(strip, (int(centre[0] - strip.width / 2), int(centre[1] - strip.height / 2)))


def centred_text(draw, text, centre, size, fill):
    font = ImageFont.truetype(FONT, size)
    box = font.getbbox(text)
    draw.text((centre[0] - (box[2] + box[0]) / 2, centre[1] - (box[3] + box[1]) / 2), text, font=font, fill=fill)


def side_panels(top, bottom):
    """The two side panels with a gradient (the window see-through)."""
    return vertical_gradient((W, H), top, bottom)


def cut_window(image):
    ImageDraw.Draw(image).rectangle((X0, 0, X1 - 1, H - 1), fill=(0, 0, 0, 0))


def window_edges(image, colour, accent, width=12, accent_width=4):
    """Bevelled bars along the window's left and right edges, with an accent line."""
    draw = ImageDraw.Draw(image)
    w, a = width * S, accent_width * S
    for x in (X0 - w, X1):
        draw.rectangle((x, 0, x + w - 1, H - 1), fill=colour)
    draw.rectangle((X0 - w, 0, X0 - w + S, H - 1), fill=(255, 255, 255, 40))
    draw.rectangle((X1 + w - S, 0, X1 + w - 1, H - 1), fill=(255, 255, 255, 40))
    if accent:
        draw.rectangle((X0 - a - 2 * S, 0, X0 - 2 * S - 1, H - 1), fill=accent)
        draw.rectangle((X1 + 2 * S, 0, X1 + 2 * S + a - 1, H - 1), fill=accent)


def soft_shadow(image, box, radius, blur, alpha=150):
    shadow = Image.new("RGBA", image.size, (0, 0, 0, 0))
    ImageDraw.Draw(shadow).rounded_rectangle(box, radius=radius, fill=(0, 0, 0, alpha))
    image.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(blur)))


def gloss(image, x0, x1, alpha=70):
    """A soft light from the top of a panel."""
    light = vertical_gradient((x1 - x0, H), (255, 255, 255), (255, 255, 255))
    mask = vertical_gradient((x1 - x0, H), (alpha, 0, 0), (0, 0, 0)).split()[0]
    layer = Image.new("RGBA", image.size, (0, 0, 0, 0))
    layer.paste(light, (x0, 0), mask)
    image.alpha_composite(layer)


def dpad(image, centre, size, colour, dark):
    cx, cy = centre
    arm, thick = size // 2, size // 6
    soft_shadow(image, (cx - arm, cy - thick + 6 * S, cx + arm, cy + thick + 6 * S), 6 * S, 8 * S)
    soft_shadow(image, (cx - thick, cy - arm + 6 * S, cx + thick, cy + arm + 6 * S), 6 * S, 8 * S)
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((cx - arm, cy - thick, cx + arm, cy + thick), radius=6 * S, fill=colour)
    draw.rounded_rectangle((cx - thick, cy - arm, cx + thick, cy + arm), radius=6 * S, fill=colour)
    draw.ellipse((cx - thick * 0.6, cy - thick * 0.6, cx + thick * 0.6, cy + thick * 0.6), fill=dark)


def button(image, centre, radius, colour, label=None, label_colour=(255, 255, 255, 200)):
    cx, cy = centre
    soft_shadow(image, (cx - radius, cy - radius + 6 * S, cx + radius, cy + radius + 6 * S), radius, 8 * S)
    draw = ImageDraw.Draw(image)
    draw.ellipse((cx - radius, cy - radius, cx + radius, cy + radius), fill=colour)
    h = radius * 0.35
    draw.ellipse((cx - radius * 0.35 - h, cy - radius * 0.4 - h, cx - radius * 0.35 + h, cy - radius * 0.4 + h),
                 fill=(255, 255, 255, 70))
    if label:
        centred_text(draw, label, (cx, cy), int(radius * 0.8), label_colour)


def face_buttons(image, centre, distance, radius):
    """The Super Famicom's four: X top (blue), A right (red), B bottom (yellow), Y left (green)."""
    cx, cy = centre
    for (x, y), colour, label in (((cx, cy - distance), BLUE, "X"), ((cx + distance, cy), RED, "A"),
                                  ((cx, cy + distance), YELLOW, "B"), ((cx - distance, cy), GREEN, "Y")):
        button(image, (x, y), radius, colour + (255,), label)


# --- Super Nintendo -------------------------------------------------------------------------------

def snes_console():
    image = side_panels((52, 50, 62), (18, 17, 23))
    draw = ImageDraw.Draw(image)
    for x0, x1 in ((24 * S, 216 * S), (1704 * S, 1896 * S)):
        for i in range(7):
            y = (880 + i * 18) * S
            draw.rounded_rectangle((x0, y, x1, y + 7 * S), radius=3 * S, fill=(10, 10, 14, 255))
    cut_window(image)
    window_edges(image, (34, 33, 40, 255), (101, 72, 214, 255))
    dpad(image, (LEFT, 560 * S), 130 * S, (150, 150, 162, 255), (110, 110, 122, 255))
    face_buttons(image, (RIGHT, 560 * S), 46 * S, 30 * S)
    vertical_text(image, "SUPER NINTENDO", (LEFT, 250 * S), 30 * S, (200, 200, 212, 220))
    vertical_text(image, "SUPER NINTENDO", (RIGHT, 250 * S), 30 * S, (200, 200, 212, 220), angle=-90)
    return image


def snes_sfc():
    image = side_panels((236, 236, 240), (196, 196, 204))
    gloss(image, 0, X0, 50)
    gloss(image, X1, W, 50)
    cut_window(image)
    draw = ImageDraw.Draw(image)
    # The four colours as lines beside the picture.
    for i, colour in enumerate((RED, YELLOW, GREEN, BLUE)):
        draw.rectangle((X0 - (8 + i * 7) * S, 0, X0 - (8 + i * 7) * S + 4 * S - 1, H - 1), fill=colour + (255,))
        draw.rectangle((X1 + (4 + i * 7) * S, 0, X1 + (4 + i * 7) * S + 4 * S - 1, H - 1), fill=colour + (255,))
    window_edges(image, (150, 150, 160, 255), None, width=6)
    face_buttons(image, (LEFT, 760 * S), 48 * S, 32 * S)
    dpad(image, (RIGHT, 760 * S), 140 * S, (70, 70, 80, 255), (50, 50, 58, 255))
    vertical_text(image, "SUPER FAMICOM", (LEFT, 330 * S), 32 * S, (90, 90, 104, 255))
    vertical_text(image, "SUPER FAMICOM", (RIGHT, 330 * S), 32 * S, (90, 90, 104, 255), angle=-90)
    return image


def snes_purple():
    image = side_panels((98, 62, 212), (26, 14, 72))
    rnd = random.Random(16)
    draw = ImageDraw.Draw(image)
    # Pixel stars, brighter near the top.
    for _ in range(140):
        x = rnd.choice((rnd.randrange(0, X0 - 24 * S), rnd.randrange(X1 + 24 * S, W)))
        y = rnd.randrange(H)
        size = rnd.choice((4, 6, 8)) * S
        alpha = int(255 * (1.0 - y / H) * rnd.uniform(0.3, 1.0))
        draw.rectangle((x, y, x + size, y + size), fill=(230, 220, 255, alpha))
    # A wave of light across each panel.
    light = Image.new("RGBA", image.size, (0, 0, 0, 0))
    ImageDraw.Draw(light).ellipse((-300 * S, 620 * S, 2220 * S, 1500 * S), outline=(190, 160, 255, 90), width=26 * S)
    image.alpha_composite(light.filter(ImageFilter.GaussianBlur(10 * S)))
    cut_window(image)
    window_edges(image, (40, 22, 98, 255), (190, 160, 255, 255))
    vertical_text(image, "SUPER NINTENDO", (LEFT, 300 * S), 34 * S, (255, 255, 255, 235), shadow=True)
    vertical_text(image, "16-BIT", (RIGHT, 300 * S), 46 * S, (255, 255, 255, 235), angle=-90, shadow=True)
    face_buttons(image, (RIGHT, 820 * S), 40 * S, 26 * S)
    return image


# --- NES ------------------------------------------------------------------------------------------

def nes_console():
    image = side_panels((214, 214, 218), (168, 168, 174))
    draw = ImageDraw.Draw(image)
    for x0, x1 in ((0, X0), (X1, W)):
        draw.rectangle((x0, 700 * S, x1 - 1, 860 * S), fill=(38, 38, 42, 255))
        for i in range(8):
            y = (712 + i * 18) * S
            draw.rectangle((x0, y, x1 - 1, y + 4 * S), fill=(58, 58, 64, 255))
    cut_window(image)
    window_edges(image, (30, 30, 34, 255), (200, 36, 40, 255))
    draw = ImageDraw.Draw(image)
    for i, label in enumerate(("POWER", "RESET")):
        y = (560 + i * 70) * S
        draw.rounded_rectangle((60 * S, y, 180 * S, y + 40 * S), radius=6 * S, fill=(70, 70, 76, 255))
        centred_text(draw, label, (LEFT, y + 20 * S), 14 * S, (220, 220, 226, 255))
    draw.ellipse((112 * S, 520 * S, 128 * S, 536 * S), fill=(220, 30, 36, 255))
    draw.rounded_rectangle((1720 * S, 520 * S, 1880 * S, 640 * S), radius=14 * S, fill=(26, 26, 30, 255))
    button(image, (1762 * S, 590 * S), 26 * S, (204, 34, 40, 255), "B")
    button(image, (1838 * S, 590 * S), 26 * S, (204, 34, 40, 255), "A")
    vertical_text(image, "NINTENDO", (LEFT, 250 * S), 32 * S, (60, 60, 66, 255))
    vertical_text(image, "ENTERTAINMENT", (RIGHT, 270 * S), 26 * S, (60, 60, 66, 255), angle=-90)
    return image


def nes_famicom():
    image = side_panels((246, 240, 228), (222, 214, 198))
    draw = ImageDraw.Draw(image)
    # The Famicom's dark red, with gold lines.
    for x0, x1 in ((0, X0), (X1, W)):
        for y0, y1 in ((0, 120 * S), (H - 150 * S, H)):
            draw.rectangle((x0, y0, x1 - 1, y1), fill=(132, 18, 28, 255))
        draw.rectangle((x0, 120 * S, x1 - 1, 126 * S), fill=(214, 172, 84, 255))
        draw.rectangle((x0, H - 156 * S, x1 - 1, H - 150 * S), fill=(214, 172, 84, 255))
    cut_window(image)
    window_edges(image, (120, 16, 26, 255), (214, 172, 84, 255))
    # The Famicom pad: dark red body, the gold plate, the D-pad and two red buttons.
    for cx in (LEFT, RIGHT):
        soft_shadow(image, (cx - 92 * S, 620 * S, cx + 92 * S, 800 * S), 16 * S, 10 * S)
        draw = ImageDraw.Draw(image)
        draw.rounded_rectangle((cx - 92 * S, 610 * S, cx + 92 * S, 790 * S), radius=16 * S, fill=(140, 20, 30, 255))
        draw.rounded_rectangle((cx - 78 * S, 630 * S, cx + 78 * S, 770 * S), radius=8 * S, fill=(214, 172, 84, 255))
    dpad(image, (LEFT, 700 * S), 92 * S, (30, 30, 32, 255), (20, 20, 22, 255))
    button(image, (RIGHT - 32 * S, 712 * S), 22 * S, (190, 24, 34, 255), "B")
    button(image, (RIGHT + 32 * S, 712 * S), 22 * S, (190, 24, 34, 255), "A")
    vertical_text(image, "FAMICOM", (LEFT, 370 * S), 40 * S, (132, 18, 28, 255))
    vertical_text(image, "FAMICOM", (RIGHT, 370 * S), 40 * S, (132, 18, 28, 255), angle=-90)
    return image


def nes_black():
    image = side_panels((32, 32, 34), (8, 8, 9))
    draw = ImageDraw.Draw(image)
    for x0, x1 in ((24 * S, X0 - 40 * S), (X1 + 40 * S, W - 24 * S)):
        for i in range(3):
            y = (150 + i * 16) * S
            draw.rectangle((x0, y, x1, y + 5 * S), fill=(196, 30, 36, 255))
            y = H - (166 + i * 16) * S
            draw.rectangle((x0, y, x1, y + 5 * S), fill=(196, 30, 36, 255))
    cut_window(image)
    window_edges(image, (12, 12, 13, 255), (196, 30, 36, 255))
    vertical_text(image, "NES", (LEFT, 420 * S), 96 * S, (220, 34, 40, 255), spacing=0.3)
    dpad(image, (LEFT, 760 * S), 120 * S, (64, 64, 70, 255), (40, 40, 44, 255))
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((RIGHT - 80 * S, 500 * S, RIGHT + 80 * S, 860 * S), radius=20 * S, fill=(52, 52, 58, 255))
    button(image, (RIGHT, 600 * S), 34 * S, (204, 34, 40, 255), "B")
    button(image, (RIGHT, 760 * S), 34 * S, (204, 34, 40, 255), "A")
    return image


# --- Nintendo 64 ----------------------------------------------------------------------------------

def n64_squares(image, centre, size, gap, colours=(N64_BLUE, N64_RED, N64_GREEN, N64_YELLOW), alpha=255):
    cx, cy = centre
    for (dx, dy), colour in zip(((-1, -1), (1, -1), (-1, 1), (1, 1)), colours):
        x = cx + dx * (size // 2 + gap // 2) - size // 2
        y = cy + dy * (size // 2 + gap // 2) - size // 2
        soft_shadow(image, (x, y + 5 * S, x + size, y + size + 5 * S), 10 * S, 6 * S)
        ImageDraw.Draw(image).rounded_rectangle((x, y, x + size, y + size), radius=10 * S, fill=colour + (alpha,))


def stick(image, centre, well=(8, 8, 10), rim=(30, 30, 34), cap=(120, 120, 128)):
    sx, sy = centre
    draw = ImageDraw.Draw(image)
    draw.ellipse((sx - 70 * S, sy - 70 * S, sx + 70 * S, sy + 70 * S), fill=well + (255,))
    draw.ellipse((sx - 62 * S, sy - 62 * S, sx + 62 * S, sy + 62 * S), fill=rim + (255,))
    soft_shadow(image, (sx - 34 * S, sy - 30 * S, sx + 34 * S, sy + 38 * S), 34 * S, 6 * S)
    ImageDraw.Draw(image).ellipse((sx - 34 * S, sy - 34 * S, sx + 34 * S, sy + 34 * S), fill=cap + (255,))


def n64_console():
    image = side_panels((40, 40, 44), (14, 14, 16))
    cut_window(image)
    window_edges(image, (26, 26, 29, 255), (0, 158, 73, 255))
    n64_squares(image, (RIGHT, 560 * S), 52 * S, 8 * S)
    stick(image, (LEFT, 560 * S))
    vertical_text(image, "NINTENDO 64", (LEFT, 250 * S), 30 * S, (210, 210, 216, 230))
    vertical_text(image, "NINTENDO 64", (RIGHT, 250 * S), 30 * S, (210, 210, 216, 230), angle=-90)
    return image


def n64_colors():
    image = Image.new("RGBA", (W, H), (0, 0, 0, 255))
    draw = ImageDraw.Draw(image)
    half = X0 // 2
    for x0, x1, colour in ((0, half, N64_BLUE), (half, X0, N64_RED), (X1, X1 + half, N64_GREEN),
                           (X1 + half, W, N64_YELLOW)):
        band = vertical_gradient((x1 - x0, H), tuple(min(255, c + 30) for c in colour),
                                 tuple(int(c * 0.55) for c in colour))
        image.paste(band, (x0, 0))
    gloss(image, 0, X0, 90)
    gloss(image, X1, W, 90)
    cut_window(image)
    window_edges(image, (16, 16, 18, 255), (240, 240, 244, 255), width=10, accent_width=3)
    vertical_text(image, "NINTENDO 64", (LEFT, 540 * S), 40 * S, (255, 255, 255, 240), shadow=True)
    vertical_text(image, "NINTENDO 64", (RIGHT, 540 * S), 40 * S, (255, 255, 255, 240), angle=-90, shadow=True)
    return image


def n64_atomic():
    image = side_panels((112, 66, 168), (44, 18, 76))
    rnd = random.Random(64)
    # The insides seen through the purple plastic: traces and chips, faint.
    insides = Image.new("RGBA", image.size, (0, 0, 0, 0))
    draw = ImageDraw.Draw(insides)
    for panel in ((20 * S, X0 - 30 * S), (X1 + 30 * S, W - 20 * S)):
        for _ in range(26):
            x = rnd.randrange(*panel)
            y = rnd.randrange(60 * S, H - 60 * S)
            length = rnd.randrange(60, 220) * S
            if rnd.random() < 0.5:
                draw.line((x, y, min(panel[1], x + length // 3), y), fill=(30, 10, 50, 120), width=4 * S)
            else:
                draw.line((x, y, x, min(H, y + length)), fill=(30, 10, 50, 120), width=4 * S)
            draw.ellipse((x - 5 * S, y - 5 * S, x + 5 * S, y + 5 * S), fill=(230, 200, 120, 90))
        for _ in range(4):
            x = rnd.randrange(panel[0], panel[1] - 70 * S)
            y = rnd.randrange(80 * S, H - 160 * S)
            draw.rectangle((x, y, x + 60 * S, y + 80 * S), fill=(20, 8, 34, 140))
    image.alpha_composite(insides.filter(ImageFilter.GaussianBlur(1.5 * S)))
    gloss(image, 0, X0, 80)
    gloss(image, X1, W, 80)
    cut_window(image)
    window_edges(image, (60, 28, 98, 255), (200, 170, 255, 255))
    n64_squares(image, (RIGHT, 820 * S), 46 * S, 8 * S, alpha=230)
    stick(image, (LEFT, 820 * S), well=(40, 16, 66), rim=(80, 44, 120), cap=(170, 140, 210))
    vertical_text(image, "NINTENDO 64", (LEFT, 330 * S), 32 * S, (240, 230, 255, 235), shadow=True)
    vertical_text(image, "ATOMIC PURPLE", (RIGHT, 330 * S), 28 * S, (240, 230, 255, 235), angle=-90, shadow=True)
    return image


# --- The Game Boys --------------------------------------------------------------------------------

GB_WINDOW = (480 * S, 108 * S, 1440 * S, 972 * S)   # 960x864: 6x the Game Boy's 160x144
GBA_WINDOW = (360 * S, 140 * S, 1560 * S, 940 * S)  # 1200x800: 5x the Advance's 240x160
GB_INK = (36, 44, 112)  # the first Game Boy's dark blue letters
COLOR_LETTERS = ((228, 44, 56), (132, 64, 206), (52, 176, 80), (244, 200, 40), (36, 116, 224))


def cut(image, window):
    """The see-through window, square-cornered (the emulator fits the picture in it exactly)."""
    ImageDraw.Draw(image).rectangle((window[0], window[1], window[2] - 1, window[3] - 1), fill=(0, 0, 0, 0))


def lens(image, box, colour, radius, big_corner=0):
    """The dark frame around a handheld's screen; big_corner rounds its bottom right (the first
    Game Boy's)."""
    mask = Image.new("L", image.size, 0)
    draw = ImageDraw.Draw(mask)
    draw.rounded_rectangle(box, radius=radius, fill=255)
    if big_corner:
        x1, y1 = box[2], box[3]
        draw.rectangle((x1 - big_corner, y1 - big_corner, x1, y1), fill=0)
        draw.pieslice((x1 - 2 * big_corner, y1 - 2 * big_corner, x1, y1), 0, 90, fill=255)
    soft_shadow(image, (box[0], box[1] + 6 * S, box[2], box[3] + 6 * S), radius, 10 * S, 110)
    image.paste(Image.new("RGBA", image.size, colour + (255,)), (0, 0), mask)


def text_at(image, text, centre, size, fill, spacing=0.0):
    """Text centred on `centre`, its letters spaced out."""
    vertical_text(image, text, centre, size, fill, angle=0, spacing=spacing)


def coloured_word(image, word, colours, left_centre, size):
    """A word with each letter in its own colour (the Game Boy Color's COLOR), starting at left."""
    font = ImageFont.truetype(FONT, size)
    draw = ImageDraw.Draw(image)
    x, y = left_centre
    for letter, colour in zip(word, colours):
        box = font.getbbox(letter)
        draw.text((x - box[0], y - (box[3] + box[1]) / 2), letter, font=font, fill=colour + (255,))
        x += box[2] - box[0] + size * 0.08


def word_width(word, size, gap=0.08):
    font = ImageFont.truetype(FONT, size)
    return sum(font.getbbox(c)[2] - font.getbbox(c)[0] for c in word) + size * gap * (len(word) - 1)


def pill(image, centre, size, colour, angle=-25):
    """A SELECT / START button: a rubber pill, tilted."""
    w, h = size
    layer = Image.new("RGBA", (w + 20 * S, w + 20 * S), (0, 0, 0, 0))
    ImageDraw.Draw(layer).rounded_rectangle(((layer.width - w) // 2, (layer.height - h) // 2, (layer.width + w) // 2,
                                             (layer.height + h) // 2), radius=h // 2, fill=colour + (255,))
    layer = layer.rotate(-angle, resample=Image.BICUBIC)
    shadow = layer.split()[3].point(lambda a: a * 0.5)
    dark = Image.new("RGBA", layer.size, (0, 0, 0, 255))
    dark.putalpha(shadow.filter(ImageFilter.GaussianBlur(4 * S)))
    image.alpha_composite(dark, (int(centre[0] - layer.width / 2), int(centre[1] - layer.height / 2 + 4 * S)))
    image.alpha_composite(layer, (int(centre[0] - layer.width / 2), int(centre[1] - layer.height / 2)))


def led(image, centre, colour, label, ink):
    cx, cy = centre
    draw = ImageDraw.Draw(image)
    draw.ellipse((cx - 9 * S, cy - 9 * S, cx + 9 * S, cy + 9 * S), fill=(20, 20, 24, 255))
    draw.ellipse((cx - 6 * S, cy - 6 * S, cx + 6 * S, cy + 6 * S), fill=colour + (255,))
    glow = Image.new("RGBA", image.size, (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse((cx - 16 * S, cy - 16 * S, cx + 16 * S, cy + 16 * S), fill=colour + (90,))
    image.alpha_composite(glow.filter(ImageFilter.GaussianBlur(6 * S)))
    centred_text(ImageDraw.Draw(image), label, (cx, cy + 26 * S), 10 * S, ink)


def speaker_slots(image, origin, count, colour):
    """The first Game Boy's slanted speaker slots, bottom right."""
    draw = ImageDraw.Draw(image)
    x, y = origin
    for i in range(count):
        dx = i * 30 * S
        draw.line((x + dx, y, x + dx + 56 * S, y - 96 * S), fill=colour + (255,), width=12 * S)


def see_through(image, panels, seed, line=(30, 10, 50, 120), dot=(230, 200, 120, 90)):
    """The insides seen through clear plastic: traces, solder dots and chips, faint."""
    rnd = random.Random(seed)
    insides = Image.new("RGBA", image.size, (0, 0, 0, 0))
    draw = ImageDraw.Draw(insides)
    for x0, y0, x1, y1 in panels:
        for _ in range(14):
            x = rnd.randrange(x0, x1)
            y = rnd.randrange(y0, y1)
            length = rnd.randrange(60, 220) * S
            if rnd.random() < 0.5:
                draw.line((x, y, min(x1, x + length // 2), y), fill=line, width=4 * S)
            else:
                draw.line((x, y, x, min(y1, y + length)), fill=line, width=4 * S)
            draw.ellipse((x - 5 * S, y - 5 * S, x + 5 * S, y + 5 * S), fill=dot)
        for _ in range(2):
            x = rnd.randrange(x0, max(x0 + 1, x1 - 70 * S))
            y = rnd.randrange(y0, max(y0 + 1, y1 - 90 * S))
            draw.rectangle((x, y, x + 60 * S, y + 80 * S), fill=line[:3] + (110,))
    image.alpha_composite(insides.filter(ImageFilter.GaussianBlur(1.5 * S)))


def game_boy(body, ink, lens_colour, buttons, pad_colour, pills, title, subtitle=None, lines=True, big_corner=True):
    """A Game Boy standing up, its controls beside the screen: the lens with the dot matrix lines
    and the battery light, GAME BOY at the top left, the D-pad, B and A, SELECT and START."""
    image = side_panels(*body)
    gloss(image, 0, W, 40)
    draw = ImageDraw.Draw(image)
    # The ridge along the top, and the slanted grille.
    draw.rectangle((0, 14 * S, W, 18 * S), fill=tuple(int(c * 0.8) for c in body[0]) + (255,))
    box = (400 * S, 44 * S, 1520 * S, 1036 * S)
    lens(image, box, lens_colour, 26 * S, 150 * S if big_corner else 0)
    if lines:
        draw = ImageDraw.Draw(image)
        for y, colour in ((70, (128, 32, 72)), (80, GB_INK)):
            draw.rectangle((440 * S, y * S, 760 * S, (y + 4) * S), fill=colour + (255,))
            draw.rectangle((1160 * S, y * S, 1480 * S, (y + 4) * S), fill=colour + (255,))
        centred_text(draw, "DOT MATRIX WITH STEREO SOUND", (960 * S, 77 * S), 15 * S, (214, 214, 226, 255))
    led(image, (440 * S, 420 * S), (236, 30, 40), "BATTERY", (200, 200, 214, 255))
    cut(image, GB_WINDOW)
    text_at(image, title, (200 * S, 150 * S), 40 * S, ink + (255,), spacing=0.06)
    if subtitle:
        text_at(image, subtitle, (200 * S, 205 * S), 26 * S, ink + (255,), spacing=0.1)
    draw = ImageDraw.Draw(image)
    draw.ellipse((200 * S - 120 * S, 600 * S - 120 * S, 200 * S + 120 * S, 600 * S + 120 * S),
                 fill=tuple(int(c * 0.86) for c in body[0]) + (255,))
    dpad(image, (200 * S, 600 * S), 180 * S, pad_colour + (255,), tuple(int(c * 0.7) for c in pad_colour) + (255,))
    # B and A in their slanted recess.
    recess = Image.new("RGBA", image.size, (0, 0, 0, 0))
    ImageDraw.Draw(recess).rounded_rectangle((1600 * S, 560 * S, 1850 * S, 680 * S), radius=60 * S,
                                             fill=tuple(int(c * 0.86) for c in body[0]) + (255,))
    image.alpha_composite(recess.rotate(25, center=(1725 * S, 620 * S), resample=Image.BICUBIC))
    button(image, (1665 * S, 650 * S), 50 * S, buttons + (255,))
    button(image, (1785 * S, 590 * S), 50 * S, buttons + (255,))
    centred_text(ImageDraw.Draw(image), "B", (1665 * S, 728 * S), 26 * S, ink + (255,))
    centred_text(ImageDraw.Draw(image), "A", (1785 * S, 668 * S), 26 * S, ink + (255,))
    for x, label in ((1640, "SELECT"), (1770, "START")):
        pill(image, (x * S, 850 * S), (86 * S, 24 * S), pills)
        centred_text(ImageDraw.Draw(image), label, (x * S, 900 * S), 15 * S, ink + (255,))
    speaker_slots(image, (1640 * S, 1060 * S), 6, tuple(int(c * 0.62) for c in body[1]))
    return image


def gb_console():
    return game_boy(((206, 206, 200), (176, 176, 170)), GB_INK, (86, 90, 106), (150, 28, 76), (36, 36, 40),
                    (120, 120, 132), "GAME BOY")


def gb_pocket():
    image = game_boy(((214, 216, 222), (148, 150, 158)), (40, 40, 48), (52, 54, 62), (44, 44, 50), (32, 32, 36),
                     (70, 70, 78), "GAME BOY", "pocket", lines=False, big_corner=False)
    return image


def gb_green():
    """The Game Boy's screen as the frame: its four greens, with the dots of its matrix."""
    shades = ((15, 56, 15), (48, 98, 48), (139, 172, 15), (155, 188, 15))
    image = side_panels(shades[1], shades[0])
    dots = Image.new("RGBA", image.size, (0, 0, 0, 0))
    draw = ImageDraw.Draw(dots)
    step = 24 * S
    for y in range(0, H, step):
        for x in range(0, W, step):
            draw.rectangle((x + 4 * S, y + 4 * S, x + step - 4 * S, y + step - 4 * S), fill=shades[2] + (26,))
    image.alpha_composite(dots)
    draw = ImageDraw.Draw(image)
    x0, y0, x1, y1 = GB_WINDOW
    for i, colour in enumerate(shades[::-1]):
        pad = (8 + i * 10) * S
        draw.rectangle((x0 - pad, y0 - pad, x1 + pad - 1, y1 + pad - 1), outline=colour + (255,), width=6 * S)
    cut(image, GB_WINDOW)
    vertical_text(image, "GAME BOY", (240 * S, 540 * S), 64 * S, shades[3] + (255,), spacing=0.3)
    vertical_text(image, "DOT MATRIX", (1680 * S, 540 * S), 52 * S, shades[3] + (255,), angle=-90, spacing=0.3)
    return image


def game_boy_color(body, panels_dark, see=None):
    """The Game Boy Color: rounder, its dark lens with GAME BOY COLOR under the screen."""
    image = side_panels(*body)
    if see:
        # Above the controls only, faint.
        see_through(image, ((30 * S, 60 * S, 370 * S, 400 * S), (1550 * S, 60 * S, W - 30 * S, 400 * S)), see,
                    line=(30, 10, 50, 80), dot=(230, 200, 120, 70))
    gloss(image, 0, W, 60)
    box = (404 * S, 30 * S, 1516 * S, 1050 * S)
    lens(image, box, (34, 34, 40), 40 * S)
    led(image, (442 * S, 430 * S), (240, 40, 40), "POWER", (190, 190, 200, 255))
    total = word_width("GAME BOY ", 28 * S) + word_width("COLOR", 30 * S)
    left = 960 * S - total / 2
    coloured_word(image, "GAME BOY ", [(214, 214, 224)] * 9, (left, 1012 * S), 28 * S)
    coloured_word(image, "COLOR", COLOR_LETTERS, (left + word_width("GAME BOY ", 28 * S) + 6 * S, 1012 * S), 30 * S)
    cut(image, GB_WINDOW)
    dpad(image, (200 * S, 560 * S), 180 * S, (34, 34, 38, 255), (24, 24, 28, 255))
    button(image, (1660 * S, 610 * S), 50 * S, panels_dark + (255,), "B")
    button(image, (1785 * S, 540 * S), 50 * S, panels_dark + (255,), "A")
    for x, label in ((150, "SELECT"), (260, "START")):
        pill(image, (x * S, 860 * S), (80 * S, 22 * S), (40, 40, 46), angle=0)
        centred_text(ImageDraw.Draw(image), label, (x * S, 905 * S), 15 * S, (240, 240, 246, 230))
    draw = ImageDraw.Draw(image)
    for i in range(3):
        for j in range(3):
            cx, cy = (1700 + i * 34) * S, (860 + j * 34) * S
            draw.ellipse((cx - 8 * S, cy - 8 * S, cx + 8 * S, cy + 8 * S), fill=(0, 0, 0, 120))
    return image


def gbc_console():
    return game_boy_color(((196, 24, 92), (126, 10, 56)), (60, 14, 36))


def gbc_teal():
    return game_boy_color(((0, 160, 170), (0, 92, 106)), (12, 54, 60))


def gbc_atomic():
    return game_boy_color(((120, 72, 176), (48, 20, 82)), (52, 26, 86), see=19)


def game_boy_advance(body, edge, buttons, ink, see=None):
    """The Game Boy Advance: a wide body with round ends on black, L and R on top, the D-pad left,
    A and B right, START and SELECT under the D-pad."""
    image = Image.new("RGBA", (W, H), (10, 10, 12, 255))
    draw = ImageDraw.Draw(image)
    for x0, x1, label in ((70, 470, "L"), (1450, 1850, "R")):
        draw.rounded_rectangle((x0 * S, 14 * S, x1 * S, 120 * S), radius=50 * S, fill=(150, 150, 160, 255))
        centred_text(draw, label, (((x0 + x1) // 2) * S, 44 * S), 26 * S, (70, 70, 80, 255))
    shape = Image.new("L", (W, H), 0)
    ImageDraw.Draw(shape).rounded_rectangle((12 * S, 56 * S, W - 12 * S, H - 12 * S), radius=330 * S, fill=255)
    soft_shadow(image, (12 * S, 62 * S, W - 12 * S, H - 6 * S), 330 * S, 14 * S, 160)
    image.paste(vertical_gradient((W, H), *body), (0, 0), shape)
    if see:
        # Above the controls only, faint.
        see_through(image, ((70 * S, 170 * S, 290 * S, 400 * S), (1630 * S, 170 * S, W - 70 * S, 380 * S)), see,
                    line=(40, 70, 100, 80), dot=(240, 220, 150, 70))
    gloss(image, 0, W, 50)
    box = (300 * S, 88 * S, 1620 * S, 1012 * S)
    lens(image, box, (22, 22, 26), 34 * S)
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle(box, radius=34 * S, outline=edge + (255,), width=3 * S)
    led(image, (330 * S, 300 * S), (60, 226, 96), "POWER", (190, 190, 200, 255))
    centred_text(ImageDraw.Draw(image), "GAME BOY ADVANCE", (960 * S, 976 * S), 26 * S, (210, 210, 222, 255))
    cut(image, GBA_WINDOW)
    dpad(image, (165 * S, 520 * S), 170 * S, (30, 30, 34, 255), (20, 20, 24, 255))
    button(image, (1790 * S, 470 * S), 48 * S, buttons + (255,), "A")
    button(image, (1690 * S, 560 * S), 48 * S, buttons + (255,), "B")
    for y, label in ((770, "START"), (850, "SELECT")):
        button(image, (120 * S, y * S), 14 * S, (40, 40, 46, 255))
        draw = ImageDraw.Draw(image)
        font = ImageFont.truetype(FONT, 14 * S)
        draw.text((146 * S, y * S - 9 * S), label, font=font, fill=ink + (255,))
    draw = ImageDraw.Draw(image)
    for k in range(6):
        cx, cy = 1740 * S + (k % 3) * 34 * S, 780 * S + (k // 3) * 34 * S + (k % 3) * 10 * S
        draw.ellipse((cx - 8 * S, cy - 8 * S, cx + 8 * S, cy + 8 * S), fill=(0, 0, 0, 130))
    return image


def gba_console():
    return game_boy_advance(((92, 80, 186), (48, 40, 116)), (140, 130, 220), (60, 52, 130), (230, 230, 245))


def gba_glacier():
    return game_boy_advance(((168, 214, 238), (84, 140, 190)), (210, 236, 250), (110, 150, 190), (30, 50, 80),
                            see=33)


def gba_black():
    return game_boy_advance(((48, 48, 54), (16, 16, 18)), (180, 180, 190), (34, 34, 38), (210, 210, 220))


# --- The TV ----------------------------------------------------------------------------------------

def tv():
    """A wooden tube TV: the screen (4:3, 1200x900) on the left, the speaker and knobs on the right."""
    image = vertical_gradient((W, H), (96, 64, 38), (58, 37, 22))
    rnd = random.Random(64)
    grain = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    draw = ImageDraw.Draw(grain)
    for _ in range(900):
        x = rnd.randrange(W)
        draw.line((x, 0, x + rnd.randrange(-40, 40), H), fill=(30, 18, 8, rnd.randrange(10, 40)),
                  width=rnd.randrange(1, 4) * S)
    image.alpha_composite(grain.filter(ImageFilter.GaussianBlur(2 * S)))
    sx0, sy0, sx1, sy1 = 280 * S, 90 * S, 1480 * S, 990 * S  # the screen window
    soft_shadow(image, (sx0 - 60 * S, sy0 - 60 * S, sx1 + 60 * S, sy1 + 60 * S), 80 * S, 18 * S, 180)
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((sx0 - 50 * S, sy0 - 50 * S, sx1 + 50 * S, sy1 + 50 * S), radius=80 * S,
                           fill=(22, 20, 19, 255))
    draw.rounded_rectangle((sx0 - 14 * S, sy0 - 14 * S, sx1 + 14 * S, sy1 + 14 * S), radius=52 * S,
                           fill=(8, 8, 8, 255))
    mask = Image.new("L", (W, H), 0)
    ImageDraw.Draw(mask).rounded_rectangle((sx0, sy0, sx1, sy1), radius=44 * S, fill=255)
    image.paste(Image.new("RGBA", (W, H), (0, 0, 0, 0)), (0, 0), mask)
    draw = ImageDraw.Draw(image)
    gx0, gx1 = 1600 * S, 1840 * S
    draw.rounded_rectangle((gx0 - 20 * S, 110 * S, gx1 + 20 * S, 560 * S), radius=24 * S, fill=(40, 28, 18, 255))
    for i in range(18):
        y = (130 + i * 23) * S
        draw.rounded_rectangle((gx0, y, gx1, y + 10 * S), radius=5 * S, fill=(16, 11, 7, 255))
    for i, y in enumerate((690, 860)):
        cx, cy, r = 1720 * S, y * S, 62 * S
        soft_shadow(image, (cx - r, cy - r + 8 * S, cx + r, cy + r + 8 * S), r, 10 * S)
        draw = ImageDraw.Draw(image)
        draw.ellipse((cx - r, cy - r, cx + r, cy + r), fill=(44, 42, 40, 255))
        draw.ellipse((cx - r * 0.78, cy - r * 0.78, cx + r * 0.78, cy + r * 0.78), fill=(70, 66, 62, 255))
        draw.line((cx, cy, cx + r * (0.55 if i else 0.0), cy - r * (0.0 if i else 0.6)), fill=(230, 220, 200, 255),
                  width=8 * S)
    centred_text(draw, "EmuSuNi", (880 * S, 1040 * S), 28 * S, (214, 184, 120, 255))
    return image


BEZELS = {
    "snes-console": snes_console, "snes-sfc": snes_sfc, "snes-purple": snes_purple,
    "nes-console": nes_console, "nes-famicom": nes_famicom, "nes-black": nes_black,
    "n64-console": n64_console, "n64-colors": n64_colors, "n64-atomic": n64_atomic,
    "gb-console": gb_console, "gb-pocket": gb_pocket, "gb-green": gb_green,
    "gbc-console": gbc_console, "gbc-teal": gbc_teal, "gbc-atomic": gbc_atomic,
    "gba-console": gba_console, "gba-glacier": gba_glacier, "gba-black": gba_black,
    "tv": tv,
}


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    game = Image.open(sys.argv[1]).convert("RGBA") if len(sys.argv) > 1 else None
    for name, make in BEZELS.items():
        image = make().resize((W // S, H // S), Image.LANCZOS)
        path = os.path.join(OUT, name + ".png")
        image.save(path, optimize=True)
        print(path, os.path.getsize(path) // 1024, "KiB")
        if game:
            # A preview: the game picture in the window (found as the emulator does), the frame over it.
            os.makedirs("/tmp/bezel-previews", exist_ok=True)
            alpha = image.split()[3]
            cx, cy = 960, 540
            x0 = cx
            while x0 > 0 and alpha.getpixel((x0 - 1, cy)) < 128:
                x0 -= 1
            x1 = cx
            while x1 < 1919 and alpha.getpixel((x1 + 1, cy)) < 128:
                x1 += 1
            mx = (x0 + x1) // 2
            y0 = cy
            while y0 > 0 and alpha.getpixel((mx, y0 - 1)) < 128:
                y0 -= 1
            y1 = cy
            while y1 < 1079 and alpha.getpixel((mx, y1 + 1)) < 128:
                y1 += 1
            ww, wh = x1 - x0 + 1, y1 - y0 + 1
            h = wh
            w = min(ww, int(h * 4 / 3))
            h = int(w * 3 / 4)
            back = Image.new("RGBA", (1920, 1080), (0, 0, 0, 255))
            back.paste(game.resize((w, h)), (x0 + (ww - w) // 2, y0 + (wh - h) // 2))
            back.alpha_composite(image)
            back.convert("RGB").resize((960, 540)).save("/tmp/bezel-previews/" + name + ".jpg", quality=85)
