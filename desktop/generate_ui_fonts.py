"""Generate ui_fonts.c / ui_fonts.h: the UI's 1-bit LVGL fonts (Pillow required).

    python generate_ui_fonts.py <DejaVuSans-Bold.ttf>

- 12 and 16 px (labels, captions): DejaVu Sans Bold. One copy ships with
  MSYS2's matplotlib (lib/python3.12/site-packages/matplotlib/mpl-data/fonts/ttf).
- 24 px and up (titles, badges, readings): Open Sans ExtraBold, from
  fonts/OpenSans[wdth,wght].ttf (variable font, ExtraBold instance). Its decimal
  point keeps half a digit's side bearing on each side, so "999.9" stays tight.
- ui_chart_12 (everything inside history charts): Spleen 6x12, a hand-drawn
  bitmap font, from fonts/spleen-6x12.bdf, emboldened by one pixel (each ink
  pixel also inks its right neighbour; the advance grows by one) so the chart
  text holds its own next to the heavy readings.

DejaVu glyphs are FreeType's hinted grey rendering (through Pillow) cut to 1 bit
at half coverage. Open Sans glyphs are unhinted: drawn at 8x size and box-averaged
to true pixel coverage before the same cut, each shifted by the sub-pixel
phase that leaves the fewest one-pixel bumps and dents (see supersampled_glyph). Its hinting left nubs on curves (3)
and stray steps inside openings (4); unhinted, round glyphs overshoot the flat
ones by up to one pixel from 48 px, as the design intends. Each glyph is one bitmap placed at whole-pixel
advances, so repeated characters are identical. Licences are in fonts/.
"""
import math
import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

HERE = Path(__file__).parent
SIZES = [12, 16, 24, 28, 32, 40, 48, 60, 64, 72, 80, 96, 112]
CHARS = list(range(32, 127)) + [176, 179, 181]
OPEN_SANS = HERE / 'fonts' / 'OpenSans[wdth,wght].ttf'
SPLEEN = HERE / 'fonts' / 'spleen-6x12.bdf'
LARGE_FROM = 24


def outline_font(path, size, variation=None):
    f = ImageFont.truetype(str(path), size)
    if variation:
        f.set_variation_by_name(variation)
    return f


def outline_glyph(f, ch):
    """(advance, w, h, ofs_x, ofs_y, bits) of one glyph, cut at half coverage."""
    x0, y0, x1, y1 = f.getbbox(ch, anchor='ls')
    w, h = x1 - x0, y1 - y0
    im = Image.new('L', (max(1, w), max(1, h)))
    ImageDraw.Draw(im).text((-x0, -y0), ch, font=f, fill=255, anchor='ls')
    bits = [int(v >= 128) for v in im.getdata()] if w * h else []
    return round(f.getlength(ch)), w, h, x0, -y1, bits


def coverage(f8, ch, px, py, scale):
    """Pixel coverage of ch shifted by (px, py)/scale px: (image, X0, Y0)."""
    x0, y0, x1, y1 = f8.getbbox(ch, anchor='ls')
    X0, Y0 = math.floor((x0 + px) / scale), math.floor((y0 + py) / scale)
    X1, Y1 = math.ceil((x1 + px) / scale), math.ceil((y1 + py) / scale)
    im = Image.new('L', ((X1 - X0) * scale, (Y1 - Y0) * scale))
    ImageDraw.Draw(im).text((-X0 * scale + px, -Y0 * scale + py), ch, font=f8, fill=255, anchor='ls')
    return im.resize((X1 - X0, Y1 - Y0), Image.BOX), X0, Y0


def ink_rows(bits, w, h, X0, Y0):
    """Ink box (left, top, right, bottom) in pixel coordinates, or None."""
    ys = [r for r in range(h) if any(bits[r * w:(r + 1) * w])]
    if not ys:
        return None
    xs = [x for x in range(w) if any(bits[r * w + x] for r in ys)]
    return X0 + xs[0], Y0 + ys[0], X0 + xs[-1], Y0 + ys[-1]


def nubs(bits, w, h):
    """One-pixel bumps (ink with at most one ink 4-neighbour) and dents
    (paper with three or four), the stray steps 1-bit curves show."""
    n = 0
    for y in range(h):
        for x in range(w):
            k = sum(1 for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1))
                    if 0 <= x + dx < w and 0 <= y + dy < h and bits[(y + dy) * w + x + dx])
            if (bits[y * w + x] and k <= 1) or (not bits[y * w + x] and k >= 3):
                n += 1
    return n


