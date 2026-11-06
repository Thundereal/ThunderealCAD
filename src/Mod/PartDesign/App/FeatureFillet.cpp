/***************************************************************************
 *   Copyright (c) 2008 Werner Mayer <wmayer[at]users.sourceforge.net>     *
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
# include <BRepAdaptor_Surface.hxx>
# include <BRepAlgo.hxx>
# include <BRepBuilderAPI_MakeVertex.hxx>
# include <BRepExtrema_DistShapeShape.hxx>
# include <BRepFilletAPI_MakeFillet.hxx>
# include <BRepGProp.hxx>
# include <BRepLProp_SLProps.hxx>
# include <BRep_Tool.hxx>
# include <GeomAPI_ProjectPointOnSurf.hxx>
# include <GProp_GProps.hxx>
# include <TopExp.hxx>
# include <TopExp_Explorer.hxx>
# include <TopoDS.hxx>
# include <TopoDS_Edge.hxx>
# include <TopTools_ListOfShape.hxx>
# include <ShapeFix_Shape.hxx>
# include <ShapeFix_ShapeTolerance.hxx>
#endif

#include <algorithm>

#include <Base/Console.h>
#include <Base/Exception.h>
#include <Base/Reader.h>
#include <App/Document.h>
#include <Mod/Part/App/FeatureFillet.h>
#include <Mod/Part/App/PartFeature.h>
#include <Mod/Part/App/PropertyTopoShape.h>
#include <Mod/Part/App/TopoShape.h>

#include "FeatureFillet.h"

FC_LOG_LEVEL_INIT("PartDesign",true,true)

using namespace PartDesign;


PROPERTY_SOURCE(PartDesign::Fillet, PartDesign::DressUp)

const App::PropertyQuantityConstraint::Constraints floatRadius = {0.0,FLT_MAX,0.1};

Fillet::Fillet()
{
    ADD_PROPERTY_TYPE(Radius, (1.0), "Fillet", App::Prop_None, "Fillet radius.");
    ADD_PROPERTY(Segments,());
    Radius.setUnit(Base::Unit::Length);
    Radius.setConstraints(&floatRadius);

    Segments.connectLinkProperty(Base);

    ADD_PROPERTY_TYPE(Corners, (), "Fillet", App::Prop_None,
      "Setback corners, by vertex: each fillet ending at the vertex stops a distance from it,\n"
      "measured along its edge, and one patch tangent to them closes the opening.\n"
      "A corner holds the setback of all its fillets (less than 0 for none) and the setbacks\n"
      "of single fillets by edge, and the depths of faces by face: the patch's boundary on\n"
      "the face bows into it, away from the vertex, that far at its middle. Needs the OCCT fork.");
    // its edges are Base's and follow it; its vertexes and faces it links itself
    Corners.connectLinkProperty(Base);

    ADD_PROPERTY_TYPE(UseAllEdges, (false), "Fillet", App::Prop_None,
      "Fillet all edges if true, else use only those edges in Base property.\n"
      "If true, then this overrides any edge changes made to the Base property or in the dialog.\n");

    ADD_PROPERTY_TYPE(RemoveSlivers, (true), "Fillet", App::Prop_None,
      "Where the fillet of an edge fails: take out the faces narrower than a quarter of\n"
      "the radius at its ends -- most often a step left between two features meant to\n"
      "meet -- their neighbours extended over them, and fillet again. Kept only when\n"
      "fewer fillets fail; the faces taken out are reported.");

    ADD_PROPERTY_TYPE(PastFeatures, (true), "Fillet", App::Prop_None,
      "Where the fillet of an edge fails, and RemoveSlivers did not help: take out the\n"
      "features near it -- a pocket or a boss within the radius of the edge, that its\n"
      "fillet runs into -- fillet, and put them back, a pocket then cutting into the\n"
      "fillet. Kept only when fewer fillets fail; the faces taken out are reported.");

    ADD_PROPERTY_TYPE(FullRound, (false), "Fillet", App::Prop_None,
      "Each face selected rounded whole: its two sides filleted so that their lines meet along\n"
      "the middle of the face, which goes (a full round). The face is a strip of one width\n"
      "between two side faces; Radius is not used for it. Edges selected are filleted as before.\n"
      "Needs the OCCT fork.");

    ADD_PROPERTY_TYPE(_Version, (0), "Part Design", App::Prop_Hidden, 0);
}

namespace {

// A full round of <face>: its two sides -- of its edges, the longest pair
// that share no vertex -- and the radius of each that puts the fillet's
// line on the face along its middle: half the face's width times tan(a / 2),
// a the angle of the solid between the face and the side's other face.
// Empty, with the reason, where the face is no strip of one width between
// two convex edges.
std::vector<std::pair<TopoDS_Edge, double>> fullRoundEdges(const Part::TopoShape &shape,
                                                           const TopoDS_Face &face,
                                                           std::string &reason)
{
    std::vector<std::pair<TopoDS_Edge, double>> ret;
    std::vector<TopoDS_Edge> edges;
    for (TopExp_Explorer ex(face, TopAbs_EDGE); ex.More(); ex.Next()) {
        const TopoDS_Edge &e = TopoDS::Edge(ex.Current());
        if (BRep_Tool::Degenerated(e))
            continue;
        if (std::none_of(edges.begin(), edges.end(),
                         [&](const TopoDS_Edge &o) { return o.IsSame(e); }))
            edges.push_back(e);
    }
    auto length = [](const TopoDS_Edge &e) {
        GProp_GProps props;
        BRepGProp::LinearProperties(e, props);
        return props.Mass();
    };
    auto shareVertex = [](const TopoDS_Edge &a, const TopoDS_Edge &b) {
        TopoDS_Vertex a1, a2, b1, b2;
        TopExp::Vertices(a, a1, a2);
        TopExp::Vertices(b, b1, b2);
        return a1.IsSame(b1) || a1.IsSame(b2) || a2.IsSame(b1) || a2.IsSame(b2);
    };
    int ia = -1, ib = -1;
    double best = 0.0;
    for (int i = 0; i < (int)edges.size(); ++i) {
        for (int j = i + 1; j < (int)edges.size(); ++j) {
            if (shareVertex(edges[i], edges[j]))
                continue;
            double l = std::min(length(edges[i]), length(edges[j]));
            if (l > best) {
                best = l;
                ia = i;
                ib = j;
            }
        }
    }
    if (ia < 0) {
        reason = "no two sides apart";
        return ret;
    }
    // the width along the sides, and at each the way into the face
    double wMin = DBL_MAX, wMax = 0.0;
    gp_Vec inward[2];
    for (int k = 0; k < 2; ++k) {
        const TopoDS_Edge &side = edges[k == 0 ? ia : ib];
        const TopoDS_Edge &other = edges[k == 0 ? ib : ia];
        double f, l;
        Handle(Geom_Curve) c = BRep_Tool::Curve(side, f, l);
        if (c.IsNull()) {
            reason = "a side with no curve";
            return ret;
        }
        for (int i = 0; i <= 4; ++i) {
            gp_Pnt p = c->Value(f + (l - f) * i / 4.0);
            BRepExtrema_DistShapeShape dist(BRepBuilderAPI_MakeVertex(p).Vertex(), other);
            if (!dist.IsDone() || dist.NbSolution() == 0) {
                reason = "no width";
                return ret;
            }
            wMin = std::min(wMin, dist.Value());
            wMax = std::max(wMax, dist.Value());
            if (i == 2)
                inward[k] = gp_Vec(p, dist.PointOnShape2(1));
        }
    }
    if (wMax <= Precision::Confusion() || wMax - wMin > 1e-6 * std::max(1.0, wMax)) {
        reason = "not of one width";
        return ret;
    }
    const double width = 0.5 * (wMin + wMax);
    // the outward normal of a face at a point on it
    auto normal = [](const TopoDS_Face &f, const gp_Pnt &p, gp_Vec &n) {
        GeomAPI_ProjectPointOnSurf proj(p, BRep_Tool::Surface(f));
        if (proj.NbPoints() == 0)
            return false;
        double u, v;
        proj.LowerDistanceParameters(u, v);
        BRepAdaptor_Surface s(f);
        BRepLProp_SLProps props(s, u, v, 1, Precision::Confusion());
        if (!props.IsNormalDefined())
            return false;
        n = props.Normal();
        if (f.Orientation() == TopAbs_REVERSED)
            n.Reverse();
        return true;
    };
    for (int k = 0; k < 2; ++k) {
        const TopoDS_Edge &side = edges[k == 0 ? ia : ib];
        TopoDS_Face neighbour;
        for (const auto &f : shape.findAncestorsShapes(side, TopAbs_FACE)) {
            if (!f.IsSame(face))
                neighbour = TopoDS::Face(f);
        }
        double f, l;
        Handle(Geom_Curve) c = BRep_Tool::Curve(side, f, l);
        gp_Pnt mid = c->Value(0.5 * (f + l));
        gp_Vec nFace, nSide;
        if (neighbour.IsNull() || !normal(face, mid, nFace) || !normal(neighbour, mid, nSide)) {
            reason = "a side with no face beyond";
            return ret;
        }
        // convex: the side face turns away from the way into the face
        if (nSide.Dot(inward[k]) >= 0.0) {
            reason = "a concave side";
            return ret;
        }
        const double a = M_PI - nFace.Angle(nSide);
        ret.emplace_back(side, 0.5 * width * std::tan(0.5 * a));
    }
    return ret;
}

} // namespace


void Fillet::setupObject()
{
    DressUp::setupObject();
    _Version.setValue(1);
}

short Fillet::mustExecute() const
{
    if (Placement.isTouched()
            || Radius.isTouched()
            || Segments.isTouched()
            || Corners.isTouched())
        return 1;
    return DressUp::mustExecute();
}

App::DocumentObjectExecReturn *Fillet::execute()
{
    Part::TopoShape baseShape;
    try {
        baseShape = getBaseShape();
    }
    catch (Base::Exception& e) {
        return new App::DocumentObjectExecReturn(e.what());
    }
    baseShape.setTransform(Base::Matrix4D());

    // As many fillets as can be made: an edge whose reference is gone or
    // whose fillet fails is left out, the result holds the others', and the
    // feature is in error still (docs/HandsOnQueue.md entry 143)
    std::vector<std::string> missing;
    auto edges = UseAllEdges.getValue() ? baseShape.getSubTopoShapes(TopAbs_EDGE)
                                        : getContinuousEdges(baseShape, &missing);
    if (edges.empty()) {
        setFailedEdges(missing);
        return new App::DocumentObjectExecReturn(QT_TRANSLATE_NOOP("Exception", "Fillet not possible on selected shapes"));
    }

    // Full rounds (FullRound): each face selected stands for its two sides,
    // their radii its own
    std::vector<std::pair<TopoDS_Edge, double>> rounds;
    if (FullRound.getValue() && !UseAllEdges.getValue()) {
        std::vector<TopoDS_Shape> roundFaces;
        for (const auto &v : Base.getShadowSubs()) {
            const auto &ref = v.first.size() ? v.first : v.second;
            TopoDS_Shape sub = baseShape.getSubShape(ref.c_str(), true);
            if (sub.IsNull() || sub.ShapeType() != TopAbs_FACE)
                continue;
            std::string reason;
            auto sides = fullRoundEdges(baseShape, TopoDS::Face(sub), reason);
            if (sides.empty())
                return new App::DocumentObjectExecReturn(
                    "Full round not possible on " + v.second + ": " + reason);
            roundFaces.push_back(sub);
            rounds.insert(rounds.end(), sides.begin(), sides.end());
        }
        // the faces' own edges out, their sides in
        edges.erase(std::remove_if(edges.begin(), edges.end(), [&](const TopoShape &e) {
            for (const auto &f : roundFaces) {
                for (TopExp_Explorer ex(f, TopAbs_EDGE); ex.More(); ex.Next()) {
                    if (ex.Current().IsSame(e.getShape()))
                        return true;
                }
            }
            return false;
        }), edges.end());
        for (const auto &r : rounds) {
            if (std::none_of(edges.begin(), edges.end(),
                             [&](const TopoShape &e) { return e.getShape().IsSame(r.first); }))
                edges.emplace_back(r.first);
        }
    }

    double radius = Radius.getValue();

    if(radius <= 0 && rounds.empty())
        return new App::DocumentObjectExecReturn(QT_TRANSLATE_NOOP("Exception", "Fillet radius must be greater than zero"));

    this->positionByBaseFeature();

    try {
        TopoShape shape(0,getDocument()->getStringHasher());

        std::vector<TopoShape::FilletSegments> segmentList;
        std::string sub;
        for (const auto &e : edges) {
            int index = baseShape.findShape(e.getShape());
            segmentList.emplace_back();
            auto &conf = segmentList.back();
            if (index == 0)
                continue;
            // a full round's side, its own radius
            auto round = std::find_if(rounds.begin(), rounds.end(),
                                      [&](const std::pair<TopoDS_Edge, double> &r) {
                                          return r.first.IsSame(e.getShape());
                                      });
            if (round != rounds.end()) {
                conf.emplace_back(0.0, round->second);
                continue;
            }
            sub = "Edge";
            sub += std::to_string(index);
            const auto &segments = Segments.getValue(sub);
            for (const auto &segment : segments)
                conf.emplace_back(segment.param, segment.radius, segment.length);
        }

        Part::TopoShape::FilletPartial partial;
        Part::FilletFlatEnd flatEnd(_Version.getValue() >= 1);
        auto corners = getCorners(baseShape);
        shape.makEFillet(baseShape,edges,segmentList,Radius.getValue(),nullptr,
                         corners,&partial);
        if (shape.isNull())
            return new App::DocumentObjectExecReturn(QT_TRANSLATE_NOOP("Exception", "Resulting shape is null"));

        // Where some fail, tried again: first with the slivers at them taken
        // out -- faces narrower than a quarter of the radius, most often a
        // step left between two features meant to meet (RemoveSlivers) --
        // then with the features near them taken out, a pocket or a boss the
        // fillet runs into, and put back after, cutting into the fillet
        // (PastFeatures). Each kept when fewer fail.
        TopoShape usedBase = baseShape;
        auto usedEdges = edges;
        auto usedCorners = corners;
        for (int pass = 0; pass < 2 && !partial.failed.empty(); ++pass) {
            const bool features = pass == 1;
            if (!(features ? PastFeatures.getValue() : RemoveSlivers.getValue()))
                continue;
            TopoShape freedBase = usedBase;
            auto freedEdges = usedEdges;
            std::vector<TopoDS_Shape> extra;
            for (const auto &corner : usedCorners) {
                extra.push_back(corner.vertex);
                for (const auto &v : corner.edges)
                    extra.push_back(v.first);
                for (const auto &v : corner.faces)
                    extra.push_back(v.first);
            }
            std::vector<std::string> taken;
            TopoShape pockets, bosses;
            if (features ? !removeFeatures(freedBase, freedEdges, partial.failed,
                                           Radius.getValue(), extra, taken, pockets, bosses)
                         : !removeSlivers(freedBase, freedEdges, partial.failed,
                                          Radius.getValue() / 4.0, extra, taken))
                continue;
            // the corners as they are after, what went dropped
            Part::TopoShape::FilletCorners freedCorners;
            std::size_t k = 0;
            for (const auto &corner : usedCorners) {
                auto freed = corner;
                freed.vertex = extra[k++];
                for (auto &v : freed.edges)
                    v.first = extra[k++];
                for (auto &v : freed.faces)
                    v.first = extra[k++];
                auto gone = [](const std::pair<TopoDS_Shape, double> &v) {
                    return v.first.IsNull();
                };
                freed.edges.erase(std::remove_if(freed.edges.begin(), freed.edges.end(), gone),
                                  freed.edges.end());
                freed.faces.erase(std::remove_if(freed.faces.begin(), freed.faces.end(), gone),
                                  freed.faces.end());
                if (!freed.vertex.IsNull())
                    freedCorners.push_back(freed);
            }
            try {
                TopoShape retry(0, getDocument()->getStringHasher());
                Part::TopoShape::FilletPartial retryPartial;
                retry.makEFillet(freedBase, freedEdges, segmentList, Radius.getValue(), nullptr,
                                 freedCorners, &retryPartial);
                if (features && !retry.isNull())
                    retry = putBackFeatures(retry, pockets, bosses);
                if (!retry.isNull() && retryPartial.failed.size() < partial.failed.size()) {
                    std::string names;
                    for (const auto &name : taken)
                        names += " " + name;
                    if (features)
                        FC_WARN(getFullName() << ": features taken out to fillet, and put back:"
                                << names);
                    else
                        FC_WARN(getFullName() << ": sliver faces taken out to fillet:" << names);
                    shape = retry;
                    partial = retryPartial;
                    // put back, the features are the base's again
                    if (!features) {
                        usedBase = freedBase;
                        usedEdges = freedEdges;
                        usedCorners = freedCorners;
                    }
                }
            }
            catch (Base::Exception &) {
            }
            catch (Standard_Failure &) {
            }
        }

        // The faces of each edge's fillet, kept for when it fails later; the
        // failed ones' as last made
        std::vector<std::string> failedNames;
        setFailedEdges(baseShape, edges, partial, missing, failedNames);

        TopTools_ListOfShape aLarg;
        aLarg.Append(usedBase.getShape());
        bool failed = false;
        // Checked as it will be kept: a run on copies of the inputs (a
        // frozen input refused, Part::Feature::recompute()) puts back what
        // it did not change. Otherwise every face of the result is new to
        // the base and checked whole, and a base face the file had as it is
        // failed the fillet (Fillet007 in scanner.FCStd, entry 160).
        if (!BRepAlgo::IsValid(aLarg, Part::ThawInputs::shareBack(shape.getShape()),
                               Standard_False, Standard_False)) {
            ShapeFix_ShapeTolerance aSFT;
            aSFT.LimitTolerance(shape.getShape(), Precision::Confusion(), Precision::Confusion(), TopAbs_SHAPE);
            // For backward compatibility
            if (FixShape.getValue() == 0) {
                shape.fix();
                if (!BRepAlgo::IsValid(aLarg, Part::ThawInputs::shareBack(shape.getShape()),
                                       Standard_False, Standard_False))
                    failed = true;
            }
        }

        if (!failed) {
            shape = refineShapeIfActive(shape);
            shape = getSolid(shape);
        }
        this->Shape.setValue(shape);
        // An invalid result that a fix "repairs" by dropping faces -- the one
        // above, or the property's own (FixShape) as it takes the value --
        // leaves an open shell: valid, and no solid. Features after it would
        // take it for a missing base -- a Pocket then hands back its own tool.
        if (!failed && baseShape.hasSubShape(TopAbs_SOLID)
                && !Shape.getShape().hasSubShape(TopAbs_SOLID))
            return new App::DocumentObjectExecReturn(QT_TRANSLATE_NOOP("Exception", "Resulting shape is not a solid"));

        if (failed)
            return new App::DocumentObjectExecReturn("Resulting shape is invalid");
        if (auto ret = failedEdgesError("Fillet failed on", failedNames))
            return ret;
        return App::DocumentObject::StdReturn;
    }
    catch (Standard_Failure& e) {
        return new App::DocumentObjectExecReturn(e.GetMessageString());
    }
}

Part::TopoShape::FilletCorners Fillet::getCorners(const Part::TopoShape &baseShape) const
{
    Part::TopoShape::FilletCorners corners;
    if (Corners.getValue().empty())
        return corners;

    // The edges of Corners are Base's, its vertexes and faces its own link's;
    // resolve them by their mapped names
    std::map<std::string, std::string> mappedNames;
    for (const App::PropertyLinkSub *link : {&Base, static_cast<const App::PropertyLinkSub*>(&Corners)}) {
        for (const auto &shadow : link->getShadowSubs()) {
            if (!shadow.first.empty())
                mappedNames[shadow.second] = shadow.first;
        }
    }
    auto find = [&](const std::string &name, TopAbs_ShapeEnum type) {
        auto it = mappedNames.find(name);
        const auto &ref = it == mappedNames.end() ? name : it->second;
        TopoDS_Shape shape = baseShape.getSubShape(ref.c_str(), true);
        if (shape.IsNull() || shape.ShapeType() != type) {
            FC_WARN(getFullName() << ": skip fillet corner reference " << name);
            return TopoDS_Shape();
        }
        return shape;
    };

    for (const auto &v : Corners.getValue()) {
        const auto &setting = v.second;
        if (setting.setback < 0.0 && setting.edges.empty())
            continue;
        TopoDS_Shape vertex = find(v.first, TopAbs_VERTEX);
        if (vertex.IsNull())
            continue;
        corners.emplace_back();
        auto &corner = corners.back();
        corner.vertex = vertex;
        corner.setback = setting.setback;
        // A corner that no longer ends a fillet is kept, and skipped
        corner.optional = true;
        for (const auto &e : setting.edges) {
            if (Part::PropertyFilletCorners::isFaceName(e.first)) {
                TopoDS_Shape face = find(e.first, TopAbs_FACE);
                if (!face.IsNull())
                    corner.faces.emplace_back(face, e.second);
                continue;
            }
            TopoDS_Shape edge = find(e.first, TopAbs_EDGE);
            if (!edge.IsNull())
                corner.edges.emplace_back(edge, e.second);
        }
    }
    return corners;
}

void Fillet::onChanged(const App::Property *prop)
{
    // Corners links the vertexes and faces of the object Base is on. Not
    // while this is being removed: the removal clears the links one by one
    // (PropertyLinkBase::breakLinks), Corners before Base, and Corners
    // linked again here stayed in the base's InList after the delete -- a
    // dangling object the next recompute's link check read.
    if ((prop == &Corners || prop == &Base) && getDocument()
            && !testStatus(App::Remove)
            && !isRestoring() && !getDocument()->isPerformingTransaction()
            && Base.getValue() && Corners.getLinkObject() != Base.getValue())
        Corners.setLinkObject(Base.getValue());
    DressUp::onChanged(prop);
}

void Fillet::onDocumentRestored()
{
    // A fillet from before Corners was a link kept its corners' vertexes in
    // Base, so that their names followed the topology; they move to Corners,
    // which maps the names again
    if (!Corners.getValue().empty() && !Corners.getLinkObject() && Base.getValue()) {
        const auto &subs = Base.getSubValues();
        const auto &shadows = Base.getShadowSubs();
        std::vector<std::string> keep;
        std::vector<App::PropertyLinkBase::ShadowSub> keepShadows;
        bool moved = false;
        for (std::size_t i=0; i<subs.size(); ++i) {
            App::PropertyLinkBase::ShadowSub shadow;
            if (i < shadows.size())
                shadow = shadows[i];
            const std::string &name = shadow.second.empty() ? subs[i] : shadow.second;
            if (Corners.getValue(name)) {
                moved = true;
                continue;
            }
            keep.push_back(subs[i]);
            keepShadows.push_back(shadow);
        }
        Corners.setLinkObject(Base.getValue());
        if (moved)
            Base.setValue(Base.getValue(), std::move(keep), std::move(keepShadows));
    }
    DressUp::onDocumentRestored();
}

void Fillet::handleChangedPropertyType(Base::XMLReader &reader, const char * TypeName, App::Property * prop)
{
    if (prop && strcmp(TypeName,"App::PropertyFloatConstraint") == 0 &&
        strcmp(prop->getTypeId().getName(), "App::PropertyQuantityConstraint") == 0) {
        App::PropertyFloatConstraint p;
        p.Restore(reader);
        static_cast<App::PropertyQuantityConstraint*>(prop)->setValue(p.getValue());
    }
    else {
        DressUp::handleChangedPropertyType(reader, TypeName, prop);
    }
}
