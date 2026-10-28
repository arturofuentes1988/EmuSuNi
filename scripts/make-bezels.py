#!/usr/bin/env python3
# EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
#
# Makes the built-in bezels (all original): frames that fill the sides a 4:3 picture leaves on a
# 16:9 TV, see-through where the game goes (the emulator finds that window and fits the picture
# in it). Three for each of the first consoles, one for each of the others (00.63), an old TV and six frames for
# all of them:
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
#   assets/xmb/bezels/ps1-console.png    the PlayStation's grey, its disc lid, the four symbols
#   assets/xmb/bezels/ps1-psone.png      the PSone's white and silver
#   assets/xmb/bezels/ps1-dark.png       Midnight: black and deep blue, the symbols glowing
#   assets/xmb/bezels/tv.png             a wooden tube TV around the picture
#   assets/xmb/bezels/tv-169.png         the same TV with a widescreen tube, for wide pictures (00.63)
# 00.63, the consoles that had none of their own (no logos or names of theirs, only their shapes and colours):
#   assets/xmb/bezels/sg1000-console.png black, the red band between white lines, the joystick, 1 and 2
#   assets/xmb/bezels/sms-console.png    black, the ribbed top and red lines, the pad, 1 and 2, PAUSE
#   assets/xmb/bezels/gg-console.png     the Game Gear around its 4:3 screen (1152x864)
#   assets/xmb/bezels/md-console.png     black with round grooves, 16-BIT, the round D-pad, A B C
#   assets/xmb/bezels/psp-console.png    the PSP in glossy black around a 16:9 window (1600x900)
#   assets/xmb/bezels/dc-console.png     white, the round lid, the orange light, the four coloured buttons
#   assets/xmb/bezels/naomi-console.png  a white cabinet with gold trim (the NAOMI)
#   assets/xmb/bezels/atomiswave-console.png  a black cabinet with green lights and a cartridge
#   assets/xmb/bezels/saturn-console.png the Saturn (00.69): charcoal, the round lid, 32-BIT, the D-pad, X Y Z A B C
#   assets/xmb/bezels/pce-console.png    the PC Engine (00.82): white, the expansion port, a card in its slot, 16-BIT,
#                                        the pad's D-pad, SELECT and RUN, II and I with their turbo switches
#   (00.80 to 00.82 also had the Sega CD's, the 32X's, the PC Engine CD's, the Neo Geo Pockets' and the WonderSwans':
#   gone in 00.83 with those consoles.)
#   assets/xmb/bezels/nds-console[-wide|-tall|-169].png  the DS in white, a window for each DS Screens view
#   assets/xmb/bezels/3ds-console[-wide|-tall|-169].png  the 3DS in aqua blue, a window for each 3DS Screens view
# and six frames for every console, each with a 4:3, a 16:9 (-169), a wide (-wide: two screens side by side) and
# a tall (-tall) window (Emulator.cpp picks the one for the picture's shape):
#   assets/xmb/bezels/frame-cabinet*.png an arcade cabinet's sides: art, trim, marquee, stick, buttons, coins
#   assets/xmb/bezels/frame-minimal*.png dark and plain, a thin rim
#   assets/xmb/bezels/frame-neon*.png    a neon night: grid, sun, stars, glowing tubes
#   assets/xmb/bezels/frame-wood*.png    walnut, black ribs, chrome trim, two switches
#   assets/xmb/bezels/frame-carbon*.png  carbon fibre, brushed aluminium, a red pinstripe, screws
#   assets/xmb/bezels/frame-stars*.png   deep space, a nebula, stars
# The handhelds' windows have their screen's shape at a whole multiple: 960x864 (6x 160x144) for
# the Game Boy and Color, 1200x800 (5x 240x160) for the Advance.
#
#   python3 scripts/make-bezels.py [names or prefixes...] [preview.png]   (needs Pillow, with libimagequant for
#                       the palette PNGs; with a game picture, also writes previews to /tmp/bezel-previews;
#                       BEZELS_OUT=dir writes elsewhere)

import math
import os
import random
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.environ.get("BEZELS_OUT", os.path.join(ROOT, "assets/xmb/bezels"))  # (BEZELS_OUT: elsewhere, to try)
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


# --- The PlayStation ------------------------------------------------------------------------------

PS_GREEN, PS_RED, PS_BLUE, PS_PINK = (64, 200, 150), (236, 80, 80), (96, 140, 230), (226, 130, 190)


def ps_symbols(image, centre, distance, size, width, colours=(PS_GREEN, PS_RED, PS_BLUE, PS_PINK), alpha=255):
    """The pad's four symbols, as outlines: triangle up, circle right, cross down, square left."""
    cx, cy = centre
    draw = ImageDraw.Draw(image)
    tri, circ, cross, sq = (c + (alpha,) for c in colours)
    x, y = cx, cy - distance
    draw.polygon([(x, y - size), (x + size * 0.95, y + size * 0.6), (x - size * 0.95, y + size * 0.6)], outline=tri,
                 width=width)
    x, y = cx + distance, cy
    draw.ellipse((x - size * 0.85, y - size * 0.85, x + size * 0.85, y + size * 0.85), outline=circ, width=width)
    x, y = cx, cy + distance
    s = size * 0.75
    draw.line((x - s, y - s, x + s, y + s), fill=cross, width=width)
    draw.line((x - s, y + s, x + s, y - s), fill=cross, width=width)
    x, y = cx - distance, cy
    draw.rectangle((x - s, y - s, x + s, y + s), outline=sq, width=width)


def ps1_console():
    """The first PlayStation's light grey: its disc lid's ring, the four symbols, the buttons."""
    image = side_panels((206, 206, 210), (168, 168, 174))
    gloss(image, 0, X0, 45)
    gloss(image, X1, W, 45)
    draw = ImageDraw.Draw(image)
    for cx in (LEFT, RIGHT):
        draw.ellipse((cx - 96 * S, 470 * S, cx + 96 * S, 662 * S), outline=(150, 150, 158, 255), width=5 * S)
        draw.ellipse((cx - 18 * S, 548 * S, cx + 18 * S, 584 * S), fill=(150, 150, 158, 255))
    for i, label in enumerate(("POWER", "RESET", "OPEN")):
        y = (800 + i * 64) * S
        draw.ellipse((LEFT - 22 * S, y - 22 * S, LEFT + 22 * S, y + 22 * S), fill=(140, 140, 150, 255))
        centred_text(draw, label, (LEFT, y + 38 * S), 13 * S, (90, 90, 100, 255))
    cut_window(image)
    window_edges(image, (120, 120, 130, 255), (60, 60, 70, 255))
    ps_symbols(image, (RIGHT, 860 * S), 52 * S, 22 * S, 5 * S)
    vertical_text(image, "PLAYSTATION", (LEFT, 250 * S), 30 * S, (80, 80, 92, 255))
    vertical_text(image, "PLAYSTATION", (RIGHT, 250 * S), 30 * S, (80, 80, 92, 255), angle=-90)
    return image


def ps1_psone():
    """The PSone: white and silver, round and slim, the four symbols in colour."""
    image = side_panels((248, 248, 250), (214, 216, 222))
    gloss(image, 0, X0, 60)
    gloss(image, X1, W, 60)
    draw = ImageDraw.Draw(image)
    for cx in (LEFT, RIGHT):
        draw.ellipse((cx - 104 * S, 440 * S, cx + 104 * S, 648 * S), fill=(232, 233, 238, 255),
                     outline=(196, 198, 206, 255), width=4 * S)
        draw.ellipse((cx - 28 * S, 516 * S, cx + 28 * S, 572 * S), fill=(200, 202, 210, 255))
    cut_window(image)
    window_edges(image, (196, 198, 206, 255), (120, 160, 230, 255), width=8, accent_width=3)
    ps_symbols(image, (LEFT, 840 * S), 52 * S, 22 * S, 6 * S)
    ps_symbols(image, (RIGHT, 840 * S), 52 * S, 22 * S, 6 * S)
    vertical_text(image, "PS one", (LEFT, 250 * S), 40 * S, (150, 156, 170, 255), spacing=0.3)
    vertical_text(image, "PS one", (RIGHT, 250 * S), 40 * S, (150, 156, 170, 255), angle=-90, spacing=0.3)
    return image


def ps1_dark():
    """Midnight: black and deep blue, the four symbols glowing."""
    image = side_panels((24, 30, 60), (6, 6, 14))
    rnd = random.Random(1994)
    glow = Image.new("RGBA", image.size, (0, 0, 0, 0))
    for _ in range(26):
        x = rnd.choice((rnd.randrange(30 * S, X0 - 60 * S), rnd.randrange(X1 + 60 * S, W - 30 * S)))
        y = rnd.randrange(80 * S, H - 80 * S)
        size = rnd.randrange(14, 30) * S
        colours = (rnd.choice((PS_GREEN, PS_RED, PS_BLUE, PS_PINK)),) * 4
        ps_symbols(glow, (x, y), 0, size, 3 * S, colours, alpha=70)
    image.alpha_composite(glow.filter(ImageFilter.GaussianBlur(1.5 * S)))
    cut_window(image)
    window_edges(image, (10, 12, 24, 255), (70, 110, 220, 255))
    ps_symbols(image, (RIGHT, 560 * S), 56 * S, 24 * S, 6 * S)
    vertical_text(image, "PLAYSTATION", (LEFT, 540 * S), 40 * S, (210, 220, 255, 240), shadow=True)
    return image


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


