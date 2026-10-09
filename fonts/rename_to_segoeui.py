"""Make local copies of Microsoft's Selawik fonts named "Segoe UI".

Selawik is metric-compatible with Segoe UI. Wine's FontSubstitutes only cover
GDI, but Altium also draws text through APIs that look fonts up by their real
family name, so the family name itself has to be "Segoe UI".
"""
import sys
from pathlib import Path

from fontTools.ttLib import TTFont

# Name IDs: 1 family, 4 full name, 6 PostScript name, 16 typographic family.
FAMILY_IDS = (1, 4, 16)
POSTSCRIPT_ID = 6

src_dir, out_dir = Path(sys.argv[1]), Path(sys.argv[2])
out_dir.mkdir(parents=True, exist_ok=True)

for src in sorted(src_dir.glob("selawk*.ttf")):
    font = TTFont(src)
    for record in font["name"].names:
        text = record.toUnicode()
        if record.nameID in FAMILY_IDS:
            record.string = text.replace("Selawik", "Segoe UI")
        elif record.nameID == POSTSCRIPT_ID:
            record.string = text.replace("Selawik", "SegoeUI")
    out = out_dir / src.name.replace("selawk", "segoeui-selawik")
    font.save(out)
    print(out.name)
