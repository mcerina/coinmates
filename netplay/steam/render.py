#!/usr/bin/env python3
# Coinmates' icon and artwork, as the 90s arcades: pixel lettering with a metal gradient, outline and
# extrusion, a sunburst, CRT scanlines, the players' colours. All rectangles: no fonts to ship.
# Writes, with rsvg-convert and ImageMagick for the preview, the icon (../flatpak, from which the
# flatpak, the macOS app and Steam take PNGs), the Windows exe's icon (../../scripts/resources/windows),
# the pictures and the icon for Steam's library (README, "Artwork in Steam") and preview.png of them for the README
import os, subprocess

here = os.path.dirname(os.path.abspath(__file__))
# 1P red, 2P blue, 3P yellow, 4P green: the most common choice in the arcades (Gauntlet, Daytona USA)
PLAYERS = ['#ff3b4e', '#3a86ff', '#ffd23f', '#20c997']

FONT = {
    'A': ['.###.', '#...#', '#...#', '#####', '#...#', '#...#', '#...#'],
    'C': ['.####', '#....', '#....', '#....', '#....', '#....', '.####'],
    'D': ['####.', '#...#', '#...#', '#...#', '#...#', '#...#', '####.'],
    'E': ['#####', '#....', '#....', '####.', '#....', '#....', '#####'],
    'G': ['.####', '#....', '#....', '#.###', '#...#', '#...#', '.####'],
    'I': ['###', '.#.', '.#.', '.#.', '.#.', '.#.', '###'],
    'L': ['#....', '#....', '#....', '#....', '#....', '#....', '#####'],
    'M': ['#...#', '##.##', '#.#.#', '#.#.#', '#...#', '#...#', '#...#'],
    'N': ['#...#', '##..#', '#.#.#', '#..##', '#...#', '#...#', '#...#'],
    'O': ['.###.', '#...#', '#...#', '#...#', '#...#', '#...#', '.###.'],
    'P': ['####.', '#...#', '#...#', '####.', '#....', '#....', '#....'],
    'R': ['####.', '#...#', '#...#', '####.', '#.#..', '#..#.', '#...#'],
    'S': ['.####', '#....', '#....', '.###.', '....#', '....#', '####.'],
    'T': ['#####', '..#..', '..#..', '..#..', '..#..', '..#..', '..#..'],
    '1': ['.#.', '##.', '.#.', '.#.', '.#.', '.#.', '###'],
    '2': ['.###.', '#...#', '....#', '...#.', '..#..', '.#...', '#####'],
    '3': ['####.', '....#', '....#', '.###.', '....#', '....#', '####.'],
    '4': ['...#.', '..##.', '.#.#.', '#..#.', '#####', '...#.', '...#.'],
    '-': ['...', '...', '...', '###', '...', '...', '...'],
    ',': ['..', '..', '..', '..', '..', '.#', '#.'],
    ' ': ['...', '...', '...', '...', '...', '...', '...'],
}

BOLD = {
    'C': ['.#####', '##....', '##....', '##....', '##....', '##....', '.#####'],
    'O': ['.#####.', '##...##', '##...##', '##...##', '##...##', '##...##', '.#####.'],
    'I': ['######', '..##..', '..##..', '..##..', '..##..', '..##..', '######'],
    'N': ['##...##', '###..##', '####.##', '##.####', '##..###', '##...##', '##...##'],
    'M': ['##.....##', '###...###', '####.####', '##.###.##', '##..#..##', '##.....##', '##.....##'],
    'A': ['.#####.', '##...##', '##...##', '#######', '##...##', '##...##', '##...##'],
    'T': ['######', '..##..', '..##..', '..##..', '..##..', '..##..', '..##..'],
    'E': ['######', '##....', '##....', '#####.', '##....', '##....', '######'],
    'S': ['.######', '##.....', '##.....', '.#####.', '.....##', '.....##', '######.'],
}

