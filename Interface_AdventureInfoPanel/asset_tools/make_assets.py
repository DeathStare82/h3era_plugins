# -*- coding: utf-8 -*-
"""
Adventure Info Panel - 그래픽 리소스 생성기

승인된 v2 시안과 같은 그림을 H3 포맷(PCX 8bit / DEF type 0x47, RLE format 1)으로 만든다.
결과물은 ../Data 폴더에 저장된다. (모드의 Data 폴더에 그대로 복사해서 사용)

  DsIpBg.pcx   194x105  배경 (H2 돌판 + 얇은 테두리 + 구분선/금장식)
  DsIpPlq.def  87x26    시간 명판      프레임 0 = 낮(나무판), 1 = 밤(어두운 홈)
  DsIpIco.def  15x20    시간 아이콘    0 = 해(낮), 1 = 달(밤), 2 = 모래시계(낮), 3 = 모래시계(밤)
  DsIpDgt.def  9x20     숫자 0~9, ':'  그룹 0 = 낮(흰 글자), 그룹 1 = 밤(금박 글자)
  DsIpBtn.def  178x29   VISIT BLOG 버튼 프레임 0 = 일반, 1 = 눌림 (색은 그대로, 안쪽으로 눌린 모양)

모든 프레임은 배경까지 미리 합성한 "불투명" 그림이라서 DEF 투명색/특수색 처리에 의존하지 않는다.
C++ 쪽 좌표 상수(InfoPanelLayout.h)와 반드시 같은 값을 써야 한다.

필요: Python 3 + Pillow,  글꼴: Liberation Serif Bold (없으면 Windows의 Times New Roman Bold)
"""
import os
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.normpath(os.path.join(HERE, '..', 'Data'))
SRC_TEXTURE = os.path.join(HERE, 'src', 'stone_texture.png')

# ------------------------------------------------------------------ layout (InfoPanelLayout.h 와 동일)
PANEL_W, PANEL_H = 194, 105
PLAQUE_X = (8, 99)
PLAQUE_Y, PLAQUE_W, PLAQUE_H = 25, 87, 26
INNER = 3                                  # 명판 테두리 두께 → 안쪽 칸은 y 3..22 (20줄)
CELL_H = PLAQUE_H - 2 * INNER              # 20
ICON_W = 15
DIGIT_W, COLON_W = 9, 5
DIV_Y = 57
BTN_X, BTN_Y, BTN_W, BTN_H = 8, 63, 178, 29

FONT_CANDIDATES = [
    '/usr/share/fonts/truetype/liberation/LiberationSerif-Bold.ttf',
    'C:/Windows/Fonts/LiberationSerif-Bold.ttf',
    'C:/Windows/Fonts/timesbd.ttf',
]

# ------------------------------------------------------------------ palette constants
GOLD_LT = (247, 223, 123)
GOLD_NAME = (189, 154, 24)
DARK = (33, 12, 0)
DARK2 = (57, 24, 0)
FRAME = [(107, 52, 16), (214, 170, 140), (231, 199, 173), (41, 16, 0)]   # H2 베벨 (원본 7px → 4px)
SHADOW = [0.50, 0.74]                                                       # 안쪽 그림자 (원본 ~5px → 2px)
WOOD = {99: (99, 48, 8), 115: (115, 60, 16), 123: (123, 69, 24), 132: (132, 77, 33),
        140: (140, 85, 41), 148: (148, 97, 49)}

ICON_PAL = {
    'k': (33, 12, 0), 'y': (255, 215, 66), 'Y': (255, 239, 132), 'w': (255, 251, 222),
    'o': (231, 138, 24), 'O': (189, 97, 16), 'r': (255, 199, 57),
    'm': (239, 231, 181), 'M': (206, 190, 123), 'n': (156, 142, 90), '*': (255, 255, 255),
    'W': (222, 186, 156), 'P': (156, 105, 57), 'p': (99, 48, 8),
    'g': (148, 162, 214), 'G': (198, 207, 255), 's': (255, 215, 99), 'S': (231, 170, 49),
}
SUN = ["......r......", "..r...r...r..", "...r.kkk.r...", "....kwYyk....", "...kwYyyyk...",
       "rrrkYyyyokrrr", "...kyyyyok...", "...kyyyook...", "....kyook....", ".....kkk.....",
       "...r.....r...", "..r...r...r..", "......r......"]
