#!/usr/bin/env python3
# EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
#
# Makes the app's pictures and sounds (all original):
#   assets/xmb/logo.png      the "EmuSuNi" letters (intro)
#   sce_sys/icon0.png        the PS4 home screen icon (512x512)
#   sce_sys/pic0.png, pic1.png  what the PS4 shows while EmuSuNi starts and behind its icon
#                            (1920x1080): the logo in the middle
#   assets/xmb/sounds/*.wav  8-bit menu sounds and the intro jingle: square, triangle and noise
#                            channels with an echo, the way the Super Nintendo's sound chip did it
#
#   python3 scripts/make-assets.py      (needs Pillow)

import math
import os
import random
import struct
import wave

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT = os.path.join(ROOT, "assets/xmb/fonts/Nunito-ExtraBold.ttf")
# The four face buttons of the Super Famicom pad: A red, B yellow, Y green, X blue.
RED, YELLOW, GREEN, BLUE = (229, 72, 77), (242, 194, 48), (61, 180, 109), (63, 120, 216)


# --- Pictures ------------------------------------------------------------------------------------

def emblem(draw, cx, cy, size):
    """The four buttons in a diamond: X top, A right, B bottom, Y left."""
    r, d = size * 0.2, size * 0.3
    for (x, y), colour in (((cx + d, cy), RED), ((cx, cy + d), YELLOW), ((cx - d, cy), GREEN),
                           ((cx, cy - d), BLUE)):
        draw.ellipse((x - r + r * 0.12, y - r + r * 0.16, x + r + r * 0.12, y + r + r * 0.16),
                     fill=(0, 0, 0, 90))
        draw.ellipse((x - r, y - r, x + r, y + r), fill=colour + (255,))
        h = r * 0.32
        draw.ellipse((x - r * 0.3 - h, y - r * 0.3 - h, x - r * 0.3 + h, y - r * 0.3 + h),
                     fill=(255, 255, 255, 80))


def gradient(width, height, top, bottom):
    image = Image.new("RGBA", (width, height))
    pixels = image.load()
    for y in range(height):
        t = y / max(1, height - 1)
        colour = tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)) + (255,)
        for x in range(width):
            pixels[x, y] = colour
    return image


