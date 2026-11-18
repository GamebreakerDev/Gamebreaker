"""Apply this project's toolkit patches without Git.

    python apply_patch.py <toolkit folder> <patch> [<patch> ...]

A player's PC has no Git, so setup applies port/patches/*.patch with this
instead of `git apply`. Standard library only; run by the embeddable Python
setup fetched.

It handles what our patches contain and nothing more: `git diff` output that
changes existing text files (no new, deleted, renamed or binary files). It
matches context exactly, line for line (no fuzz), and ignores line endings
when comparing, because a Git checkout on Windows has CRLF where the patch
has LF. Each changed file keeps its own line endings.

Safe to run again. For each patch:
  - every hunk's result already present, in order  -> "already applied", nothing written;
  - every hunk's original text present, in order   -> applied, all files written at the end;
  - anything else                                  -> exit 1, nothing written.
"""

import os
import re
import sys

HUNK = re.compile(r'^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@')


def parse(patch_path):
    """Return [(relative path, [(old_start, old_lines, new_lines), ...]), ...]."""
    with open(patch_path, encoding='utf-8', newline='') as f:
        lines = f.read().replace('\r\n', '\n').split('\n')
    files, cur, hunk = [], None, None
    i = 0
    while i < len(lines):
        ln = lines[i]
        if ln.startswith('diff --git '):
            cur, hunk = None, None
        elif ln.startswith(('new file mode', 'deleted file mode', 'rename from', 'Binary files', 'GIT binary patch')):
            raise SystemExit(f'{patch_path}: unsupported change ({ln.strip()}); only edits of existing text files are handled')
        elif ln.startswith('--- '):
            pass
        elif ln.startswith('+++ '):
            path = ln[4:].strip()
            if path.startswith('b/'):
                path = path[2:]
            cur = (path, [])
            files.append(cur)
        elif ln.startswith('@@'):
            m = HUNK.match(ln)
            if not m or cur is None:
                raise SystemExit(f'{patch_path}: cannot read hunk header: {ln}')
            hunk = (int(m.group(1)), [], [])
            cur[1].append(hunk)
        elif hunk is not None and ln[:1] in (' ', '-', '+'):
            if ln[0] != '+':
                hunk[1].append(ln[1:])
            if ln[0] != '-':
                hunk[2].append(ln[1:])
        elif hunk is not None and ln == '':
            # A blank context line some editors strip to nothing; only valid
            # inside a hunk and never at the very end of the patch.
            if i != len(lines) - 1:
                hunk[1].append('')
                hunk[2].append('')
        elif ln.startswith('\\'):
            pass                        # "\ No newline at end of file"
        i += 1
    return files


def find(text, block, start):
    """First index >= start where block matches text line for line, or -1."""
    n = len(block)
    if n == 0:
        return start
    for k in range(start, len(text) - n + 1):
        if text[k:k + n] == block:
            return k
    return -1


def locate(text, hunks, which):
    """Positions of each hunk's old (which=1) or new (which=2) block, in order, or None."""
    pos, out = 0, []
    for h in hunks:
        k = find(text, h[which], pos)
        if k < 0:
            return None
        out.append(k)
        pos = k + len(h[which])
    return out


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    root = argv[1]
    status = 0
    for patch in argv[2:]:
        name = os.path.basename(patch)
        plan, applied = [], True
        try:
            files = parse(patch)
        except SystemExit as e:
            print(e)
            return 1
        for rel, hunks in files:
            path = os.path.join(root, rel.replace('/', os.sep))
            if not os.path.isfile(path):
                print(f'{name}: {rel} is missing in {root}')
                return 1
            with open(path, 'rb') as f:
                raw = f.read()
            eol = '\r\n' if b'\r\n' in raw else '\n'
            body = raw.decode('utf-8')
            ends_with_eol = body.endswith(('\n',))
            text = body.replace('\r\n', '\n').split('\n')
            if ends_with_eol:
                text = text[:-1]
            if locate(text, hunks, 2) is not None:
                plan.append((path, None, eol, ends_with_eol))     # already in the patched state
                continue
            applied = False
            where = locate(text, hunks, 1)
            if where is None:
                print(f'{name}: does not apply to {rel} (its text differs from what the patch expects). Nothing was written.')
                return 1
            out, prev = [], 0
            for h, k in zip(hunks, where):
                out.extend(text[prev:k])
                out.extend(h[2])
                prev = k + len(h[1])
            out.extend(text[prev:])
            plan.append((path, out, eol, ends_with_eol))
        if applied:
            print(f'already applied: {name}')
            continue
        if any(p[1] is None for p in plan):
            print(f'{name}: partly applied (some files changed, some not). Nothing was written; restore the toolkit and run again.')
            return 1
        for path, out, eol, ends in plan:
            data = eol.join(out) + (eol if ends else '')
            tmp = path + '.gbpatch'
            with open(tmp, 'wb') as f:
                f.write(data.encode('utf-8'))
            os.replace(tmp, path)
        print(f'applied: {name}')
    return status


if __name__ == '__main__':
    sys.exit(main(sys.argv))