MOON = ["....kkkk.....", "...kmmk....*.", "..kmmk....*w*", ".kmmk......*.", ".kmMk........",
        "kmmMk........", "kmmMMk.......", "kmmMMnkk..k..", ".kmMMMnnkkmk.", ".kmmMMMMnnk..",
        "..kmmMMMMk...", "...kkmmmk....", ".....kkk....."]
HOURGLASS = ["kkkkkkkkkkk", "kWWWWWWWWWk", "kkkkkkkkkkk", "PkGgggggGkP", "PkssssssskP",
             "P.kssssSk.P", "P..ksSSk..P", "P...kSk...P", "P..kgSgk..P", "P.kggSggk.P",
             "PkggsSsggkP", "PkssssssSkP", "kkkkkkkkkkk", "kWWWWWWWWWk", "kkkkkkkkkkk"]
GEM = ["......k......", ".....kYk.....", "....kYwyk....", "kkkkYwyyOkkkk",
       "....kyyOk....", ".....kOk.....", "......k......"]

# H2 BUILD/EXIT 버튼 글자: B U I L D E X T 는 원본에서 픽셀 그대로, 나머지는 같은 스타일로 직접 그림
H2_GLYPHS = {
    'B': ["########..", "..#.....#.", "..#......#", "..#......#", "..#......#",
          "..#######.", "..#......#", "..#......#", "..#.....#.", "########.."],
    'I': ["#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#..", "..#..", "..#..", "#####"],
    'L': ["#####...", "..#.....", "..#.....", "..#.....", "..#.....", "..#.....", "..#.....",
          "..#.....", "..#....#", "########"],
    'T': ["###########", "#....#....#", "#....#....#", ".....#.....", ".....#.....",
          ".....#.....", ".....#.....", ".....#.....", ".....#.....", "....###...."],
    'V': ["####....####", ".#........#.", ".#........#.", "..#......#..", "..#......#..",
          "...#....#...", "...#....#...", "....#..#....", "....#..#....", ".....##....."],
    'S': ["..#####.#", ".#.....##", "#.......#", "#........", ".##......",
          "...####..", ".......#.", "#.......#", "##.....#.", "#.#####.."],
    'O': ["..######..", ".#......#.", "#........#", "#........#", "#........#",
          "#........#", "#........#", "#........#", ".#......#.", "..######.."],
    'G': ["..#####.#.", ".#.....##.", "#.......#.", "#.........", "#.........",
          "#....#####", "#.......#.", "#.......#.", ".#.....##.", "..#####.#."],
}
BTN = {'.': (222, 186, 156), '_': (214, 170, 140), '-': (231, 199, 173), '+': (247, 223, 206),
       '*': (255, 239, 222), ':': (189, 138, 99), 'c': (156, 105, 57), 'd': (123, 69, 24),
       'D': (82, 36, 0), 'e': (107, 48, 16)}
CHISEL = {'t': (214, 166, 123), ';': (165, 113, 74), '=': (239, 211, 189), 'W': (255, 255, 255),
          '#': (99, 44, 8)}


# ================================================================== drawing helpers
def pixels(im):
    """Pillow 버전에 상관없이 픽셀 목록을 돌려준다."""
    flat = getattr(im, 'get_flattened_data', None)
    return list(flat()) if flat else list(im.getdata())


def from_rows(rows, pal):
    h, w = len(rows), max(len(r) for r in rows)
    im = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    for yy, r in enumerate(rows):
        for xx, ch in enumerate(r):
            if ch in pal:
                im.putpixel((xx, yy), pal[ch] + (255,))
    return im


def mask_paint(img, mask, x, y, color, dx=0, dy=0):
    img.paste(Image.new('RGB', mask.size, color), (x + dx, y + dy), mask)


