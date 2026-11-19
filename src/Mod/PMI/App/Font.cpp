#include "PreCompiled.h"

#include <cmath>
#include <fstream>

#include <BRep_Builder.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <gp_Pln.hxx>
#include <gp_Trsf.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Wire.hxx>

#include <App/Application.h>
#include <Base/Console.h>
#include <Mod/Part/App/FaceMakerBullseye.h>

#ifdef FCUseFreeType
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#endif

#include "Font.h"

FC_LOG_LEVEL_INIT("PMI", true, true)

using namespace PMI;

struct Font::Private
{
    std::vector<char> buffer;
#ifdef FCUseFreeType
    FT_Library lib = nullptr;
    FT_Face face = nullptr;
#endif
};

namespace
{
#ifdef FCUseFreeType
/// A glyph outline, flattened: closed polygons in font units
struct Outline
{
    std::vector<std::vector<gp_Pnt>> contours;
    gp_Pnt last;
};

constexpr int CurveSteps = 6;

int moveTo(const FT_Vector* to, void* user)
{
    auto o = static_cast<Outline*>(user);
    o->contours.emplace_back();
    o->last = gp_Pnt(to->x, to->y, 0);
    o->contours.back().push_back(o->last);
    return 0;
}

int lineTo(const FT_Vector* to, void* user)
{
    auto o = static_cast<Outline*>(user);
    gp_Pnt p(to->x, to->y, 0);
    if (!p.IsEqual(o->last, 1e-9)) {
        o->contours.back().push_back(p);
        o->last = p;
    }
    return 0;
}

int conicTo(const FT_Vector* c, const FT_Vector* to, void* user)
{
    auto o = static_cast<Outline*>(user);
    gp_Pnt p0 = o->last;
    gp_Pnt p1(c->x, c->y, 0);
    gp_Pnt p2(to->x, to->y, 0);
    for (int i = 1; i <= CurveSteps; ++i) {
        double t = double(i) / CurveSteps;
        double a = (1 - t) * (1 - t), b = 2 * (1 - t) * t, e = t * t;
        o->contours.back().emplace_back(a * p0.X() + b * p1.X() + e * p2.X(),
                                        a * p0.Y() + b * p1.Y() + e * p2.Y(), 0);
    }
    o->last = p2;
    return 0;
}

int cubicTo(const FT_Vector* c1, const FT_Vector* c2, const FT_Vector* to, void* user)
{
    auto o = static_cast<Outline*>(user);
    gp_Pnt p0 = o->last;
    gp_Pnt p1(c1->x, c1->y, 0);
    gp_Pnt p2(c2->x, c2->y, 0);
    gp_Pnt p3(to->x, to->y, 0);
    for (int i = 1; i <= CurveSteps; ++i) {
        double t = double(i) / CurveSteps, u = 1 - t;
        double a = u * u * u, b = 3 * u * u * t, e = 3 * u * t * t, f = t * t * t;
        o->contours.back().emplace_back(
            a * p0.X() + b * p1.X() + e * p2.X() + f * p3.X(),
            a * p0.Y() + b * p1.Y() + e * p2.Y() + f * p3.Y(),
            0);
    }
    o->last = p3;
    return 0;
}
#endif
}  // namespace

Font& Font::instance()
{
    static Font font;
    return font;
}

Font::Font()
    : d(new Private)
{
#ifdef FCUseFreeType
    std::string path =
        App::Application::getResourceDir() + "Mod/PMI/Resources/fonts/osifont-lgpl3fe.ttf";
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        _error = "cannot read the font " + path;
        FC_WARN(_error);
        return;
    }
    d->buffer.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    if (FT_Init_FreeType(&d->lib)
        || FT_New_Memory_Face(d->lib,
                              reinterpret_cast<const FT_Byte*>(d->buffer.data()),
                              static_cast<FT_Long>(d->buffer.size()),
                              0,
                              &d->face)) {
        _error = "FreeType cannot open " + path;
        FC_WARN(_error);
        return;
    }
    // Unhinted outlines at a size large enough to keep their detail
    FT_Set_Char_Size(d->face, 0, 1000 * 64, 72, 72);
    // The capital height: the top of H, else the font's ascender
    _capHeight = 0.0;
    if (FT_Load_Char(d->face, 'H', FT_LOAD_NO_SCALE | FT_LOAD_NO_BITMAP) == 0) {
        FT_BBox box;
        FT_Outline_Get_CBox(&d->face->glyph->outline, &box);
        _capHeight = double(box.yMax);
    }
    if (_capHeight <= 0.0) {
        _capHeight = double(d->face->ascender);
    }
    _valid = _capHeight > 0.0;