def cells(text, bold=False):
    out, x = [], 0
    for ch in text:
        g = (BOLD if bold else FONT)[ch]
        w = len(g[0])
        for y, row in enumerate(g):
            for i, c in enumerate(row):
                if c == '#':
                    out.append((x + i, y))
        x += w + 1
    return sorted(set(out)), x - 1

def runs(pixels):
    # horizontal runs, to write fewer rectangles
    rows = {}
    for x, y in pixels:
        rows.setdefault(y, []).append(x)
    for y, xs in sorted(rows.items()):
        xs.sort()
        start = prev = xs[0]
        for x in xs[1:] + [None]:
            if x is not None and x == prev + 1:
                prev = x
                continue
            yield start, y, prev - start + 1
            if x is not None:
                start = prev = x

def rects(pixels, u, ox, oy, fill, grow=0.0, extra=''):
    return ''.join(f'<rect x="{ox + x * u - grow:.1f}" y="{oy + y * u - grow:.1f}" width="{w * u + 2 * grow:.1f}" '
                   f'height="{u + 2 * grow:.1f}" fill="{fill}"{extra}/>' for x, y, w in runs(pixels))

uid = [0]
def logo(cx, top, u, anchor='middle', text='COINMATES', most=1e9):
    # the name: metal gradient, dark outline, extrusion down and right, slanted; at most `most` wide
    pix, width = cells(text, bold=True)
    u = min(u, most / (width + 1.5))
    ox = cx - width * u / 2 if anchor == 'middle' else cx
    uid[0] += 1
    g = f'metal{uid[0]}'
    defs = (f'<defs><linearGradient id="{g}" gradientUnits="userSpaceOnUse" x1="0" y1="{top}" x2="0" y2="{top + 7 * u}">'
            f'<stop offset="0" stop-color="#fffbe6"/><stop offset=".42" stop-color="#ffd23f"/>'
            f'<stop offset=".5" stop-color="#ff8a00"/><stop offset="1" stop-color="#e5243b"/></linearGradient></defs>')
    depth = max(3, int(u * .9))
    body = []
    for k in range(depth, 0, -1):
        d = k * u / depth * .9
        body.append(rects(pix, u, ox + d, top + d, '#3b0a57', grow=u * .28))
    body.append(rects(pix, u, ox, top, '#12021f', grow=u * .28))
    body.append(rects(pix, u, ox, top, f'url(#{g})'))
    # a shine on the top row of every letter
    shine = [(x, y) for x, y in pix if y == 0]
    body.append(rects(shine, u, ox, top, '#ffffff', extra=' opacity=".55"'))
    skew = f'transform="translate({cx if anchor == "middle" else ox} {top + 3.5 * u}) skewX(-9) translate({-(cx if anchor == "middle" else ox)} {-(top + 3.5 * u)})"'
    return defs + f'<g {skew}>' + ''.join(body) + '</g>', width * u

def label(text, cx, top, u, fill, anchor='middle', shadow='#12021f'):
    pix, width = cells(text)
    ox = cx - width * u / 2 if anchor == 'middle' else cx
    return rects(pix, u, ox + u * .6, top + u * .6, shadow) + rects(pix, u, ox, top, fill), width * u

def players(cx, top, u, anchor='middle'):
    parts, widths = [], []
    for i in range(4):
        widths.append(cells(f'{i + 1}P')[1] * u)
    gap = u * 4
    total = sum(widths) + gap * 3
    x = cx - total / 2 if anchor == 'middle' else cx
    for i, c in enumerate(PLAYERS):
        parts.append(label(f'{i + 1}P', x, top, u, c, anchor='start')[0])
        x += widths[i] + gap
    return ''.join(parts)

