r"""List or extract the entries of one of the game's packs (magic C0 FB), and
read or write the text in its language packs.

    py -3 c0fb.py list    <pack>                     # number, offset, size, name, kind
    py -3 c0fb.py extract <pack> <entry> <out>       # entry by name or number
    py -3 c0fb.py strings <pack> [<entry>]           # every line of text, numbered
    py -3 c0fb.py text    <pack> <entry> <n> <out>   # one line of text as a .txt file
    py -3 c0fb.py picture <entry.shpx> list          # the images in a picture entry
    py -3 c0fb.py picture <entry.shpx> <image> <out.png>
    py -3 c0fb.py import  <entry.shpx> <image> <in.png> <new.shpx>

The packs are fe\*.bin (menu pictures, fonts, layouts, palettes),
locale\*.eus (the text), credits\xcredits.bin and speech\*hdrs.big.

For mod makers: a mod replaces one entry of a pack with a file at
    mods\<mod>\<pack path>\<entry name or number>
for example  mods\My menu\fe\xaports.bin\12.shpx  (number counted from 0;
what follows the first dot is ignored), and one line of text with a file at
    mods\<mod>\<pack path>\<entry name or number>\<line number>.txt
for example  mods\My text\locale\xloc_fe.eus\mainmenu.txt\1.txt
holding the new text in UTF-8. A line break in the file becomes a line break
on the screen. `strings` prints the numbers to use.

A picture entry (SHPX) holds one or more images. `picture` turns one into a
PNG to edit, and `import` puts an edited PNG of the same size back into a copy
of the entry, which is then the mod's file. Only 32-bit images (type 0x7d,
such as the menus' backgrounds in fe\xafebg.bin) can go to PNG and back for
now; the 8-bit ones are not decoded yet. Reads your own copy of the game;
writes nothing but the file asked for.
"""
import struct
import zlib
import sys


def read_pack(path):
    with open(path, 'rb') as f:
        data = f.read()
    if data[:2] != b'\xc0\xfb':
        raise SystemExit(f'{path}: not a C0FB pack')
    count = struct.unpack('>H', data[4:6])[0]
    entries, at = [], 6
    for _ in range(count):
        off = int.from_bytes(data[at:at + 3], 'big')
        size = int.from_bytes(data[at + 3:at + 6], 'big')
        end = data.index(b'\0', at + 6)
        entries.append((off, size, data[at + 6:end].decode('latin-1')))
        at = end + 1
    return data, entries


def kind(data):
    return {b'SHPX': 'picture (SHPX)', b'LOCH': 'text (LOCH)', b'FNTX': 'font (FNTX)',
            b'\x01\x00\x00\x00': 'menu layout'}.get(data[:4], data[:4].hex())


def find(entries, want):
    for i, (_, _, name) in enumerate(entries):
        if name and name.lower() == want.lower():
            return i
    stem = want.split('.', 1)[0]
    if stem.isdigit() and int(stem) < len(entries):
        return int(stem)
    raise SystemExit(f'no entry named or numbered {want!r}')


def loch_strings(blob):
    """The lines of a LOCH text table, in order."""
    if blob[:4] != b'LOCH':
        raise SystemExit('that entry is not text (LOCH)')
    locl = struct.unpack('<I', blob[16:20])[0]
    count = struct.unpack('<I', blob[locl + 12:locl + 16])[0]
    out = []
    for k in range(count):
        p = locl + struct.unpack('<I', blob[locl + 16 + 4 * k:locl + 20 + 4 * k])[0]
        q = p
        while blob[q:q + 2] != b'\0\0':
            q += 2
        out.append(blob[p:q].decode('utf-16-le'))
    return out


def swizzle(x, y, w, h):
    """Where pixel (x, y) is stored: the Xbox keeps textures in Morton order,
    the bits of x and y interleaved while both have bits left."""
    r = bit = xb = yb = 0
    mx, my = w - 1, h - 1
    while mx or my:
        if mx:
            r |= ((x >> xb) & 1) << bit
            bit, xb, mx = bit + 1, xb + 1, mx >> 1
        if my:
            r |= ((y >> yb) & 1) << bit
            bit, yb, my = bit + 1, yb + 1, my >> 1
    return r


def shpx_images(blob):
    """(name, offset, type, width, height) of each image in a picture entry."""
    if blob[:4] != b'SHPX':
        raise SystemExit('that entry is not a picture (SHPX)')
    out = []
    for k in range(struct.unpack('<I', blob[8:12])[0]):
        name, off = struct.unpack('<4sI', blob[16 + 8 * k:24 + 8 * k])
        w, h = struct.unpack('<HH', blob[off + 4:off + 8])
        out.append((name.decode('latin-1'), off, blob[off], w, h))
    return out