def wood_profile(n):
    rows = [115] * n
    c0, c1 = (n - 1) // 2, n // 2
    for k, v in enumerate([148, 140, 132, 123]):
        if c0 - k >= 0:
            rows[c0 - k] = v
        if c1 + k < n:
            rows[c1 + k] = v
    rows[0] = rows[-1] = 99
    return [WOOD[v] for v in rows]


def plaque(style):
    w, h = PLAQUE_W, PLAQUE_H
    p = Image.new('RGB', (w, h))
    d = ImageDraw.Draw(p)
    d.rectangle([0, 0, w - 1, h - 1], outline=DARK)
    d.rectangle([1, 1, w - 2, h - 2], outline=GOLD_NAME)
    d.line([(2, 1), (w - 2, 1)], fill=(231, 199, 82))
    d.line([(w - 2, 1), (w - 2, h - 3)], fill=(231, 199, 82))
    d.rectangle([2, 2, w - 3, h - 3], outline=DARK2)
    ix, iy, iw, ih = INNER, INNER, w - 2 * INNER, h - 2 * INNER
    if style == 'day':
        for k, col in enumerate(wood_profile(ih)):
            d.line([(ix, iy + k), (ix + iw - 1, iy + k)], fill=col)
        d.line([(ix, iy), (ix, iy + ih - 1)], fill=(82, 36, 0))
    else:
        for k in range(ih):
            v = k / max(1, ih - 1)
            d.line([(ix, iy + k), (ix + iw - 1, iy + k)],
                   fill=(int(24 + 18 * v), int(10 + 8 * v), int(2 + 2 * v)))
        d.line([(ix, iy), (ix + iw - 1, iy)], fill=(8, 4, 0))
    for c in [(0, 0), (w - 1, 0), (0, h - 1), (w - 1, h - 1)]:
        p.putpixel(c, GOLD_LT)
    return p


def cell_bg(style, width):
    """명판 안쪽 한 열의 세로 그라데이션을 가로로 늘린 칸 (가로로는 색이 같으므로 어디에 놓아도 이음새가 없다)."""
    col = plaque(style).crop((40, INNER, 41, INNER + CELL_H))
    return col.resize((width, CELL_H), Image.NEAREST)


