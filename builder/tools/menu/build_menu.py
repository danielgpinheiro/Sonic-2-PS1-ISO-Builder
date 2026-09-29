#!/usr/bin/env python3
"""PS1 menu art for Sonic 2 (docs/30 phase 4.2): the mobile menus' 2D art and fonts, converted at build time.

The 2013 menus (RSDKv4/NativeObjects, not built on PS1) draw RGBA textures from Data/Game/Menu: BMFont fonts
(Heading / Label / Text_EN.fnt + page PNGs), PlayerSelect.png (portraits), Symbols.png (pixel character icons,
emeralds), SonicLogo.png, BG1.png (line-art watermark). This tool scales them to the PS1 menus' 320 x 240 layout
(user decision 2026-09-26: the approved mockups) and writes three packs:

  Data/Game/Menu/PS1Fonts.pmn  resident: small fonts for the pause menu and dialogs, in VRAM rows 242-255
                               (x 0-319: free in every stage, under framebuffer 0), CLUT row 499, and the menus'
                               strings; uploaded at boot
  Data/Game/Menu/PS1Menu.pmn   menu screens: large fonts + pictures, in the sprite / tile columns (x 320-1023:
                               no stage is drawn while the menus are open), CLUT rows 500-511; uploaded when the
                               menus open
  Data/Game/Menu/PS1TimeAttack.pmn  the Time Attack act pictures (TimeAttack1-4.png, 128 x 96, 8-bit), in page row 1
                               of the same columns (the menu pack keeps to page row 0); uploaded with the Time
                               Attack screen (phase 4.3c)

Alpha becomes three levels: transparent (0x0000), 50 % (a colour with the STP bit: blended B/2 + F/2 when the
primitive is semi-transparent) and opaque (STP clear). Colours are reduced only as the format needs (4-bit: 15
entries, 8-bit: 255), with the opaque and the 50 % colours quantized apart. ps1/menu_pack.h (generated) names the
images, fonts and strings.

Pack (little endian):
  header : 'PMN1', u16 images, fonts, glyphs, blocks, strings, pad, u32 dataOffset          (20 B)
  image  : u16 w, h, u8 u, v, u16 tpage (GP0 E1h bits 0-8), u16 clut (CLUT word), u8 semi, pad (12 B)
  font   : u16 firstGlyph, glyphCount, u8 lineHeight, base, u16 pad                         (8 B)
  glyph  : u16 code, image, s8 xoff, yoff, u16 advance (1/16 px)                           (8 B)
  block  : u16 x, y, w, h (VRAM halfwords), u32 offset of w*h u16 at dataOffset + offset   (12 B)
  string : u16 length, then the bytes (ASCII upper range kept as Latin-1)
Self-check: every image is decoded back from the pack (blocks -> VRAM, CLUT + texpage + uv) and compared with its
converted art; exact.

Sonic 1 (--game 1, docs/37 phase 4): the same art from its Data/Game/Menu, its strings (StageName1-8, SaveStageName1-26),
and its Time Attack pictures: 18 acts (TimeAttack1-3, the act's slot (3 * zone + act) % 6), the Final Zone (Intro.png at
641, 721) and 6 special stages (TimeAttack4 slots 0-5) -- 25 pictures at 112 x 84, six per texture page, one 8-bit CLUT
per source sheet (y 508-510, under the pictures); the header is ps1/menu_pack_s1.h.

Usage: build_menu.py GAME_DATA_DIR OUT_DATA_DIR [--preview DIR] [--game 1|2]   (GAME_DATA_DIR: the extracted Data/)
"""
import os, re, struct, sys
import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fnt import Font  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..'))