# --- 00.63: frames for every console, with a window for each shape of picture ------------------------------------
# A frame's window for each shape (1x pixels: x0, y0, x1, y1; Emulator.cpp BezelShape picks the file by the
# picture's shape, <name><shape>.png, else <name>.png):
WINDOWS = {
    "": (240, 0, 1680, 1080),       # 4:3 at full height (as X0, X1)
    "-169": (160, 90, 1760, 990),   # 16:9, 1600x900: a widescreen picture (the PSP) made a little smaller
    "-wide": (0, 180, 1920, 900),   # 8:3, the whole width: the DS's two screens side by side (the 3DS's 3:1 fits)
    "-tall": (510, 0, 1410, 1080),  # 5:6 at full height: screens one above the other, a vertical arcade game
}


def scaled(rect):
    return tuple(v * S for v in rect)


def cut_box(image, rect):
    """The see-through window (1x rect), square-cornered."""
    x0, y0, x1, y1 = scaled(rect)
    ImageDraw.Draw(image).rectangle((x0, y0, x1 - 1, y1 - 1), fill=(0, 0, 0, 0))


def regions(rect):
    """The frame's parts around a window (2x boxes): the sides and the bands above and below, where there is room."""
    x0, y0, x1, y1 = scaled(rect)
    boxes = {"left": (0, 0, x0, H), "right": (x1, 0, W, H), "top": (x0, 0, x1, y0), "bottom": (x0, y1, x1, H)}
    return {k: b for k, b in boxes.items() if b[2] - b[0] >= 60 * S and b[3] - b[1] >= 60 * S}


def glow(image, layer, blur, strength=1):
    """A layer with its glow (blurred copies under it)."""
    soft = layer.filter(ImageFilter.GaussianBlur(blur))
    for _ in range(strength):
        image.alpha_composite(soft)
    image.alpha_composite(layer)


def ring(draw, rect, pad, colour, width):
    """A line `pad` (1x) outside a window's edges."""
    x0, y0, x1, y1 = scaled(rect)
    p = pad * S
    draw.rectangle((x0 - p, y0 - p, x1 + p - 1, y1 + p - 1), outline=colour, width=width * S)


def light_from_above(image, alpha=22):
    light = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(light).ellipse((int(W * 0.1), int(-H * 0.6), int(W * 0.9), int(H * 0.35)), fill=(255, 255, 255, alpha))
    image.alpha_composite(light.filter(ImageFilter.GaussianBlur(110 * S)))


