#!/usr/bin/env python3
"""WAD image preview / Mega Drive conversion viewer.

Pick one of the extracted WAD PNGs (res/originaldoom/**) on the left; the tool
shows the original and, on the right, exactly how it will look once converted for
the Mega Drive: box-filter downscaled to the target size and quantized to the
game's 16-colour palette. This mirrors the Convert-Image logic in
tools/convert-freedoom-assets.ps1 (area-average, then nearest-palette).

Run:  python tools/wad-image-preview.py
Deps: Pillow, numpy (both already used by the project tooling), tkinter (stdlib).
"""

import os
import struct
import sys
import tkinter as tk
from tkinter import ttk

import numpy as np
from PIL import Image, ImageTk

# --- Game palette (must match $palette in convert-freedoom-assets.ps1) ---------
PALETTE = np.array([
    (0x00, 0x00, 0x00),  # 0  transparent / black
    (0xD8, 0xD8, 0xD8),  # 1  near-white
    (0x18, 0x14, 0x10),  # 2  very dark
    (0x38, 0x30, 0x30),  # 3  dark
    (0x58, 0x50, 0x48),  # 4  gray
    (0x88, 0x80, 0x78),  # 5  gray
    (0xB4, 0xAC, 0xA0),  # 6  light gray
    (0xE8, 0xE0, 0xD0),  # 7  near white
    (0x30, 0x1E, 0x10),  # 8  dark brown
    (0x48, 0x78, 0xA8),  # 9  blue (sky)
    (0x78, 0x50, 0x2C),  # 10 brown
    (0xD8, 0xB0, 0x48),  # 11 gold
    (0x98, 0x28, 0x18),  # 12 red
    (0xA8, 0x68, 0x38),  # 13 tan
    (0x48, 0x40, 0x38),  # 14 olive
    (0x4C, 0x60, 0x28),  # 15 green
], dtype=np.int32)

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSET_DIR = os.path.join(ROOT_DIR, "res", "originaldoom")
DEFAULT_WAD = os.path.join(ROOT_DIR, "DOOM1.WAD")
PREVIEW_PX = 320  # on-screen size of each preview pane

# Doom thing type (doomednum) -> sprite lump prefix, so a map's THINGS list can be
# turned into the sprite PNGs it spawns. Player/DM starts (1-4, 11) spawn no sprite
# and are omitted. Covers the shareware (E1) thing set plus common decorations.
DOOMEDNUM_SPRITE = {
    # monsters
    3004: "POSS", 9: "SPOS", 3001: "TROO", 3002: "SARG", 58: "SARG",
    3006: "SKUL", 3005: "HEAD", 3003: "BOSS", 69: "BOS2", 68: "BSPI",
    64: "VILE", 66: "SKEL", 67: "FATT", 71: "PAIN", 65: "CPOS",
    16: "CYBR", 7: "SPID", 84: "SSWV", 72: "KEEN", 88: "BBRN",
    # weapons
    2005: "CSAW", 2001: "SHOT", 82: "SGN2", 2002: "MGUN", 2003: "LAUN",
    2004: "PLAS", 2006: "BFUG",
    # ammo
    2007: "CLIP", 2048: "AMMO", 2010: "ROCK", 2046: "BROK",
    2047: "CELL", 17: "CELP", 2008: "SHEL", 2049: "SBOX", 8: "BPAK",
    # health & armor
    2011: "STIM", 2012: "MEDI", 2013: "SOUL", 2014: "BON1", 2015: "BON2",
    2018: "ARM1", 2019: "ARM2",
    # powerups
    2022: "PINV", 2023: "PSTR", 2024: "PINS", 2025: "SUIT",
    2026: "PMAP", 2045: "PVIS",
    # keys
    5: "BKEY", 40: "BSKU", 13: "RKEY", 38: "RSKU", 6: "YKEY", 39: "YSKU",
    # obstacles / decorations
    2035: "BAR1", 48: "ELEC", 2028: "COLU", 35: "CBRA", 34: "CAND",
    30: "COL1", 31: "COL2", 32: "COL3", 33: "COL4", 36: "COL5", 37: "COL6",
    47: "SMIT", 43: "TRE1", 54: "TRE2",
    44: "TBLU", 45: "TGRN", 46: "TRED", 55: "SMBT", 56: "SMGT", 57: "SMRT",
    # gibs / dead things
    10: "PLAY", 12: "PLAY", 15: "PLAY", 24: "POL5",
    18: "POSS", 19: "SPOS", 20: "TROO", 21: "SARG", 22: "HEAD", 23: "SKUL",
    25: "POL1", 26: "POL6", 27: "POL4", 28: "POL2", 29: "POL3",
    49: "GOR1", 50: "GOR2", 51: "GOR3", 52: "GOR4", 53: "GOR5",
    59: "GOR2", 60: "GOR4", 61: "GOR3", 62: "GOR5", 63: "GOR1",
}


