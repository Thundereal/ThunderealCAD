#ifndef PMI_DRAWING_H
#define PMI_DRAWING_H

#include <string>
#include <vector>

#include <gp_Pnt2d.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Shape.hxx>

#include <Mod/PMI/PMIGlobal.h>

namespace PMI
{

/** Strokes and fills in an annotation plane's coordinates: x the reading
 * direction, y up, z toward the reader (docs/GDandT.md sec 6.1-6.2).
 *
 * The result is one compound: edges for the strokes (frames, symbols,
 * leaders), planar faces for the fills (text, arrowheads, the datum
 * triangle) -- what the 3D view draws, the STEP export tessellates, and a
 * drawing can read.
 */
class PMIExport Drawing
{
public:
    Drawing();

    void line(double x1, double y1, double x2, double y2);
    void polyline(const std::vector<gp_Pnt2d>& points, bool closed = false);
    void rect(double x, double y, double w, double h);
    void circle(double cx, double cy, double r);
    /// An arc from angle a0 to a1 (radians, counter-clockwise)
    void arc(double cx, double cy, double r, double a0, double a1);
    void fill(const std::vector<gp_Pnt2d>& polygon);
    /// An arrowhead with its tip at (x, y), pointing along (dx, dy)
    void arrow(double x, double y, double dx, double dy, double length);

    /// \a text at capital height \a h, the baseline's start at (x, y);
    /// returns its width
    double text(const std::u32string& text, double h, double x, double y);
    double textWidth(const std::u32string& text, double h) const;

    /// A letter in a circle (a modifier: M, L, P, F, T, U, E) whose left is
    /// at x and centre at height cy; returns its width
    double circled(char32_t letter, double h, double x, double cy);

    /// A geometric characteristic's symbol in the square cell of side s
    /// whose lower left is (x, y). The values of
    /// GeometricTolerance::CharacteristicValue.
    void characteristic(int which, double x, double y, double s);

    TopoDS_Shape shape() const
    {
        return _comp;
    }
    bool isEmpty() const
    {
        return _empty;
    }

    /// The bounds of what was drawn
    double xmin = 0.0, ymin = 0.0, xmax = 0.0, ymax = 0.0;

private:
    void add(const TopoDS_Shape& s);
    void grow(double x, double y);

    TopoDS_Compound _comp;
    bool _empty = true;
};

}  // namespace PMI

#endif  // PMI_DRAWING_H