def background(w, h, fx, fy):
    import math
    out = [f'<rect width="{w}" height="{h}" fill="#140b3d"/>']
    rays = 24
    r = math.hypot(w, h)
    path = []
    for i in range(rays):
        a0 = 2 * math.pi * i / rays
        a1 = a0 + math.pi / rays
        path.append(f'M{fx:.1f} {fy:.1f} L{fx + r * math.cos(a0):.1f} {fy + r * math.sin(a0):.1f} '
                    f'L{fx + r * math.cos(a1):.1f} {fy + r * math.sin(a1):.1f}Z')
    out.append(f'<path d="{" ".join(path)}" fill="#22146b"/>')
    out.append(f'<defs><radialGradient id="vignette" cx="{fx / w:.3f}" cy="{fy / h:.3f}" r=".75">'
               f'<stop offset="0" stop-color="#6a3cff" stop-opacity=".45"/><stop offset=".55" stop-color="#140b3d" stop-opacity="0"/>'
               f'<stop offset="1" stop-color="#05020f" stop-opacity=".85"/></radialGradient>'
               f'<pattern id="scan" width="4" height="4" patternUnits="userSpaceOnUse"><rect width="4" height="1.4" fill="#000" opacity=".22"/></pattern></defs>')
    out.append(f'<rect width="{w}" height="{h}" fill="url(#vignette)"/>')
    seed = 11
    s = max(2, round(w / 400))
    for _ in range(int(w * h / 6000)):
        seed = (seed * 1103515245 + 12345) & 0x7fffffff
        x = seed % w
        seed = (seed * 1103515245 + 12345) & 0x7fffffff
        y = seed % h
        size = s * (1 + seed % 2)
        out.append(f'<rect x="{x}" y="{y}" width="{size}" height="{size}" fill="#fff" opacity="{.3 + (seed % 5) * .12:.2f}"/>')
    return ''.join(out)

def scanlines(w, h):
    return f'<rect width="{w}" height="{h}" fill="url(#scan)"/>'

# the icon: four arcade joysticks in pixel art, one per player, on a 32x32 grid: a ball of
# 6x6 (b body, h light, d shadow), a stick (s, S), the panel (P top, p front, o holes), outline k
def icon_pixels():
    grid = [['.'] * 32 for _ in range(32)]
    def put(x, y, c):
        if 0 <= x < 32 and 0 <= y < 32:
            grid[y][x] = c
    ball = ['.bbbb.', 'bhhbbb', 'bhbbbb', 'bbbbbd', 'bbbbdd', '.bddd.']
    for i in range(4):
        x0 = 1 + i * 8
        for y in range(10, 21):
            put(x0 + 2, y, 's'); put(x0 + 3, y, 'S')
        for dy, row in enumerate(ball):
            for dx, c in enumerate(row):
                if c != '.':
                    put(x0 + dx, 5 + dy, c + str(i))
    for y in range(20, 29):
        for x in range(1, 31):
            if (y in (20, 28)) and x in (1, 30):
                continue
            put(x, y, 'p' if y > 21 else 'P')
    for i in range(4):
        x0 = 1 + i * 8
        for dx in range(1, 5):
            put(x0 + dx, 21, 'o')
    # outline: every empty cell next to a filled one
    filled = {(x, y) for y in range(32) for x in range(32) if grid[y][x] != '.'}
    for x, y in list(filled):
        for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            if (nx, ny) not in filled and 0 <= nx < 32 and 0 <= ny < 32:
                put(nx, ny, 'k')
    return grid

def shade(hexcolor, f):
    r, g, b = (int(hexcolor[i:i + 2], 16) for i in (1, 3, 5))
    if f > 1:
        r, g, b = (int(c + (255 - c) * (f - 1)) for c in (r, g, b))
    else:
        r, g, b = (int(c * f) for c in (r, g, b))
    return f'#{r:02x}{g:02x}{b:02x}'

def icon_svg(px=None):
    colors = {'s': '#c7cbe0', 'S': '#7d8299', 'p': '#2b2d52', 'P': '#4a4e86', 'o': '#0c0d1c', 'k': '#0c0d1c'}
    for i, c in enumerate(PLAYERS):
        colors[f'b{i}'] = c
        colors[f'h{i}'] = shade(c, 1.65)
        colors[f'd{i}'] = shade(c, .55)
    grid = icon_pixels()
    out = []
    for y, row in enumerate(grid):
        x = 0
        while x < 32:
            c = row[x]
            if c == '.':
                x += 1
                continue
            n = 1
            while x + n < 32 and row[x + n] == c:
                n += 1
            out.append(f'<rect x="{x * 4}" y="{y * 4}" width="{n * 4}" height="4" fill="{colors[c]}"/>')
            x += n
    return ''.join(out)

