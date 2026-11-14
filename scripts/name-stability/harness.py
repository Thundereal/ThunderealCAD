# The name stability harness (docs/TopoNamingEnhance.md sec 9.5).
#
# A case builds a model.  Every face, edge and vertex of every feature is then
# referenced, the model is changed upstream, and each reference is sorted by
# what it found: the element it was made on, another one, or nothing.  The
# truth is the geometry: an element whose geometry is in the new shape
# unchanged is still that element, whatever its name says.
#
# Run through run.sh; this file is the library, cases.py the corpus.
import collections
import re

import FreeCAD as App
import Part

TOL = 1e-6


class Names:
    """A mapped name taken apart: the step that made it, and what it stands on."""

    ID = re.compile(r"^#([0-9a-f]+)(?::([0-9a-f]+))?$")
    TAG = re.compile(r";:H(-?[0-9a-f]*)(?::([0-9a-f]+))?,(.)$")
    OWN = re.compile(r"^;:(MG|M|G|U|L|I|C|X|R|B)(-?\d*)(?:-(\d+))?|^;(D)([0-9a-f]+)|^;(I)(\d+)")
    INPUT = re.compile(r"^(Face|Edge|Vertex|Wire|Shell|Solid)\d+$")

    def __init__(self, hasher):
        self.hasher = hasher
        self.memo = {}
        # (what is under, M/G/MG) of every step seen with a number: its
        # number-less sibling is the first of several, a place like theirs.
        self.numbered = set()

    def text(self, name):
        """One level of dehashing: '#a:4' -> 'g4;SKT'.  None when it is not an id."""
        m = self.ID.match(name)
        if not m or not self.hasher:
            return None
        try:
            sid = self.hasher.getID(int(m.group(1), 16))
        except Exception:
            return None
        if sid is None or sid.IsHashed or sid.IsBinary:
            return None
        sid.Index = int(m.group(2), 16) if m.group(2) else 0
        return sid.Data

    def split(self, name):
        """(own postfix, what is under it), or (None, name) for a name with no tag."""
        m = self.TAG.search(name)
        if not m:
            return None, name
        head = name[: m.start()]
        n = int(m.group(2), 16) if m.group(2) else 0
        if n > len(head):
            n = 0
        return head[len(head) - n :], head[: len(head) - n]

    def kind(self, own, base=None):
        if own is None:
            return "root"
        if not own:
            return "pass"
        m = self.OWN.match(own)
        if not m:
            return "op"
        if m.group(4):
            return "D"
        if m.group(6):
            return "I"
        k, n = m.group(1), m.group(2)
        if k in ("M", "G", "MG"):
            # '0' and '00' are the two fixed places (parallel, coplanar)
            if n in ("0", "00"):
                return "hist"
            # the last piece of an edge along its curve
            if n == "-1":
                return "Me"
            # a piece between the ends, with how many pieces there are
            if m.group(3):
                return "Mk"
            if n:
                return "Mn"
            return "M1" if (base, k) in self.numbered else "hist"
        if k == "B":
            # named by all its upper elements; a number only among those
            # that have the very same ones
            return "Bn" if n else "B"
        return k

    def learn(self, name):
        """Note the numbered steps of a name, before any name is labelled."""
        cur = name
        for _ in range(200):
            own, base = self.split(cur)
            if own is None:
                cur = self.text(cur)
                if cur is None:
                    return
                continue
            m = self.OWN.match(own)
            if m and m.group(1) in ("M", "G", "MG") and m.group(2) not in ("", "0", "00"):
                self.numbered.add((base, m.group(1)))
            cur = base

    def chain(self, name):
        """The kinds of every step from the name's own down to its root."""
        if name in self.memo:
            return self.memo[name]
        res = []
        cur = name
        for _ in range(200):
            own, base = self.split(cur)
            k = self.kind(own, base)
            if own is None:
                under = self.text(cur)
                if under is None:
                    res.append("input" if self.INPUT.match(cur) else "root")
                    break
                cur = under
                continue
            res.append(k)
            cur = base
        self.memo[name] = res
        return res

    # U: a place among an upper element's unnamed ones.  Mn, M1: a place among
    # the results of the same sources (M1 the number-less first).  Me, Mk: a
    # piece of an edge, the last along its curve and one between the ends (the
    # first is M1).  D: a place among elements of one name.  I: a place in a
    # pattern or an array, which is meant.
    POSITIONAL = ("U", "Bn", "Mn", "M1", "Me", "Mk", "D", "I")

    def own(self, name):
        return self.chain(name)[0]

    def via(self, name):
        """The nearest step holding a position, the name's own included; '-' for none."""
        for k in self.chain(name):
            if k in self.POSITIONAL:
                return k
        return "-"

    def rooted_in_input(self, name):
        return self.chain(name)[-1] == "input"

    def spell(self, name, depth=6):
        """The name with its ids opened, for a reader."""
        out = []
        cur = name
        for _ in range(depth):
            own, base = self.split(cur)
            if own is None:
                under = self.text(cur)
                if under is None:
                    out.append(cur)
                    break
                cur = under
                continue
            m = self.TAG.search(cur)
            out.append((own or "-") + "@" + m.group(1))
            cur = base
        else:
            out.append("...")
        return " <- ".join(out)


