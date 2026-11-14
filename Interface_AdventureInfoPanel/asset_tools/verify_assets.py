# -*- coding: utf-8 -*-
"""
생성된 PCX/DEF 를 게임과 같은 방식(H3 PCX, DEF format 1)으로 다시 읽어서
C++ InfoPanel::Render() 와 똑같은 순서로 패널을 합성해 PNG 로 저장한다. (리소스 검증용)

  python verify_assets.py  →  asset_tools/preview/preview_day.png, preview_night.png, preview_pressed.png
"""
import os
import struct
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.normpath(os.path.join(HERE, '..', 'Data'))
PREVIEW = os.path.join(HERE, 'preview')

# InfoPanelLayout.h 와 같은 값
PANEL_W, PANEL_H = 194, 105
PLAQUE_X, PLAQUE_Y, PLAQUE_W, PLAQUE_H, PLAQUE_INNER = (8, 99), 25, 87, 26, 3
PLAQUE_INNER_W = PLAQUE_W - 2 * PLAQUE_INNER
ICON_W, ICON_GAP, DIGIT_W, COLON_W, CELL_H = 15, 3, 9, 5, 20
BTN_X, BTN_Y = 8, 63


def read_pcx(path):
    b = open(path, 'rb').read()
    size, w, h = struct.unpack_from('<III', b, 0)
    assert size == w * h, 'only 8-bit H3 pcx expected'
    pix = b[12:12 + size]
    pal = b[12 + size:12 + size + 768]
    assert len(pal) == 768
    im = Image.new('RGB', (w, h))
    im.putdata([tuple(pal[i * 3:i * 3 + 3]) for i in pix])
    return im


def read_def(path):
    b = open(path, 'rb').read()
    typ, W, H, ngroups = struct.unpack_from('<IIII', b, 0)
    pal = [tuple(b[16 + i * 3:16 + i * 3 + 3]) for i in range(256)]
    pos = 16 + 768
    groups = {}
    for _ in range(ngroups):
        gid, count, _u1, _u2 = struct.unpack_from('<IIII', b, pos)
        pos += 16 + 13 * count
        offs = struct.unpack_from('<%dI' % count, b, pos)
        pos += 4 * count
        frames = []
        for off in offs:
            size, fmt, fw, fh, w, h, lm, tm = struct.unpack_from('<IIIIIIii', b, off)
            assert fmt == 1 and lm == 0 and tm == 0 and (w, h) == (fw, fh)
            base = off + 32
            assert base + size <= len(b)
            rows = struct.unpack_from('<%dI' % h, b, base)
            px = []
            for r in rows:
                p, line = base + r, []
                while len(line) < w:
                    code, ln = b[p], b[p + 1] + 1
                    p += 2
                    if code == 0xFF:
                        line += list(b[p:p + ln])
                        p += ln
                    else:
                        line += [code] * ln
                assert len(line) == w, 'row overflow'
                assert all(8 <= i <= 254 for i in line), 'special colour index used'
                px += line
            im = Image.new('RGB', (w, h))
            im.putdata([pal[i] for i in px])
            frames.append(im)
        groups[gid] = frames
    return typ, (W, H), groups


def render(bg, plq, ico, dgt, btn, now='21:30', play='01:25', pressed=False):
    hour = int(now.split(':')[0])
    night = not (6 <= hour < 18)
    c = bg.copy()
    for i, (text, icon) in enumerate([(now, 1 if night else 0), (play, 3 if night else 2)]):
        c.paste(plq[0][int(night)], (PLAQUE_X[i], PLAQUE_Y))
        tw = sum(COLON_W if ch == ':' else DIGIT_W for ch in text)
        total = ICON_W + ICON_GAP + tw
        x = PLAQUE_X[i] + PLAQUE_INNER + (PLAQUE_INNER_W - total) // 2
        y = PLAQUE_Y + PLAQUE_INNER
        c.paste(ico[0][icon], (x, y))
        x += ICON_W + ICON_GAP
        for ch in text:
            w = COLON_W if ch == ':' else DIGIT_W
            fr = dgt[int(night)][10 if ch == ':' else int(ch)]
            c.paste(fr.crop((0, 0, w, CELL_H)), (x, y))
            x += w
    c.paste(btn[0][int(pressed)], (BTN_X, BTN_Y))
    return c


if __name__ == '__main__':
    d = sys.argv[1] if len(sys.argv) > 1 else DATA
    bg = read_pcx(os.path.join(d, 'DsIpBg.pcx'))
    parts = {}
    for n in ('DsIpPlq', 'DsIpIco', 'DsIpDgt', 'DsIpBtn'):
        typ, size, groups = read_def(os.path.join(d, n + '.def'))
        print('%-8s type=0x%X size=%s groups=%s' % (n, typ, size, {k: len(v) for k, v in groups.items()}))
        parts[n] = groups
    args = (bg, parts['DsIpPlq'], parts['DsIpIco'], parts['DsIpDgt'], parts['DsIpBtn'])
    os.makedirs(PREVIEW, exist_ok=True)
    render(*args, now='14:05', play='01:25').save(os.path.join(PREVIEW, 'preview_day.png'))
    render(*args, now='21:30', play='123:45').save(os.path.join(PREVIEW, 'preview_night.png'))
    render(*args, now='21:30', play='01:25', pressed=True).save(os.path.join(PREVIEW, 'preview_pressed.png'))
    print('ok')