def svg(w, h, body):
    return f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}" shape-rendering="crispEdges">{body}</svg>\n'

ICON = icon_svg()
def emblem(x, y, size):
    return f'<g transform="translate({x} {y}) scale({size / 128})">{ICON}</g>'

def tag(cx, top, u, anchor='middle', most=1e9):
    u = min(u, most / cells('CO-OP ARCADE GAMES, ONLINE')[1])
    return label('CO-OP ARCADE GAMES, ONLINE', cx, top, u, '#e8e2ff', anchor)[0]

art = {}
b, wl = logo(300, 500, 20, most=540)
art['cover'] = svg(600, 900, background(600, 900, 300, 300) + emblem(140, 110, 320) + b
                   + players(300, 640, 8) + tag(300, 760, 5, most=540) + scanlines(600, 900))
b, wl = logo(320, 110, 20, 'start', most=570)
art['wide-cover'] = svg(920, 430, background(920, 430, 160, 215) + emblem(20, 95, 250) + b
                  + players(322, 250, 7, 'start') + tag(322, 335, 5, 'start', most=570) + scanlines(920, 430))
art['background'] = svg(1920, 620, background(1920, 620, 960, 310) + scanlines(1920, 620))
b, wl = logo(340, 90, 30, 'start', most=900)
art['logo'] = svg(1280, 360, emblem(10, 30, 300) + b + players(342, 280, 7, 'start'))
for key, text in art.items():
    path = os.path.join(here, f'coinmates-{key}')
    open(path + '.svg', 'w').write(text)
    subprocess.run(['rsvg-convert', path + '.svg', '-o', path + '.png'], check=True)
p = lambda key: os.path.join(here, f'coinmates-{key}.png')
# as Steam's page: the background with the logo, then the covers; transparent, for GitHub's light and dark themes
subprocess.run(['magick', '-size', '920x637', 'xc:none',
                '(', p('background'), '-resize', '920x', ')', '-geometry', '+0+0', '-composite',
                '(', p('logo'), '-resize', '420x', ')', '-geometry', '+30+159', '-composite',
                '(', p('cover'), '-resize', 'x320', ')', '-geometry', '+0+317', '-composite',
                '(', p('wide-cover'), '-resize', 'x320', ')', '-geometry', '+235+317', '-composite',
                '-depth', '8', os.path.join(here, 'preview.png')], check=True)

icon = os.path.join(here, '..', 'flatpak', 'io.github.mcerina.Coinmates.svg')
open(icon, 'w').write('<!-- Coinmates: four arcade joysticks, one per player, in pixel art (written by ../steam/render.py) -->\n'
                      f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 128 128" shape-rendering="crispEdges">{ICON}</svg>\n')
pngs = []
for size in (16, 24, 32, 48, 64, 128, 256):
    pngs.append(os.path.join(here, f'icon{size}.png'))
    subprocess.run(['rsvg-convert', '-w', str(size), '-h', str(size), icon, '-o', pngs[-1]], check=True)
# an .ico of PNG images (Windows Vista and later), much smaller than bitmaps
import struct
images = []
for png in pngs:
    images.append(open(png, 'rb').read())
    os.remove(png)
# for Steam's shortcut, as Steam can't see the icons of the flatpaks installed for the whole system
open(os.path.join(here, 'coinmates-icon.png'), 'wb').write(images[-1])
ico = struct.pack('<HHH', 0, 1, len(images))
offset = 6 + 16 * len(images)
for size, data in zip((16, 24, 32, 48, 64, 128, 256), images):
    ico += struct.pack('<BBBBHHII', size % 256, size % 256, 0, 0, 1, 32, len(data), offset)
    offset += len(data)
open(os.path.join(here, '..', '..', 'scripts', 'resources', 'windows', 'mame', 'coinmates.ico'), 'wb').write(ico + b''.join(images))