def spot(rect, where):
    """Where a small detail goes (2x centre): "led" near the bottom right of the frame, "low" the lower part of the
    side panels (or of the band below), "high" their upper part."""
    x0, y0, x1, y1 = scaled(rect)
    parts = regions(rect)
    if "right" in parts:
        cx = (x1 + W) // 2
        return {"led": (cx, H - 50 * S), "low": (cx, int(H * 0.72)), "high": (cx, int(H * 0.25))}[where]
    return {"led": (W - 70 * S, (y1 + H) // 2), "low": (W - 220 * S, (y1 + H) // 2), "high": (W - 220 * S, y0 // 2)}[where]


def power_led(image, centre, colour=(90, 200, 255)):
    cx, cy = centre
    halo = Image.new("RGBA", image.size, (0, 0, 0, 0))
    ImageDraw.Draw(halo).ellipse((cx - 18 * S, cy - 18 * S, cx + 18 * S, cy + 18 * S), fill=colour + (110,))
    image.alpha_composite(halo.filter(ImageFilter.GaussianBlur(8 * S)))
    ImageDraw.Draw(image).ellipse((cx - 5 * S, cy - 5 * S, cx + 5 * S, cy + 5 * S), fill=(235, 250, 255, 255))


def frame_minimal(rect):
    """Dark Minimal: a plain charcoal frame, lit softly from above, a thin rim around the picture."""
    image = vertical_gradient((W, H), (40, 40, 45), (13, 13, 16))
    light_from_above(image, 26)
    x0, y0, x1, y1 = scaled(rect)
    draw = ImageDraw.Draw(image)
    for pad, colour in ((16, (66, 66, 72, 255)), (14, (24, 24, 27, 255)), (6, (6, 6, 7, 255))):
        p = pad * S
        draw.rounded_rectangle((x0 - p, y0 - p, x1 + p - 1, y1 + p - 1), radius=p, fill=colour)
    cut_box(image, rect)
    power_led(image, spot(rect, "led"), (120, 210, 255))
    return image


def frame_neon(rect):
    """Neon: a night of the 1980s - a magenta grid to the horizon, a setting sun, stars, and neon tubes around the
    picture."""
    image = vertical_gradient((W, H), (24, 8, 48), (6, 2, 16))
    x0, y0, x1, y1 = scaled(rect)
    parts = regions(rect)
    sides = "left" in parts
    horizon = int(H * 0.62) if sides else y1 + 40 * S
    rnd = random.Random(1984)
    draw = ImageDraw.Draw(image)
    for _ in range(220):
        x, y = rnd.randrange(W), rnd.randrange(max(1, horizon))
        a = int(230 * (1.0 - y / max(1, horizon)) * rnd.uniform(0.3, 1.0))
        r = rnd.choice((1, 1, 1, 2)) * S
        draw.ellipse((x - r, y - r, x + r, y + r), fill=(255, 210, 250, a))
    # The floor: darker magenta below the horizon.
    floor = vertical_gradient((W, H - horizon), (60, 6, 70), (14, 2, 26))
    image.paste(floor, (0, horizon))
    # The setting suns, one in each side (their lower halves cut in stripes).
    if sides:
        radius = int(min(parts["left"][2] * 0.38, 230 * S))
        for cx in ((parts["left"][2]) // 2, (parts["right"][0] + W) // 2):
            # The sun: yellow at its top to pink at the horizon, its lower part cut by bands that widen downwards.
            top = int(horizon - radius * 1.7)
            mask = Image.new("L", (W, H), 0)
            mdraw = ImageDraw.Draw(mask)
            mdraw.ellipse((cx - radius, top, cx + radius, top + 2 * radius), fill=255)
            for k in range(5):
                band = horizon - radius * 0.95 + k * radius * 0.21
                mdraw.rectangle((cx - radius, band, cx + radius, band + radius * (0.04 + 0.025 * k)), fill=0)
            mdraw.rectangle((0, horizon, W, H), fill=0)
            halo = Image.new("RGBA", (W, H), (0, 0, 0, 0))
            ImageDraw.Draw(halo).ellipse((cx - radius * 1.35, top - radius * 0.35, cx + radius * 1.35, top + radius * 2.35),
                                         fill=(255, 70, 170, 90))
            image.alpha_composite(halo.filter(ImageFilter.GaussianBlur(36 * S)))
            sun = Image.new("RGBA", (W, H), (0, 0, 0, 0))
            sun.paste(vertical_gradient((2 * radius, 2 * radius), (255, 226, 80), (255, 36, 160)), (cx - radius, top))
            image.paste(sun, (0, 0), mask)
    # The grid: lines across, closer together towards the horizon, and lines to the middle of the horizon.
    grid = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    gdraw = ImageDraw.Draw(grid)
    colour = (255, 70, 210, 230)
    rows = 14
    for k in range(1, rows + 1):
        y = horizon + (H - horizon) * (k / rows) ** 2.2
        gdraw.line((0, y, W, y), fill=colour, width=2 * S)
    for i in range(-24, 25):
        gdraw.line((W // 2 + i * 26 * S, horizon, W // 2 + i * 260 * S, H), fill=colour, width=2 * S)
    gdraw.line((0, horizon, W, horizon), fill=(255, 150, 230, 255), width=3 * S)
    glow(image, grid, 6 * S)
    # The neon tubes around the picture: cyan inside, magenta outside, each with its white-hot core.
    tubes = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    tdraw = ImageDraw.Draw(tubes)
    ring(tdraw, rect, 16, (70, 235, 255, 255), 7)
    ring(tdraw, rect, 38, (255, 60, 210, 255), 6)
    glow(image, tubes, 16 * S, 3)
    cores = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    cdraw = ImageDraw.Draw(cores)
    ring(cdraw, rect, 18, (215, 255, 255, 255), 2)
    ring(cdraw, rect, 40, (255, 205, 245, 255), 2)
    glow(image, cores, 3 * S)
    cut_box(image, rect)
    return image


def woodgrain(size, top, bottom, seed):
    """Dark wood with its grain running across."""
    w, h = size
    image = vertical_gradient((w, h), top, bottom)
    rnd = random.Random(seed)
    grain = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    draw = ImageDraw.Draw(grain)
    for _ in range(int(h / (5 * S))):
        y = rnd.uniform(-20 * S, h + 20 * S)
        amp = rnd.uniform(2, 16) * S
        freq = rnd.uniform(0.6, 2.4) / (1000 * S)
        phase = rnd.uniform(0, 6.3)
        dark = rnd.random() < 0.72
        colour = (34, 16, 6, rnd.randrange(40, 105)) if dark else (226, 170, 110, rnd.randrange(16, 44))
        points = [(x, y + amp * math.sin(freq * x + phase) + amp * 0.35 * math.sin(freq * 3.1 * x + phase * 1.7))
                  for x in range(-40 * S, w + 80 * S, 40 * S)]
        draw.line(points, fill=colour, width=rnd.choice((1, 1, 2, 3)) * S)
    image.alpha_composite(grain.filter(ImageFilter.GaussianBlur(1.2 * S)))
    return image


def toggle(image, centre, up=True):
    """A chrome toggle switch: its plate, its lever."""
    cx, cy = centre
    soft_shadow(image, (cx - 34 * S, cy - 50 * S + 6 * S, cx + 34 * S, cy + 50 * S + 6 * S), 12 * S, 8 * S)
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((cx - 34 * S, cy - 50 * S, cx + 34 * S, cy + 50 * S), radius=12 * S, fill=(26, 26, 28, 255))
    draw.rounded_rectangle((cx - 12 * S, cy - 40 * S, cx + 12 * S, cy + 40 * S), radius=8 * S, fill=(8, 8, 9, 255))
    tip = cy - 30 * S if up else cy + 30 * S
    draw.line((cx, cy, cx, tip), fill=(150, 152, 158, 255), width=10 * S)
    draw.ellipse((cx - 11 * S, tip - 11 * S, cx + 11 * S, tip + 11 * S), fill=(214, 216, 222, 255))
    draw.ellipse((cx - 5 * S, tip - 7 * S, cx + 2 * S, tip), fill=(255, 255, 255, 200))


def frame_wood(rect):
    """Wood Panel: dark walnut with black ribbed plastic and a chrome trim around the picture, two switches."""
    image = woodgrain((W, H), (138, 86, 46), (78, 46, 22), 1977)
    gloss(image, 0, W, 34)
    x0, y0, x1, y1 = scaled(rect)
    parts = regions(rect)
    draw = ImageDraw.Draw(image)
    # The black ribbed strips beside the picture (above and below it when it fills the width).
    rib = 70 * S
    if "left" in parts:
        strips = [(x0 - rib, 0, x0, H, "v"), (x1, 0, x1 + rib, H, "v")]
    else:
        strips = []
    if "top" in parts:
        strips += [(0, y0 - rib, W, y0, "h"), (0, y1, W, y1 + rib, "h")]
    for sx0, sy0, sx1, sy1, way in strips:
        draw.rectangle((sx0, sy0, sx1, sy1), fill=(16, 16, 17, 255))
        if way == "v":
            for x in range(sx0 + 4 * S, sx1, 10 * S):
                draw.rectangle((x, sy0, x + 3 * S, sy1), fill=(34, 34, 36, 255))
        else:
            for y in range(sy0 + 4 * S, sy1, 10 * S):
                draw.rectangle((sx0, y, sx1, y + 3 * S), fill=(34, 34, 36, 255))
    # The chrome trim.
    for pad, colour in ((8, (90, 92, 98, 255)), (6, (216, 218, 224, 255)), (3, (150, 152, 158, 255))):
        p = pad * S
        draw.rectangle((x0 - p, y0 - p, x1 + p - 1, y1 + p - 1), fill=colour)
    cut_box(image, rect)
    # Two switches on the wood.
    cx, cy = spot(rect, "low")
    if "right" in parts:
        toggle(image, (cx, cy - 70 * S), True)
        toggle(image, (cx, cy + 90 * S), False)
    else:
        toggle(image, (cx - 60 * S, cy), True)
        toggle(image, (cx + 60 * S, cy), False)
    return image


def carbon(size, cell=10):
    """Carbon fibre: a twill of tows, each lit along its length."""
    c = cell * S
    tile = Image.new("RGBA", (4 * c, 4 * c))
    across = vertical_gradient((c, c), (66, 66, 72), (20, 20, 23)).rotate(90)
    down = vertical_gradient((c, c), (54, 54, 60), (16, 16, 19))
    for i in range(4):
        for j in range(4):
            tile.paste(across if ((i + j) // 2) % 2 == 0 else down, (i * c, j * c))
    image = Image.new("RGBA", size)
    for x in range(0, size[0], tile.width):
        for y in range(0, size[1], tile.height):
            image.paste(tile, (x, y))
    return image


def screw(image, centre):
    cx, cy = centre
    draw = ImageDraw.Draw(image)
    draw.ellipse((cx - 13 * S, cy - 13 * S, cx + 13 * S, cy + 13 * S), fill=(40, 40, 44, 255))
    draw.ellipse((cx - 11 * S, cy - 11 * S, cx + 11 * S, cy + 11 * S), fill=(150, 152, 160, 255))
    hexagon = [(cx + 6 * S * math.cos(a * math.pi / 3), cy + 6 * S * math.sin(a * math.pi / 3)) for a in range(6)]
    draw.polygon(hexagon, fill=(46, 46, 50, 255))


def frame_carbon(rect):
    """Carbon: carbon fibre, a brushed aluminium edge with a red pinstripe, screws in the corners."""
    image = carbon((W, H))
    light_from_above(image, 30)
    x0, y0, x1, y1 = scaled(rect)
    draw = ImageDraw.Draw(image)
    draw.rectangle((x0 - 26 * S, y0 - 26 * S, x1 + 26 * S - 1, y1 + 26 * S - 1), fill=(196, 30, 40, 255))
    draw.rectangle((x0 - 22 * S, y0 - 22 * S, x1 + 22 * S - 1, y1 + 22 * S - 1), fill=(12, 12, 14, 255))
    # Brushed aluminium: fine lines along it, lit from above.
    metal = vertical_gradient((W, H), (196, 198, 204), (112, 114, 120))
    rnd = random.Random(7)
    mdraw = ImageDraw.Draw(metal)
    for y in range(0, H, S):
        if rnd.random() < 0.45:
            mdraw.line((0, y, W, y), fill=(255, 255, 255, rnd.randrange(10, 40)) if rnd.random() < 0.5 else
                       (0, 0, 0, rnd.randrange(10, 36)), width=S)
    mask = Image.new("L", (W, H), 0)
    ImageDraw.Draw(mask).rectangle((x0 - 18 * S, y0 - 18 * S, x1 + 18 * S - 1, y1 + 18 * S - 1), fill=255)
    image.paste(metal, (0, 0), mask)
    draw = ImageDraw.Draw(image)
    draw.rectangle((x0 - 18 * S, y0 - 18 * S, x1 + 18 * S - 1, y1 + 18 * S - 1), outline=(230, 232, 238, 255), width=S)
    draw.rectangle((x0 - 4 * S, y0 - 4 * S, x1 + 4 * S - 1, y1 + 4 * S - 1), fill=(20, 20, 22, 255))
    cut_box(image, rect)
    for name, (bx0, by0, bx1, by1) in regions(rect).items():
        for cx, cy in ((bx0 + 34 * S, by0 + 34 * S), (bx1 - 34 * S, by0 + 34 * S), (bx0 + 34 * S, by1 - 34 * S),
                       (bx1 - 34 * S, by1 - 34 * S)):
            if bx1 - bx0 > 120 * S and by1 - by0 > 120 * S:
                screw(image, (cx, cy))
    return image


def frame_stars(rect):
    """Starfield: deep space, a faint nebula, stars and a few bright ones, a thin glow around the picture."""
    image = vertical_gradient((W, H), (10, 12, 36), (2, 2, 10))
    rnd = random.Random(2063)
    nebula = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    draw = ImageDraw.Draw(nebula)
    for colour in ((130, 50, 180, 80), (40, 110, 190, 70), (200, 70, 140, 50), (70, 40, 160, 70)):
        for _ in range(3):
            cx, cy = rnd.randrange(W), rnd.randrange(H)
            rx, ry = rnd.randrange(160, 520) * S, rnd.randrange(110, 340) * S
            draw.ellipse((cx - rx, cy - ry, cx + rx, cy + ry), fill=colour)
    image.alpha_composite(nebula.filter(ImageFilter.GaussianBlur(100 * S)))
    stars = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    draw = ImageDraw.Draw(stars)
    for _ in range(900):
        x, y = rnd.randrange(W), rnd.randrange(H)
        r = rnd.choice((0.7, 0.9, 1.1, 1.5)) * S
        tint = rnd.choice(((255, 255, 255), (200, 220, 255), (255, 236, 210)))
        draw.ellipse((x - r, y - r, x + r, y + r), fill=tint + (rnd.randrange(70, 255),))
    image.alpha_composite(stars)
    for _ in range(14):
        x, y = rnd.randrange(W), rnd.randrange(H)
        star = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        sdraw = ImageDraw.Draw(star)
        length = rnd.randrange(16, 34) * S
        for dx, dy in ((1, 0), (0, 1)):
            sdraw.line((x - dx * length, y - dy * length, x + dx * length, y + dy * length), fill=(210, 230, 255, 150),
                       width=S)
        sdraw.ellipse((x - 3 * S, y - 3 * S, x + 3 * S, y + 3 * S), fill=(255, 255, 255, 255))
        glow(image, star, 3 * S)
    rim_layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ring(ImageDraw.Draw(rim_layer), rect, 6, (130, 180, 255, 230), 2)
    glow(image, rim_layer, 6 * S, 2)
    cut_box(image, rect)
    return image


def joystick(image, centre, size, ball=(222, 34, 44)):
    """An arcade stick from above: its dust washer, the shaft's shadow, the ball."""
    cx, cy = centre
    r = size // 2
    draw = ImageDraw.Draw(image)
    draw.ellipse((cx - r, cy - r, cx + r, cy + r), fill=(14, 14, 16, 255))
    draw.ellipse((cx - r * 0.86, cy - r * 0.86, cx + r * 0.86, cy + r * 0.86), outline=(46, 46, 52, 255), width=2 * S)
    soft_shadow(image, (cx - r * 0.5 + 8 * S, cy - r * 0.5 + 14 * S, cx + r * 0.5 + 8 * S, cy + r * 0.5 + 14 * S), r // 2,
                8 * S, 170)
    br = r * 0.56
    draw = ImageDraw.Draw(image)
    draw.ellipse((cx - br, cy - br, cx + br, cy + br), fill=ball + (255,))
    shade = Image.new("RGBA", image.size, (0, 0, 0, 0))
    ImageDraw.Draw(shade).ellipse((cx - br * 0.55, cy - br * 0.75, cx + br * 0.05, cy - br * 0.2), fill=(255, 255, 255, 120))
    image.alpha_composite(shade.filter(ImageFilter.GaussianBlur(3 * S)))


def arcade_button(image, centre, radius, colour):
    """A round arcade button from above: its black ring, the convex top."""
    cx, cy = centre
    draw = ImageDraw.Draw(image)
    draw.ellipse((cx - radius * 1.28, cy - radius * 1.28, cx + radius * 1.28, cy + radius * 1.28), fill=(18, 18, 20, 255))
    soft_shadow(image, (cx - radius, cy - radius + 4 * S, cx + radius, cy + radius + 4 * S), radius, 4 * S, 140)
    draw = ImageDraw.Draw(image)
    draw.ellipse((cx - radius, cy - radius, cx + radius, cy + radius), fill=colour + (255,))
    lighter = tuple(min(255, c + 60) for c in colour)
    draw.ellipse((cx - radius * 0.7, cy - radius * 0.78, cx + radius * 0.5, cy + radius * 0.3), fill=lighter + (110,))
    draw.ellipse((cx - radius * 0.42, cy - radius * 0.6, cx - radius * 0.06, cy - radius * 0.3), fill=(255, 255, 255, 120))


def light_box(image, box, colours=((255, 236, 170), (255, 120, 40))):
    """A backlit marquee: a glowing panel with rays from its middle."""
    x0, y0, x1, y1 = box
    soft_shadow(image, (x0, y0 + 6 * S, x1, y1 + 6 * S), 18 * S, 10 * S, 160)
    panel = Image.new("RGBA", (x1 - x0, y1 - y0), (0, 0, 0, 0))
    panel.paste(vertical_gradient((x1 - x0, y1 - y0), *colours), (0, 0))
    rays = Image.new("RGBA", panel.size, (0, 0, 0, 0))
    draw = ImageDraw.Draw(rays)
    cx, cy = panel.width / 2, panel.height / 2
    reach = max(panel.width, panel.height)
    for k in range(18):
        a0 = k * math.pi / 9
        a1 = a0 + math.pi / 30
        draw.polygon([(cx, cy), (cx + reach * math.cos(a0), cy + reach * math.sin(a0)),
                      (cx + reach * math.cos(a1), cy + reach * math.sin(a1))], fill=(255, 255, 255, 70))
    panel.alpha_composite(rays)
    mask = Image.new("L", panel.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, panel.width - 1, panel.height - 1), radius=16 * S, fill=255)
    image.paste(panel, (x0, y0), mask)
    ImageDraw.Draw(image).rounded_rectangle(box, radius=16 * S, outline=(30, 30, 34, 255), width=5 * S)


def coin_door(image, centre):
    """Two coin slots, their red lights."""
    cx, cy = centre
    soft_shadow(image, (cx - 70 * S, cy - 90 * S + 6 * S, cx + 70 * S, cy + 90 * S + 6 * S), 10 * S, 8 * S)
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((cx - 70 * S, cy - 90 * S, cx + 70 * S, cy + 90 * S), radius=10 * S, fill=(120, 122, 128, 255))
    draw.rounded_rectangle((cx - 64 * S, cy - 84 * S, cx + 64 * S, cy + 84 * S), radius=8 * S, outline=(160, 162, 168, 255),
                           width=2 * S)
    for dx in (-30, 30):
        x = cx + dx * S
        draw.rounded_rectangle((x - 16 * S, cy - 64 * S, x + 16 * S, cy - 6 * S), radius=5 * S, fill=(70, 72, 78, 255))
        draw.rectangle((x - 3 * S, cy - 56 * S, x + 3 * S, cy - 14 * S), fill=(10, 10, 12, 255))
        light = Image.new("RGBA", image.size, (0, 0, 0, 0))
        ImageDraw.Draw(light).rounded_rectangle((x - 16 * S, cy + 16 * S, x + 16 * S, cy + 60 * S), radius=5 * S,
                                                fill=(255, 40, 40, 255))
        glow(image, light, 6 * S)


BUTTONS = ((222, 38, 48), (248, 196, 32), (36, 110, 222), (40, 170, 80), (240, 240, 240), (150, 60, 200))


def cartridge(image, centre):
    """An arcade cartridge in its slot, seen from above: the slot, the black cartridge, its label."""
    cx, cy = centre
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((cx - 86 * S, cy - 20 * S, cx + 86 * S, cy + 20 * S), radius=8 * S, fill=(6, 6, 7, 255))
    soft_shadow(image, (cx - 74 * S, cy - 150 * S + 8 * S, cx + 74 * S, cy + 10 * S), 10 * S, 8 * S, 170)
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((cx - 74 * S, cy - 150 * S, cx + 74 * S, cy + 8 * S), radius=10 * S, fill=(34, 36, 38, 255))
    draw.rounded_rectangle((cx - 60 * S, cy - 132 * S, cx + 60 * S, cy - 40 * S), radius=6 * S, fill=(80, 170, 60, 255))
    for k in range(3):
        draw.rectangle((cx - 44 * S, cy - (118 - k * 22) * S, cx + (20 - k * 12) * S, cy - (110 - k * 22) * S),
                       fill=(230, 250, 220, 220))


def frame_cabinet(rect, ball=(222, 34, 44), trim=(250, 190, 30), stripes=((250, 200, 40), (240, 120, 30),
                                                                          (220, 40, 50)), buttons=BUTTONS, base=None,
                  waves=False, marquee=((255, 236, 170), (255, 120, 40)), door="coins"):
    """Arcade Cabinet: the cabinet's black sides with their art (three bold stripes, or waves), the coloured trim
    beside the picture, a backlit marquee above, the stick and the buttons below, and the coin door (or a
    cartridge)."""
    image = vertical_gradient((W, H), *(base or ((30, 30, 34), (10, 10, 12))))
    light_from_above(image, 20)
    x0, y0, x1, y1 = scaled(rect)
    parts = regions(rect)
    sides = "left" in parts
    # The side art: three stripes rising to the right on each side (or across the bands), or three waves.
    art = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    draw = ImageDraw.Draw(art)
    slope = 1.1
    for k, colour in enumerate(stripes):
        if waves:
            points = [(x, H * 0.36 + k * 34 * S + 70 * S * math.sin(x / (W * 0.16) + k * 0.5)) for x in range(0, W + 8 * S, 8 * S)]
            draw.line(points, fill=colour + (235,), width=14 * S)
            continue
        for origin in ((-300 + k * 60) * S, (1500 + k * 60) * S):
            x = origin
            draw.polygon([(x, H), (x + 34 * S, H), (x + 34 * S + H * slope, 0), (x + H * slope, 0)], fill=colour + (235,))
    image.alpha_composite(art)
    # The trim: a T-molding along the picture's sides (or above and below it).
    draw = ImageDraw.Draw(image)
    t = 14 * S
    edges = []
    if sides:
        edges += [(x0 - t, 0, x0, H), (x1, 0, x1 + t, H)]
    if "top" in parts:
        edges += [(0, y0 - t, W, y0), (0, y1, W, y1 + t)]
    for ex0, ey0, ex1, ey1 in edges:
        draw.rectangle((ex0, ey0, ex1 - 1, ey1 - 1), fill=trim + (255,))
        if ex1 - ex0 == t:
            draw.rectangle((ex0 + 4 * S, ey0, ex0 + 7 * S, ey1 - 1), fill=(255, 255, 255, 110))
        else:
            draw.rectangle((ex0, ey0 + 4 * S, ex1 - 1, ey0 + 7 * S), fill=(255, 255, 255, 110))
    cut_box(image, rect)
    # The marquee, the controls, the coin door, as there is room.
    if sides:
        lx0, rx0 = parts["left"][2], parts["right"][0]
        for bx0, bx1 in ((0, lx0 - t), (rx0 + t, W)):
            m = 26 * S
            light_box(image, (bx0 + m, 40 * S, bx1 - m, 40 * S + int((bx1 - bx0) * 0.55)), marquee)
        lcx, rcx = (lx0 - t) // 2, (rx0 + t + W) // 2
        panel = min(lx0 - t, 360 * S)
        joystick(image, (lcx, int(H * 0.80)), int(panel * 0.62), ball)
        r = int(panel * 0.105)
        if lx0 - t > 400 * S:
            for i in range(6):
                arcade_button(image, (rcx + (i % 3 - 1) * int(r * 3.0), int(H * 0.77) + (i // 3) * int(r * 3.0) +
                                      (i % 3) * -int(r * 0.6)), r, buttons[i])
        else:
            for i in range(3):
                arcade_button(image, (rcx, int(H * 0.69) + i * int(r * 3.0)), r, buttons[i])
        if door == "cartridge":
            cartridge(image, (lcx, int(H * 0.58)))
        else:
            coin_door(image, (lcx, int(H * 0.53)))
    else:
        if "top" in parts:
            bx0, by0, bx1, by1 = parts["top"]
            light_box(image, (bx0 + 30 * S, by0 + 18 * S, bx1 - 30 * S, by1 - 18 * S - t), marquee)
        if "bottom" in parts:
            bx0, by0, bx1, by1 = parts["bottom"]
            cy = (by0 + t + by1) // 2
            size = min(by1 - by0 - t - 20 * S, 150 * S)
            joystick(image, (bx0 + 180 * S, cy), size, ball)
            r = int(size * 0.17)
            for i in range(6):
                arcade_button(image, (bx1 - 420 * S + (i % 3) * int(r * 3.2), cy - int(r * 1.5) + (i // 3) * int(r * 3.0)),
                              r, buttons[i])
            for i in range(2):
                arcade_button(image, (W // 2 + (i * 2 - 1) * 60 * S, cy), int(r * 0.7), (240, 240, 240))
    return image


# --- 00.63: the consoles that had none of their own -------------------------------------------------------------

def dot_grille(image, centre, columns, rows, step, radius, colour):
    """A speaker's holes in a grid."""
    cx, cy = centre
    draw = ImageDraw.Draw(image)
    for i in range(columns):
        for j in range(rows):
            x = cx + (i - (columns - 1) / 2) * step
            y = cy + (j - (rows - 1) / 2) * step
            draw.ellipse((x - radius, y - radius, x + radius, y + radius), fill=colour)


def diamond(image, centre, distance, radius, buttons):
    """Four face buttons in a diamond: (colour, label) at the top, left, right and bottom."""
    cx, cy = centre
    for (dx, dy), (colour, label, ink) in zip(((0, -1), (-1, 0), (1, 0), (0, 1)), buttons):
        button(image, (cx + dx * distance, cy + dy * distance), radius, colour + (255,), label, ink)


def sg1000_console():
    """The SG-1000: black with its red band between white lines; the joystick, buttons 1 and 2."""
    image = side_panels((36, 36, 39), (10, 10, 12))
    gloss(image, 0, X0, 40)
    gloss(image, X1, W, 40)
    draw = ImageDraw.Draw(image)
    for x0, x1 in ((0, X0), (X1, W)):
        draw.rectangle((x0, 300 * S, x1, 372 * S), fill=(206, 28, 38, 255))
        for y in (282, 290, 382, 390):
            draw.rectangle((x0, y * S, x1, y * S + 3 * S), fill=(236, 236, 240, 255))
    cut_window(image)
    window_edges(image, (18, 18, 20, 255), (206, 28, 38, 255))
    joystick(image, (LEFT, 730 * S), 180 * S, (206, 28, 38))
    button(image, (RIGHT, 650 * S), 36 * S, (206, 28, 38, 255), "1")
    button(image, (RIGHT, 800 * S), 36 * S, (206, 28, 38, 255), "2")
    vertical_text(image, "8-BIT", (LEFT, 150 * S), 34 * S, (236, 236, 240, 230), spacing=0.3)
    vertical_text(image, "8-BIT", (RIGHT, 150 * S), 34 * S, (236, 236, 240, 230), angle=-90, spacing=0.3)
    return image


def sms_console():
    """The Master System: black, its ribbed top and red lines; the pad's D-pad, buttons 1 and 2, PAUSE."""
    image = side_panels((32, 32, 35), (8, 8, 10))
    draw = ImageDraw.Draw(image)
    for x0, x1 in ((22 * S, X0 - 40 * S), (X1 + 40 * S, W - 22 * S)):
        for y in range(80 * S, 420 * S, 14 * S):
            draw.rectangle((x0, y, x1, y + 5 * S), fill=(50, 50, 55, 255))
        draw.rectangle((x0, 444 * S, x1, 451 * S), fill=(214, 26, 36, 255))
        draw.rectangle((x0, 460 * S, x1, 463 * S), fill=(214, 26, 36, 255))
    cut_window(image)
    window_edges(image, (14, 14, 16, 255), (214, 26, 36, 255))
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((LEFT - 80 * S, 660 * S, LEFT + 80 * S, 820 * S), radius=14 * S, fill=(20, 20, 22, 255))
    dpad(image, (LEFT, 740 * S), 130 * S, (66, 66, 72, 255), (44, 44, 50, 255))
    button(image, (RIGHT, 690 * S), 34 * S, (60, 60, 66, 255), "1")
    button(image, (RIGHT, 820 * S), 34 * S, (60, 60, 66, 255), "2")
    for x, label in ((LEFT, "PAUSE"),):
        pill(image, (x, 560 * S), (90 * S, 26 * S), (214, 26, 36), angle=0)
        centred_text(ImageDraw.Draw(image), label, (x, 598 * S), 15 * S, (220, 220, 226, 255))
    return image


GG_WINDOW = (384 * S, 108 * S, 1536 * S, 972 * S)  # 1152x864: the Game Gear's 4:3 picture, 6x its 192x144 at 4:3


def gg_console():
    """The Game Gear: its black body with round ends, the wide dark lens, the D-pad and START left, 1 and 2 right."""
    image = Image.new("RGBA", (W, H), (8, 8, 10, 255))
    shape = Image.new("L", (W, H), 0)
    ImageDraw.Draw(shape).rounded_rectangle((10 * S, 26 * S, W - 10 * S, H - 26 * S), radius=400 * S, fill=255)
    soft_shadow(image, (10 * S, 36 * S, W - 10 * S, H - 16 * S), 400 * S, 14 * S, 160)
    image.paste(vertical_gradient((W, H), (56, 56, 62), (18, 18, 22)), (0, 0), shape)
    gloss(image, 0, W, 40)
    box = (330 * S, 56 * S, 1590 * S, 1024 * S)
    lens(image, box, (26, 26, 32), 34 * S)
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle(box, radius=34 * S, outline=(70, 70, 80, 255), width=2 * S)
    for k, colour in enumerate(((0, 160, 200), (36, 112, 214))):
        draw.rectangle((420 * S, (990 + k * 9) * S, 1500 * S, (994 + k * 9) * S), fill=colour + (255,))
    led(image, (366 * S, 300 * S), (60, 226, 96), "POWER", (190, 190, 200, 255))
    cut(image, GG_WINDOW)
    draw = ImageDraw.Draw(image)
    draw.ellipse((172 * S - 120 * S, 470 * S - 120 * S, 172 * S + 120 * S, 470 * S + 120 * S), fill=(40, 40, 46, 255))
    dpad(image, (172 * S, 470 * S), 190 * S, (26, 26, 30, 255), (16, 16, 18, 255))
    pill(image, (228 * S, 760 * S), (74 * S, 28 * S), (44, 110, 210), angle=-20)
    centred_text(ImageDraw.Draw(image), "START", (228 * S, 806 * S), 15 * S, (200, 200, 210, 255))
    button(image, (1690 * S, 560 * S), 46 * S, (44, 44, 50, 255), "1")
    button(image, (1808 * S, 470 * S), 46 * S, (44, 44, 50, 255), "2")
    dot_grille(image, (1750 * S, 800 * S), 4, 3, 30 * S, 7 * S, (0, 0, 0, 140))
    return image


def md_console():
    """The Genesis (Mega Drive): black, the round grooves of its top, 16-BIT in gold; the round D-pad, A, B, C."""
    image = side_panels((32, 32, 34), (6, 6, 7))
    grooves = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    draw = ImageDraw.Draw(grooves)
    for k in range(12):
        r = (740 + k * 18) * S
        draw.ellipse((W / 2 - r, H / 2 - r, W / 2 + r, H / 2 + r), outline=(80, 80, 88, 255), width=3 * S)
    image.alpha_composite(grooves)
    gloss(image, 0, X0, 46)
    gloss(image, X1, W, 46)
    cut_window(image)
    window_edges(image, (12, 12, 13, 255), (210, 30, 40, 255))
    vertical_text(image, "16-BIT", (LEFT, 230 * S), 42 * S, (214, 172, 84, 255), spacing=0.3)
    vertical_text(image, "16-BIT", (RIGHT, 230 * S), 42 * S, (214, 172, 84, 255), angle=-90, spacing=0.3)
    draw = ImageDraw.Draw(image)
    soft_shadow(image, (LEFT - 78 * S, 652 * S + 6 * S, LEFT + 78 * S, 808 * S + 6 * S), 78 * S, 8 * S)
    ImageDraw.Draw(image).ellipse((LEFT - 78 * S, 652 * S, LEFT + 78 * S, 808 * S), fill=(48, 48, 54, 255))
    dpad(image, (LEFT, 730 * S), 120 * S, (30, 30, 34, 255), (20, 20, 24, 255))
    for i, label in enumerate("ABC"):
        button(image, (RIGHT, (620 + i * 115) * S), 40 * S, (40, 40, 46, 255), label)
    led(image, (LEFT, 900 * S), (230, 30, 30), "POWER", (190, 190, 200, 255))
    return image


def psp_console():
    """The PSP in glossy black around its 16:9 screen (in the 1600x900 window): the D-pad and the stick left, the
    four symbols right, the shoulder buttons above, START and SELECT below."""
    rect = WINDOWS["-169"]
    x0, y0, x1, y1 = scaled(rect)
    image = Image.new("RGBA", (W, H), (0, 0, 0, 255))
    shape = Image.new("L", (W, H), 0)
    ImageDraw.Draw(shape).rounded_rectangle((0, 8 * S, W - 1, H - 8 * S), radius=300 * S, fill=255)
    image.paste(vertical_gradient((W, H), (44, 44, 50), (8, 8, 10)), (0, 0), shape)
    gloss(image, 0, W, 70)
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((2 * S, 10 * S, W - 3 * S, H - 10 * S), radius=298 * S, outline=(150, 152, 162, 200), width=3 * S)
    for sx0, sx1 in ((70 * S, 360 * S), (W - 360 * S, W - 70 * S)):
        draw.rounded_rectangle((sx0, 20 * S, sx1, 46 * S), radius=13 * S, fill=(64, 64, 72, 255))
    draw.rounded_rectangle((x0 - 20 * S, y0 - 20 * S, x1 + 20 * S - 1, y1 + 20 * S - 1), radius=16 * S, fill=(14, 14, 16, 255))
    cut_box(image, rect)
    # The D-pad: four buttons around a dip.
    cx, cy, d = 80 * S, 420 * S, 40 * S
    draw = ImageDraw.Draw(image)
    draw.ellipse((cx - 64 * S, cy - 64 * S, cx + 64 * S, cy + 64 * S), fill=(22, 22, 26, 255))
    for dx, dy in ((0, -1), (0, 1), (-1, 0), (1, 0)):
        bx, by = cx + dx * d, cy + dy * d
        w, h = (22 * S, 30 * S) if dx == 0 else (30 * S, 22 * S)
        draw.rounded_rectangle((bx - w, by - h, bx + w, by + h), radius=8 * S, fill=(70, 70, 78, 255))
        tip = (bx + dx * 8 * S, by + dy * 8 * S)
        draw.polygon([(tip[0] + dx * 6 * S, tip[1] + dy * 6 * S), (tip[0] - dy * 7 * S - dx * 6 * S, tip[1] - dx * 7 * S - dy * 6 * S),
                      (tip[0] + dy * 7 * S - dx * 6 * S, tip[1] + dx * 7 * S - dy * 6 * S)], fill=(170, 170, 180, 255))
    # The stick: a small round nub in its well.
    draw.ellipse((cx - 40 * S, 700 * S - 40 * S, cx + 40 * S, 700 * S + 40 * S), fill=(16, 16, 18, 255))
    soft_shadow(image, (cx - 28 * S, 700 * S - 24 * S, cx + 28 * S, 700 * S + 32 * S), 28 * S, 5 * S)
    ImageDraw.Draw(image).ellipse((cx - 28 * S, 700 * S - 28 * S, cx + 28 * S, 700 * S + 28 * S), fill=(120, 122, 130, 255))
    # The four symbols on their buttons.
    sx, sy = W - 80 * S, 420 * S
    for dx, dy in ((0, -1), (-1, 0), (1, 0), (0, 1)):
        button(image, (sx + dx * 44 * S, sy + dy * 44 * S), 24 * S, (34, 34, 40, 255))
    ps_symbols(image, (sx, sy), 44 * S, 11 * S, 3 * S, colours=((200, 202, 210),) * 4)
    for x, label in ((W - 300 * S, "SELECT"), (W - 170 * S, "START")):
        pill(image, (x, H - 46 * S), (70 * S, 20 * S), (90, 92, 100), angle=0)
    pill(image, (240 * S, H - 46 * S), (90 * S, 20 * S), (90, 92, 100), angle=0)
    dot_grille(image, (420 * S, H - 46 * S), 6, 2, 16 * S, 4 * S, (0, 0, 0, 200))
    return image


def dc_console():
    """The Dreamcast's white: its round lid, the orange light, 128-BIT; the controller's stick, D-pad and four
    coloured buttons."""
    image = side_panels((244, 244, 246), (206, 208, 214))
    gloss(image, 0, X0, 60)
    gloss(image, X1, W, 60)
    draw = ImageDraw.Draw(image)
    for cx in (LEFT, RIGHT):
        draw.ellipse((cx - 100 * S, 330 * S, cx + 100 * S, 530 * S), fill=(236, 237, 240, 255), outline=(190, 192, 200, 255),
                     width=5 * S)
        draw.ellipse((cx - 30 * S, 400 * S, cx + 30 * S, 460 * S), fill=(214, 216, 222, 255))
    cut_window(image)
    window_edges(image, (200, 202, 210, 255), (242, 110, 30, 255), width=8, accent_width=3)
    led(image, (LEFT, 600 * S), (255, 130, 20), "POWER", (120, 120, 130, 255))
    stick(image, (LEFT, 770 * S), well=(176, 178, 186), rim=(206, 208, 214), cap=(140, 142, 150))
    dpad(image, (LEFT, 950 * S), 110 * S, (96, 98, 106, 255), (76, 78, 86, 255))
    diamond(image, (RIGHT, 760 * S), 48 * S, 28 * S, (((60, 170, 80), "Y", (255, 255, 255, 220)),
                                                      ((240, 196, 40), "X", (255, 255, 255, 220)),
                                                      ((50, 110, 220), "B", (255, 255, 255, 220)),
                                                      ((222, 50, 50), "A", (255, 255, 255, 220))))
    vertical_text(image, "128-BIT", (LEFT, 140 * S), 32 * S, (150, 152, 162, 255), spacing=0.3)
    vertical_text(image, "128-BIT", (RIGHT, 140 * S), 32 * S, (150, 152, 162, 255), angle=-90, spacing=0.3)
    return image


def naomi_console():
    """The NAOMI: a white arcade cabinet with gold trim and gold waves, a cool white marquee, the stick, buttons."""
    return frame_cabinet(WINDOWS[""], ball=(40, 92, 210), trim=(206, 164, 52),
                         stripes=((214, 172, 64), (186, 136, 40), (236, 214, 150)), base=((240, 240, 236), (198, 198, 192)),
                         waves=True, marquee=((255, 255, 255), (186, 206, 255)))


def atomiswave_console():
    """The Atomiswave: a black cabinet with green trim and green light waves, its cartridge, the stick, buttons."""
    return frame_cabinet(WINDOWS[""], ball=(80, 200, 70), trim=(86, 170, 60),
                         stripes=((120, 230, 90), (70, 170, 50), (200, 250, 170)), base=((26, 30, 26), (6, 8, 6)),
                         waves=True, marquee=((214, 255, 200), (60, 160, 50)), door="cartridge",
                         buttons=((80, 200, 70), (240, 240, 240), (80, 200, 70), (240, 240, 240), (80, 200, 70),
                                  (240, 240, 240)))


def saturn_console():
    """The Saturn (00.69): charcoal black, its round lid in each side, 32-BIT; the pad's round D-pad and START left,
    POWER; its six buttons right - X Y Z small and A B C big, its two rows turned up the side - with L and R above."""
    image = side_panels((50, 50, 56), (12, 12, 15))
    gloss(image, 0, X0, 42)
    gloss(image, X1, W, 42)
    for cx in (LEFT, RIGHT):
        # The round lid: a dark disc in a lighter ring, a smaller ring and the dip in its middle.
        soft_shadow(image, (cx - 96 * S, 250 * S + 6 * S, cx + 96 * S, 442 * S + 6 * S), 96 * S, 8 * S)
        draw = ImageDraw.Draw(image)
        draw.ellipse((cx - 96 * S, 250 * S, cx + 96 * S, 442 * S), fill=(34, 34, 40, 255), outline=(92, 92, 104, 255),
                     width=4 * S)
        draw.ellipse((cx - 60 * S, 286 * S, cx + 60 * S, 406 * S), outline=(58, 58, 66, 255), width=2 * S)
        draw.ellipse((cx - 14 * S, 332 * S, cx + 14 * S, 360 * S), fill=(24, 24, 28, 255))
    cut_window(image)
    window_edges(image, (22, 22, 26, 255), (70, 126, 224, 255))
    vertical_text(image, "32-BIT", (LEFT, 130 * S), 34 * S, (150, 178, 236, 235), spacing=0.3)
    vertical_text(image, "32-BIT", (RIGHT, 130 * S), 34 * S, (150, 178, 236, 235), angle=-90, spacing=0.3)
    # The round D-pad on its disc, START, the power light.
    soft_shadow(image, (LEFT - 74 * S, 586 * S + 6 * S, LEFT + 74 * S, 734 * S + 6 * S), 74 * S, 8 * S)
    ImageDraw.Draw(image).ellipse((LEFT - 74 * S, 586 * S, LEFT + 74 * S, 734 * S), fill=(44, 44, 50, 255))
    dpad(image, (LEFT, 660 * S), 116 * S, (78, 78, 88, 255), (56, 56, 64, 255))
    pill(image, (LEFT, 800 * S), (64 * S, 22 * S), (72, 72, 82), angle=0)
    centred_text(ImageDraw.Draw(image), "START", (LEFT, 832 * S), 14 * S, (180, 180, 192, 255))
    led(image, (LEFT, 930 * S), (70, 200, 90), "POWER", (170, 170, 182, 255))
    # L and R, then X Y Z (small, the pad's upper row) beside A B C (big).
    for x, label in ((RIGHT - 46 * S, "L"), (RIGHT + 46 * S, "R")):
        pill(image, (x, 540 * S), (70 * S, 24 * S), (62, 62, 70), angle=0)
        centred_text(ImageDraw.Draw(image), label, (x, 540 * S), 14 * S, (200, 200, 210, 255))
    for i, (small, big) in enumerate((("X", "A"), ("Y", "B"), ("Z", "C"))):
        y = (650 + i * 110) * S
        button(image, (RIGHT - 46 * S, y - 26 * S), 26 * S, (58, 58, 66, 255), small)
        button(image, (RIGHT + 40 * S, y), 34 * S, (58, 58, 66, 255), big)
    return image


# --- 00.82: the PC Engine ------------------------------------------------------------------------------------------

PCE_ORANGE = (234, 98, 34)


def card_in_slot(image, centre, label):
    """A game card standing in its slot (the PC Engine's), seen from above: the slot, the silver card and its label,
    the ribs it is held by."""
    cx, cy = centre
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((cx - 86 * S, cy - 12 * S, cx + 86 * S, cy + 16 * S), radius=8 * S, fill=(34, 34, 40, 255))
    soft_shadow(image, (cx - 66 * S, cy - 118 * S + 8 * S, cx + 66 * S, cy + 4 * S), 8 * S, 8 * S, 140)
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((cx - 66 * S, cy - 118 * S, cx + 66 * S, cy + 4 * S), radius=8 * S, fill=(214, 216, 222, 255),
                           outline=(164, 166, 176, 255), width=2 * S)
    draw.rounded_rectangle((cx - 54 * S, cy - 106 * S, cx + 54 * S, cy - 40 * S), radius=5 * S, fill=label + (255,))
    for k in range(3):
        draw.rectangle((cx - 42 * S, cy - (94 - k * 18) * S, cx + (24 - k * 14) * S, cy - (89 - k * 18) * S),
                       fill=(255, 255, 255, 210))
    for k in range(4):
        y = cy - (30 - k * 7) * S
        draw.rectangle((cx - 54 * S, y, cx + 54 * S, y + 2 * S), fill=(176, 178, 188, 255))


def expansion_port(image, centre, width):
    """The PC Engine's expansion port along its back: a dark ribbed block, the connector's slot, its gold contacts."""
    cx, cy = centre
    w = width // 2
    soft_shadow(image, (cx - w, cy - 56 * S + 6 * S, cx + w, cy + 56 * S + 6 * S), 12 * S, 8 * S, 120)
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((cx - w, cy - 56 * S, cx + w, cy + 56 * S), radius=12 * S, fill=(48, 48, 54, 255))
    for k in range(5):
        y = cy - (44 - k * 8) * S
        draw.rectangle((cx - w + 14 * S, y, cx + w - 14 * S, y + 3 * S), fill=(68, 68, 76, 255))
    draw.rounded_rectangle((cx - w + 16 * S, cy + 4 * S, cx + w - 16 * S, cy + 40 * S), radius=5 * S, fill=(14, 14, 16, 255))
    for x in range(cx - w + 26 * S, cx + w - 26 * S, 8 * S):
        draw.rectangle((x, cy + 12 * S, x + 3 * S, cy + 32 * S), fill=(204, 168, 86, 255))


def slide_switch(image, centre, position):
    """A small three-way slide switch (a pad's turbo), its knob at position 0, 1 or 2 (top to bottom)."""
    cx, cy = centre
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((cx - 9 * S, cy - 26 * S, cx + 9 * S, cy + 26 * S), radius=9 * S, fill=(30, 30, 34, 255))
    ky = cy + (position - 1) * 15 * S
    draw.rounded_rectangle((cx - 13 * S, ky - 9 * S, cx + 13 * S, ky + 9 * S), radius=5 * S, fill=(126, 128, 138, 255))
    draw.rectangle((cx - 8 * S, ky - S, cx + 8 * S, ky + S), fill=(86, 88, 96, 255))


def pce_pad(image, ink, disc, start_y=800):
    """The PC Engine's pad on the side panels: the D-pad on its disc, SELECT and RUN, the power light left; II and I
    with their turbo switches right."""
    soft_shadow(image, (LEFT - 74 * S, 586 * S + 6 * S, LEFT + 74 * S, 734 * S + 6 * S), 74 * S, 8 * S)
    ImageDraw.Draw(image).ellipse((LEFT - 74 * S, 586 * S, LEFT + 74 * S, 734 * S), fill=disc)
    dpad(image, (LEFT, 660 * S), 116 * S, (34, 34, 38, 255), (20, 20, 24, 255))
    for x, label in ((LEFT - 46 * S, "SELECT"), (LEFT + 46 * S, "RUN")):
        pill(image, (x, start_y * S), (58 * S, 20 * S), (40, 40, 46), angle=0)
        centred_text(ImageDraw.Draw(image), label, (x, (start_y + 30) * S), 12 * S, ink)
    led(image, (LEFT, (start_y + 130) * S), (236, 50, 40), "POWER", ink)
    for i, (label, turbo) in enumerate((("II", 0), ("I", 2))):
        x = RIGHT + (i * 2 - 1) * 46 * S
        slide_switch(image, (x, 560 * S), turbo)
        button(image, (x, 670 * S), 36 * S, (40, 40, 46, 255))
        centred_text(ImageDraw.Draw(image), label, (x, 738 * S), 24 * S, ink)


def pce_console():
    """The PC Engine (00.82): its white, the dark expansion port at its back, a game card in its slot, 16-BIT in grey,
    an orange line by the picture, vents; the pad's D-pad, SELECT and RUN, II and I with their turbo switches."""
    image = side_panels((248, 248, 250), (206, 208, 214))
    gloss(image, 0, X0, 50)
    gloss(image, X1, W, 50)
    draw = ImageDraw.Draw(image)
    for x0, x1 in ((40 * S, 200 * S), (1720 * S, 1880 * S)):
        for i in range(5):
            y = (1000 + i * 13) * S
            draw.rounded_rectangle((x0, y, x1, y + 5 * S), radius=2 * S, fill=(176, 178, 188, 255))
    cut_window(image)
    window_edges(image, (58, 58, 66, 255), PCE_ORANGE + (255,))
    card_in_slot(image, (LEFT, 450 * S), (228, 96, 40))
    expansion_port(image, (RIGHT, 400 * S), 176 * S)
    vertical_text(image, "16-BIT", (LEFT, 150 * S), 36 * S, (130, 132, 146, 255), spacing=0.3)
    vertical_text(image, "16-BIT", (RIGHT, 150 * S), 36 * S, (130, 132, 146, 255), angle=-90, spacing=0.3)
    pce_pad(image, (100, 102, 116, 255), (226, 228, 234, 255))
    return image


# The DS's and the 3DS's windows for each way their screens are shown (Emulator.cpp BezelShape): one screen (4:3),
# side by side (the DS's 8:3, the 3DS's 3:1), one above the other (the DS's 2:3, the 3DS's 5:6), the big top screen
# or the 3DS's top screen alone (16:9, the picture made smaller).
NDS_WINDOWS = {"": (240, 0, 1680, 1080), "-wide": (0, 180, 1920, 900), "-tall": (600, 0, 1320, 1080),
               "-169": (160, 90, 1760, 990)}
DS3_WINDOWS = {"": (240, 0, 1680, 1080), "-wide": (0, 220, 1920, 860), "-tall": (510, 0, 1410, 1080),
               "-169": (160, 90, 1760, 990)}


def handheld_controls(image, rect, colours):
    """The DS's (or the 3DS's) controls where the window leaves room: the speaker grilles above, the D-pad (and the
    3DS's Circle Pad) left, A B X Y and START and SELECT right; the hinge between screens one above the other."""
    x0, y0, x1, y1 = scaled(rect)
    parts = regions(rect)
    body, dark, keys, ink, grille, pad = colours
    abxy = (((keys, "X", ink)), ((keys, "Y", ink)), ((keys, "A", ink)), ((keys, "B", ink)))
    if "left" in parts and x0 >= 400 * S:
        # One above the other: wide sides, the hinge across them at the middle.
        draw = ImageDraw.Draw(image)
        for hx0, hx1 in ((0, x0 - 30 * S), (x1 + 30 * S, W)):
            draw.rectangle((hx0, H // 2 - 26 * S, hx1, H // 2 + 26 * S), fill=tuple(int(c * 0.82) for c in body) + (255,))
            draw.rectangle((hx0, H // 2 - 26 * S, hx1, H // 2 - 22 * S), fill=(255, 255, 255, 90))
        lx, rx = x0 // 2, (x1 + W) // 2
        dot_grille(image, (lx, 250 * S), 5, 5, 28 * S, 6 * S, grille)
        dot_grille(image, (rx, 250 * S), 5, 5, 28 * S, 6 * S, grille)
        if pad:
            stick(image, (lx, 690 * S), well=dark, rim=tuple(min(255, c + 30) for c in dark), cap=pad)
            dpad(image, (lx, 900 * S), 170 * S, dark + (255,), tuple(int(c * 0.7) for c in dark) + (255,))
        else:
            dpad(image, (lx, 780 * S), 210 * S, dark + (255,), tuple(int(c * 0.7) for c in dark) + (255,))
        diamond(image, (rx, 760 * S), 64 * S, 38 * S, abxy)
        for k in range(2):
            pill(image, (rx - 50 * S + k * 100 * S, 940 * S), (70 * S, 22 * S), dark, angle=0)
    elif "left" in parts:
        # One screen (4:3) or the 16:9 window: narrow sides.
        wide = x0 >= 200 * S
        lx, rx = x0 // 2, (x1 + W) // 2
        size = 150 * S if wide else 104 * S
        dot_grille(image, (lx, (150 if wide else 210) * S), 3, 4, 24 * S, 5 * S, grille)
        dot_grille(image, (rx, (150 if wide else 210) * S), 3, 4, 24 * S, 5 * S, grille)
        if pad:
            stick(image, (lx, (520 if wide else 480) * S), well=dark, rim=tuple(min(255, c + 30) for c in dark), cap=pad)
        dpad(image, (lx, (740 if wide else 680) * S), size, dark + (255,), tuple(int(c * 0.7) for c in dark) + (255,))
        diamond(image, (rx, (600 if wide else 560) * S), int(size * 0.32), int(size * 0.19), abxy)
        for k in range(2):
            pill(image, (rx, (820 + k * 60) * S if wide else (790 + k * 50) * S), (60 * S, 18 * S), dark, angle=0)
    else:
        # Side by side: the bands above and below.
        cy = (y1 + H) // 2
        dot_grille(image, (220 * S, y0 // 2), 7, 3, 26 * S, 6 * S, grille)
        dot_grille(image, (W - 220 * S, y0 // 2), 7, 3, 26 * S, 6 * S, grille)
        size = min(H - y1 - 30 * S, 150 * S)
        if pad:
            stick(image, (120 * S, cy), well=dark, rim=tuple(min(255, c + 30) for c in dark), cap=pad)
            dpad(image, (300 * S, cy), size, dark + (255,), tuple(int(c * 0.7) for c in dark) + (255,))
        else:
            dpad(image, (200 * S, cy), size, dark + (255,), tuple(int(c * 0.7) for c in dark) + (255,))
        diamond(image, (W - 200 * S, cy), int(size * 0.3), int(size * 0.18), abxy)
        for k in range(2):
            pill(image, (W - 520 * S + k * 100 * S, cy), (70 * S, 22 * S), dark, angle=0)


def nds_console(shape):
    """The Nintendo DS in white: its glossy body, the screens' dark surround, the speakers, the D-pad, A B X Y."""
    rect = NDS_WINDOWS[shape]
    x0, y0, x1, y1 = scaled(rect)
    image = vertical_gradient((W, H), (250, 250, 252), (210, 212, 218))
    gloss(image, 0, W, 60)
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((x0 - 22 * S, y0 - 22 * S, x1 + 22 * S - 1, y1 + 22 * S - 1), radius=18 * S, fill=(52, 52, 58, 255))
    draw.rounded_rectangle((x0 - 22 * S, y0 - 22 * S, x1 + 22 * S - 1, y1 + 22 * S - 1), radius=18 * S,
                           outline=(170, 172, 180, 255), width=2 * S)
    cut_box(image, rect)
    handheld_controls(image, rect, ((236, 237, 240), (70, 72, 80), (224, 226, 232), (60, 62, 70, 255),
                                    (150, 152, 160, 255), None))
    power_led(image, spot(rect, "led"), (90, 220, 120))
    return image


def ds3_console(shape):
    """The Nintendo 3DS in aqua blue: its metallic body, the screens' black face, the speakers, the Circle Pad and
    D-pad, A B X Y."""
    rect = DS3_WINDOWS[shape]
    x0, y0, x1, y1 = scaled(rect)
    image = vertical_gradient((W, H), (74, 178, 214), (18, 86, 128))
    rnd = random.Random(3)
    flakes = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    fdraw = ImageDraw.Draw(flakes)
    for _ in range(1400):
        x, y = rnd.randrange(W), rnd.randrange(H)
        fdraw.point((x, y), fill=(220, 250, 255, rnd.randrange(30, 90)))
    image.alpha_composite(flakes)
    gloss(image, 0, W, 70)
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((x0 - 34 * S, y0 - 34 * S, x1 + 34 * S - 1, y1 + 34 * S - 1), radius=24 * S, fill=(14, 16, 20, 255))
    cut_box(image, rect)
    handheld_controls(image, rect, ((40, 120, 160), (40, 42, 48), (34, 36, 42), (230, 232, 240, 255),
                                    (8, 40, 60, 200), (150, 152, 160)))
    # The 3D slider beside the top screen.
    parts = regions(rect)
    if "right" in parts:
        sx = x1 + 70 * S if x1 + 120 * S < W else W - 40 * S
        sy = 140 * S
        draw = ImageDraw.Draw(image)
        draw.rounded_rectangle((sx - 8 * S, sy, sx + 8 * S, sy + 120 * S), radius=8 * S, fill=(10, 30, 44, 255))
        draw.rounded_rectangle((sx - 14 * S, sy + 70 * S, sx + 14 * S, sy + 100 * S), radius=6 * S, fill=(200, 210, 220, 255))
    power_led(image, spot(rect, "led"), (90, 200, 255))
    return image


def tv_wide():
    """The wooden tube TV with a widescreen tube (16:9, 1440x810, for wide pictures): the speaker and knobs right."""
    image = vertical_gradient((W, H), (96, 64, 38), (58, 37, 22))
    rnd = random.Random(64)
    grain = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    draw = ImageDraw.Draw(grain)
    for _ in range(900):
        x = rnd.randrange(W)
        draw.line((x, 0, x + rnd.randrange(-40, 40), H), fill=(30, 18, 8, rnd.randrange(10, 40)),
                  width=rnd.randrange(1, 4) * S)
    image.alpha_composite(grain.filter(ImageFilter.GaussianBlur(2 * S)))
    sx0, sy0, sx1, sy1 = 110 * S, 120 * S, 1550 * S, 930 * S  # the screen window
    soft_shadow(image, (sx0 - 60 * S, sy0 - 60 * S, sx1 + 60 * S, sy1 + 60 * S), 80 * S, 18 * S, 180)
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((sx0 - 50 * S, sy0 - 50 * S, sx1 + 50 * S, sy1 + 50 * S), radius=80 * S, fill=(22, 20, 19, 255))
    draw.rounded_rectangle((sx0 - 14 * S, sy0 - 14 * S, sx1 + 14 * S, sy1 + 14 * S), radius=52 * S, fill=(8, 8, 8, 255))
    mask = Image.new("L", (W, H), 0)
    ImageDraw.Draw(mask).rounded_rectangle((sx0, sy0, sx1 - 1, sy1 - 1), radius=44 * S, fill=255)
    image.paste(Image.new("RGBA", (W, H), (0, 0, 0, 0)), (0, 0), mask)
    draw = ImageDraw.Draw(image)
    gx0, gx1 = 1660 * S, 1850 * S
    draw.rounded_rectangle((gx0 - 20 * S, 110 * S, gx1 + 20 * S, 560 * S), radius=24 * S, fill=(40, 28, 18, 255))
    for i in range(18):
        y = (130 + i * 23) * S
        draw.rounded_rectangle((gx0, y, gx1, y + 10 * S), radius=5 * S, fill=(16, 11, 7, 255))
    for i, y in enumerate((690, 860)):
        cx, cy, r = 1755 * S, y * S, 62 * S
        soft_shadow(image, (cx - r, cy - r + 8 * S, cx + r, cy + r + 8 * S), r, 10 * S)
        draw = ImageDraw.Draw(image)
        draw.ellipse((cx - r, cy - r, cx + r, cy + r), fill=(44, 42, 40, 255))
        draw.ellipse((cx - r * 0.78, cy - r * 0.78, cx + r * 0.78, cy + r * 0.78), fill=(70, 66, 62, 255))
        draw.line((cx, cy, cx + r * (0.55 if i else 0.0), cy - r * (0.0 if i else 0.6)), fill=(230, 220, 200, 255),
                  width=8 * S)
    centred_text(draw, "EmuSuNi", (830 * S, 1030 * S), 28 * S, (214, 184, 120, 255))
    return image


def frames(name, make):
    """A shared frame in every window: frame-<name>.png and its -169, -wide and -tall windows."""
    return {"frame-" + name + shape: (lambda make=make, rect=rect: make(rect)) for shape, rect in WINDOWS.items()}


BEZELS = {
    "snes-console": snes_console, "snes-sfc": snes_sfc, "snes-purple": snes_purple,
    "nes-console": nes_console, "nes-famicom": nes_famicom, "nes-black": nes_black,
    "n64-console": n64_console, "n64-colors": n64_colors, "n64-atomic": n64_atomic,
    "gb-console": gb_console, "gb-pocket": gb_pocket, "gb-green": gb_green,
    "gbc-console": gbc_console, "gbc-teal": gbc_teal, "gbc-atomic": gbc_atomic,
    "gba-console": gba_console, "gba-glacier": gba_glacier, "gba-black": gba_black,
    "ps1-console": ps1_console, "ps1-psone": ps1_psone, "ps1-dark": ps1_dark,
    "tv": tv,
    **frames("cabinet", frame_cabinet), **frames("minimal", frame_minimal), **frames("neon", frame_neon),
    **frames("wood", frame_wood), **frames("carbon", frame_carbon), **frames("stars", frame_stars),
    "tv-169": tv_wide,
    "sg1000-console": sg1000_console, "sms-console": sms_console, "gg-console": gg_console, "md-console": md_console,
    "psp-console": psp_console, "dc-console": dc_console, "naomi-console": naomi_console,
    "atomiswave-console": atomiswave_console, "saturn-console": saturn_console,
    "pce-console": pce_console,
    **{"nds-console" + shape: (lambda shape=shape: nds_console(shape)) for shape in NDS_WINDOWS},
    **{"3ds-console" + shape: (lambda shape=shape: ds3_console(shape)) for shape in DS3_WINDOWS},
}
# Since 00.63 every bezel is saved as a palette PNG (libimagequant, dithered: half the size or less, the same look;
# the older ones were made smaller that way too, from their files).
PALETTE = set(BEZELS)


def save(image, path, palette):
    if palette:
        try:
            image = image.quantize(colors=256, method=Image.Quantize.LIBIMAGEQUANT, dither=Image.Dither.FLOYDSTEINBERG)
        except (ValueError, OSError):
            print("  (no libimagequant in this Pillow: saved as RGBA)")
    image.save(path, optimize=True)


if __name__ == "__main__":
    # make-bezels.py [names or name prefixes...] [game picture.png for previews]
    args = sys.argv[1:]
    game = None
    if args and args[-1].endswith(".png") and os.path.isfile(args[-1]):
        game = Image.open(args.pop()).convert("RGBA")
    os.makedirs(OUT, exist_ok=True)
    for name, make in BEZELS.items():
        if args and not any(name == a or name.startswith(a) for a in args):
            continue
        image = make().resize((W // S, H // S), Image.LANCZOS)
        path = os.path.join(OUT, name + ".png")
        save(image, path, name in PALETTE)
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