def _is_map_marker(n):
    if len(n) == 4 and n[0] == "E" and n[2] == "M":
        return n[1].isdigit() and n[3].isdigit()
    return n.startswith("MAP") and n[3:].isdigit()


def read_wad(path):
    """Return (data, lumps) where lumps is [(name, filepos, size)], or None."""
    try:
        with open(path, "rb") as fh:
            data = fh.read()
    except OSError:
        return None
    sig, num, diroff = struct.unpack_from("<4sii", data, 0)
    if sig.decode("ascii", "ignore") not in ("IWAD", "PWAD"):
        return None
    lumps = []
    for i in range(num):
        fp, sz, raw = struct.unpack_from("<ii8s", data, diroff + i * 16)
        name = raw.split(b"\x00", 1)[0].decode("ascii", "ignore").upper()
        lumps.append((name, fp, sz))
    return data, lumps


def wad_map_names(wad):
    return [n for n, _, _ in wad[1] if _is_map_marker(n)]


def _map_member(wad, mapn, member):
    data, lumps = wad
    for i, (n, _, _) in enumerate(lumps):
        if n == mapn:
            for j in range(i + 1, min(i + 12, len(lumps))):
                if lumps[j][0] == member:
                    return data[lumps[j][1]:lumps[j][1] + lumps[j][2]]
                if _is_map_marker(lumps[j][0]):
                    break
            return None
    return None