def clean(bits, w, h):
    """Clear remaining one-pixel bumps and fill one-pixel dents. The first and
    last rows and columns never change, so heights, baseline and width stay."""
    bits = list(bits)
    for _ in range(4):
        changed = False
        for y in range(1, h - 1):
            for x in range(1, w - 1):
                k = sum(bits[(y + dy) * w + x + dx] for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)))
                v = bits[y * w + x]
                if v and k <= 1 or not v and k >= 3:
                    bits[y * w + x] = 1 - v
                    changed = True
        if not changed:
            break
    return bits


def supersampled_glyph(f8, ch, scale=8):
    """Like outline_glyph, from a font at scale x the size, box-averaged (unhinted).
    The glyph is shifted by the sub-pixel phase (any horizontal, up to a quarter
    pixel vertical) that leaves the fewest one-pixel bumps and dents, then the
    fewest part-covered pixels, so stems land on whole pixels and openings are
    even. A vertical shift may not move the ink's top or bottom row, so
    digits keep one cap line and baseline. Leftover bumps and dents are cleared."""
    x0, y0, x1, y1 = f8.getbbox(ch, anchor='ls')
    if x1 <= x0 or y1 <= y0:
        return round(f8.getlength(ch) / scale), 0, 0, 0, 0, []
    best = None
    for px in range(scale):
        ref = None
        for py in (0, -1, 1, -2, 2):
            im, X0, Y0 = coverage(f8, ch, px, py, scale)
            w, h = im.size
            data = list(im.getdata())
            bits = [int(v >= 128) for v in data]
            box = ink_rows(bits, w, h, X0, Y0)
            if box is None:
                continue
            if py == 0:
                ref = box
            elif ref is None or (box[1], box[3]) != (ref[1], ref[3]):
                continue
            score = (nubs(bits, w, h), sum(1 for v in data if 40 < v < 215))
            if best is None or score < best[0]:
                best = (score, bits, w, h, X0, Y0)
    if best is None:
        return round(f8.getlength(ch) / scale), 0, 0, 0, 0, []
    _, bits, w, h, X0, Y0 = best
    l, t, r, b = ink_rows(bits, w, h, X0, Y0)
    l, t, r, b = l - X0, t - Y0, r - X0, b - Y0
    bits = [bits[y * w + x] for y in range(t, b + 1) for x in range(l, r + 1)]
    W, H = r - l + 1, b - t + 1
    bits = clean(bits, W, H)
    return round(f8.getlength(ch) / scale), W, H, X0 + l, -(Y0 + b + 1), bits


def parse_bdf(path):
    glyphs, asc, desc, cur = {}, 0, 0, None
    lines = path.read_text(encoding='latin-1').splitlines()
    i = 0
    while i < len(lines):
        p = lines[i].split()
        if p and p[0] == 'FONT_ASCENT': asc = int(p[1])
        elif p and p[0] == 'FONT_DESCENT': desc = int(p[1])
        elif p and p[0] == 'ENCODING': cur = {'enc': int(p[1])}
        elif p and p[0] == 'DWIDTH': cur['adv'] = int(p[1])
        elif p and p[0] == 'BBX': cur['bbx'] = tuple(map(int, p[1:5]))
        elif p and p[0] == 'BITMAP':
            w, h = cur['bbx'][:2]
            rows = []
            for k in range(h):
                hexrow = lines[i + 1 + k]
                v, n = int(hexrow, 16), len(hexrow) * 4
                rows.append([(v >> (n - 1 - x)) & 1 for x in range(w)])
            cur['rows'] = rows
            glyphs[cur['enc']] = cur
            i += h
        i += 1
    return glyphs, asc, desc


def bdf_glyph(g):
    """Crop a BDF cell to its ink, keeping its position against the baseline."""
    w, h, xo, yo = g['bbx']
    rows = g['rows']
    inked = [i for i, r in enumerate(rows) if any(r)]
    if not inked:
        return g['adv'], 0, 0, 0, 0, []
    top, bot = inked[0], inked[-1]
    left = min(r.index(1) for r in rows if any(r))
    right = max(len(r) - 1 - r[::-1].index(1) for r in rows if any(r))
    bits = [b for r in rows[top:bot + 1] for b in r[left:right + 1]]
    return g['adv'], right - left + 1, bot - top + 1, xo + left, yo + (h - 1 - bot), bits