# Strings the menus show (StringList.txt keys, English): screens, items, messages, stage names.
STRINGS = ['StartGame', 'TimeAttack', 'HelpAndOptions', 'SaveSelect', 'PlayerSelect', 'NoSave', 'NewGame', 'Delete',
           'DeleteSavedGame', 'Yes', 'No', 'Continue', 'Restart', 'Exit', 'Pause', 'RestartMessage', 'ExitMessage',
           'Settings', 'Music', 'SoundFX', 'SpinDash', 'On', 'Off', 'StaffCredits', 'Records', 'NewBestTime',
           'TotalTime', 'Play', 'Go', 'NextAct', 'PressStart', 'Sonic', 'Tails', 'Knuckles'] + \
          ['StageName%d' % i for i in range(1, 13)] + ['SaveStageName%d' % i for i in range(1, 26)]
EXTRA_STRINGS = {'SonicAndTails': 'SONIC & TAILS', 'Options': 'OPTIONS', 'Back': 'BACK'}
EXTRA_CHARS = "0123456789:'\".,!?-&/()<> "

# Fonts: (name in the pack, .fnt, page, scale, strings drawn with it, pack)
FONTS = [
    ('HEADING_S', 'Heading_EN', 'Heading_EN@1x.png', 0.11, 'heading', 'resident'),  # its S: 13 px of the 14 rows
    ('LABEL_S', 'Label_EN', 'Label_EN@1x.png', 0.105, 'label', 'resident'),         # its O: 13.7 px
    ('TEXT_S', 'Text_EN', 'Text_EN.png', 0.20, 'text', 'resident'),
    ('HEADING_L', 'Heading_EN', 'Heading_EN@1x.png', 0.20, 'heading', 'menu'),
    ('LABEL_L', 'Label_EN', 'Label_EN@1x.png', 0.14, 'label', 'menu'),
    ('LABEL_M', 'Label_EN', 'Label_EN@1x.png', 0.10, 'label', 'menu'),  # save files' stage names (native: 0.08 vs 0.1)
]
# Which strings each font style draws (their characters are the glyph set). The resident fonts (pause menu and its
# dialogs, 14 VRAM rows tall) carry only what those screens show.
HEADING_KEYS = ['Pause', 'SaveSelect', 'PlayerSelect', 'TimeAttack', 'Settings', 'Records', 'HelpAndOptions', 'Options']
TEXT_KEYS = ['RestartMessage', 'ExitMessage', 'DeleteSavedGame', 'NewBestTime', 'TotalTime', 'PressStart']
RESIDENT_KEYS = {'heading': ['Pause'], 'label': ['Continue', 'Restart', 'Exit', 'Yes', 'No'],
                 'text': ['RestartMessage', 'ExitMessage']}