def background():
    tex = Image.open(SRC_TEXTURE).convert('RGB')
    n = len(FRAME)
    bg = Image.new('RGB', (PANEL_W, PANEL_H))
    bg.paste(tex.crop((0, 0, PANEL_W - 2 * n, PANEL_H - 2 * n)), (n, n))
    px = bg.load()
    for k, f in enumerate(SHADOW):
        x0, y0, x1, y1 = n + k, n + k, PANEL_W - 1 - n - k, PANEL_H - 1 - n - k
        pts = [(x, y) for x in range(x0, x1 + 1) for y in (y0, y1)] + \
              [(x, y) for y in range(y0 + 1, y1) for x in (x0, x1)]
        for x, y in pts:
            r, g, b = px[x, y]
            px[x, y] = (int(r * f), int(g * f), int(b * f))
    d = ImageDraw.Draw(bg)
    for k, col in enumerate(FRAME):
        d.rectangle([k, k, PANEL_W - 1 - k, PANEL_H - 1 - k], outline=col)
    # 홈 파인 구분선 + 가운데 금장식
    y, x0, x1 = DIV_Y, 14, PANEL_W - 15
    d.line([(x0, y), (x1, y)], fill=(41, 16, 0))
    d.line([(x0 + 1, y + 1), (x1 + 1, y + 1)], fill=(165, 113, 74))
    gem = from_rows(GEM, ICON_PAL)
    bg.paste(gem, (PANEL_W // 2 - 6, y - 3), gem)
    for sx in (PANEL_W // 2 - 16, PANEL_W // 2 + 16):
        for dx, dy in [(0, 0), (-1, 0), (1, 0), (0, -1), (0, 1)]:
            bg.putpixel((sx + dx, y + dy), (41, 16, 0))
        bg.putpixel((sx, y), GOLD_LT)
    return bg


def icon_frame(name, style):
    cell = cell_bg(style, ICON_W)
    ic = from_rows({'sun': SUN, 'moon': MOON, 'hourglass': HOURGLASS}[name], ICON_PAL)
    # 시안과 같은 세로 위치: 명판 안에서 (26 - h)//2 → 안쪽 칸 기준으로 3 을 뺀다
    y = (PLAQUE_H - ic.height) // 2 - INNER
    cell.paste(ic, ((ICON_W - ic.width) // 2, y), ic)
    return cell


def load_digit_font():
    for path in FONT_CANDIDATES:
        if os.path.exists(path):
            return ImageFont.truetype(path, 16)
    sys.exit('Liberation Serif Bold / Times New Roman Bold 글꼴을 찾을 수 없습니다.')


def glyph_mask(ch, fnt):
    """비트맵 게임 글꼴처럼 안티앨리어싱 없이 그린 글자. 기준선은 모든 글자가 같다."""
    m = Image.new('L', (24, 32), 0)
    d = ImageDraw.Draw(m)
    d.fontmode = '1'
    d.text((4, 4), ch, font=fnt, fill=255)
    return m


def digit_frames(style, fnt):
    masks = {c: glyph_mask(c, fnt) for c in '0123456789:'}
    # 숫자 전체의 잉크 범위 (세로 기준선/가로 폭 맞추기)
    top = min(masks[c].getbbox()[1] for c in '0123456789')
    bottom = max(masks[c].getbbox()[3] for c in '0123456789')
    ink_h = bottom - top
    y_off = (CELL_H - ink_h) // 2 - top          # 20줄 칸 안에서 세로 가운데
    ramp = [(255, 251, 206), (255, 239, 156), (255, 223, 107), (247, 199, 66),
            (231, 174, 41), (214, 150, 33), (189, 125, 24), (165, 105, 16)]
    frames = []
    for c in '0123456789:':
        m = masks[c]
        bb = m.getbbox()
        box_w = (DIGIT_W if c != ':' else COLON_W) - 1        # 마지막 1열은 그림자 자리
        x_off = (box_w - (bb[2] - bb[0])) // 2 - bb[0]
        cell = cell_bg(style, DIGIT_W)
        shifted = Image.new('L', (DIGIT_W, CELL_H), 0)
        shifted.paste(m, (x_off, y_off))
        if style == 'day':
            mask_paint(cell, shifted, 0, 0, (0, 0, 0), 1, 1)
            mask_paint(cell, shifted, 0, 0, (255, 255, 255))
        else:
            mask_paint(cell, shifted, 0, 0, (8, 4, 0), 1, 1)
            grad = Image.new('RGB', (DIGIT_W, CELL_H))
            gd = ImageDraw.Draw(grad)
            gtop = top + y_off
            for yy in range(CELL_H):
                k = min(len(ramp) - 1, max(0, (yy - gtop) * len(ramp) // max(1, ink_h)))
                gd.line([(0, yy), (DIGIT_W, yy)], fill=ramp[k])
            cell.paste(grad, (0, 0), shifted)
        frames.append(cell)
    return frames


def word_mask(text):
    glyphs = [H2_GLYPHS.get(c) for c in text]
    w = sum((len(g[0]) + 2) if g else 6 for g in glyphs) - 2
    m = Image.new('L', (w, 10), 0)
    x = 0
    for g in glyphs:
        if g is None:
            x += 6
            continue
        for yy, row in enumerate(g):
            for xx, ch in enumerate(row):
                if ch == '#':
                    m.putpixel((x + xx, yy), 255)
        x += len(g[0]) + 2
    return m


def chisel(img, m, x, y):
    for o in [(0, -1), (2, -2), (1, -2)]:
        mask_paint(img, m, x, y, CHISEL['t'], *o)
    for o in [(1, -1), (1, 0)]:
        mask_paint(img, m, x, y, CHISEL[';'], *o)
    for o in [(-2, 0), (-2, 1), (-1, 2), (0, 2)]:
        mask_paint(img, m, x, y, CHISEL['='], *o)
    for o in [(-1, 0), (0, 1), (-1, 1)]:
        mask_paint(img, m, x, y, CHISEL['W'], *o)
    mask_paint(img, m, x, y, CHISEL['#'])


def button(pressed, bg_under):
    """H2 BUILD/EXIT 와 같은 베벨. 눌림 = 색은 그대로, 베벨만 반대로(안으로 들어간 모양) + 글자 1px 이동."""
    w, h = BTN_W, BTN_H
    b = Image.new('RGB', (w, h), BTN['.'])
    d = ImageDraw.Draw(b)
    if not pressed:
        d.line([(2, 0), (w - 1, 0)], fill=BTN['_'])
        d.line([(4, 1), (w - 2, 1)], fill=BTN['-'])
        d.line([(4, 2), (w - 4, 2)], fill=BTN['+'])
        d.line([(int(w * 0.68), 2), (w - 5, 2)], fill=BTN['*'])
        d.line([(4, 3), (w - 4, 3)], fill=BTN['-'])
        for k, ch in enumerate('-*._'):
            d.line([(w - 4 + k, 1 + (k > 1)), (w - 4 + k, h - 5)], fill=BTN[ch])
        d.line([(w - 3, int(h * 0.6)), (w - 3, h - 6)], fill=BTN['+'])
        for k, ch in enumerate('Ddc:'):
            d.line([(k, 1 + (k == 0)), (k, h - 1)], fill=BTN[ch])
        for k, ch in enumerate(':cdD'):
            d.line([(4 - min(k, 3) * (k > 0), h - 4 + k), (w - 2 - (k == 3), h - 4 + k)], fill=BTN[ch])
        for k in range(4):
            d.line([(k, h - 4 + k), (k, h - 1)], fill=BTN['Ddc:'[k]])
        off = 0
    else:
        # 위·왼쪽은 어둡게, 아래·오른쪽은 밝게 → 버튼이 판 안으로 눌려 들어간 모양
        for k, ch in enumerate('Ddc:'):
            d.line([(k, k), (w - 1 - k, k)], fill=BTN[ch])          # top
            d.line([(k, k), (k, h - 1 - k)], fill=BTN[ch])          # left
        for k, ch in enumerate('_-+-'):
            d.line([(k + 1, h - 1 - k), (w - 1 - k, h - 1 - k)], fill=BTN[ch])   # bottom
            d.line([(w - 1 - k, k + 1), (w - 1 - k, h - 1 - k)], fill=BTN[ch])   # right
        off = 1
    m = word_mask('VISIT BLOG')
    chisel(b, m, (w - m.width) // 2 + off, (h - 4 - m.height) // 2 + 2 + off)
    # 둥근 모서리 자리에 실제 배경을 미리 합성해서 프레임을 불투명하게 만든다
    for c in [(0, 0), (1, 0), (0, 1), (w - 1, h - 1)]:
        b.putpixel(c, bg_under.getpixel(c))
    return b


# ================================================================== H3 file writers
SPECIAL = [(0, 255, 255), (255, 150, 255), (255, 100, 255), (255, 50, 255),
           (255, 0, 255), (255, 255, 0), (180, 0, 255), (0, 255, 0)]


def build_palette(images, first=8, last=254):
    """특수색 0~7 과 RLE 예약값 255 는 비워 두고 실제 색은 8~254 에만 배치한다."""
    colors = []
    seen = set()
    for im in images:
        for c in pixels(im):
            if c not in seen:
                seen.add(c)
                colors.append(c)
    if len(colors) > last - first + 1:
        raise SystemExit('색이 너무 많습니다: %d' % len(colors))
    pal = list(SPECIAL) + [(0, 0, 0)] * (256 - len(SPECIAL))
    index = {}
    for i, c in enumerate(colors):
        pal[first + i] = c
        index[c] = first + i
    return pal, index


def rle_row(row):
    """DEF format 1 한 줄: [색, 길이-1] 반복 구간 또는 [0xFF, 길이-1] + 원본 픽셀."""
    out = bytearray()
    i, n = 0, len(row)
    while i < n:
        j = i
        while j < n and row[j] == row[i] and j - i < 256:
            j += 1
        run = j - i
        if run >= 3:
            out += bytes((row[i], run - 1))
            i = j
            continue
        k = i
        while k < n and k - i < 256:
            r = 1
            while k + r < n and row[k + r] == row[k] and r < 3:
                r += 1
            if r >= 3:
                break
            k += 1
        k = max(k, i + 1)
        out += bytes((0xFF, k - i - 1)) + bytes(row[i:k])
        i = k
    return out


def encode_frame(img, index):
    w, h = img.size
    px = [index[c] for c in pixels(img)]
    rows = [rle_row(px[y * w:(y + 1) * w]) for y in range(h)]
    offsets, data, pos = [], bytearray(), 4 * h
    for r in rows:
        offsets.append(pos)
        data += r
        pos += len(r)
    body = struct.pack('<%dI' % h, *offsets) + bytes(data)
    header = struct.pack('<IIIIIIii', len(body), 1, w, h, w, h, 0, 0)
    return header + body


def write_def(path, groups):
    """groups: [(group_id, [PIL.Image, ...]), ...]"""
    all_imgs = [im for _, ims in groups for im in ims]
    pal, index = build_palette(all_imgs)
    W = max(im.width for im in all_imgs)
    H = max(im.height for im in all_imgs)
    base = os.path.splitext(os.path.basename(path))[0].lower()[:6]
    head = struct.pack('<IIII', 0x47, W, H, len(groups)) + bytes(c for rgb in pal for c in rgb)
    blocks_size = sum(16 + len(ims) * 17 for _, ims in groups)
    frame_pos = len(head) + blocks_size
    blocks, frames = bytearray(), bytearray()
    for gid, ims in groups:
        names, offs = bytearray(), bytearray()
        for fi, im in enumerate(ims):
            names += ('%s%d%d.pcx' % (base, gid, fi)).encode('ascii')[:12].ljust(13, b'\0')
            offs += struct.pack('<I', frame_pos + len(frames))
            frames += encode_frame(im, index)
        blocks += struct.pack('<IIII', gid, len(ims), 0, 0) + names + offs
    with open(path, 'wb') as f:
        f.write(head + blocks + frames)


def write_pcx8(path, img):
    """H3 자체 PCX: [크기][너비][높이] + 8bit 픽셀 + 팔레트(RGB 256색)."""
    q = img if len(set(pixels(img))) <= 256 else img.quantize(256).convert('RGB')
    data = pixels(q)
    colors = sorted(set(data))
    index = {c: i for i, c in enumerate(colors)}
    w, h = img.size
    pix = bytes(index[c] for c in data)
    pal = colors + [(0, 0, 0)] * (256 - len(colors))
    with open(path, 'wb') as f:
        f.write(struct.pack('<III', w * h, w, h) + pix + bytes(c for rgb in pal for c in rgb))


# ================================================================== main
def build_all(out_dir=OUT):
    os.makedirs(out_dir, exist_ok=True)
    fnt = load_digit_font()
    bg = background()
    write_pcx8(os.path.join(out_dir, 'DsIpBg.pcx'), bg)
    write_def(os.path.join(out_dir, 'DsIpPlq.def'), [(0, [plaque('day'), plaque('night')])])
    write_def(os.path.join(out_dir, 'DsIpIco.def'),
              [(0, [icon_frame('sun', 'day'), icon_frame('moon', 'night'),
                    icon_frame('hourglass', 'day'), icon_frame('hourglass', 'night')])])
    write_def(os.path.join(out_dir, 'DsIpDgt.def'),
              [(0, digit_frames('day', fnt)), (1, digit_frames('night', fnt))])
    under = bg.crop((BTN_X, BTN_Y, BTN_X + BTN_W, BTN_Y + BTN_H))
    write_def(os.path.join(out_dir, 'DsIpBtn.def'), [(0, [button(False, under), button(True, under)])])
    return out_dir


if __name__ == '__main__':
    print('written to', build_all(sys.argv[1] if len(sys.argv) > 1 else OUT))
