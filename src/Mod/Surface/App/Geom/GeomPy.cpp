// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
 *                                                                         *
 *   This file is part of the FreeCAD CAx development system.              *
 *                                                                         *
 *   This library is free software; you can redistribute it and/or         *
 *   modify it under the terms of the GNU Library General Public           *
 *   License as published by the Free Software Foundation; either          *
 *   version 2 of the License, or (at your option) any later version.      *
 *                                                                         *
 *   This library  is distributed in the hope that it will be useful,      *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this library; see the file COPYING.LIB. If not,    *
 *   write to the Free Software Foundation, Inc., 59 Temple Place,         *
 *   Suite 330, Boston, MA  02111-1307, USA                                *
 *                                                                         *
 ***************************************************************************/

#include "PreCompiled.h"

#ifndef _PreComp_
#include <array>

#include <BRepAdaptor_Curve.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <TopoDS.hxx>
#endif

#include <Base/Interpreter.h>
#include <Base/VectorPy.h>
#include <Base/PyWrapParseTupleAndKeywords.h>
#include <Mod/Part/App/Geometry.h>
#include <Mod/Part/App/GeometryPy.h>
#include <Mod/Part/App/TopoShapePy.h>

#include "GeomPy.h"
#include "Continuity.h"
#include "NetworkSurface.h"

namespace Surface::Geom
{

namespace
{

/// A Part.Curve, or an edge's curve over its range and where it is
Handle(Geom_Curve) toCurve(const Py::Object& o, const char* what, std::size_t index)
{
    if (PyObject_TypeCheck(o.ptr(), &Part::GeometryPy::Type)) {
        auto geo = static_cast<Part::GeometryPy*>(o.ptr())->getGeometryPtr();
        Handle(Geom_Curve) curve = Handle(Geom_Curve)::DownCast(geo->handle());
        if (!curve.IsNull()) {
            return Handle(Geom_Curve)::DownCast(curve->Copy());
        }
    }
    else if (PyObject_TypeCheck(o.ptr(), &Part::TopoShapePy::Type)) {
        const TopoDS_Shape& s =
            static_cast<Part::TopoShapePy*>(o.ptr())->getTopoShapePtr()->getShape();
        if (!s.IsNull() && s.ShapeType() == TopAbs_EDGE) {
            BRepAdaptor_Curve adapt(TopoDS::Edge(s));
            Handle(Geom_Curve) curve = new Geom_TrimmedCurve(adapt.Curve().Curve(),
                                                             adapt.FirstParameter(),
                                                             adapt.LastParameter());
            if (!s.Location().IsIdentity()) {
                curve->Transform(s.Location().Transformation());
            }
            return curve;
        }
    }
    throw Py::TypeError(std::string(what) + " " + std::to_string(index + 1)
                        + " is neither a Part.Curve nor an edge");
}

std::vector<Handle(Geom_Curve)> toCurves(const Py::Object& seq, const char* what)
{
    Py::Sequence list(seq);
    std::vector<Handle(Geom_Curve)> curves;
    for (std::size_t i = 0; i < (std::size_t)list.size(); ++i) {
        curves.push_back(toCurve(list[i], what, i));
    }
    return curves;
}

template<typename T>
Py::List toList(const std::vector<T>& v)
{
    Py::List list;
    for (const auto& x : v) {
        if constexpr (std::is_same_v<T, bool>) {
            list.append(Py::Boolean(x));
        }
        else {
            list.append(Py::Long(x));
        }
    }
    return list;
}

class Module: public Py::ExtensionModule<Module>
{
public:
    Module()
        : Py::ExtensionModule<Module>("Surface.Geom")
    {
        add_keyword_method(
            "network",
            &Module::network,
            "network(profiles, guides, tolerance=1e-4, fitTolerance=1e-4)\n"
            "  -> (Part.BSplineSurface, dict)\n\n"
            "A surface through a network of curves (Part.Curve or edges), profiles one\n"
            "way and guides the other, in any order and either way round. Every profile\n"
            "has to cross every guide once, within tolerance; each curve is refitted at\n"
            "degree 3 within fitTolerance. The dict has ProfileOrder and GuideOrder (the\n"
            "input indices in order along the surface), ProfileReversed and\n"
            "GuideReversed, MaxGap and MaxFitDeviation. Raises ValueError when the\n"
            "network cannot be spanned.");
        add_keyword_method(
            "families",
            &Module::families,
            "families(curves, tolerance=1e-4) -> (profiles, guides)\n\n"
            "The curves of a whole network sorted into its two families, as lists of\n"
            "input indices; the first curve's family is the profiles. Raises ValueError\n"
            "when the curves are not one network: a curve that meets nothing, one that\n"
            "meets both families, or a profile short of a guide.");
        add_keyword_method(
            "continuity",
            &Module::continuity,
            "continuity(edge, face1, face2, samples=50) -> dict\n\n"
            "How face1 and face2 meet along edge, which lies on both, sampled: MaxGap,\n"
            "MaxAngle (radians, 0 tangent, whichever way the normals point),\n"
            "MaxCurvatureGap (of the normal curvature across the edge), each with its\n"
            "worst point (WorstGap, WorstAngle, WorstCurvature), and Order: 2, 1 or 0\n"
            "for G2, G1 or G0 within the default tolerances, -1 when they do not meet.");
        initialize("Surface's geometry layer: functions over curves and surfaces, no document");
    }

private:
    Py::Object network(const Py::Tuple& args, const Py::Dict& kwds)
    {
        PyObject* profiles = nullptr;
        PyObject* guides = nullptr;
        NetworkOptions options;
        static const std::array<const char*, 5> keywords {"profiles",
                                                          "guides",
                                                          "tolerance",
                                                          "fitTolerance",
                                                          nullptr};
        if (!Base::Wrapped_ParseTupleAndKeywords(args.ptr(),
                                                 kwds.ptr(),
                                                 "OO|dd",
                                                 keywords,
                                                 &profiles,
                                                 &guides,
                                                 &options.tolerance,
                                                 &options.fitTolerance)) {
            throw Py::Exception();
        }
        auto result = networkSurface(toCurves(Py::Object(profiles), "profile"),
                                     toCurves(Py::Object(guides), "guide"),
                                     options);
        if (!result.isDone()) {
            throw Py::ValueError(result.error);
        }
        Py::Dict info;
        info.setItem("ProfileOrder", toList(result.profileOrder));
        info.setItem("GuideOrder", toList(result.guideOrder));
        info.setItem("ProfileReversed", toList(result.profileReversed));
        info.setItem("GuideReversed", toList(result.guideReversed));
        info.setItem("MaxGap", Py::Float(result.maxGap));
        info.setItem("MaxFitDeviation", Py::Float(result.maxFitDeviation));
        Part::GeomBSplineSurface surface(result.surface);
        return Py::TupleN(Py::asObject(surface.getPyObject()), info);
    }