TYPES = (("Face", "Faces"), ("Edge", "Edges"), ("Vertex", "Vertexes"))


def _pt(p):
    return (p.x, p.y, p.z)


def signature(kind, el):
    """What makes two elements the same piece of geometry."""
    try:
        if kind == "Vertex":
            return _pt(el.Point)
        if kind == "Edge":
            a, b = el.FirstParameter, el.LastParameter
            ends = sorted([_pt(el.valueAt(a)), _pt(el.valueAt(b))])
            return _pt(el.valueAt(0.5 * (a + b))) + (el.Length,) + ends[0] + ends[1]
        bb = el.BoundBox
        return _pt(el.CenterOfMass) + (
            el.Area,
            bb.XMin,
            bb.YMin,
            bb.ZMin,
            bb.XMax,
            bb.YMax,
            bb.ZMax,
            len(el.Edges),
        )
    except Exception:
        return None


def _close(a, b):
    if a is None or b is None or len(a) != len(b):
        return False
    for x, y in zip(a, b):
        if abs(x - y) > TOL * max(1.0, abs(x), abs(y)):
            return False
    return True


class Index:
    """Signatures of one shape's elements of one kind, found again by value."""

    CELL = 1e-3

    def __init__(self, sigs):
        self.sigs = sigs
        self.cells = collections.defaultdict(list)
        for i, s in enumerate(sigs):
            if s is not None:
                self.cells[self._cell(s)].append(i)

    def _cell(self, s):
        return tuple(int(round(c / self.CELL)) for c in s[:3])

    def find(self, s):
        if s is None:
            return []
        c = self._cell(s)
        res = []
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for dz in (-1, 0, 1):
                    for i in self.cells.get((c[0] + dx, c[1] + dy, c[2] + dz), ()):
                        if _close(s, self.sigs[i]):
                            res.append(i)
        return res


def sample(kind, el):
    """A point of the element that is not on its border."""
    if kind == "Vertex":
        return el.Point
    if kind == "Edge":
        return el.valueAt(0.5 * (el.FirstParameter + el.LastParameter))
    pts, tris = el.tessellate(0.01)
    best, area = None, -1.0
    for a, b, c in tris:
        ar = (pts[b] - pts[a]).cross(pts[c] - pts[a]).Length
        if ar > area:
            best, area = (pts[a] + pts[b] + pts[c]) * (1.0 / 3.0), ar
    if best is None:
        return el.CenterOfMass
    try:
        u, v = el.Surface.parameter(best)
        return el.valueAt(u, v)
    except Exception:
        return best


def overlaps(kind, old, new):
    """Whether the two share ground: either one's inner point lies on the other."""
    tol = 1e-6 if kind != "Face" else 1e-4
    for a, b in ((old, new), (new, old)):
        try:
            if b.distToShape(Part.Vertex(sample(kind, a)))[0] <= tol:
                return True
        except Exception:
            pass
    return False


class Snapshot:
    """One feature's elements before the change."""

    def __init__(self, feat, names):
        self.name = feat.Name
        shape = feat.Shape
        self.els = {}
        for kind, attr in TYPES:
            els = getattr(shape, attr)
            rows = []
            for i, el in enumerate(els):
                sub = "%s%d" % (kind, i + 1)
                mapped = shape.getElementMappedName(sub)
                rows.append(dict(sub=sub, el=el, name=mapped, sig=signature(kind, el)))
                if mapped != sub:
                    names.learn(mapped)
            self.els[kind] = rows

    def label(self, names):
        """Once every snapshot is taken: numbered siblings are known only then."""
        for _, row in self.rows():
            if row["name"] == row["sub"]:
                row.update(own="unnamed", via="-", input=True)
            else:
                row.update(
                    own=names.own(row["name"]),
                    via=names.via(row["name"]),
                    input=names.rooted_in_input(row["name"]),
                )

    def subs(self):
        return [r["sub"] for kind, _ in TYPES for r in self.els[kind]]

    def rows(self):
        return [(kind, r) for kind, _ in TYPES for r in self.els[kind]]


OUTCOMES = ("same", "lost", "WRONG", "gone", "follows", "elsewhere")


def judge(kind, row, new_els, index, resolved):
    """Sort one reference.  `resolved` is the 0-based index it names now, or None.

    The element's geometry is in the new shape unchanged:
        same    the reference names it
        lost    the reference names nothing
        WRONG   the reference names another element
    The geometry is not there any more:
        gone    the reference names nothing
        follows the reference names an element sharing ground with the old one
        elsewhere  the reference names an element sharing none, right or not
    """
    there = index.find(row["sig"])
    if there:
        if resolved is None:
            return "lost"
        return "same" if resolved in there else "WRONG"
    if resolved is None:
        return "gone"
    if resolved >= len(new_els):
        return "elsewhere"
    return "follows" if overlaps(kind, row["el"], new_els[resolved]) else "elsewhere"


