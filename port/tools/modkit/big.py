r"""List or extract the entries of one of the game's BIG archives (magic BIGF).

    py -3 big.py list    <archive>                 # number, offset, size, name, kind
    py -3 big.py extract <archive> <entry> <out>   # entry by name or number

For mod makers: a mod replaces one entry with a file at
    mods\<mod>\<archive path>\<entry name or number>
for example  mods\My court\xf\forest.viv\foresto.xsh  or
             mods\My kit\xf\xf000002\417.xsh  (number counted from 0; what
follows the first dot is ignored). Reads your own copy of the game; writes
nothing but the file asked for.
"""
import struct
import sys


def read_big(path):
    with open(path, 'rb') as f:
        head = f.read(16)
        if head[:4] != b'BIGF':
            raise SystemExit(f'{path}: not a BIG archive')
        count, hsize = struct.unpack('>II', head[8:16])
        table = head + f.read(hsize - 16)
    entries, at = [], 16
    for _ in range(count):
        off, size = struct.unpack('>II', table[at:at + 8])
        end = table.index(b'\0', at + 8)
        entries.append((off, size, table[at + 8:end].decode('latin-1')))
        at = end + 1
    return entries


def kind(path, off):
    with open(path, 'rb') as f:
        f.seek(off)
        magic = f.read(4)
    return {b'SHPX': 'texture (SHPX)', b'\x7fELF': 'model (ELF object)',
            b'BIGF': 'archive (BIGF)'}.get(magic, magic.hex())


def find(entries, want):
    for i, (_, _, name) in enumerate(entries):
        if name and name.lower() == want.lower():
            return i
    stem = want.split('.', 1)[0]
    if stem.isdigit() and int(stem) < len(entries):
        return int(stem)
    raise SystemExit(f'no entry named or numbered {want!r}')


def main(argv):
    if len(argv) >= 2 and argv[0] == 'list':
        for i, (off, size, name) in enumerate(read_big(argv[1])):
            print(f'{i:5d} {off:10d} {size:10d}  {name or "-":20s} {kind(argv[1], off)}')
    elif len(argv) == 4 and argv[0] == 'extract':
        entries = read_big(argv[1])
        off, size, _ = entries[find(entries, argv[2])]
        with open(argv[1], 'rb') as f, open(argv[3], 'wb') as out:
            f.seek(off)
            out.write(f.read(size))
    else:
        raise SystemExit(__doc__)


if __name__ == '__main__':
    main(sys.argv[1:])