    Py::Object families(const Py::Tuple& args, const Py::Dict& kwds)
    {
        PyObject* curves = nullptr;
        double tolerance = NetworkOptions().tolerance;
        static const std::array<const char*, 3> keywords {"curves", "tolerance", nullptr};
        if (!Base::Wrapped_ParseTupleAndKeywords(args.ptr(),
                                                 kwds.ptr(),
                                                 "O|d",
                                                 keywords,
                                                 &curves,
                                                 &tolerance)) {
            throw Py::Exception();
        }
        auto result = sortFamilies(toCurves(Py::Object(curves), "curve"), tolerance);
        if (!result.isDone()) {
            throw Py::ValueError(result.error);
        }
        return Py::TupleN(toList(result.profiles), toList(result.guides));
    }

    Py::Object continuity(const Py::Tuple& args, const Py::Dict& kwds)
    {
        PyObject *edge = nullptr, *face1 = nullptr, *face2 = nullptr;
        int samples = 50;
        static const std::array<const char*, 5> keywords {"edge",
                                                          "face1",
                                                          "face2",
                                                          "samples",
                                                          nullptr};
        if (!Base::Wrapped_ParseTupleAndKeywords(args.ptr(),
                                                 kwds.ptr(),
                                                 "O!O!O!|i",
                                                 keywords,
                                                 &Part::TopoShapePy::Type,
                                                 &edge,
                                                 &Part::TopoShapePy::Type,
                                                 &face1,
                                                 &Part::TopoShapePy::Type,
                                                 &face2,
                                                 &samples)) {
            throw Py::Exception();
        }
        auto shape = [](PyObject* o) {
            return static_cast<Part::TopoShapePy*>(o)->getTopoShapePtr()->getShape();
        };
        const TopoDS_Shape &e = shape(edge), &f1 = shape(face1), &f2 = shape(face2);
        if (e.IsNull() || e.ShapeType() != TopAbs_EDGE || f1.IsNull()
            || f1.ShapeType() != TopAbs_FACE || f2.IsNull() || f2.ShapeType() != TopAbs_FACE) {
            throw Py::TypeError("an edge and two faces are needed");
        }
        auto r = edgeContinuity(TopoDS::Edge(e), TopoDS::Face(f1), TopoDS::Face(f2), samples);
        if (!r.error.empty()) {
            throw Py::ValueError(r.error);
        }
        auto vec = [](const gp_Pnt& p) {
            return Py::asObject(new Base::VectorPy(Base::Vector3d(p.X(), p.Y(), p.Z())));
        };
        Py::Dict out;
        out.setItem("MaxGap", Py::Float(r.maxGap));
        out.setItem("MaxAngle", Py::Float(r.maxAngle));
        out.setItem("MaxCurvatureGap", Py::Float(r.maxCurvatureGap));
        out.setItem("WorstGap", vec(r.worstGap));
        out.setItem("WorstAngle", vec(r.worstAngle));
        out.setItem("WorstCurvature", vec(r.worstCurvature));
        out.setItem("Order", Py::Long(r.order()));
        return out;
    }
};

}  // namespace

PyObject* initModule()
{
    return Base::Interpreter().addModule(new Module);
}

}  // namespace Surface::Geom