def scored(doc):
    """The features whose elements a user could have referenced."""
    res = []
    for o in doc.Objects:
        if not o.isDerivedFrom("Part::Feature") or o.isDerivedFrom("Part::Part2DObject"):
            continue
        # A body and a solid body show their tip's shape under their own tag:
        # every name changes with the tip, which is another matter than this.
        if (
            o.isDerivedFrom("PartDesign::Body")
            or o.isDerivedFrom("Part::Datum")
            or o.isDerivedFrom("PartDesign::SolidBody")
        ):
            continue
        if o.Name.startswith("NameRefs"):
            continue
        try:
            if o.Shape.isNull() or not o.Shape.Faces:
                continue
        except Exception:
            continue
        res.append(o)
    return res


def _index_of(kind, sub):
    if not sub or "?" in sub or not sub.startswith(kind):
        return None
    try:
        return int(sub[len(kind) :]) - 1
    except ValueError:
        return None


def run(case, label, change, examples=4):
    """Build the case, reference everything, apply the change, and count."""
    doc = App.newDocument("NameStability")
    res = dict(
        case=case.__name__,
        change=label,
        counts=collections.Counter(),
        pure=collections.Counter(),
        examples=[],
        notes=[],
        features=[],
    )
    try:
        ctx = case(doc)
        doc.recompute()
        feats = scored(doc)
        bad = [f.Name for f in feats if not f.isValid()]
        if bad:
            res["notes"].append("invalid before the change: %s" % bad)
        names = Names(doc.Hasher if hasattr(doc, "Hasher") else feats[0].Shape.Hasher)
        snaps, holders = {}, {}
        for f in feats:
            if not f.isValid():
                continue
            snaps[f.Name] = Snapshot(f, names)
            h = doc.addObject("App::FeaturePython", "NameRefs")
            h.addProperty("App::PropertyLinkSubList", "Refs")
            h.Refs = [(f, snaps[f.Name].subs())]
            holders[f.Name] = h
        for snap in snaps.values():
            snap.label(names)
        doc.recompute()
        change(ctx)
        doc.recompute()
        for fname, snap in snaps.items():
            f = doc.getObject(fname)
            if f is None:
                res["notes"].append("%s removed by the change" % fname)
                continue
            if not f.isValid():
                res["notes"].append("%s invalid after the change" % fname)
                continue
            shape = f.Shape
            subs = []
            for _, lst in holders[fname].Refs:
                subs.extend(lst)
            total = len(snap.subs())
            if len(subs) != total:
                res["notes"].append("%s: %d references came back as %d" % (fname, total, len(subs)))
                continue
            moved = False
            verdicts = []
            n = 0
            for kind, attr in TYPES:
                new_els = getattr(shape, attr)
                index = Index([signature(kind, e) for e in new_els])
                if len(new_els) != len(snap.els[kind]):
                    moved = True
                for row in snap.els[kind]:
                    sub = subs[n]
                    n += 1
                    if sub != row["sub"]:
                        moved = True
                    ref = judge(kind, row, new_els, index, _index_of(kind, sub))
                    # The name alone, with nothing looking for it when it is missing.
                    pure = None
                    if row["name"] != row["sub"]:
                        found = shape.getElementIndexedName(row["name"])
                        if found == row["name"]:
                            # a role name of a primitive, which is not looked up this way
                            found = sub
                        pure = _index_of(kind, found)
                        if (
                            pure is not None
                            and shape.getElementMappedName("%s%d" % (kind, pure + 1)) != row["name"]
                        ):
                            pure = None
                    verdicts.append((kind, row, sub, ref, judge(kind, row, new_els, index, pure)))
                    if ref not in ("same",):
                        moved = True
            if not moved:
                continue  # the change did not reach this feature
            by = collections.Counter()
            for _, _, _, ref, pure in verdicts:
                by[ref] += 1
                by["name " + pure] += 1
            res["features"].append(
                "%s: %s" % (fname, ", ".join("%s %d" % kv for kv in sorted(by.items())))
            )
            for kind, row, sub, ref, pure in verdicts:
                key = (kind, row["own"], row["via"], "in" if row["input"] else "")
                res["counts"][key + (ref,)] += 1
                res["pure"][key + (pure,)] += 1
                if "WRONG" in (ref, pure) and len(res["examples"]) < examples:
                    res["examples"].append(
                        "%s.%s -> %s (reference %s, name %s): %s"
                        % (fname, row["sub"], sub, ref, pure, names.spell(row["name"]))
                    )
    except Exception:
        import traceback

        res["notes"].append(traceback.format_exc())
    finally:
        App.closeDocument(doc.Name)
    return res
