#!/usr/bin/env python3
"""TouchEQ 8 skin: writes Plugin Skins (TUI.json, Q-Links.json, every PNG) from code, plus screen previews.

The MPC draws plugin screens from JSON and images only, so the live EQ graph is assembled from widgets:

  layer 1  analyser   96 vertical TrackAndThumb sliders bound to Spectrum 01..96, revealed from the bottom
  layer 2  curve      192 vertical TrackAndThumb sliders bound to Curve 001..192, revealed from the 0 dB line
                      (revealType Bipolar) with a bright cap at the value: together they draw the filled EQ curve
  layer 3  shield     an empty component over the plot that swallows touches, so no column is ever dragged
  layer 4  points     per band, an XY pad (x = Freq, y = Gain, or Q for cut/notch types), coloured, display only
  layer 5  grab pad   the selected band's XY pad (Band parameter, set by the 1-8 buttons), on top: the finger drags it.

Band-number buttons (1-8): select the band and, while QLink Follow is on, bank the Q-Links to that band's row.

Tabs: BAND 1-4, BAND 5-8 (one row of four knobs per band = one Q-Link bank: Freq, Gain, Q, Type), OUTPUT
(Output, Scale, Mix, Band, then the Adapt Q, Analyzer and QLink Follow switches: touch-only but still parameters).

Usage:  make_skin.py <build/params.json> <out skins dir> [--preview <dir>]
"""
import argparse
import json
import math
import os
import shutil
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", ".deps", "mpc-plugin-builder-sdk", "tools"))
from gen_ui import (Obj, obj, dump_json, entry, component_definition, action, knob_filmstrip_data,  # noqa: E402
                    button_data, knob_overlay_data, menu_overlay_data, overlay_component)

FONT_SEMI = os.path.join(HERE, "fonts", "BarlowSemiCondensed-SemiBold.ttf")
FONT_MED = os.path.join(HERE, "fonts", "BarlowSemiCondensed-Medium.ttf")

# ------------------------------------------------------------------------------------------------------------------
# Parameter layout: a mirror of plugins/TouchEQ8/src/TouchEQ8.cpp (checked against build/params.json below).
# ------------------------------------------------------------------------------------------------------------------
P_OUTPUT, P_SCALE, P_MIX, P_EDIT, P_ADAPT, P_ANALYZER = 0, 1, 2, 3, 4, 5
B0, PER = 6, 5
ON, TYPE, FREQ, GAIN, Q = 0, 1, 2, 3, 4
NBANDS, NC, NA = 8, 192, 96
CURVE0 = B0 + NBANDS * PER
SPEC0 = CURVE0 + NC
P_FOLLOW = SPEC0 + NA          # QLink Follow (0.1.2)
PICK0 = P_FOLLOW + 1           # Pick 1..8: momentary triggers for the band-number buttons (0.1.2)
NPARAMS = PICK0 + NBANDS
TYPES = ["LoCut48", "LoCut12", "LoShelf", "Bell", "HiShelf", "Notch", "HiCut12", "HiCut48"]
FMIN, FMAX, GRANGE, SPEC_FLOOR = 20.0, 20000.0, 15.0, -90.0


def bp(b, which):
    return B0 + b * PER + which


def H(i):
    return "Parameter %d" % i


# If the hardware shows the points upside down (dragging up lowers the gain), set this to True and rebuild:
# the pads then bind y through the skin's Invert/ modifier.
XY_INVERT_Y = False

# ------------------------------------------------------------------------------------------------------------------
# Geometry (tab area 1280 x 628)
# ------------------------------------------------------------------------------------------------------------------
W, HT = 1280, 628
PX, PY, PW, PH = 64, 36, 1152, 264          # the plot
CW, AW = PW // NC, PW // NA                 # 6 px curve columns, 12 px analyser columns
GRAPH_H = 330                               # the graph panel (header, plot, frequency labels)
ROW_Y0, ROW_H = 334, 72
CELL_X = [182, 452, 722, 992]
CELL_W, CELL_H = 262, 60
KNOB = 52
TYPE_W, TYPE_H = 72, 48
SEL = 54
PWR = 44

# ------------------------------------------------------------------------------------------------------------------
# Palette
# ------------------------------------------------------------------------------------------------------------------
BAND_RGB = [(255, 92, 92), (255, 159, 67), (255, 214, 64), (107, 219, 107),
            (77, 210, 255), (92, 141, 255), (166, 107, 255), (255, 107, 214)]
CURVE_RGB = (255, 211, 107)
SPEC_RGB = (124, 108, 230)
PANEL_TOP, PANEL_BOT = (29, 31, 41), (16, 17, 23)
LOWER = (21, 22, 28)
CARD = (31, 33, 42)
TEXT = (234, 236, 244)
MUTED = (139, 144, 166)
GRID = (255, 255, 255)


def argb(rgb, a=255):
    return "%02x%02x%02x%02x" % (a, rgb[0], rgb[1], rgb[2])


def font(path, size):
    return ImageFont.truetype(path, size)


# ------------------------------------------------------------------------------------------------------------------
# The EQ maths, for the previews (same equations as common/toucheq_engine.h)
# ------------------------------------------------------------------------------------------------------------------
BW8 = [0.50979558, 0.60134489, 0.89997622, 2.56291545]
FS = 44100.0


def q_from_norm(u):
    return 0.1 * (0.70710678 / 0.1) ** (2 * u) if u <= 0.5 else 0.70710678 * (18 / 0.70710678) ** (2 * u - 1)


def norm_from_q(q):
    return 0.5 * math.log(q / 0.1) / math.log(0.70710678 / 0.1) if q <= 0.70710678 else \
        0.5 + 0.5 * math.log(q / 0.70710678) / math.log(18 / 0.70710678)


def design(t, f, g, q):
    gg = math.tan(math.pi * min(max(f, 10), 0.45 * FS) / FS)
    secs = []
    if t in (1, 6):
        k = 1 / q
        secs.append((gg, k) + ((1, -k, -1) if t == 1 else (0, 0, 1)))
    elif t in (0, 7):
        for i in range(4):
            qi = BW8[i] * (q / 0.70710678 if i == 3 else 1)
            k = 1 / max(qi, 0.05)
            secs.append((gg, k) + ((1, -k, -1) if t == 0 else (0, 0, 1)))
    elif t == 5:
        k = 1 / q
        secs.append((gg, k, 1, -k, 0))
    elif t == 2:
        A = 10 ** (g / 40); k = 1 / q
        secs.append((gg / math.sqrt(A), k, 1, k * (A - 1), A * A - 1))
    elif t == 4:
        A = 10 ** (g / 40); k = 1 / q
        secs.append((gg * math.sqrt(A), k, A * A, k * (1 - A) * A, 1 - A * A))
    else:
        A = 10 ** (g / 40); k = 1 / (q * A)
        secs.append((gg, k, 1, k * (A * A - 1), 0))
    return secs