def embolden(glyph):
    """Bitmap bold: every ink pixel also inks the pixel to its right."""
    adv, w, h, ox, oy, bits = glyph
    if not w:
        return adv + 1, w, h, ox, oy, bits
    rows = [bits[r * w:(r + 1) * w] + [0] for r in range(h)]
    bold = [int(r[x] or (x > 0 and r[x - 1])) for r in rows for x in range(w + 1)]
    return adv + 1, w + 1, h, ox, oy, bold


def emit(name, glyphs, line_height, base_line):
    """C source for one lv_font_fmt_txt font from (adv, w, h, ofs_x, ofs_y, bits)."""
    data, ds = [], ['{0}']
    for adv, w, h, ox, oy, bits in glyphs:
        packed = [sum((bits[j + k] if j + k < len(bits) else 0) << (7 - k) for k in range(8))
                  for j in range(0, len(bits), 8)]
        ds.append('{.bitmap_index=%d,.adv_w=%d,.box_w=%d,.box_h=%d,.ofs_x=%d,.ofs_y=%d}'
                  % (len(data), adv * 16, w, h, ox, oy))
        data += packed
    n = name
    return [
        'static const uint8_t bitmap_%s[]={%s};' % (n, ','.join(map(str, data or [0]))),
        'static const lv_font_fmt_txt_glyph_dsc_t glyph_%s[]={%s};' % (n, ','.join(ds)),
        'static const uint16_t unicode_%s[]={%s};' % (n, ','.join(str(c - 32) for c in CHARS)),
        'static const lv_font_fmt_txt_cmap_t map_%s[]={ {.range_start=32,.range_length=150,.glyph_id_start=1,'
        '.unicode_list=unicode_%s,.list_length=%d,.type=LV_FONT_FMT_TXT_CMAP_SPARSE_TINY} };' % (n, n, len(CHARS)),
        'static const lv_font_fmt_txt_dsc_t dsc_%s={.glyph_bitmap=bitmap_%s,.glyph_dsc=glyph_%s,.cmaps=map_%s,'
        '.cmap_num=1,.bpp=1};' % (n, n, n, n),
        'const lv_font_t ui_%s={.get_glyph_dsc=lv_font_get_glyph_dsc_fmt_txt,.get_glyph_bitmap='
        'lv_font_get_bitmap_fmt_txt,.line_height=%d,.base_line=%d,.dsc=&dsc_%s};' % (n, line_height, base_line, n),
    ]


def main():
    dejavu = Path(sys.argv[1])
    out = ['/* Generated by generate_ui_fonts.py; see fonts/LICENSE_*. */', '#include "ui_fonts.h"']
    names = []
    for size in SIZES:
        large = size >= LARGE_FROM
        f = outline_font(OPEN_SANS, size, 'ExtraBold') if large else outline_font(dejavu, size)
        asc, desc = f.getmetrics()
        if large:
            f8 = outline_font(OPEN_SANS, size * 8, 'ExtraBold')
            glyphs = [supersampled_glyph(f8, chr(c)) for c in CHARS]
            # Decimal point: half a digit's side bearing on each side.
            i = CHARS.index(ord('.'))
            _, w, h, _, oy, bits = glyphs[i]
            side = max(1, supersampled_glyph(f8, '0')[3] // 2)
            glyphs[i] = (w + 2 * side, w, h, side, oy, bits)
        else:
            glyphs = [outline_glyph(f, chr(c)) for c in CHARS]
        out += emit('bold_%d' % size, glyphs, asc + desc, desc)
        names.append('ui_bold_%d' % size)
    sp, asc, desc = parse_bdf(SPLEEN)
    out += emit('chart_12', [embolden(bdf_glyph(sp.get(c) or sp[ord('?')])) for c in CHARS], asc + desc, desc)
    names.append('ui_chart_12')
    (HERE / 'ui_fonts.c').write_text('\n'.join(out) + '\n')
    (HERE / 'ui_fonts.h').write_text('#pragma once\n#include "lvgl.h"\n'
                                     + ''.join('LV_FONT_DECLARE(%s);\n' % n for n in names))


if __name__ == '__main__':
    main()