def map_used_assets(wad, mapn):
    """(exact_names, sprite_prefixes) referenced by a map's walls/flats/things."""
    names, prefixes = set(), set()

    sides = _map_member(wad, mapn, "SIDEDEFS")
    if sides:
        for i in range(len(sides) // 30):
            _, _, up, lo, mid, _ = struct.unpack_from("<hh8s8s8sH", sides, i * 30)
            for t in (up, lo, mid):
                s = t.split(b"\x00", 1)[0].decode("ascii", "ignore").upper()
                if s and s != "-":
                    names.add(s)

    sectors = _map_member(wad, mapn, "SECTORS")
    if sectors:
        for i in range(len(sectors) // 26):
            _, _, fl, cl, _, _, _ = struct.unpack_from("<hh8s8shhh", sectors, i * 26)
            for t in (fl, cl):
                s = t.split(b"\x00", 1)[0].decode("ascii", "ignore").upper()
                if s:
                    names.add(s)

    things = _map_member(wad, mapn, "THINGS")
    if things:
        for i in range(len(things) // 10):
            _, _, _, typ, _ = struct.unpack_from("<hhhhh", things, i * 10)
            spr = DOOMEDNUM_SPRITE.get(typ)
            if spr:
                prefixes.add(spr)

    return names, prefixes


def nearest_index(r, g, b):
    """Nearest palette index by squared RGB distance (matches the converter)."""
    d = PALETTE - np.array((r, g, b), dtype=np.int32)
    return int(np.argmin((d * d).sum(axis=1)))


def convert_to_md(img, width, height, use_alpha):
    """Return a (indices HxW, has_alpha) tuple replicating Convert-Image.

    Area-average each output texel's source rectangle, then quantize. For alpha
    assets a texel is transparent only if its covered area is mostly transparent;
    otherwise the opaque pixels are averaged."""
    src = np.asarray(img.convert("RGBA"), dtype=np.int32)
    ih, iw = src.shape[:2]
    out = np.zeros((height, width), dtype=np.uint8)

    for y in range(height):
        sy0 = min(ih - 1, (y * ih) // height)
        sy1 = ((y + 1) * ih) // height
        if sy1 <= sy0:
            sy1 = sy0 + 1
        sy1 = min(sy1, ih)

        for x in range(width):
            sx0 = min(iw - 1, (x * iw) // width)
            sx1 = ((x + 1) * iw) // width
            if sx1 <= sx0:
                sx1 = sx0 + 1
            sx1 = min(sx1, iw)

            block = src[sy0:sy1, sx0:sx1].reshape(-1, 4)
            alpha = block[:, 3]

            # PowerShell's [int] cast rounds half-to-even; np.rint matches it so the
            # preview is byte-identical to the generated .h tables.
            def avg(pool):
                return (int(np.rint(pool[:, 0].mean())),
                        int(np.rint(pool[:, 1].mean())),
                        int(np.rint(pool[:, 2].mean())))

            if not use_alpha:
                out[y, x] = nearest_index(*avg(block))
                continue

            if alpha.mean() >= 128:  # raw mean compare, matches the PS converter
                opaque = block[alpha >= 128]
                idx = nearest_index(*avg(opaque if len(opaque) else block))
                out[y, x] = 2 if idx == 0 else idx
            else:
                out[y, x] = 0  # transparent

    return out, use_alpha


def indices_to_image(indices, has_alpha):
    """Render palette indices to an RGBA PIL image (index 0 transparent if alpha)."""
    h, w = indices.shape
    rgb = PALETTE[indices].astype(np.uint8)
    a = np.full((h, w), 255, dtype=np.uint8)
    if has_alpha:
        a[indices == 0] = 0
    return Image.fromarray(np.dstack([rgb, a]), "RGBA")


def checkerboard(size, cell=16):
    """Grey checkerboard so transparent texels are visible in the preview."""
    img = Image.new("RGBA", (size, size), (60, 60, 60, 255))
    px = img.load()
    for y in range(size):
        for x in range(size):
            if ((x // cell) + (y // cell)) & 1:
                px[x, y] = (90, 90, 90, 255)
    return img


class App:
    def __init__(self, root):
        self.root = root
        root.title("MegalDoom — WAD image \u2192 Mega Drive preview")
        self._imgrefs = []  # keep PhotoImage refs alive

        # --- left: file list -------------------------------------------------
        left = ttk.Frame(root, padding=6)
        left.grid(row=0, column=0, sticky="ns")
        ttk.Label(left, text="Extracted WAD images").pack(anchor="w")

        # Optional filter by Doom stage (map): "All" shows every extracted image;
        # picking E1M1..E1M9 shows only the textures, flats and sprites that map
        # actually references (parsed live from DOOM1.WAD).
        frow = ttk.Frame(left)
        frow.pack(anchor="w", fill="x", pady=(2, 4))
        ttk.Label(frow, text="Stage:").pack(side="left")
        self.filter_var = tk.StringVar(value="All")
        self.filter_combo = ttk.Combobox(frow, textvariable=self.filter_var,
                                         state="readonly", width=18)
        self.filter_combo.pack(side="left", padx=(4, 0))
        self.filter_combo.bind("<<ComboboxSelected>>", lambda e: self.apply_filter())

        self.listbox = tk.Listbox(left, width=42, height=30, exportselection=False)
        self.listbox.pack(side="left", fill="y")
        sb = ttk.Scrollbar(left, orient="vertical", command=self.listbox.yview)
        sb.pack(side="left", fill="y")
        self.listbox.config(yscrollcommand=sb.set)
        self.listbox.bind("<<ListboxSelect>>", lambda e: self.on_select())

        # --- right: controls + previews -------------------------------------
        right = ttk.Frame(root, padding=6)
        right.grid(row=0, column=1, sticky="nsew")

        ctrl = ttk.Frame(right)
        ctrl.pack(anchor="w", pady=(0, 6))
        ttk.Label(ctrl, text="W").pack(side="left")
        self.w_var = tk.IntVar(value=16)
        ttk.Spinbox(ctrl, from_=1, to=256, width=5, textvariable=self.w_var,
                    command=self.render).pack(side="left", padx=(2, 8))
        ttk.Label(ctrl, text="H").pack(side="left")
        self.h_var = tk.IntVar(value=16)
        ttk.Spinbox(ctrl, from_=1, to=256, width=5, textvariable=self.h_var,
                    command=self.render).pack(side="left", padx=(2, 8))
        self.alpha_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(ctrl, text="Alpha transparency (sprites)",
                        variable=self.alpha_var, command=self.render).pack(side="left")

        panes = ttk.Frame(right)
        panes.pack()
        oframe = ttk.Frame(panes, padding=4)
        oframe.grid(row=0, column=0)
        ttk.Label(oframe, text="Original").pack()
        self.orig_label = ttk.Label(oframe)
        self.orig_label.pack()
        self.orig_dims = ttk.Label(oframe, text="")
        self.orig_dims.pack()

        cframe = ttk.Frame(panes, padding=4)
        cframe.grid(row=0, column=1)
        ttk.Label(cframe, text="Mega Drive (16 colours)").pack()
        self.conv_label = ttk.Label(cframe)
        self.conv_label.pack()
        self.conv_dims = ttk.Label(cframe, text="")
        self.conv_dims.pack()

        self.all_files = []   # (full, rel, category, basename_upper) for every png
        self.files = []       # full paths currently shown (after filter)
        self.wad = read_wad(DEFAULT_WAD)
        self._stage_cache = {}  # map name -> (names set, prefixes set)
        self.scan()
        self.apply_filter(select_first=True)

    def scan(self):
        """Discover every png once and populate the stage-filter choices."""
        if not os.path.isdir(ASSET_DIR):
            self.listbox.insert("end", f"(missing) {ASSET_DIR}")
            return
        for dirpath, _, names in os.walk(ASSET_DIR):
            for n in sorted(names):
                if n.lower().endswith(".png"):
                    full = os.path.join(dirpath, n)
                    rel = os.path.relpath(full, ASSET_DIR).replace("\\", "/")
                    cat = rel.split("/", 1)[0] if "/" in rel else "(root)"
                    base = os.path.splitext(n)[0].upper()
                    self.all_files.append((full, rel, cat, base))

        stages = ["All"]
        if self.wad:
            stages += sorted(wad_map_names(self.wad))
        self.filter_combo["values"] = stages

    def _stage_assets(self, stage):
        if stage not in self._stage_cache:
            self._stage_cache[stage] = map_used_assets(self.wad, stage)
        return self._stage_cache[stage]

    def apply_filter(self, select_first=True):
        """Rebuild the listbox from all_files, honouring the stage filter."""
        stage = self.filter_var.get()
        self.listbox.delete(0, "end")
        self.files = []

        names = prefixes = None
        if stage != "All" and self.wad:
            names, prefixes = self._stage_assets(stage)

        for full, rel, cat, base in self.all_files:
            if names is not None:
                # A map uses a texture/flat by exact name, or a sprite by 4-char
                # prefix (POSSA1, POSSB1, ... all belong to the POSS actor).
                used = base in names or (cat == "sprites" and base[:4] in prefixes)
                if not used:
                    continue
            self.files.append(full)
            self.listbox.insert("end", rel)

        if select_first and self.files:
            self.listbox.selection_set(0)
            self.on_select()

    def _guess_alpha(self, path):
        return "sprites" in path.replace("\\", "/").lower()

    def on_select(self):
        """Auto-enable alpha for sprites when a new file is picked, then render."""
        sel = self.listbox.curselection()
        if sel and self.files:
            self.alpha_var.set(self._guess_alpha(self.files[sel[0]]))
        self.render()

    def render(self):
        sel = self.listbox.curselection()
        if not sel or not self.files:
            return
        path = self.files[sel[0]]

        # Auto-set alpha the first time a sprite/non-sprite is picked is annoying;
        # instead just honour the checkbox but seed a sensible default on load.
        try:
            img = Image.open(path)
        except Exception as e:  # noqa: BLE001
            self.orig_dims.config(text=f"error: {e}")
            return

        w = max(1, int(self.w_var.get()))
        h = max(1, int(self.h_var.get()))
        use_alpha = self.alpha_var.get()

        # Original preview (fit into PREVIEW_PX, nearest so pixels stay crisp).
        orig = img.convert("RGBA")
        scale = max(1, PREVIEW_PX // max(orig.width, orig.height))
        obg = checkerboard(PREVIEW_PX)
        oimg = orig.resize((orig.width * scale, orig.height * scale), Image.NEAREST)
        ox = (PREVIEW_PX - oimg.width) // 2
        oy = (PREVIEW_PX - oimg.height) // 2
        obg.alpha_composite(oimg, (max(0, ox), max(0, oy)))
        otk = ImageTk.PhotoImage(obg)
        self.orig_label.config(image=otk)
        self.orig_dims.config(text=f"{orig.width}\u00d7{orig.height}")

        # Converted preview: box-filter + palette, then blow up with NEAREST so the
        # actual MD texels are visible 1:1.
        indices, has_alpha = convert_to_md(img, w, h, use_alpha)
        conv = indices_to_image(indices, has_alpha)
        cscale = max(1, PREVIEW_PX // max(w, h))
        cbg = checkerboard(PREVIEW_PX)
        cimg = conv.resize((w * cscale, h * cscale), Image.NEAREST)
        cx = (PREVIEW_PX - cimg.width) // 2
        cy = (PREVIEW_PX - cimg.height) // 2
        cbg.alpha_composite(cimg, (max(0, cx), max(0, cy)))
        ctk = ImageTk.PhotoImage(cbg)
        self.conv_label.config(image=ctk)
        self.conv_dims.config(text=f"{w}\u00d7{h}, 16 colours")

        self._imgrefs = [otk, ctk]


def main():
    root = tk.Tk()
    App(root)
    root.mainloop()


if __name__ == "__main__":
    if not os.path.isdir(ASSET_DIR):
        print(f"Asset folder not found: {ASSET_DIR}", file=sys.stderr)
    main()