def response_db(bands, f):
    t = math.tan(math.pi * min(f, 0.49 * FS) / FS)
    db = 0.0
    for on, typ, fr, g, q in bands:
        if not on:
            continue
        for (gg, k, m0, m1, m2) in design(typ, fr, g, q):
            w = t / gg
            nr, ni = (m0 + m2) - m0 * w * w, (m0 * k + m1) * w
            dr, di = 1 - w * w, k * w
            db += 10 * math.log10(max((nr * nr + ni * ni) / max(dr * dr + di * di, 1e-30), 1e-20))
    return db


def col_freq(i, n):
    return FMIN * (FMAX / FMIN) ** ((i + 0.5) / n)


def x_of_freq(f):
    return PX + PW * math.log(f / FMIN) / math.log(FMAX / FMIN)


def y_of_db(db):
    return PY + PH * (0.5 - db / (2 * GRANGE))


# ------------------------------------------------------------------------------------------------------------------
# Images
# ------------------------------------------------------------------------------------------------------------------
SS = 4   # supersampling for anything round


def vgradient(w, h, top, bot):
    im = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(im)
    for y in range(h):
        t = y / max(h - 1, 1)
        d.line([(0, y), (w, y)], fill=tuple(int(top[i] + (bot[i] - top[i]) * t) for i in range(3)))
    return im