# VRAM areas (halfwords): x, y, w, h
RESIDENT_AREA = [(0, 242, 320, 14)]               # glyph strips; 4-bit texture pages at pageX 0-4, pageY 0
RESIDENT_CLUT_ROW = 499
MENU_COLUMNS = list(range(5, 16))                 # x 320-1023: the atlas (5-8, 13-15) and tile (9-12) columns
MENU_CLUT_ROWS = list(range(500, 512))            # 8-bit CLUTs: one row each (x 0-255); 4-bit CLUTs at x 256-319
# Time Attack pack (PS1TimeAttack.pmn, loaded with the Time Attack screen, beside PS1Menu.pmn): the act pictures of
# TimeAttack1-4.png at 128 x 96, 8-bit, four per texture page in page row 1 (y 256-447) of the menu columns 5-14,
# their CLUTs below them (y 448-457, x 320 / 576).
TA_COLUMNS = list(range(5, 15))
TA_CLUTS = [(x, y) for y in range(448, 458) for x in (320, 576)]
TA_SIZE = (128, 96)
# (sheet, slot) per picture, in the order ps1/menu.cpp indexes them: the records' acts 0-16 (Emerald Hill 1 ...
# Metropolis 3) and 17 (Wing Fortress) at sheet r // 6 + 1, slot r % 6; then Boss Attack (TimeAttack4 slot 0) and
# Hidden Palace (slot 1) -- RecordsScreen.cpp's timeAttackU / V
TA_PICTURES = [(r // 6 + 1, r % 6) for r in range(18)] + [(4, 0), (4, 1)]
TA_U = [1, 321, 641, 1, 321, 641]
TA_V = [1, 1, 1, 241, 241, 241]
# Sonic 1 (RecordsScreen.cpp GAME_SONIC1): (sheet or 'Intro', u, v) per picture: acts 0-17 (sheet zone // 2 + 1, slot
# (3 * zone + act) % 6), the Final Zone, special stages 1-6
TA_PICTURES_S1 = [(z // 2 + 1, TA_U[(3 * z + a) % 6], TA_V[(3 * z + a) % 6]) for z in range(6) for a in range(3)] + \
                 [('Intro', 641, 721)] + [(4, TA_U[k], TA_V[k]) for k in range(6)]
TA_SIZE_S1 = (112, 84)
TA_CLUTS_S1 = [(320, 508), (576, 508), (320, 509), (576, 509), (320, 510)]  # one per sheet (TimeAttack1-4, Intro)
STRINGS_S1 = STRINGS[:STRINGS.index('StageName1')] + ['StageName%d' % i for i in range(1, 9)] + \
             ['SaveStageName%d' % i for i in range(1, 27)]


BLEND_ALL = {'WATERMARK'}  # pictures whose every texel blends (drawn +25 % additive: a faint line art)


class PackError(Exception):
    pass


def ps1_colour(r, g, b, stp):
    c = (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)
    if stp:
        return c | 0x8000
    return c if c else 0x0421  # opaque black: 0x0000 would be transparent


def classify(img, blend_all=False):
    """RGBA -> (rgb array, level array: 0 transparent, 1 50 %, 2 opaque). blend_all: every visible texel blends
    (the watermark, drawn +25 % additive)."""
    a = np.array(img.convert('RGBA'), dtype=np.int32)
    lvl = np.where(a[..., 3] >= 192, 2, np.where(a[..., 3] >= 64, 1, 0))
    if blend_all:
        lvl = np.where(a[..., 3] >= 64, 1, 0)
    return a[..., :3], lvl


def quantize(rgb, lvl, depth):
    """Indexed image + CLUT (list of PS1 colours, entry 0 = transparent). Opaque and 50 % colours apart."""
    maxc = 15 if depth == 4 else 255
    idx = np.zeros(lvl.shape, np.uint8)
    clut = [0x0000]
    opaque, semi = rgb[lvl == 2], rgb[lvl == 1]
    uniq_o = np.unique(opaque.reshape(-1, 3), axis=0) if len(opaque) else np.zeros((0, 3), np.int32)
    uniq_s = np.unique(semi.reshape(-1, 3), axis=0) if len(semi) else np.zeros((0, 3), np.int32)
    n_s = min(len(uniq_s), (max(1, maxc // 4) if len(uniq_o) else maxc) if len(uniq_s) else 0)
    n_o = min(len(uniq_o), maxc - n_s)
    for level, uniq, n in ((2, uniq_o, n_o), (1, uniq_s, n_s)):
        if not n:
            continue
        px = rgb[lvl == level].reshape(-1, 3).astype(np.uint8)
        if len(uniq) <= n:
            pal = uniq
        else:  # reduce: PIL median cut on the level's pixels
            q = Image.fromarray(px.reshape(1, -1, 3), 'RGB').quantize(colors=n, method=Image.Quantize.MEDIANCUT)
            p = q.getpalette()[:3 * n]
            pal = np.array(p, np.int32).reshape(-1, 3)
        base = len(clut)
        clut += [ps1_colour(int(c[0]), int(c[1]), int(c[2]), level == 1) for c in pal]
        # nearest palette colour per pixel (in 5-bit space: what the PS1 shows)
        d = ((px[:, None, :].astype(np.int32) >> 3) - (pal[None, :, :] >> 3)) ** 2
        near = d.sum(axis=2).argmin(axis=1)
        idx[lvl == level] = (base + near).astype(np.uint8)
    return idx, clut


def glyph_images(font, scale, chars):
    """{code: (RGBA glyph image or None, xoff, yoff, advance 1/16 px)} at `scale`."""
    out = {}
    for ch in sorted(chars):
        g = font.chars.get(ord(ch))
        if not g:
            continue
        adv = int(round(g['xadv'] * scale * 16))
        if not g['w'] or not g['h']:
            out[ord(ch)] = (None, 0, 0, adv)
            continue
        k = font.k
        box = (round(g['x'] * k), round(g['y'] * k), round((g['x'] + g['w']) * k), round((g['y'] + g['h']) * k))
        img = font.page.crop(box)
        w, h = max(1, round(g['w'] * scale)), max(1, round(g['h'] * scale))
        img = img.resize((w, h), Image.LANCZOS)
        out[ord(ch)] = (img, int(round(g['xoff'] * scale)), int(round(g['yoff'] * scale)), adv)
    return out


class Packer:
    """Shelf packer over texture pages of one depth inside a set of VRAM columns (64 hw each, 256 lines per page)."""

    def __init__(self, columns, page_rows, y_ranges=None):
        self.free = []  # (pageX column list, pageY, y0, y1)
        self.pages = {}  # (col, pageY, depth) -> {'shelves': [[y, h, x]]}
        self.columns, self.page_rows = columns, page_rows
        self.y_ranges = y_ranges or {py: (0, 256) for py in page_rows}
        self.used = set()  # (col, pageY) claimed by a depth

    def place(self, w, h, depth):
        """(tpage x col, pageY, u, v) for a w x h texel image. Images take whole 32-bit words of VRAM (8 texels at
        4-bit, 4 at 8-bit): u stays word-aligned, so neighbours never share a halfword and every upload moves an
        even number of halfwords (psyqo uploadToVRAM rejects odd counts)."""
        per = 8 if depth == 4 else 4
        w += -w % per
        span = 1 if depth == 4 else 2  # an 8-bit page spans 2 columns (256 texels = 128 hw)
        for py in self.page_rows:
            y0, y1 = self.y_ranges[py]
            for i, col in enumerate(self.columns):
                cols = self.columns[i:i + span]
                if len(cols) < span or any(c != col + k for k, c in enumerate(cols)):
                    continue
                key = (col, py, depth)
                if key not in self.pages:
                    if any((c, py) in self.used for c in cols):
                        continue
                    for c in cols:
                        self.used.add((c, py))
                    self.pages[key] = {'shelves': []}
                pg = self.pages[key]
                for sh in pg['shelves']:
                    if h <= sh[1] and sh[2] + w <= 256:
                        u = sh[2]
                        sh[2] += w
                        return col, py, u, sh[0]
                top = pg['shelves'][-1][0] + pg['shelves'][-1][1] if pg['shelves'] else y0
                if top + h <= y1 and w <= 256:
                    pg['shelves'].append([top, h, w])
                    return col, py, 0, top
        raise PackError('no VRAM room for a %dx%d %d-bit image' % (w, h, depth))


def decode_check(checks, vram, name):
    """Sample every image back from the VRAM words: exact."""
    for key, idx, clut, col, py, u, v, depth, cx, cy in checks:
        h, w = idx.shape
        per = 4 if depth == 4 else 2
        for yy in range(h):
            for xx in range(w):
                tx = u + xx
                hw = vram.get((col * 64 + tx // per, py * 256 + v + yy), 0)
                t = (hw >> ((tx % per) * (4 if depth == 4 else 8))) & (0xF if depth == 4 else 0xFF)
                if t != idx[yy, xx]:
                    raise PackError('%s: %s texel (%d,%d) %d != %d' % (name, key, xx, yy, t, idx[yy, xx]))
                if vram.get((cx + t, cy), 0) != (clut[t] if t < len(clut) else 0):
                    raise PackError('%s: %s CLUT entry %d differs' % (name, key, t))


def write_pack(path, images, fonts, glyphs, blocks, strings):
    out = bytearray()
    blk_tab, pixels = bytearray(), bytearray()
    for x, y, w, h, data in blocks:
        blk_tab += struct.pack('<HHHHI', x, y, w, h, len(pixels))
        pixels += struct.pack('<%dH' % len(data), *data)
    str_tab = bytearray()
    for s in strings:
        b = s.encode('latin-1', 'replace')
        str_tab += struct.pack('<H', len(b)) + b
    img_tab = b''.join(struct.pack('<HHBBHHBB', w, h, u, v, tp, cl, semi, 0) for (w, h, u, v, tp, cl, semi) in images)
    fnt_tab = b''.join(struct.pack('<HHBBH', f, n, lh, base, 0) for (f, n, lh, base) in fonts)
    gly_tab = b''.join(struct.pack('<HHbbH', c, im, xo, yo, adv) for (c, im, xo, yo, adv) in glyphs)
    head_len = 20 + len(img_tab) + len(fnt_tab) + len(gly_tab) + len(blk_tab) + len(str_tab)
    data_off = (head_len + 3) & ~3
    out += struct.pack('<4sHHHHHHI', b'PMN1', len(images), len(fonts), len(glyphs), len(blocks), len(strings), 0, data_off)
    out += img_tab + fnt_tab + gly_tab + blk_tab + str_tab
    out += b'\0' * (data_off - len(out))
    out += pixels
    open(path, 'wb').write(out)
    return len(out)


def load_strings(game, keys=None):
    t = open(os.path.join(game, 'Game', 'StringList.txt'), 'rb').read().decode('utf-16')
    en = {k: v.strip() for lang, k, v in re.findall(r'\n(\w+):(\w+):\s*\n\t?(.*?)\nend string', t, re.S) if lang == 'en'}
    out = {}
    for k in keys or STRINGS:
        if k not in en:
            raise PackError('StringList.txt has no en:%s' % k)
        out[k] = en[k].replace('\r', '').replace('\n\t', '\n').replace('\t', '')
    out.update(EXTRA_STRINGS)
    return out


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        sys.exit(__doc__)
    game, outdata = args[0], args[1]
    preview = args[args.index('--preview') + 1] if '--preview' in args else None
    s1 = '--game' in args and args[args.index('--game') + 1] == '1'
    menu = os.path.join(game, 'Game', 'Menu')
    os.makedirs(os.path.join(outdata, 'Game', 'Menu'), exist_ok=True) # the disc tree leaves the mobile art out
    strings = load_strings(game, STRINGS_S1 if s1 else STRINGS)
    keys = list(strings)
    heading_chars = set(''.join(strings[k] for k in HEADING_KEYS)) | set(EXTRA_CHARS)
    text_chars = set(''.join(strings[k] for k in TEXT_KEYS)) | set(EXTRA_CHARS)
    label_chars = set(''.join(strings.values())) | set(EXTRA_CHARS)
    style_chars = {'heading': heading_chars, 'label': label_chars, 'text': text_chars}
    resident_chars = {k: set(''.join(strings[x] for x in v)) | {' '} for k, v in RESIDENT_KEYS.items()}
    fonts_by_file = {}

    packs = {'resident': [], 'menu': []}  # items: (kind, key, img, depth)
    fontdefs = {'resident': [], 'menu': [], 'ta': []}
    for fname, fnt, page, scale, style, pack in FONTS:
        key = (fnt, page)
        if key not in fonts_by_file:
            fonts_by_file[key] = Font(menu, fnt, page)
        f = fonts_by_file[key]
        glyphs = glyph_images(f, scale, (resident_chars if pack == 'resident' else style_chars)[style])
        gl = []
        for code, (img, xo, yo, adv) in sorted(glyphs.items()):
            if img is not None:
                packs[pack].append(('glyph', '%s:%d' % (fname, code), img, 4))
                gl.append((code, len(packs[pack]) - 1, xo, yo, adv))
            else:
                gl.append((code, 0xFFFF, 0, 0, adv))
        fontdefs[pack].append((fname, gl, int(round(f.line * scale)), int(round(f.base * scale))))

    # menu pictures
    ps = Image.open(os.path.join(menu, 'PlayerSelect.png')).convert('RGBA')
    sym = Image.open(os.path.join(menu, 'Symbols.png')).convert('RGBA')
    logo = Image.open(os.path.join(menu, 'SonicLogo.png')).convert('RGBA').resize((200, 100), Image.LANCZOS)
    bg1 = Image.open(os.path.join(menu, 'BG1.png')).convert('RGBA').resize((230, 230), Image.LANCZOS)
    pics = [
        ('LOGO', logo, 8),
        ('PORTRAIT_SONIC', ps.crop((0, 0, 256, 256)).resize((140, 140), Image.LANCZOS), 8),
        ('PORTRAIT_TAILS', ps.crop((256, 0, 512, 256)).resize((140, 140), Image.LANCZOS), 8),
        ('PORTRAIT_KNUX', ps.crop((0, 256, 256, 512)).resize((140, 140), Image.LANCZOS), 8),
        ('PLATE', ps.crop((256, 256, 512, 512)).resize((150, 150), Image.LANCZOS), 4),
        ('ICON_SONIC', sym.crop((0, 160, 48, 256)).resize((14, 28), Image.NEAREST), 4),
        ('ICON_TAILS', sym.crop((48, 160, 112, 256)).resize((19, 28), Image.NEAREST), 4),
        ('ICON_KNUX', sym.crop((112, 160, 192, 256)).resize((23, 28), Image.NEAREST), 4),
        ('EMERALD_ON', sym.crop((192, 0, 256, 64)).resize((10, 10), Image.LANCZOS), 4),
        ('EMERALD_OFF', sym.crop((128, 0, 192, 64)).resize((10, 10), Image.LANCZOS), 4),
        ('LOCK', sym.crop((192, 64, 256, 128)).resize((16, 16), Image.LANCZOS), 4),
        ('WATERMARK', bg1, 4),
    ]
    pic_index = {}
    for key, img, depth in pics:
        packs['menu'].append(('image', key, img, depth))
        pic_index[key] = len(packs['menu']) - 1
    packs['ta'] = []
    sheets = {}
    if s1:
        for i, (sheet, u, v) in enumerate(TA_PICTURES_S1):
            if sheet not in sheets:
                name = 'Intro.png' if sheet == 'Intro' else 'TimeAttack%d.png' % sheet
                sheets[sheet] = Image.open(os.path.join(menu, name)).convert('RGBA')
            pic = sheets[sheet].crop((u, v, u + 318, v + 238)).resize(TA_SIZE_S1, Image.LANCZOS)
            pic.putalpha(255)
            packs['ta'].append(('grouped', 'SHEET%s:%d' % (sheet, i), pic, 8))
    else:
        for i, (sheet, slot) in enumerate(TA_PICTURES):
            if sheet not in sheets:
                sheets[sheet] = Image.open(os.path.join(menu, 'TimeAttack%d.png' % sheet)).convert('RGBA')
            u, v = TA_U[slot], TA_V[slot]
            pic = sheets[sheet].crop((u, v, u + 318, v + 238)).resize(TA_SIZE, Image.LANCZOS)
            pic.putalpha(255)  # screenshots: every texel opaque
            packs['ta'].append(('image', 'TA%d' % i, pic, 8))

    header = ['// Generated by tools/menu/build_menu.py (docs/30 phase 4.2): ids in the PS1 menu packs. Do not edit.',
              '#pragma once', '']
    total = {}
    for pack, fname, packer, clut_rows, clut_x4 in (
            ('resident', 'PS1Fonts.pmn', Packer(list(range(0, 5)), [0], {0: (242, 256)}), [RESIDENT_CLUT_ROW],
             list(range(0, 320, 16))),
            ('menu', 'PS1Menu.pmn', Packer(MENU_COLUMNS, [0]), MENU_CLUT_ROWS, list(range(256, 320, 16))),
            ('ta', 'PS1TimeAttack.pmn', Packer(TA_COLUMNS, [1], {1: (0, 252 if s1 else 192)}), [], [])):
        if pack == 'resident' and any(depth == 8 for _, _, _, depth in packs[pack]):
            raise PackError('resident pack: 4-bit only (one CLUT row)')
        images, blocks, vram, checks = build_pack(packs[pack], packer, clut_rows, clut_x4, pack,
                                                  (TA_CLUTS_S1 if s1 else TA_CLUTS) if pack == 'ta' else None)
        decode_check(checks, vram, pack)
        glyphs, fonts = [], []
        for fname_font, gl, lh, base in fontdefs[pack]:
            fonts.append((len(glyphs), len(gl), lh, base))
            glyphs += gl
        strs = [strings[k] for k in keys] if pack == 'resident' else []  # strings: the resident pack only
        size = write_pack(os.path.join(outdata, 'Game', 'Menu', fname), images, fonts, glyphs, blocks, strs)
        total[pack] = (size, len(images), len(blocks))
        for i, (fname_font, gl, lh, base) in enumerate(fontdefs[pack]):
            header.append('#define PS1_FONT_%s %d // %s' % (fname_font, i, pack))
        if pack == 'menu':
            for key, idx in pic_index.items():
                header.append('#define PS1_MENU_%s %d' % (key, idx))
        if pack == 'ta':
            header.append(('#define PS1_TA_PICTURES %d // PS1TimeAttack.pmn: acts 0-17, Final Zone, special stages 1-6 (112 x 84)'
                           if s1 else '#define PS1_TA_PICTURES %d // PS1TimeAttack.pmn: acts 0-16, Wing Fortress, Boss Attack, '
                           'Hidden Palace') % len(images))
        if preview:
            os.makedirs(preview, exist_ok=True)
            show_vram(vram, os.path.join(preview, fname + '.png'))
            contact_sheet(checks, os.path.join(preview, fname + '.sheet.png'))
    header.append('')
    for i, k in enumerate(keys):
        header.append('#define PS1_STR_%s %d' % (re.sub(r'(?<!^)(?=[A-Z])', '_', k).upper(), i))
    header.append('#define PS1_STR_COUNT %d' % len(keys))
    open(os.path.join(REPO, 'ps1', 'menu_pack_s1.h' if s1 else 'menu_pack.h'), 'w').write('\n'.join(header) + '\n')
    for pack, (size, n, nb) in total.items():
        print('menu pack %-8s %7d B, %3d images, %4d blocks, self-check exact' % (pack, size, n, nb))


def build_pack(items, packer, clut_rows, clut_x4, name, clut8_pos=None):
    """items: [(kind, key, RGBA image, depth)] -> (image records, VRAM blocks, VRAM words, checks). One CLUT per
    font (its glyphs quantized together) or picture: 8-bit CLUTs take x 0-255 of a CLUT row (or the clut8_pos
    (x, y) places), 4-bit ones the clut_x4 columns of those rows."""
    groups = {}
    for i, (kind, key, img, depth) in enumerate(items):
        g = key.split(':')[0] if kind in ('glyph', 'grouped') else key  # grouped: one CLUT per source sheet
        groups.setdefault(g, []).append(i)
    vram, images, checks = {}, [None] * len(items), []
    clut8 = list(clut8_pos) if clut8_pos else [(0, r) for r in clut_rows]
    clut4 = [(x, r) for r in clut_rows for x in clut_x4]
    for g, members in groups.items():
        depth = items[members[0]][3]
        cls = [classify(items[i][2], items[i][1] in BLEND_ALL) for i in members]
        rgb = np.concatenate([c[0].reshape(-1, 3) for c in cls])[None]
        lvl = np.concatenate([c[1].reshape(-1) for c in cls])[None]
        idx_all, clut = quantize(rgb, lvl, depth)
        if depth == 8:
            if not clut8:
                raise PackError('%s: out of 8-bit CLUT rows' % name)
            cx, cy = clut8.pop(0)
            entries = clut + [0] * (256 - len(clut))
        else:
            if not clut4:
                raise PackError('%s: out of 4-bit CLUT slots' % name)
            cx, cy = clut4.pop(0)
            entries = clut + [0] * (16 - len(clut))
        for k, c in enumerate(entries):
            vram[(cx + k, cy)] = c
        off = 0
        for i, (rgb_i, lvl_i) in zip(members, cls):
            h, w = lvl_i.shape
            idx = idx_all[0, off:off + h * w].reshape(h, w)
            off += h * w
            col, py, u, v = packer.place(w, h, depth)
            per = 4 if depth == 4 else 2
            vx, vy = col * 64 + u // per, py * 256 + v
            for yy in range(h):
                for xx in range(0, w + (-w % (2 * per)), per): # whole words (the packer's allocation)
                    hw = 0
                    for k in range(per):
                        t = int(idx[yy, xx + k]) if xx + k < w else 0
                        hw |= t << ((4 if depth == 4 else 8) * k)
                    vram[(vx + xx // per, vy + yy)] = hw
            tpage = col | (py << 4) | ((0 if depth == 4 else 1) << 7)
            images[i] = (w, h, u, v, tpage, (cx >> 4) | (cy << 6), int((lvl_i == 1).any()))
            checks.append((items[i][1], idx, clut, col, py, u, v, depth, cx, cy))
    rows = {}
    for (x, y), c in vram.items():
        rows.setdefault(y, {})[x] = c
    blocks = []
    for y in sorted(rows):
        xs = sorted(rows[y])
        start = prev = xs[0]
        for x in xs[1:] + [None]:
            if x is not None and x == prev + 1:
                prev = x
                continue
            blocks.append((start, y, prev - start + 1, 1, [rows[y][i] for i in range(start, prev + 1)]))
            if x is not None:
                start = prev = x
    for b in blocks:
        if b[2] & 1:
            raise PackError('%s: odd-width VRAM run at (%d, %d)' % (name, b[0], b[1]))
    merged = []
    for b in sorted(blocks, key=lambda b: (b[0], b[2], b[1])):
        if merged and merged[-1][0] == b[0] and merged[-1][2] == b[2] and merged[-1][1] + merged[-1][3] == b[1]:
            m = merged[-1]
            merged[-1] = (m[0], m[1], m[2], m[3] + 1, m[4] + b[4])
        else:
            merged.append(b)
    return images, merged, vram, checks


def contact_sheet(checks, path):
    """Every image decoded through its CLUT (50 % entries over a checker, as the GPU blends them) on one sheet."""
    tiles = []
    for key, idx, clut, col, py, u, v, depth, cx, cy in checks:
        h, w = idx.shape
        img = Image.new('RGB', (w, h))
        px = img.load()
        for yy in range(h):
            for xx in range(w):
                c = clut[idx[yy, xx]] if idx[yy, xx] < len(clut) else 0
                bgc = (40, 60, 150) if ((xx // 4 + yy // 4) & 1) else (20, 30, 90)
                if c == 0:
                    px[xx, yy] = bgc
                    continue
                rgb = ((c & 31) << 3, ((c >> 5) & 31) << 3, ((c >> 10) & 31) << 3)
                px[xx, yy] = tuple((a + b) // 2 for a, b in zip(rgb, bgc)) if c & 0x8000 else rgb
        tiles.append(img)
    W, x, y, rowh = 640, 0, 0, 0
    pos = []
    for t in tiles:
        if x + t.size[0] > W:
            x, y, rowh = 0, y + rowh + 2, 0
        pos.append((x, y))
        x += t.size[0] + 2
        rowh = max(rowh, t.size[1])
    sheet = Image.new('RGB', (W, y + rowh + 2), (0, 0, 0))
    for t, p in zip(tiles, pos):
        sheet.paste(t, p)
    sheet.resize((sheet.size[0] * 2, sheet.size[1] * 2), Image.NEAREST).save(path)


def show_vram(vram, path):
    """A preview PNG of the pack's VRAM words (raw 15-bit view)."""
    img = Image.new('RGB', (1024, 512))
    px = img.load()
    for (x, y), c in vram.items():
        px[x, y] = ((c & 31) << 3, ((c >> 5) & 31) << 3, ((c >> 10) & 31) << 3)
    img.save(path)


if __name__ == '__main__':
    try:
        main()
    except PackError as e:
        sys.exit('ERROR %s' % e)
