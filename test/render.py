"""Draw the plugin's display list exactly as the OP-1 would: 320x160, hairlines,
the OP-1 font, palette ids resolved to the dark theme."""
import json, os, sys
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
_font_path = os.environ.get("OP1_FONT") or os.path.join(HERE, os.pardir, "..", "..", "..", "mishkat", "tools", "opfont.json")
FONT = json.load(open(_font_path)) if os.path.exists(_font_path) else None
W, H = 320, 160

ID = {14: 0xF2F0EA, 12: 0x8A8A8A, 1: 0x000000, 4: 0x262626,
      -10: 0x5A9BD6, -11: 0x6FC06A, -12: 0xD8D8D8, -13: 0xE07A5A}


def colour(c):
    if c & 0x01000000:
        return ((c >> 16) & 255, (c >> 8) & 255, c & 255)
    v = ID.get(c, 0xFFFFFF)
    return ((v >> 16) & 255, (v >> 8) & 255, v & 255)


def flatten(cmds, steps=8):
    pts, cur, start = [], (0.0, 0.0), (0.0, 0.0)
    for cd in cmds:
        op = cd[0]
        a = cd[1] if len(cd) > 1 else []
        if op == "M":
            cur = (a[0], a[1]); start = cur; pts.append(cur)
        elif op == "L":
            cur = (a[0], a[1]); pts.append(cur)
        elif op == "C":
            p0, p1, p2, p3 = cur, (a[0], a[1]), (a[2], a[3]), (a[4], a[5])
            for i in range(1, steps + 1):
                t = i / steps; m = 1 - t
                pts.append((m**3*p0[0] + 3*m*m*t*p1[0] + 3*m*t*t*p2[0] + t**3*p3[0],
                            m**3*p0[1] + 3*m*m*t*p1[1] + 3*m*t*t*p2[1] + t**3*p3[1]))
            cur = p3
        elif op == "z":
            cur = start; pts.append(cur)
    return pts


def text_width(s, size, tracking=0.14):
    if not FONT:
        return len(s) * size * 0.6
    n = 0.0
    for ch in s:
        n += FONT.get(ch.upper(), FONT[" "])["w"] * size + size * tracking
    return max(0.0, n - size * tracking)


def draw_text(dr, x, y, size, s, fill, right=False):
    if right:
        x -= text_width(s, size)
    if not FONT:
        dr.text((x, y), s.upper(), fill=fill)
        return
    pen = x
    for ch in s:
        g = FONT.get(ch.upper(), FONT[" "])
        for path in g["p"]:
            pts = [(pen + px * size, y + py * size) for px, py in flatten(path)]
            if len(pts) > 1:
                dr.line(pts, fill=fill, width=1 if size < 10 else 2, joint="curve")
        pen += g["w"] * size + size * 0.14


def main(src, outdir):
    os.makedirs(outdir, exist_ok=True)
    pages, cur = [], None
    img = dr = None
    for line in open(src):
        p = line.split()
        if not p:
            continue
        if p[0] == "PAGE":
            if img:
                pages.append((name, img))
            name = p[1]
            img = Image.new("RGB", (W, H), (0, 0, 0))
            dr = ImageDraw.Draw(img)
            continue
        if p[0] == "L":
            x0, y0, x1, y1, w, c = map(int, p[1:7])
            dr.line([(x0, y0), (x1, y1)], fill=colour(c), width=max(1, w))
        elif p[0] == "R":
            x0, y0, x1, y1, c = map(int, p[1:6])
            dr.rectangle([x0, y0, x1, y1], fill=colour(c))
        elif p[0] == "D":
            x, y, r, filled, c = map(int, p[1:6])
            if filled:
                dr.ellipse([x - r, y - r, x + r, y + r], fill=colour(c))
            else:
                dr.ellipse([x - r, y - r, x + r, y + r], outline=colour(c))
        elif p[0] in ("T", "TR"):
            x, y, size, c = map(int, p[1:5])
            s = " ".join(p[5:])
            draw_text(dr, x, y, size, s, colour(c), right=(p[0] == "TR"))
    if img:
        pages.append((name, img))
    for name, im in pages:
        im.save(os.path.join(outdir, "page-%s.png" % name))
        im.resize((W * 3, H * 3), Image.NEAREST).save(os.path.join(outdir, "page-%s@3x.png" % name))
    print("wrote", len(pages), "pages")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