def text_image(text, size, fill=(255, 255, 255, 255), shadow=True):
    font = ImageFont.truetype(FONT, size)
    left, top, right, bottom = font.getbbox(text)
    pad = size // 6
    image = Image.new("RGBA", (right - left + pad * 2, bottom - top + pad * 2), (0, 0, 0, 0))
    if shadow:
        shade = Image.new("RGBA", image.size, (0, 0, 0, 0))
        ImageDraw.Draw(shade).text((pad - left + size // 30, pad - top + size // 20), text,
                                   font=font, fill=(0, 0, 0, 150))
        image.alpha_composite(shade.filter(ImageFilter.GaussianBlur(size / 40)))
    ImageDraw.Draw(image).text((pad - left, pad - top), text, font=font, fill=fill)
    return image


def make_pictures():
    # The letters: plain white, the S and N of "Super Nintendo" a touch larger in spirit (bold).
    logo = text_image("EmuSuNi", 220)
    logo.save(os.path.join(ROOT, "assets/xmb/logo.png"))

    # Drawn in "RGBA" on an RGB picture: the shadows and shines blend in (on an RGBA picture
    # Pillow would write their colours as they are).
    icon = gradient(512, 512, (58, 36, 128), (14, 8, 40)).convert("RGB")
    draw = ImageDraw.Draw(icon, "RGBA")
    emblem(draw, 256, 196, 300)
    word = text_image("EmuSuNi", 104)
    icon.paste(word, ((512 - word.width) // 2, 340), word)
    icon.save(os.path.join(ROOT, "sce_sys/icon0.png"))

    background = gradient(1920, 1080, (42, 26, 94), (8, 4, 22)).convert("RGB")
    draw = ImageDraw.Draw(background, "RGBA")
    random.seed(7)
    for _ in range(160):  # pixels of light
        x, y = random.randrange(1920), random.randrange(1080)
        s = random.choice((3, 4, 6))
        colour = random.choice((RED, YELLOW, GREEN, BLUE, (255, 255, 255)))
        draw.rectangle((x, y, x + s, y + s), fill=colour + (random.randrange(40, 140),))
    # The logo in the middle: the four buttons with the letters under them, centred together.
    word = text_image("EmuSuNi", 150)
    size, gap = 420, 30
    top = (1080 - (size + gap + word.height)) // 2
    emblem(draw, 960, top + size // 2, size)
    background.paste(word, ((1920 - word.width) // 2, top + size + gap), word)
    background.save(os.path.join(ROOT, "sce_sys/pic1.png"))
    background.save(os.path.join(ROOT, "sce_sys/pic0.png"))


# --- Sounds --------------------------------------------------------------------------------------

RATE = 48000


def note(name):
    """'C5' -> Hz."""
    names = {"C": -9, "C#": -8, "D": -7, "D#": -6, "E": -5, "F": -4, "F#": -3, "G": -2, "G#": -1,
             "A": 0, "A#": 1, "B": 2}
    pitch, octave = name[:-1], int(name[-1])
    return 440.0 * 2 ** ((names[pitch] + (octave - 4) * 12) / 12)


class Track:
    def __init__(self, seconds):
        self.samples = [0.0] * int(seconds * RATE)

    def tone(self, start, length, freq, volume=0.3, wave_kind="square", duty=0.5,
             attack=0.004, release=0.05, slide=0.0, vibrato=0.0):
        first = int(start * RATE)
        count = int(length * RATE)
        phase = 0.0
        for i in range(count):
            at = first + i
            if at >= len(self.samples):
                break
            t = i / RATE
            f = freq * (1.0 + slide * t / max(length, 1e-6))
            if vibrato:
                f *= 1.0 + vibrato * math.sin(2 * math.pi * 6.0 * t) * min(1.0, t / 0.3)
            phase = (phase + f / RATE) % 1.0
            if wave_kind == "square":
                value = 1.0 if phase < duty else -1.0
            elif wave_kind == "triangle":
                value = 4.0 * abs(phase - 0.5) - 1.0
                value = round(value * 8) / 8  # the stepped triangle of old chips
            else:  # noise
                value = random.uniform(-1.0, 1.0)
            env = min(1.0, t / attack) if attack > 0 else 1.0
            if t > length - release:
                env *= max(0.0, (length - t) / release)
            self.samples[at] += value * volume * env

    def echo(self, delay=0.12, feedback=0.35, mix=0.35):
        """The SNES DSP's echo: a delayed, fed-back copy."""
        step = int(delay * RATE)
        buffer = list(self.samples)
        for i in range(step, len(buffer)):
            buffer[i] += buffer[i - step] * feedback
        self.samples = [d + (b - d) * mix for d, b in zip(self.samples, buffer)]

    def save(self, name, volume=0.9):
        peak = max(1e-6, max(abs(s) for s in self.samples))
        scale = min(1.0, 0.95 / peak) * volume
        path = os.path.join(ROOT, "assets/xmb/sounds", name + ".wav")
        with wave.open(path, "wb") as out:
            out.setnchannels(1)
            out.setsampwidth(2)
            out.setframerate(RATE)
            out.writeframes(b"".join(struct.pack("<h", int(max(-1.0, min(1.0, s * scale)) * 32767))
                                     for s in self.samples))


def make_sounds():
    random.seed(1)
    t = Track(0.08)
    t.tone(0, 0.05, note("E6"), 0.25, duty=0.25, release=0.03)
    t.save("move", 0.6)

    t = Track(0.16)
    t.tone(0, 0.05, note("A5"), 0.25, duty=0.25)
    t.tone(0.05, 0.08, note("E6"), 0.25, duty=0.25)
    t.save("category", 0.65)

    t = Track(0.3)
    for i, n in enumerate(("C6", "E6", "G6")):
        t.tone(i * 0.05, 0.07, note(n), 0.25, duty=0.25)
    t.tone(0.15, 0.12, note("C7"), 0.2, duty=0.125)
    t.echo(0.06, 0.3, 0.3)
    t.save("confirm", 0.7)

    t = Track(0.24)
    t.tone(0, 0.07, note("G5"), 0.25, duty=0.5)
    t.tone(0.07, 0.12, note("C5"), 0.25, duty=0.5, slide=-0.1)
    t.save("back", 0.6)

    t = Track(0.6)
    for i, n in enumerate(("C5", "E5", "G5", "C6", "E6", "G6", "C7")):
        t.tone(i * 0.035, 0.06, note(n), 0.22, duty=0.25)
    t.tone(0.25, 0.3, note("C7"), 0.18, duty=0.125, vibrato=0.01)
    t.tone(0.0, 0.3, note("C3"), 0.25, wave_kind="triangle", slide=1.0)
    t.echo(0.08, 0.35, 0.4)
    t.save("launch", 0.8)

    t = Track(0.3)
    t.tone(0, 0.11, note("A#2"), 0.3, duty=0.5)
    t.tone(0.14, 0.14, note("A2"), 0.3, duty=0.5)
    t.save("error", 0.6)

    t = Track(0.14)
    t.tone(0, 0.1, note("A5"), 0.3, wave_kind="triangle", slide=-0.25, release=0.06)
    t.save("menu", 0.7)

    # The intro jingle (2.4 s), with the animation: the four buttons land on four notes
    # (~0.43, 0.55, 0.67, 0.79 s), a shimmer when the ring of light bursts, then the chord while
    # the letters rise.
    t = Track(2.6)
    lands = (0.43, 0.55, 0.67, 0.79)
    for at, n in zip(lands, ("C5", "E5", "G5", "C6")):
        t.tone(at, 0.16, note(n), 0.24, duty=0.25, release=0.08)
        t.tone(at, 0.03, 120, 0.12, wave_kind="noise", release=0.02)  # a soft "tap"
    for i, n in enumerate(("E6", "G6", "C7", "E7", "G7", "C8")):
        t.tone(0.88 + i * 0.03, 0.08, note(n), 0.10, duty=0.125)
    # Bass walking up under the buttons.
    for at, n in zip(lands, ("C3", "E3", "G3", "C4")):
        t.tone(at, 0.12, note(n), 0.3, wave_kind="triangle")
    # Final chord: C major with a lifting fifth, vibrato on the lead.
    t.tone(1.08, 1.3, note("C3"), 0.32, wave_kind="triangle", release=0.6)
    t.tone(1.08, 1.3, note("E5"), 0.13, duty=0.5, release=0.7, vibrato=0.006)
    t.tone(1.08, 1.3, note("G5"), 0.13, duty=0.5, release=0.7, vibrato=0.006)
    t.tone(1.08, 1.35, note("C6"), 0.16, duty=0.25, release=0.75, vibrato=0.008)
    t.tone(1.32, 1.1, note("G6"), 0.07, duty=0.125, release=0.7, vibrato=0.01)
    t.echo(0.13, 0.4, 0.45)
    t.save("intro", 0.85)


if __name__ == "__main__":
    os.makedirs(os.path.join(ROOT, "sce_sys"), exist_ok=True)
    os.makedirs(os.path.join(ROOT, "assets/xmb/sounds"), exist_ok=True)
    make_pictures()
    make_sounds()
    print("assets written")