def direct_image(blob, which):
    images = shpx_images(blob)
    k = int(which) if which.isdigit() and int(which) < len(images) else \
        next((i for i, im in enumerate(images) if im[0] == which), -1)
    if k < 0:
        raise SystemExit(f'no image {which!r}; the entry has ' +
                         ', '.join(f'{i} ({im[0]}, {im[3]}x{im[4]}, type {im[2]:#x})'
                                   for i, im in enumerate(images)))
    name, off, kind, w, h = images[k]
    if kind != 0x7D:
        raise SystemExit(f'image {k} is type {kind:#x}: only 32-bit pictures (type 0x7d) can be '
                         'turned into PNG and back by this tool for now')
    return off + 16, w, h


def write_png(path, w, h, rgba):
    rows = b''.join(b'\0' + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))

    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
                + chunk(b'IDAT', zlib.compress(rows, 9)) + chunk(b'IEND', b''))


def read_png(path):
    """A plain PNG (8 bits a channel, RGB or RGBA, not interlaced) as RGBA."""
    with open(path, 'rb') as f:
        d = f.read()
    if d[:8] != b'\x89PNG\r\n\x1a\n':
        raise SystemExit(f'{path}: not a PNG')
    at, idat = 8, b''
    while at < len(d):
        n, t = struct.unpack('>I4s', d[at:at + 8])
        body = d[at + 8:at + 8 + n]
        if t == b'IHDR':
            w, h, depth, ctype, _, _, inter = struct.unpack('>IIBBBBB', body)
        elif t == b'IDAT':
            idat += body
        at += 12 + n
    if depth != 8 or ctype not in (2, 6) or inter:
        raise SystemExit(f'{path}: save it as an 8-bit RGB or RGBA PNG, not interlaced')
    bpp = 4 if ctype == 6 else 3
    raw, stride, prev, out, p = zlib.decompress(idat), w * bpp, bytearray(w * bpp), bytearray(), 0
    for _ in range(h):
        f, line = raw[p], bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b, c = prev[i], prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        for x in range(w):
            px = line[x * bpp:x * bpp + bpp]
            out += px if bpp == 4 else px + b'\xff'
        prev = line
    return w, h, bytes(out)


def main(argv):
    sys.stdout.reconfigure(encoding='utf-8')
    if len(argv) >= 2 and argv[0] == 'list':
        data, entries = read_pack(argv[1])
        for i, (off, size, name) in enumerate(entries):
            print(f'{i:5d} {off:10d} {size:10d}  {name or "-":24s} {kind(data[off:off + 4])}')
    elif len(argv) == 4 and argv[0] == 'extract':
        data, entries = read_pack(argv[1])
        off, size, _ = entries[find(entries, argv[2])]
        with open(argv[3], 'wb') as out:
            out.write(data[off:off + size])
    elif len(argv) in (2, 3) and argv[0] == 'strings':
        data, entries = read_pack(argv[1])
        which = [find(entries, argv[2])] if len(argv) == 3 else range(len(entries))
        for i in which:
            off, size, name = entries[i]
            if data[off:off + 4] != b'LOCH':
                continue
            for k, s in enumerate(loch_strings(data[off:off + size])):
                print(f'{name or i}\\{k}: {s}')
    elif len(argv) == 5 and argv[0] == 'text':
        data, entries = read_pack(argv[1])
        off, size, _ = entries[find(entries, argv[2])]
        lines = loch_strings(data[off:off + size])
        n = int(argv[3])
        if not 0 <= n < len(lines):
            raise SystemExit(f'that entry has lines 0 to {len(lines) - 1}')
        with open(argv[4], 'w', encoding='utf-8', newline='\r\n') as out:
            out.write(lines[n].replace('\\n', '\n') + '\n')
    elif len(argv) in (3, 4) and argv[0] == 'picture':
        with open(argv[1], 'rb') as f:
            blob = f.read()
        if argv[2] == 'list':
            for i, (name, _, ptype, w, h) in enumerate(shpx_images(blob)):
                print(f"{i:4d}  {name}  {w}x{h}  type {ptype:#x}")
            return 0
        if len(argv) != 4:
            print(__doc__)
            return 2
        px, w, h = direct_image(blob, argv[2])
        rgba = bytearray(w * h * 4)
        for y in range(h):
            for x in range(w):
                s = px + 4 * swizzle(x, y, w, h)
                b, g, r, a = blob[s:s + 4]
                rgba[4 * (y * w + x):4 * (y * w + x) + 4] = bytes((r, g, b, a))
        write_png(argv[3], w, h, bytes(rgba))
    elif len(argv) == 5 and argv[0] == 'import':
        with open(argv[1], 'rb') as f:
            blob = bytearray(f.read())
        px, w, h = direct_image(blob, argv[2])
        pw, ph, rgba = read_png(argv[3])
        if (pw, ph) != (w, h):
            raise SystemExit(f'the picture must stay {w}x{h} (the PNG is {pw}x{ph})')
        for y in range(h):
            for x in range(w):
                r, g, b, a = rgba[4 * (y * w + x):4 * (y * w + x) + 4]
                s = px + 4 * swizzle(x, y, w, h)
                blob[s:s + 4] = bytes((b, g, r, a))
        with open(argv[4], 'wb') as f:
            f.write(blob)
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
