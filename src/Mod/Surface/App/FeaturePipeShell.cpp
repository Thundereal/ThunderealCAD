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
#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepOffsetAPI_MakePipeShell.hxx>
#include <ShapeFix_Edge.hxx>
#include <TopExp_Explorer.hxx>
#include <BRep_Tool.hxx>
#include <Precision.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Ax2.hxx>
#endif

#include <App/Document.h>
#include <Mod/Part/App/TopoShapeOpCode.h>

#include "FeaturePipeShell.h"


using namespace Surface;

PROPERTY_SOURCE(Surface::PipeShell, Part::Feature)

namespace
{
const char* ModeEnums[] =
    {"Frenet", "CorrectedFrenet", "Fixed", "Binormal", "Discrete", "Auxiliary", "Support", nullptr};
const char* ContactEnums[] = {"NoContact", "Contact", "ContactOnBorder", nullptr};
const char* TransitionEnums[] = {"Transformed", "RightCorner", "RoundCorner", nullptr};

/// Every shape a link list names, whole objects whole
std::vector<Part::TopoShape> shapesOf(const App::PropertyLinkSubList& prop)
{
    std::vector<Part::TopoShape> shapes;
    const auto& objs = prop.getValues();
    const auto& subs = prop.getSubValues();
    for (std::size_t i = 0; i < objs.size() && i < subs.size(); ++i) {
        auto s = Part::Feature::getTopoShape(objs[i], subs[i].c_str(), true);
        if (!s.isNull()) {
            shapes.push_back(s);
        }
    }
    return shapes;
}

/// The shapes as one wire, or why not
std::string wireOf(const std::vector<Part::TopoShape>& shapes,
                   const char* what,
                   Part::TopoShape& wire)
{
    if (shapes.empty()) {
        return std::string("No ") + what;
    }
    Part::TopoShape all;
    all.makECompound(shapes, nullptr, false);
    wire = all.makEWires();
    if (wire.isNull() || wire.getShape().ShapeType() != TopAbs_WIRE) {
        return std::string("The ") + what + " is not one wire";
    }
    return {};
}
}  // namespace

PipeShell::PipeShell()
{
    ADD_PROPERTY_TYPE(Spine,
                      (nullptr),
                      "PipeShell",
                      App::Prop_None,
                      "The path: edges forming a wire");
    Spine.setScope(App::LinkScope::Global);
    ADD_PROPERTY_TYPE(Profiles,
                      (nullptr),
                      "PipeShell",
                      App::Prop_None,
                      "The sections swept: wires, edges, or a vertex at either end");
    Profiles.setScope(App::LinkScope::Global);
    ADD_PROPERTY_TYPE(Mode,
                      ((long)0),
                      "PipeShell",
                      App::Prop_None,
                      "How a section turns along the spine:\n"
                      "Frenet: with the spine's Frenet frame.\n"
                      "CorrectedFrenet: with it, its twist removed.\n"
                      "Fixed: not at all; Direction is its normal.\n"
                      "Binormal: its binormal kept along Direction.\n"
                      "Discrete: a discrete trihedron, for spines with sharp turns.\n"
                      "Auxiliary: its normal towards AuxiliarySpine.\n"
                      "Support: its binormal the normal of the Support faces the spine is on");
    Mode.setEnums(ModeEnums);
    ADD_PROPERTY_TYPE(Direction,
                      (Base::Vector3d(0, 0, 1)),
                      "PipeShell",
                      App::Prop_None,
                      "Fixed: the sections' normal. Binormal: their binormal");
    ADD_PROPERTY_TYPE(AuxiliarySpine,
                      (nullptr),
                      "PipeShell",
                      App::Prop_None,
                      "Auxiliary: the second path the sections face");
    AuxiliarySpine.setScope(App::LinkScope::Global);
    ADD_PROPERTY_TYPE(CurvilinearEquivalence,
                      (true),
                      "PipeShell",
                      App::Prop_None,
                      "Auxiliary: a point of the spine faces the point at the same share of the "
                      "auxiliary spine's length; otherwise where the plane square to the spine "
                      "cuts it");
    ADD_PROPERTY_TYPE(AuxiliaryContact,
                      ((long)0),
                      "PipeShell",
                      App::Prop_None,
                      "Auxiliary: NoContact faces it; Contact keeps it on the surface, the "
                      "sections' width constant; ContactOnBorder makes it a border, the width "
                      "varying");
    AuxiliaryContact.setEnums(ContactEnums);
    ADD_PROPERTY_TYPE(Support,
                      (nullptr),
                      "PipeShell",
                      App::Prop_None,
                      "Support: faces every edge of the spine lies on");
    Support.setScope(App::LinkScope::Global);
    ADD_PROPERTY_TYPE(Transition,
                      ((long)0),
                      "PipeShell",
                      App::Prop_None,
                      "At a sharp turn of the spine: Transformed, RightCorner or RoundCorner");
    Transition.setEnums(TransitionEnums);
    ADD_PROPERTY_TYPE(WithContact,
                      (false),
                      "PipeShell",
                      App::Prop_None,
                      "The sections moved to touch the spine");
    ADD_PROPERTY_TYPE(WithCorrection,
                      (false),
                      "PipeShell",
                      App::Prop_None,
                      "The sections turned square to the spine");
    ADD_PROPERTY_TYPE(Solid, (false), "PipeShell", App::Prop_None, "Closed with its end sections");
}

