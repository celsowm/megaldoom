"""Tile captured route frames into one PNG so a whole run can be eyeballed at once."""
import sys, pathlib
from PIL import Image, ImageDraw

src = pathlib.Path(sys.argv[1]); out = sys.argv[2]
cols = int(sys.argv[3]) if len(sys.argv) > 3 else 4
fs = sorted(src.glob("frame-*.ppm"))
if not fs: sys.exit(f"no frames in {src}")
ims = [Image.open(f).convert("RGB") for f in fs]
w, h = ims[0].size
rows = (len(ims) + cols - 1) // cols
sheet = Image.new("RGB", (cols * w, rows * (h + 14)), (20, 20, 20))
d = ImageDraw.Draw(sheet)
for i, (im, f) in enumerate(zip(ims, fs)):
    x, y = (i % cols) * w, (i // cols) * (h + 14)
    sheet.paste(im, (x, y + 14))
    d.text((x + 3, 2 + y), f.stem.replace("frame-", "f"), fill=(255, 255, 0))
sheet.save(out)
print(f"{out}  {len(ims)} frames  {sheet.size[0]}x{sheet.size[1]}")
