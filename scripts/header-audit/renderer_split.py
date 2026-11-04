"""Estimate a split of Gui/Renderer/Renderer.h: which TUs reaching it name a config struct
(defined before `struct Material`) and so would include a RenderConfig.h."""
import re, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hdrmodel
m = hdrmodel.load(sys.argv[1], sys.argv[2], sys.argv[3])
hdr = sys.argv[2] + "/src/Gui/Renderer/Renderer.h"
text = open(hdr, encoding="utf-8").read().split("\n")
mline = next(i for i, l in enumerate(text) if re.match(r"struct Material\b", l))
cfg = set()
for l in text[:mline]:
    mo = re.match(r"(?:struct|class|enum(?: class)?)\s+(?:RendererExport\s+)?(\w+)\s*(?::[^;]*)?\{?\s*$", l)
    if mo:
        if mo.group(1).endswith("Config") or mo.group(1) in ("GroundCamera", "ViewLight", "FrameDumpRequest", "RenderStats", "FinishPalette", "SurfaceFrame", "FramePalette", "TexturePalette", "Background", "OverlayAnchor", "CacheSerial", "UserShader"):
            cfg.add(mo.group(1))
hid = m.ids[hdr.lower()]
tus = [t for t in m.tus if hid in t["deps"]]
rx = re.compile(r"\b(" + "|".join(sorted(cfg)) + r")\b")
users = []
for t in tus:
    txt = ""
    for p in (t["src"], os.path.splitext(t["src"])[0] + ".h", os.path.splitext(t["src"])[0] + "P.h"):
        try:
            txt += open(p, encoding="utf-8", errors="replace").read()
        except OSError:
            pass
    if rx.search(txt):
        users.append(t)
# headers in Gui/Renderer that name config types would carry them to their includers too
hdr_users = set()
for i, n in enumerate(m.norm):
    if m.kind[i] == "fork" and "/gui/renderer/" in n and n != hdr.lower():
        try:
            if rx.search(open(m.paths[i], encoding="utf-8", errors="replace").read()):
                hdr_users.add(i)
        except OSError:
            pass
carried = [t for t in tus if t not in users and (hdr_users & t["deps"])]
ms = lambda L: sum(t["ms"] or 0 for t in L) / 60000
print("config structs:", len(cfg), "TUs reaching Renderer.h:", len(tus), f"{ms(tus):.1f} CPU-min")
print("TUs naming a config struct:", len(users), f"{ms(users):.1f} CPU-min")
print("plus TUs reaching a renderer header that names one:", len(carried), f"{ms(carried):.1f} CPU-min",
      [hdrmodel.display(m, h) for h in sorted(hdr_users)][:12])