App::DocumentObjectExecReturn* PipeShell::execute()
{
    Part::TopoShape spine;
    auto error = wireOf(shapesOf(Spine), "spine", spine);
    if (!error.empty()) {
        return new App::DocumentObjectExecReturn(error);
    }
    auto profileShapes = shapesOf(Profiles);
    if (profileShapes.empty()) {
        return new App::DocumentObjectExecReturn("No profile");
    }
    std::vector<Part::TopoShape> inputs {spine};

    try {
        TopoDS_Shape spineShape = BRepBuilderAPI_Copy(spine.getShape()).Shape();
        BRepOffsetAPI_MakePipeShell mk(TopoDS::Wire(spineShape));
        const auto& d = Direction.getValue();
        switch (Mode.getValue()) {
            case 0:
                mk.SetMode(true);
                break;
            case 1:
                mk.SetMode(false);
                break;
            case 2:
            case 3: {
                if (d.Length() < Precision::Confusion()) {
                    return new App::DocumentObjectExecReturn("Direction is not set");
                }
                gp_Dir dir(d.x, d.y, d.z);
                if (Mode.getValue() == 3) {
                    mk.SetMode(dir);
                }
                else {
                    TopoDS_Vertex v1, v2;
                    TopExp::Vertices(TopoDS::Wire(spineShape), v1, v2);
                    mk.SetMode(gp_Ax2(BRep_Tool::Pnt(v1), dir));
                }
                break;
            }
            case 4:
                mk.SetDiscreteMode();
                break;
            case 5: {
                Part::TopoShape aux;
                error = wireOf(shapesOf(AuxiliarySpine), "auxiliary spine", aux);
                if (!error.empty()) {
                    return new App::DocumentObjectExecReturn(error);
                }
                mk.SetMode(TopoDS::Wire(aux.getShape()),
                           CurvilinearEquivalence.getValue(),
                           BRepFill_TypeOfContact(AuxiliaryContact.getValue()));
                inputs.push_back(aux);
                break;
            }
            case 6: {
                auto faces = shapesOf(Support);
                if (faces.empty()) {
                    return new App::DocumentObjectExecReturn("No support");
                }
                Part::TopoShape support;
                support.makECompound(faces, nullptr, false);
                // OCCT wants each edge of the spine to have its curve on one
                // of these very faces; a spine from elsewhere has it on a
                // copy of the surface, or none: lay it on the face it is on
                for (TopExp_Explorer e(spineShape, TopAbs_EDGE); e.More(); e.Next()) {
                    const TopoDS_Edge& edge = TopoDS::Edge(e.Current());
                    for (TopExp_Explorer f(support.getShape(), TopAbs_FACE); f.More(); f.Next()) {
                        const TopoDS_Face& face = TopoDS::Face(f.Current());
                        BRepAdaptor_Curve c(edge);
                        gp_Pnt mid = c.Value((c.FirstParameter() + c.LastParameter()) / 2);
                        BRepExtrema_DistShapeShape d(BRepBuilderAPI_MakeVertex(mid).Vertex(), face);
                        if (d.IsDone() && d.Value() < 1e-6) {
                            ShapeFix_Edge().FixAddPCurve(edge, face, false, 1e-6);
                            break;
                        }
                    }
                }
                if (!mk.SetMode(support.getShape())) {
                    return new App::DocumentObjectExecReturn(
                        "The spine does not lie on the support: every edge of it has to be on "
                        "one of its faces");
                }
                inputs.push_back(support);
                break;
            }
        }
        mk.SetTransitionMode(BRepBuilderAPI_TransitionMode(Transition.getValue()));
        for (const auto& p : profileShapes) {
            TopoDS_Shape s = p.getShape();
            if (s.ShapeType() != TopAbs_VERTEX && s.ShapeType() != TopAbs_WIRE) {
                auto w = p.makEWires();
                if (w.isNull() || w.getShape().ShapeType() != TopAbs_WIRE) {
                    return new App::DocumentObjectExecReturn("A profile is not one wire");
                }
                s = w.getShape();
            }
            mk.Add(s, WithContact.getValue(), WithCorrection.getValue());
            inputs.push_back(p);
        }
        if (!mk.IsReady()) {
            return new App::DocumentObjectExecReturn("The pipe shell is not ready to build");
        }
        mk.Build();
        if (!mk.IsDone()) {
            return new App::DocumentObjectExecReturn(
                "The profiles could not be swept along the spine");
        }
        if (Solid.getValue() && !mk.MakeSolid()) {
            return new App::DocumentObjectExecReturn("The sweep could not be closed to a solid");
        }
        Part::TopoShape result(0, getDocument()->getStringHasher());
        result.makEShape(mk, inputs, Part::OpCodes::PipeShell);
        Shape.setValue(result);
    }
    catch (Standard_Failure& e) {
        return new App::DocumentObjectExecReturn(std::string("The sweep failed: ") + e.what());
    }
    return StdReturn;
}
