"""Join the mockup sources into one self-contained HTML file per direction.

Usage:  python launcher/mockups/src/build.py
Writes: launcher/mockups/<direction>.html  (no external files, no network)
"""
from pathlib import Path

SRC = Path(__file__).resolve().parent
OUT = SRC.parent

DIRECTIONS = {
    "halo": "Halo",
}


def build(direction_id: str, name: str) -> Path:
    body = (SRC / "body.html").read_text(encoding="utf-8")
    style = (SRC / "core.css").read_text(encoding="utf-8") + "\n" + (SRC / f"{direction_id}.css").read_text(encoding="utf-8")
    motion = (SRC / f"{direction_id}.motion.js").read_text(encoding="utf-8")
    script = (SRC / "core.js").read_text(encoding="utf-8")
    html = (body.replace("{{ID}}", direction_id)
                .replace("{{NAME}}", name)
                .replace("{{STYLE}}", style)
                .replace("{{MOTION}}", motion)
                .replace("{{SCRIPT}}", script))
    if "{{" in html:
        raise SystemExit(f"{direction_id}: unreplaced placeholder")
    out = OUT / f"{direction_id}.html"
    out.write_text(html, encoding="utf-8", newline="\n")
    return out


if __name__ == "__main__":
    for d, n in DIRECTIONS.items():
        if (SRC / f"{d}.css").exists() and (SRC / f"{d}.motion.js").exists():
            p = build(d, n)
            print(f"wrote {p.name} ({p.stat().st_size // 1024} KB)")
        else:
            print(f"skipped {d} (sources missing)")