#else
    _error = "built without FreeType: annotations have no text";
#endif
}

Font::~Font()
{
#ifdef FCUseFreeType
    if (d->face) {
        FT_Done_Face(d->face);
    }
    if (d->lib) {
        FT_Done_FreeType(d->lib);
    }
#endif
    delete d;
}

Font::Glyph Font::make(char32_t c, double height)
{
    Glyph g;
#ifdef FCUseFreeType
    if (!_valid) {
        g.missing = true;
        return g;
    }
    FT_UInt index = FT_Get_Char_Index(d->face, c);
    if (index == 0 && c != ' ') {
        g.missing = true;
    }
    if (FT_Load_Glyph(d->face, index, FT_LOAD_NO_SCALE | FT_LOAD_NO_BITMAP | FT_LOAD_NO_HINTING)) {
        g.missing = true;
        return g;
    }
    const double unit = 1.0 / _capHeight;
    const double scale = unit * height;
    g.advance = double(d->face->glyph->advance.x) * unit;

    Outline outline;
    FT_Outline_Funcs funcs {moveTo, lineTo, conicTo, cubicTo, 0, 0};
    FT_Outline_Decompose(&d->face->glyph->outline, &funcs, &outline);

    Part::FaceMakerBullseye maker;
    maker.setPlane(gp_Pln(gp::Origin(), gp::DZ()));
    int wires = 0;
    for (auto& contour : outline.contours) {
        if (contour.size() < 3) {
            continue;
        }
        BRepBuilderAPI_MakePolygon poly;
        for (auto& p : contour) {
            poly.Add(gp_Pnt(p.X() * scale, p.Y() * scale, 0));
        }
        poly.Close();
        if (poly.IsDone()) {
            maker.addWire(poly.Wire());
            ++wires;
        }
    }
    if (wires) {
        try {
            maker.Build();
            g.faces = maker.Shape();
        }
        catch (const Standard_Failure& e) {
            FC_WARN("glyph " << (unsigned)c << ": " << e.what());
        }
        catch (const Base::Exception& e) {
            FC_WARN("glyph " << (unsigned)c << ": " << e.what());
        }
    }
#else
    (void)c;
    g.missing = true;
#endif
    return g;
}

const Font::Glyph& Font::glyph(char32_t c, double height)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto key = std::make_pair(c, height);
    auto it = _cache.find(key);
    if (it == _cache.end()) {
        it = _cache.emplace(key, make(c, height)).first;
    }
    return it->second;
}

bool Font::has(char32_t c)
{
    return !glyph(c).missing;
}

double Font::width(const std::u32string& text, double height)
{
    double w = 0.0;
    for (char32_t c : text) {
        w += glyph(c).advance;
    }
    return w * height;
}

TopoDS_Shape Font::text(const std::u32string& text, double height, double x, double y)
{
    TopoDS_Compound comp;
    BRep_Builder builder;
    builder.MakeCompound(comp);
    double pen = 0.0;
    for (char32_t c : text) {
        const Glyph& g = glyph(c, height);
        if (!g.faces.IsNull()) {
            gp_Trsf move;
            move.SetTranslation(gp_Vec(x + pen * height, y, 0));
            builder.Add(comp, g.faces.Moved(TopLoc_Location(move)));
        }
        pen += g.advance;
    }
    return comp;
}

std::u32string Font::fromUtf8(const std::string& s)
{
    std::u32string out;
    for (std::size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        char32_t cp = 0;
        int n = 0;
        if (c < 0x80) {
            cp = c;
            n = 1;
        }
        else if ((c >> 5) == 0x6) {
            cp = c & 0x1f;
            n = 2;
        }
        else if ((c >> 4) == 0xe) {
            cp = c & 0x0f;
            n = 3;
        }
        else if ((c >> 3) == 0x1e) {
            cp = c & 0x07;
            n = 4;
        }
        else {
            ++i;
            continue;
        }
        for (int k = 1; k < n && i + k < s.size(); ++k) {
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3f);
        }
        out.push_back(cp);
        i += n;
    }
    return out;
}

std::string Font::toUtf8(const std::u32string& s)
{
    std::string out;
    for (char32_t cp : s) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        }
        else if (cp < 0x800) {
            out += static_cast<char>(0xc0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        }
        else if (cp < 0x10000) {
            out += static_cast<char>(0xe0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        }
        else {
            out += static_cast<char>(0xf0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        }
    }
    return out;
}
