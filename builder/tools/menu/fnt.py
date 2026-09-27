#!/usr/bin/env python3
"""The mobile menus' bitmap fonts (Data/Game/Menu/<Name>_EN.fnt: BMFont text format + one page PNG), read and drawn at
build time for the PS1 menus (docs/30 phase 4.2).

Font(menu_dir, name, page=None): glyphs by code point {x, y, w, h, xoff, yoff, xadv}, lineHeight, base; the page
image (RGBA). render(text, scale) -> RGBA image of the string (glyphs cut from the page, placed at their offsets and
advances like the native renderer, then scaled with a box filter).
"""
import os, re
from PIL import Image


class Font:
    def __init__(self, menu_dir, name, page=None):
        self.chars, self.line, self.base = {}, 0, 0
        pagefile = None
        for l in open(os.path.join(menu_dir, name + '.fnt'), encoding='utf-8', errors='replace'):
            kv = dict(re.findall(r'(\w+)=("[^"]*"|\S+)', l))
            if l.startswith('common '):
                self.line, self.base = int(kv['lineHeight']), int(kv['base'])
                self.scaleW = int(kv['scaleW'])
            elif l.startswith('page '):
                pagefile = kv['file'].strip('"')
            elif l.startswith('char '):
                self.chars[int(kv['id'])] = dict(x=int(kv['x']), y=int(kv['y']), w=int(kv['width']), h=int(kv['height']),
                                                  xoff=int(kv['xoffset']), yoff=int(kv['yoffset']), xadv=int(kv['xadvance']))
        path = os.path.join(menu_dir, page or pagefile)
        if not os.path.exists(path):  # the .fnt names e.g. Text.png; the file is Text_EN.png
            path = os.path.join(menu_dir, name + '.png')
        self.page = Image.open(path).convert('RGBA')
        # the page may be a half-size copy (@1x) of the one the .fnt describes
        self.k = self.page.size[0] / self.scaleW

    def width(self, text):
        return sum(self.chars.get(ord(c), self.chars.get(32, dict(xadv=0)))['xadv'] for c in text)

    def render(self, text, scale):
        """RGBA image of `text` at `scale` (1.0 = the .fnt's own pixel size)."""
        W, H = max(1, self.width(text)), self.line
        img = Image.new('RGBA', (W + 8, H), (0, 0, 0, 0))
        x = 0
        for c in text:
            g = self.chars.get(ord(c))
            if not g:
                continue
            if g['w'] and g['h']:
                k = self.k
                box = (round(g['x'] * k), round(g['y'] * k), round((g['x'] + g['w']) * k), round((g['y'] + g['h']) * k))
                gl = self.page.crop(box).resize((g['w'], g['h']), Image.LANCZOS) if k != 1 else self.page.crop(box)
                img.alpha_composite(gl, (max(0, x + g['xoff']), max(0, g['yoff'])))
            x += g['xadv']
        return img.resize((max(1, round(img.size[0] * scale)), max(1, round(img.size[1] * scale))), Image.BOX)