def draw_graph_panel(im):
    """Header, plot background, grid and axis labels, into the tab background."""
    d = ImageDraw.Draw(im, "RGBA")
    panel = vgradient(W, GRAPH_H, PANEL_TOP, PANEL_BOT)
    im.paste(panel, (0, 0))
    # plot well: slightly darker with a soft inner vignette
    well = vgradient(PW, PH, (24, 25, 34), (13, 14, 19))
    im.paste(well, (PX, PY))
    vig = Image.new("L", (PW, PH), 0)
    vd = ImageDraw.Draw(vig)
    for i in range(40):
        vd.rectangle([i, i, PW - 1 - i, PH - 1 - i], outline=int(70 * (1 - i / 40) ** 2))
    shade = Image.new("RGB", (PW, PH), (0, 0, 0))
    im.paste(shade, (PX, PY), vig.point(lambda v: v // 2))
    d = ImageDraw.Draw(im, "RGBA")
    fsm = font(FONT_MED, 17)
    # frequency grid
    for dec in (10, 100, 1000, 10000):
        for m in range(1, 10):
            f = dec * m
            if f < FMIN or f > FMAX:
                continue
            x = round(x_of_freq(f))
            major = m == 1
            d.line([(x, PY), (x, PY + PH - 1)], fill=GRID + ((34 if major else 15),))
    for f, txt in ((20, "20"), (50, "50"), (100, "100"), (200, "200"), (500, "500"), (1000, "1k"), (2000, "2k"),
                   (5000, "5k"), (10000, "10k"), (20000, "20k")):
        x = x_of_freq(f)
        tw = d.textlength(txt, font=fsm)
        x0 = min(max(x - tw / 2, PX), PX + PW - tw)
        d.text((x0, PY + PH + 5), txt, font=fsm, fill=MUTED)
    # dB grid (every 3 dB), labels every 6
    for db in range(-15, 16, 3):
        y = round(y_of_db(db))
        if db == 0:
            d.line([(PX, y), (PX + PW - 1, y)], fill=(255, 255, 255, 70))
        elif db in (-15, 15):
            continue
        else:
            d.line([(PX, y), (PX + PW - 1, y)], fill=GRID + ((24 if db % 6 == 0 else 11),))
        if db % 6 == 0 or db in (-15, 15):
            txt = ("+%d" % db) if db > 0 else str(db)
            tw = d.textlength(txt, font=fsm)
            d.text((PX - 10 - tw, y - 11), txt, font=fsm, fill=CURVE_RGB if db == 0 else MUTED)
    # analyser scale on the right (dim)
    for lv in (0, -20, -40, -60, -80):
        y = PY + PH * (lv / SPEC_FLOOR)
        txt = str(lv)
        d.text((PX + PW + 8, y - 10), txt, font=font(FONT_MED, 15), fill=SPEC_RGB + (170,))
    d.rectangle([PX - 1, PY - 1, PX + PW, PY + PH], outline=(255, 255, 255, 40))
    # header: wordmark and hint
    d.text((18, 4), "TOUCHEQ", font=font(FONT_SEMI, 25), fill=TEXT)
    tw = d.textlength("TOUCHEQ", font=font(FONT_SEMI, 25))
    d.text((18 + tw + 5, 4), "8", font=font(FONT_SEMI, 25), fill=CURVE_RGB)
    d.text((18 + tw + 24, 11), "GREASYCRATEZ", font=font(FONT_MED, 14), fill=MUTED)
    hint = "TAP 1-8 TO PICK A POINT, THEN DRAG IT"
    hf = font(FONT_MED, 15)
    d.text((W - 18 - d.textlength(hint, font=hf), 10), hint, font=hf, fill=(MUTED[0], MUTED[1], MUTED[2], 200))
    # header readout well (the labels sit on it)
    d.rounded_rectangle([300, 3, 860, 31], radius=8, fill=(0, 0, 0, 70), outline=(255, 255, 255, 22))


def draw_lower_panel(im, rows):
    """rows: list of (band index or None, caption) for each Q-Link row."""
    d = ImageDraw.Draw(im, "RGBA")
    d.rectangle([0, GRAPH_H, W, HT], fill=LOWER)
    d.line([(0, GRAPH_H), (W, GRAPH_H)], fill=(255, 255, 255, 18))
    for r, (band, caption) in enumerate(rows):
        y = ROW_Y0 + r * ROW_H
        d.rounded_rectangle([8, y + 4, W - 8, y + ROW_H - 4], radius=12, fill=CARD + (255,), outline=(255, 255, 255, 14))
        if band is not None:
            c = BAND_RGB[band]
            d.rounded_rectangle([8, y + 4, 14, y + ROW_H - 4], radius=3, fill=c + (255,))
        for x in CELL_X[1:]:
            d.line([(x - 4, y + 16), (x - 4, y + ROW_H - 16)], fill=(255, 255, 255, 16))
        if caption:
            d.text((22, y + 22), caption, font=font(FONT_SEMI, 21), fill=MUTED)


def knob_strip(colour, bipolar, size=KNOB, frames=127):
    big = size * SS
    strip = Image.new("RGBA", (size, size * frames), (0, 0, 0, 0))
    a0, a1 = 135.0, 405.0   # PIL angles: 0 = 3 o'clock, clockwise
    base = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    d = ImageDraw.Draw(base)
    c = big / 2
    rr = big * 0.47
    tr = big * 0.06
    d.arc([c - rr, c - rr, c + rr, c + rr], a0, a1, fill=(255, 255, 255, 38), width=int(tr))
    body = big * 0.34
    # body: dark disc with a soft top highlight
    for i in range(int(body), 0, -1):
        t = i / body
        g = int(30 + 22 * (1 - t))
        d.ellipse([c - i, c - i - (1 - t) * big * 0.02, c + i, c + i], fill=(g, g + 1, g + 6, 255))
    d.ellipse([c - body, c - body, c + body, c + body], outline=(255, 255, 255, 40), width=SS)
    for fi in range(frames):
        v = fi / (frames - 1)
        im = base.copy()
        dd = ImageDraw.Draw(im)
        ang = a0 + (a1 - a0) * v
        if bipolar:
            mid = (a0 + a1) / 2
            lo, hi = (ang, mid) if ang < mid else (mid, ang)
            if hi - lo > 0.5:
                dd.arc([c - rr, c - rr, c + rr, c + rr], lo, hi, fill=colour + (255,), width=int(tr))
        elif ang - a0 > 0.5:
            dd.arc([c - rr, c - rr, c + rr, c + rr], a0, ang, fill=colour + (255,), width=int(tr))
        rad = math.radians(ang)
        p0 = (c + math.cos(rad) * body * 0.30, c + math.sin(rad) * body * 0.30)
        p1 = (c + math.cos(rad) * body * 0.86, c + math.sin(rad) * body * 0.86)
        dd.line([p0, p1], fill=(245, 246, 250, 255), width=int(SS * 2.6))
        strip.paste(im.resize((size, size), Image.LANCZOS), (0, fi * size))
    return strip


def type_icon(t, colour, w=TYPE_W, h=TYPE_H):
    """A small picture of filter type t (the frame of the Type knob)."""
    big = Image.new("RGBA", (w * SS, h * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(big)
    bw, bh = w * SS, h * SS
    d.rounded_rectangle([SS, SS, bw - SS, bh - SS], radius=9 * SS, fill=(14, 15, 20, 255), outline=colour + (90,), width=SS)
    gain = {2: 9, 3: 10, 4: 9}.get(t, 0)
    pts = []
    for i in range(64):
        f = FMIN * (FMAX / FMIN) ** (i / 63)
        db = response_db([(True, t, 1000.0 if t not in (0, 1, 6, 7) else (300.0 if t in (0, 1) else 3000.0), gain,
                           2.0 if t == 5 else 0.9)], f)
        db = max(min(db, 15), -15)
        x = (8 + (w - 16) * i / 63) * SS
        y = (h / 2 - db / 15 * (h / 2 - 9)) * SS
        pts.append((x, y))
    d.line([(8 * SS, bh / 2), (bw - 8 * SS, bh / 2)], fill=(255, 255, 255, 40), width=SS)
    d.line(pts, fill=colour + (255,), width=int(2.6 * SS), joint="curve")
    return big.resize((w, h), Image.LANCZOS)


def puck(colour, n, size, state):
    """state: 'on', 'off' or 'sel'."""
    big = size * SS
    im = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    c = big / 2
    r = big * (0.30 if state != "sel" else 0.27)
    if state == "sel":
        glow = Image.new("RGBA", (big, big), (0, 0, 0, 0))
        gd = ImageDraw.Draw(glow)
        gd.ellipse([c - big * 0.46, c - big * 0.46, c + big * 0.46, c + big * 0.46], fill=colour + (110,))
        im = Image.alpha_composite(im, glow.filter(ImageFilter.GaussianBlur(big * 0.07)))
        d = ImageDraw.Draw(im)
        d.ellipse([c - big * 0.40, c - big * 0.40, c + big * 0.40, c + big * 0.40], outline=(255, 255, 255, 255), width=int(SS * 2.4))
    if state == "off":
        d.ellipse([c - r, c - r, c + r, c + r], fill=(20, 21, 27, 200), outline=colour + (150,), width=int(SS * 2))
        tc = colour + (170,)
    else:
        d.ellipse([c - r, c - r, c + r, c + r], fill=colour + (255,), outline=(255, 255, 255, 120), width=SS)
        tc = (16, 16, 20, 255)
    f = font(FONT_SEMI, int(r * 1.35))
    txt = str(n)
    tw = d.textlength(txt, font=f)
    d.text((c - tw / 2, c - r * 0.92), txt, font=f, fill=tc)
    return im.resize((size, size), Image.LANCZOS)


def band_button(colour, n, selected):
    big = SEL * SS
    im = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    c = big / 2
    if selected:
        d.ellipse([SS, SS, big - SS, big - SS], outline=(255, 255, 255, 255), width=int(SS * 2.5))
        d.ellipse([big * 0.13, big * 0.13, big * 0.87, big * 0.87], fill=colour + (255,))
        tc = (14, 14, 18, 255)
    else:
        d.ellipse([big * 0.13, big * 0.13, big * 0.87, big * 0.87], fill=(14, 15, 20, 255), outline=colour + (230,), width=int(SS * 2.5))
        tc = colour + (255,)
    f = font(FONT_SEMI, int(big * 0.42))
    tw = d.textlength(str(n), font=f)
    d.text((c - tw / 2, c - big * 0.27), str(n), font=f, fill=tc)
    return im.resize((SEL, SEL), Image.LANCZOS)


def power_button(colour, on):
    big = PWR * SS
    im = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    c = big / 2
    d.rounded_rectangle([SS, SS, big - SS, big - SS], radius=big * 0.28, fill=(14, 15, 20, 255),
                        outline=(colour + (200,)) if on else (255, 255, 255, 40), width=SS * 2)
    col = colour + (255,) if on else (110, 114, 130, 255)
    r = big * 0.22
    d.arc([c - r, c - r + SS, c + r, c + r + SS], 300, 600, fill=col, width=int(SS * 3))
    d.line([(c, c - r - SS * 2), (c, c - SS)], fill=col, width=int(SS * 3))
    if on:
        glow = im.filter(ImageFilter.GaussianBlur(SS * 3))
        im = Image.alpha_composite(glow, im)
    return im.resize((PWR, PWR), Image.LANCZOS)


def toggle_img(on):
    w, h = 84, 44
    big = Image.new("RGBA", (w * SS, h * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(big)
    d.rounded_rectangle([SS, SS, w * SS - SS, h * SS - SS], radius=h * SS / 2 - SS,
                        fill=(CURVE_RGB + (255,)) if on else (40, 42, 52, 255))
    kx = (w - h / 2 - 2) * SS if on else (h / 2 + 2) * SS
    r = (h / 2 - 6) * SS
    d.ellipse([kx - r, h * SS / 2 - r, kx + r, h * SS / 2 + r], fill=(20, 20, 26, 255) if on else (170, 174, 190, 255))
    return big.resize((w, h), Image.LANCZOS)


def curve_fill():
    """Revealed between the 0 dB line and the value: transparent at 0 dB, stronger toward the extremes."""
    im = Image.new("RGBA", (CW, PH), (0, 0, 0, 0))
    px = im.load()
    for y in range(PH):
        t = abs(y + 0.5 - PH / 2) / (PH / 2)
        a = int(18 + 92 * t ** 0.8)
        for x in range(CW):
            px[x, y] = CURVE_RGB + (a,)
    return im


def curve_cap():
    im = Image.new("RGBA", (CW, 6), (0, 0, 0, 0))
    px = im.load()
    for y, a in enumerate((60, 200, 255, 255, 200, 60)):
        for x in range(CW):
            px[x, y] = (255, 236, 180, a)
    return im


def spec_fill():
    im = Image.new("RGBA", (AW, PH), (0, 0, 0, 0))
    px = im.load()
    for y in range(PH):
        t = 1 - y / PH   # 1 at the top
        a = int(14 + 50 * t)
        for x in range(AW - 1):   # 1 px gap between analyser columns
            px[x, y] = SPEC_RGB + (a,)
    return im


def spec_cap():
    im = Image.new("RGBA", (AW, 2), (0, 0, 0, 0))
    px = im.load()
    for x in range(AW - 1):
        px[x, 0] = (170, 158, 255, 150)
        px[x, 1] = (170, 158, 255, 60)
    return im


# ------------------------------------------------------------------------------------------------------------------
# TUI pieces
# ------------------------------------------------------------------------------------------------------------------
FONT_NAME = "Titillium Web"


def label(kind, colour, height, just="left verticallyCentred", style="Regular", text=None, handle="Data"):
    pairs = [("version", 1),
             ("textStyle", obj(("version", 1),
                               ("font", obj(("version", 1), ("name", FONT_NAME), ("style", style), ("height", float(height)))),
                               ("colour", colour), ("justification", just), ("case", "Original"))),
             ("type", kind), ("handleName", handle)]
    if text is not None:
        pairs.append(("text", text))
    return Obj(pairs)


def focus_child(w, h):
    return entry("focus", "Focus", obj(("version", 1), ("backgroundColour", "14ffffff"), ("outlineColour", argb(CURVE_RGB)),
                                       ("backgroundInset", 2.0), ("outlineThickness", 1.0)),
                 "0 0 %d %d" % (w, h), "No", "Show", "WhenFocussed")


def acts(overlay, double_click=True, qlink=True):
    a = []
    if qlink:
        a.append(action("Mouse Down", "Q-Link", "Data", ""))
    if double_click:
        a.append(action("Double Click", "Show Overlay", "Data", overlay))
    a.append(action("Enter Pressed", "Show Overlay", "Data", overlay))
    return a


def knob_cell(strip, overlay):
    ky = (CELL_H - KNOB) // 2
    tx = 6 + KNOB + 12
    children = [focus_child(CELL_W, CELL_H),
                entry("knob", "Knob", knob_filmstrip_data(strip, 127), "6 %d %d %d" % (ky, KNOB, KNOB), "No", "Show", "Always"),
                entry("name", "Label", label("Name", argb(MUTED), 21.0), "%d 3 %d 26" % (tx, CELL_W - tx - 6), "No", "Show", "Always"),
                entry("value", "Label", label("Value", argb(TEXT), 26.0, style="SemiBold"), "%d 27 %d 31" % (tx, CELL_W - tx - 6),
                      "No", "Show", "Always")]
    return component_definition(acts(overlay), children, "00000000")


def type_cell(strip):
    ty = (CELL_H - TYPE_H) // 2
    tx = 6 + TYPE_W + 12
    children = [focus_child(CELL_W, CELL_H),
                entry("icon", "Knob", knob_filmstrip_data(strip, 8), "6 %d %d %d" % (ty, TYPE_W, TYPE_H), "No", "Show", "Always"),
                entry("name", "Label", label("Name", argb(MUTED), 21.0), "%d 3 %d 26" % (tx, CELL_W - tx - 6), "No", "Show", "Always"),
                entry("value", "Label", label("Value", argb(TEXT), 26.0, style="SemiBold"), "%d 27 %d 31" % (tx, CELL_W - tx - 6),
                      "No", "Show", "Always")]
    return component_definition(acts("menu overlay"), children, "00000000")


def toggle_cell():
    w = CELL_W
    children = [focus_child(w, CELL_H),
                entry("name", "Label", label("Name", argb(MUTED), 21.0), "12 3 150 26", "No", "Show", "Always"),
                entry("value", "Label", label("Value", argb(TEXT), 26.0, style="SemiBold"), "12 27 150 31", "No", "Show", "Always"),
                entry("toggle", "Button", button_data("toggle_on.png", "toggle_off.png", 1, 1), "%d 8 84 44" % (w - 96), "No", "Show", "Always")]
    return component_definition(acts("menu overlay", double_click=False, qlink=False), children, "00000000")


def button_wrapper(img_on, img_off, button_id, group, size):
    """A plain Button in a component (the power switches)."""
    children = [entry("button", "Button", button_data(img_on, img_off, button_id, group), "0 0 %d %d" % (size, size),
                      "No", "Show", "Always")]
    return component_definition([], children, "00000000")


def band_pick(b, follow):
    """Band-number button. A Button child would take the touch for itself (and the component's own actions would
    never fire), so the button is a film-strip Image of Band (lit on frame b) and the component does the work:
    Toggle Switch on the Pick b trigger (the plugin then sets Band = b) and, with QLink Follow on, a Q-Link action
    that banks the knobs to band b's row."""
    strip = obj(("version", 2), ("imageType", "FilmStrip"), ("colour", "0"), ("image", "sel%d_strip.png" % (b + 1)),
                ("numFrames", NBANDS), ("handleName", "Data"))
    children = [entry("lamp", "Image", strip, "0 0 %d %d" % (SEL, SEL), "No", "Show", "Always")]
    acts_ = [action("Mouse Down", "Toggle Switch", H(PICK0 + b), "")]
    if follow:
        acts_.append(action("Mouse Down", "Q-Link", H(bp(b, FREQ)), ""))
    return component_definition(acts_, children, "00000000")


def slider_column(name, param, x, w, fill, cap, reveal, hide_when=None):
    data = obj(("version", 4), ("sliderType", "TrackAndThumb"), ("direction", "Vertical"), ("thumbImage", cap),
               ("thumbTrackVisible", False), ("revealedImage", fill), ("revealType", reveal), ("handleName", "Data"))
    return entry(name, "Slider", data, "%d %d %d %d" % (x, PY, w, PH), "No", "Show", "Always",
                 remap=[("Data", H(param))], invalidating=hide_when)


GROUPS = [  # (type range test, y uses gain?)
    ("RangedEnabling/2/4/8/%s", True),    # LoShelf, Bell, HiShelf: height = Gain
    ("RangedEnabling/0/1/8/%s", False),   # LoCut48, LoCut12: height = Q (resonance)
    ("RangedEnabling/5/7/8/%s", False),   # Notch, HiCut12, HiCut48: height = Q
]


def y_handle(b, uses_gain):
    h = H(bp(b, GAIN if uses_gain else Q))
    return "Invert/" + h if XY_INVERT_Y else h


def xypad(image, b, uses_gain):
    return obj(("version", 1), ("xPositionHandle", H(bp(b, FREQ))), ("yPositionHandle", y_handle(b, uses_gain)),
               ("image", image))


PLOT = "%d %d %d %d" % (PX, PY, PW, PH)


def graph_component():
    ch = []
    for i in range(NA):
        ch.append(slider_column("spec %02d" % (i + 1), SPEC0 + i, PX + i * AW, AW, "sp_fill.png", "sp_cap.png", "Normal",
                                ["IndexedEnabling/1/2/%s" % H(P_ANALYZER)]))
    for i in range(NC):
        ch.append(slider_column("curve %03d" % (i + 1), CURVE0 + i, PX + i * CW, CW, "cv_fill.png", "cv_cap.png", "Bipolar"))
    ch.append(entry("shield", "touchShield", obj(("version", 1), ("handleName", "Data")), "0 %d %d %d" % (PY - 6, W, PH + 12),
                    "No", "Show", "Always"))
    for b in range(NBANDS):
        for gi, (cond, uses_gain) in enumerate(GROUPS):
            for state in (0, 1):
                ch.append(entry("point %d %s %s" % (b + 1, "gain" if uses_gain else "q%d" % gi, "on" if state else "off"), "XYPad",
                                xypad("pt%d_%s.png" % (b + 1, "on" if state else "off"), b, uses_gain), PLOT, "No", "Show", "Always",
                                invalidating=[cond % H(bp(b, TYPE)), "IndexedEnabling/%d/2/%s" % (state, H(bp(b, ON)))]))
    for b in range(NBANDS):
        for gi, (cond, uses_gain) in enumerate(GROUPS):
            ch.append(entry("grab %d %d" % (b + 1, gi), "grabPad%d_%d" % (b + 1, gi), obj(("version", 1), ("handleName", "Data")),
                            PLOT, "No", "Show", "Always",
                            invalidating=[cond % H(bp(b, TYPE)), "IndexedEnabling/%d/8/%s" % (b, H(P_EDIT))]))
    # readout of the selected band in the header
    for b in range(NBANDS):
        sel = ["IndexedEnabling/%d/8/%s" % (b, H(P_EDIT))]
        ch.append(entry("readout %d name" % (b + 1), "Label", label("Text", argb(BAND_RGB[b]), 22.0, style="SemiBold",
                                                                    text="BAND %d" % (b + 1), handle="Invalid"),
                        "314 3 80 28", "No", "Show", "Always", invalidating=sel))
        for j, (param, x, w) in enumerate(((bp(b, TYPE), 396, 96), (bp(b, FREQ), 494, 120), (bp(b, GAIN), 616, 110),
                                           (bp(b, Q), 728, 120))):
            hdl = ("WithPrefix[Q ]/" + H(param)) if j == 3 else H(param)
            ch.append(entry("readout %d %d" % (b + 1, j), "Label", label("Value", argb(TEXT), 22.0), "%d 3 %d 28" % (x, w),
                            "No", "Show", "Always", remap=[("Data", hdl)], invalidating=sel))
    return component_definition([], ch, "00000000")


def grab_pad(b, gi, uses_gain):
    child = entry("pad", "XYPad", xypad("pt%d_sel.png" % (b + 1), b, uses_gain), "0 0 %d %d" % (PW, PH), "No", "Show", "Always")
    return component_definition([], [child], "00000000")


def band_row(children, b, r):
    y = ROW_Y0 + r * ROW_H
    for follow in (1, 0):   # one of the two is shown, by QLink Follow
        children.append(entry("select %d %s" % (b + 1, "follow" if follow else "stay"), "bandPick%d%s" % (b + 1, "F" if follow else ""),
                              obj(("version", 1), ("handleName", "Data")),
                              "%d %d %d %d" % (24, y + (ROW_H - SEL) // 2, SEL, SEL), "No", "Show", "Always",
                              remap=[("Data", H(P_EDIT))], invalidating=["IndexedEnabling/%d/2/%s" % (follow, H(P_FOLLOW))]))
    children.append(entry("power %d" % (b + 1), "bandPwr%d" % (b + 1), obj(("version", 1), ("handleName", "Data")),
                          "%d %d %d %d" % (24 + SEL + 18, y + (ROW_H - PWR) // 2, PWR, PWR), "No", "Show", "Always",
                          remap=[("Data", H(bp(b, ON)))]))
    cy = y + (ROW_H - CELL_H) // 2
    for c, (which, comp) in enumerate(((FREQ, "knobUni%d" % (b + 1)), (GAIN, "knobBi%d" % (b + 1)),
                                       (Q, "knobUni%d" % (b + 1)), (TYPE, "typeKnob%d" % (b + 1)))):
        children.append(entry("%s %d" % (("Freq", "Gain", "Q", "Type")[c], b + 1), comp, obj(("version", 1), ("handleName", "Data")),
                              "%d %d %d %d" % (CELL_X[c], cy, CELL_W, CELL_H), "Yes", "Hide", "Always", remap=[("Data", H(bp(b, which)))]))


def qrect(r):
    y = ROW_Y0 + r * ROW_H + (ROW_H - CELL_H) // 2
    return "%d %d %d %d" % (CELL_X[0] - 6, y - 4, CELL_X[3] + CELL_W + 6 - (CELL_X[0] - 6), CELL_H + 8)


def build_tui():
    defs = []
    for b in range(NBANDS):
        defs.append(("knobUni%d" % (b + 1), knob_cell("knob_uni_%d.png" % (b + 1), "knob overlay")))
        defs.append(("knobBi%d" % (b + 1), knob_cell("knob_bi_%d.png" % (b + 1), "knob overlay bipolar")))
        defs.append(("typeKnob%d" % (b + 1), type_cell("type_%d.png" % (b + 1))))
        defs.append(("bandPick%dF" % (b + 1), band_pick(b, True)))
        defs.append(("bandPick%d" % (b + 1), band_pick(b, False)))
        defs.append(("bandPwr%d" % (b + 1), button_wrapper("pwr%d_on.png" % (b + 1), "pwr%d_off.png" % (b + 1), 1, 1, PWR)))
        for gi, (_, uses_gain) in enumerate(GROUPS):
            defs.append(("grabPad%d_%d" % (b + 1, gi), grab_pad(b, gi, uses_gain)))
    defs.append(("knobOut", knob_cell("knob_out_uni.png", "knob overlay")))
    defs.append(("knobOutBi", knob_cell("knob_out_bi.png", "knob overlay bipolar")))
    defs.append(("knobOutMenu", knob_cell("knob_out_uni.png", "menu overlay")))
    defs.append(("toggleCell", toggle_cell()))
    defs.append(("touchShield", component_definition([], [], "00000000")))
    defs.append(("eqGraph", graph_component()))
    bgc = argb(LOWER)
    defs.append(("knob overlay", overlay_component("KnobOverlay", knob_overlay_data(False), bgc)))
    defs.append(("knob overlay bipolar", overlay_component("KnobOverlay", knob_overlay_data(True), bgc)))
    defs.append(("menu overlay", overlay_component("MenuOverlay", menu_overlay_data(), bgc)))

    def graph_entry():
        return entry("graph", "eqGraph", obj(("version", 1), ("handleName", "Data")), "0 0 %d %d" % (W, GRAPH_H), "No", "Show", "Always")

    tabs = []
    for ti, (name, bands) in enumerate((("BAND 1-4", range(0, 4)), ("BAND 5-8", range(4, 8)))):
        children = [graph_entry()]
        for r, b in enumerate(bands):
            band_row(children, b, r)
        tabs.append(obj(("version", 1), ("tabName", name), ("fnKeyIndex", ti), ("fnKeySubIndex", 0),
                        ("componentDefinition", component_definition([], children, "ff000000", bg_image="bg_bands_%d.png" % (ti + 1))),
                        ("qlinkBoundsData", [qrect(r) for r in range(4)])))
    # OUTPUT
    children = [graph_entry()]
    cy = ROW_Y0 + (ROW_H - CELL_H) // 2
    for c, (param, comp) in enumerate(((P_OUTPUT, "knobOutBi"), (P_SCALE, "knobOut"), (P_MIX, "knobOut"), (P_EDIT, "knobOutMenu"))):
        children.append(entry(("Output", "Scale", "Mix", "Band")[c], comp, obj(("version", 1), ("handleName", "Data")),
                              "%d %d %d %d" % (CELL_X[c], cy, CELL_W, CELL_H), "Yes", "Hide", "Always", remap=[("Data", H(param))]))
    cy2 = ROW_Y0 + ROW_H + (ROW_H - CELL_H) // 2
    for c, param in enumerate((P_ADAPT, P_ANALYZER, P_FOLLOW)):
        children.append(entry(("Adapt Q", "Analyzer", "QLink Follow")[c], "toggleCell", obj(("version", 1), ("handleName", "Data")),
                              "%d %d %d %d" % (CELL_X[c], cy2, CELL_W, CELL_H), "Yes", "Hide", "Always", remap=[("Data", H(param))]))
    tabs.append(obj(("version", 1), ("tabName", "OUTPUT"), ("fnKeyIndex", 2), ("fnKeySubIndex", 0),
                    ("componentDefinition", component_definition([], children, "ff000000", bg_image="bg_output.png")),
                    ("qlinkBoundsData", [qrect(0)])))

    replacements = [obj(("key", H(P_EDIT)), ("value", [str(i + 1) for i in range(NBANDS)])),
                    obj(("key", H(P_ADAPT)), ("value", ["Off", "On"])),
                    obj(("key", H(P_ANALYZER)), ("value", ["Off", "On"])),
                    obj(("key", H(P_FOLLOW)), ("value", ["Off", "On"]))]
    for b in range(NBANDS):
        replacements.append(obj(("key", H(bp(b, ON))), ("value", ["Off", "On"])))
        replacements.append(obj(("key", H(bp(b, TYPE))), ("value", list(TYPES))))
    page = obj(("version", 1),
               ("componentDefinitions", obj(("version", 2), ("importFiles", []),
                                            ("localComponentDefinitions", [obj(("key", k), ("value", v)) for k, v in defs]))),
               ("replacementStrings", replacements),
               ("info", obj(("version", 1), ("type", "CompleteDescription"))),
               ("tabs", tabs))
    return obj(("pageData", page))


def build_qlinks():
    def qmap(rows):
        m = {}
        for r, params in enumerate(rows):          # row 0 = top row of knobs (13..16)
            for c, p in enumerate(params):
                m[13 + c - 4 * r] = p
        return Obj(("Q-Link %d" % q, m.get(q, -1)) for q in range(1, 17))
    entries = []
    for ti, bands in enumerate((range(0, 4), range(4, 8))):
        rows = [[bp(b, FREQ), bp(b, GAIN), bp(b, Q), bp(b, TYPE)] for b in bands]
        entries.append(obj(("Tab", ti + 1), ("SubTab", 1), ("Bank Direction", "RowTopDown"), ("Q-Links", qmap(rows))))
    entries.append(obj(("Tab", 3), ("SubTab", 1), ("Bank Direction", "RowTopDown"),
                       ("Q-Links", qmap([[P_OUTPUT, P_SCALE, P_MIX, P_EDIT]]))))
    # Program (track) mode: Freq and Gain of every band, two bands per row.
    prog_rows = [[bp(2 * r, FREQ), bp(2 * r, GAIN), bp(2 * r + 1, FREQ), bp(2 * r + 1, GAIN)] for r in range(4)]
    return obj(("version", 4), ("info", obj(("version", 1), ("type", "CompleteDescription"))),
               ("Screen Mode Q-Links", obj(("version", 4), ("map", entries))),
               ("Program Mode Q-Links", qmap(prog_rows)))


# ------------------------------------------------------------------------------------------------------------------
# Previews: what the tabs look like with a demo setting (approximate fonts; the MPC draws text in Titillium Web)
# ------------------------------------------------------------------------------------------------------------------
DEMO = [  # on, type, freq, gain, Q   (the "Vocal Presence"-like demo used for the previews)
    (True, 1, 90, 0, 0.71), (True, 2, 120, 2.0, 0.71), (True, 3, 300, -3.5, 1.2), (True, 3, 900, 0, 0.71),
    (True, 3, 3200, 4.0, 0.9), (True, 3, 6800, -2.5, 4.0), (True, 4, 11000, 3.0, 0.71), (False, 7, 18000, 0, 0.71)]


def render_preview(bg, assets, tab, selected=4):
    im = bg.convert("RGBA")
    d = ImageDraw.Draw(im, "RGBA")
    # analyser: a pink-ish fake spectrum
    for i in range(NA):
        f = col_freq(i, NA)
        # a mix-like spectrum: -3 dB/oct overall, a kick bump, some ripple, little above 14 kHz (tilt added below)
        lvl = -52 - 3 * math.log2(f / 1000) + 10 * math.exp(-((math.log2(f / 70)) ** 2) / 1.5) \
            + 4 * math.sin(i * 0.9) + 2 * math.sin(i * 2.3) - (20 if f > 14000 else 0)
        lvl += 4.5 * math.log2(f / 1000)
        v = min(max((lvl - SPEC_FLOOR) / -SPEC_FLOOR, 0), 1)
        h = int(PH * v)
        if h > 0:
            im.alpha_composite(assets["sp_fill.png"].crop((0, PH - h, AW, PH)), (PX + i * AW, PY + PH - h))
            im.alpha_composite(assets["sp_cap.png"], (PX + i * AW, PY + PH - h))
    # curve
    for i in range(NC):
        db = response_db(DEMO, col_freq(i, NC))
        v = min(max(0.5 + db / (2 * GRANGE), 0), 1)
        yv = int(round(PH * (1 - v)))
        top, bot = sorted((yv, PH // 2))
        if bot > top:
            im.alpha_composite(assets["cv_fill.png"].crop((0, top, CW, bot)), (PX + i * CW, PY + top))
        im.alpha_composite(assets["cv_cap.png"], (PX + i * CW, PY + max(min(yv - 3, PH - 6), 0)))
    # points
    for b, (on, t, f, g, q) in enumerate(DEMO):
        x = x_of_freq(f)
        yv = g if t in (2, 3, 4) else (norm_from_q(q) - 0.5) * 2 * GRANGE
        y = y_of_db(yv)
        img = assets["pt%d_%s.png" % (b + 1, "sel" if b == selected else ("on" if on else "off"))]
        im.alpha_composite(img, (int(x - img.width / 2), int(y - img.height / 2)))
    # readout
    on, t, f, g, q = DEMO[selected]
    rf = font(FONT_SEMI, 21)
    d.text((316, 5), "BAND %d" % (selected + 1), font=rf, fill=BAND_RGB[selected])
    d.text((398, 5), TYPES[t], font=font(FONT_MED, 21), fill=TEXT)
    d.text((496, 5), ("%.2f kHz" % (f / 1000)) if f >= 1000 else "%.0f Hz" % f, font=font(FONT_MED, 21), fill=TEXT)
    d.text((618, 5), ("%+.1f dB" % g) if t in (2, 3, 4) else "--", font=font(FONT_MED, 21), fill=TEXT)
    d.text((730, 5), "Q %.2f" % q, font=font(FONT_MED, 21), fill=TEXT)

    nf, vf = font(FONT_MED, 20), font(FONT_SEMI, 24)

    def cell(x, y, img, name, value, knob=True):
        im.alpha_composite(img, (x + 6, y + (CELL_H - img.height) // 2))
        tx = x + 6 + (KNOB if knob else TYPE_W) + 12
        d.text((tx, y + 4), name, font=nf, fill=MUTED)
        d.text((tx, y + 28), value, font=vf, fill=TEXT)

    def frame(path, v, frames):
        s = assets[path]
        hh = s.height // frames
        k = int(round(v * (frames - 1)))
        return s.crop((0, k * hh, s.width, (k + 1) * hh))

    if tab in (0, 1):
        for r, b in enumerate(range(tab * 4, tab * 4 + 4)):
            on, t, f, g, q = DEMO[b]
            y = ROW_Y0 + r * ROW_H
            im.alpha_composite(assets["sel%d_%s.png" % (b + 1, "on" if b == selected else "off")], (24, y + (ROW_H - SEL) // 2))
            im.alpha_composite(assets["pwr%d_%s.png" % (b + 1, "on" if on else "off")], (24 + SEL + 18, y + (ROW_H - PWR) // 2))
            cy = y + (ROW_H - CELL_H) // 2
            fn = math.log(f / FMIN) / math.log(FMAX / FMIN)
            cell(CELL_X[0], cy, frame("knob_uni_%d.png" % (b + 1), fn, 127), "Freq %d" % (b + 1),
                 ("%.2f kHz" % (f / 1000)) if f >= 1000 else "%.0f Hz" % f)
            cell(CELL_X[1], cy, frame("knob_bi_%d.png" % (b + 1), 0.5 + g / 30, 127), "Gain %d" % (b + 1),
                 ("%+.1f dB" % g) if t in (2, 3, 4) else "--")
            cell(CELL_X[2], cy, frame("knob_uni_%d.png" % (b + 1), norm_from_q(q), 127), "Q %d" % (b + 1), "%.2f" % q)
            cell(CELL_X[3], cy, frame("type_%d.png" % (b + 1), t / 7, 8), "Type %d" % (b + 1), TYPES[t], knob=False)
        rects = [qrect(r) for r in range(1)]
    else:
        cy = ROW_Y0 + (ROW_H - CELL_H) // 2
        cell(CELL_X[0], cy, frame("knob_out_bi.png", 0.5, 127), "Output", "0.0 dB")
        cell(CELL_X[1], cy, frame("knob_out_uni.png", 0.5, 127), "Scale", "100 %")
        cell(CELL_X[2], cy, frame("knob_out_uni.png", 1.0, 127), "Mix", "100 %")
        cell(CELL_X[3], cy, frame("knob_out_uni.png", selected / 7, 127), "Band", str(selected + 1))
        cy2 = ROW_Y0 + ROW_H + (ROW_H - CELL_H) // 2
        for c, (nm, val) in enumerate((("Adapt Q", False), ("Analyzer", True), ("QLink Follow", True))):
            x = CELL_X[c]
            d.text((x + 12, cy2 + 4), nm, font=nf, fill=MUTED)
            d.text((x + 12, cy2 + 28), "On" if val else "Off", font=vf, fill=TEXT)
            im.alpha_composite(assets["toggle_%s.png" % ("on" if val else "off")], (x + CELL_W - 96, cy2 + 8))
        rects = [qrect(0)]
    # the MPC's Q-Link highlight on the first row
    x, y, w, h = map(int, rects[0].split())
    d.rounded_rectangle([x, y, x + w, y + h], radius=8, outline=argb_tuple("ffffa21a"), width=2)
    return im


def dump_compact(value):
    """Like gen_ui.dump_json (ordered, duplicate keys kept) but without whitespace."""
    if isinstance(value, Obj):
        return "{" + ",".join(json.dumps(str(k)) + ":" + dump_compact(v) for k, v in value) + "}"
    if isinstance(value, dict):
        return dump_compact(Obj(value.items()))
    if isinstance(value, (list, tuple)):
        return "[" + ",".join(dump_compact(v) for v in value) + "]"
    if isinstance(value, bool):
        return "true" if value else "false"
    if value is None:
        return "null"
    if isinstance(value, int):
        return str(value)
    if isinstance(value, float):
        return repr(value)
    return json.dumps(value, ensure_ascii=False)


def argb_tuple(s):
    return (int(s[2:4], 16), int(s[4:6], 16), int(s[6:8], 16), int(s[0:2], 16))


# ------------------------------------------------------------------------------------------------------------------
def check_params(params_json):
    with open(params_json, encoding="utf-8") as f:
        params = json.load(f)["params"]
    names = [p["name"] for p in params]
    want = {P_OUTPUT: "Output", P_SCALE: "Scale", P_MIX: "Mix", P_EDIT: "Band", P_ADAPT: "Adapt Q", P_ANALYZER: "Analyzer",
            CURVE0: "Curve 001", SPEC0 - 1: "Curve %03d" % NC, SPEC0: "Spectrum 01", P_FOLLOW - 1: "Spectrum %02d" % NA,
            P_FOLLOW: "QLink Follow"}
    for b in range(NBANDS):
        want[PICK0 + b] = "Pick %d" % (b + 1)
    for b in range(NBANDS):
        for w, n in ((ON, "On"), (TYPE, "Type"), (FREQ, "Freq"), (GAIN, "Gain"), (Q, "Q")):
            want[bp(b, w)] = "%s %d" % (n, b + 1)
    if len(names) != NPARAMS:
        sys.exit("make_skin: the plugin has %d parameters, the skin expects %d" % (len(names), NPARAMS))
    for i, n in want.items():
        if names[i] != n:
            sys.exit('make_skin: parameter %d is "%s", the skin expects "%s"' % (i, names[i], n))
    for b in range(NBANDS):
        if params[bp(b, TYPE)]["choices"] and params[bp(b, TYPE)]["choices"] != TYPES:
            sys.exit("make_skin: type names differ from the plugin's: %s" % params[bp(b, TYPE)]["choices"])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("params")
    ap.add_argument("out")
    ap.add_argument("--preview")
    a = ap.parse_args()
    check_params(a.params)
    if os.path.isdir(a.out):
        shutil.rmtree(a.out)
    os.makedirs(a.out)

    assets = {}

    def save(name, im):
        assets[name] = im
        im.save(os.path.join(a.out, name), optimize=True)

    for b in range(NBANDS):
        c = BAND_RGB[b]
        save("knob_uni_%d.png" % (b + 1), knob_strip(c, False))
        save("knob_bi_%d.png" % (b + 1), knob_strip(c, True))
        strip = Image.new("RGBA", (TYPE_W, TYPE_H * 8), (0, 0, 0, 0))
        for t in range(8):
            strip.paste(type_icon(t, c), (0, t * TYPE_H))
        save("type_%d.png" % (b + 1), strip)
        save("pt%d_on.png" % (b + 1), puck(c, b + 1, 34, "on"))
        save("pt%d_off.png" % (b + 1), puck(c, b + 1, 34, "off"))
        save("pt%d_sel.png" % (b + 1), puck(c, b + 1, 52, "sel"))
        save("sel%d_on.png" % (b + 1), band_button(c, b + 1, True))
        save("sel%d_off.png" % (b + 1), band_button(c, b + 1, False))
        strip = Image.new("RGBA", (SEL, SEL * NBANDS), (0, 0, 0, 0))   # frame k = Band k: lit only on frame b
        for k in range(NBANDS):
            strip.paste(assets["sel%d_%s.png" % (b + 1, "on" if k == b else "off")], (0, k * SEL))
        save("sel%d_strip.png" % (b + 1), strip)
        save("pwr%d_on.png" % (b + 1), power_button(c, True))
        save("pwr%d_off.png" % (b + 1), power_button(c, False))
    save("knob_out_uni.png", knob_strip(CURVE_RGB, False))
    save("knob_out_bi.png", knob_strip(CURVE_RGB, True))
    save("toggle_on.png", toggle_img(True))
    save("toggle_off.png", toggle_img(False))
    save("cv_fill.png", curve_fill())
    save("cv_cap.png", curve_cap())
    save("sp_fill.png", spec_fill())
    save("sp_cap.png", spec_cap())

    bgs = []
    for ti, bands in enumerate((range(0, 4), range(4, 8))):
        im = Image.new("RGB", (W, HT), LOWER)
        draw_graph_panel(im)
        draw_lower_panel(im, [(b, None) for b in bands])
        im.save(os.path.join(a.out, "bg_bands_%d.png" % (ti + 1)), optimize=True)
        bgs.append(im)
    im = Image.new("RGB", (W, HT), LOWER)
    draw_graph_panel(im)
    draw_lower_panel(im, [(None, None), (None, None)])
    d = ImageDraw.Draw(im, "RGBA")
    d.text((22, ROW_Y0 + 24), "MASTER", font=font(FONT_SEMI, 21), fill=MUTED)
    d.text((22, ROW_Y0 + ROW_H + 24), "OPTIONS", font=font(FONT_SEMI, 21), fill=MUTED)
    notes = ["Scale multiplies every band's gain (0-200 %).  Mix blends the dry signal back in.  Band picks which point the",
             "finger drags (the same as the 1-8 buttons).  Adapt Q narrows bells as they get louder.  QLink Follow: tapping a",
             "band number also puts the Q-Links on that band's row (off: they stay on the row you last touched a knob in)."]
    for i, line in enumerate(notes):
        d.text((24, ROW_Y0 + 2 * ROW_H + 14 + i * 24), line, font=font(FONT_MED, 18), fill=MUTED)
    im.save(os.path.join(a.out, "bg_output.png"), optimize=True)
    bgs.append(im)

    with open(os.path.join(a.out, "TUI.json"), "w", encoding="utf-8", newline="\n") as f:
        f.write(dump_compact(build_tui()) + "\n")   # compact: the graph has ~400 widgets
    with open(os.path.join(a.out, "Q-Links.json"), "w", encoding="utf-8", newline="\n") as f:
        f.write(dump_json(build_qlinks()) + "\n")

    if a.preview:
        os.makedirs(a.preview, exist_ok=True)
        for ti, name in enumerate(("BAND 1-4", "BAND 5-8", "OUTPUT")):
            sel = 4 if ti != 0 else 2
            render_preview(bgs[ti], assets, ti, selected=sel).convert("RGB").save(
                os.path.join(a.preview, "TouchEQ 8 - %s.png" % name))
    total = sum(os.path.getsize(os.path.join(a.out, n)) for n in os.listdir(a.out))
    print("make_skin: %d files, %.1f KB" % (len(os.listdir(a.out)), total / 1024))


if __name__ == "__main__":
    main()
