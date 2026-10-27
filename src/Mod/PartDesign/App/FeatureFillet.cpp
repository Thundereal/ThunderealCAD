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
# include <BRepAlgo.hxx>
# include <BRepFilletAPI_MakeFillet.hxx>
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

    // Output: a colour changes no shape. A file without it gives every
    // group an automatic colour, as before (docs/HandsOnQueue.md entry 202)
    ADD_PROPERTY_TYPE(GroupColors, (), "Fillet",
      static_cast<App::PropertyType>(App::Prop_Hidden | App::Prop_Output),
      "The colour of an edge group's handles in the view, by group id ('#rrggbb').\n"
      "A group not listed takes an automatic colour.");

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

    ADD_PROPERTY_TYPE(_Version, (0), "Part Design", App::Prop_Hidden, 0);
}

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

    double radius = Radius.getValue();

    if(radius <= 0)
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
    // while this fillet is being removed: the removal clears Corners before
    // Base, and linking it again left this fillet in the InList of the object
    // Base is on, a dangling pointer once it was deleted (a recompute walking
    // that list crashed, TestFilletSlivers.testBaseOutsideTheBody)
    if ((prop == &Corners || prop == &Base) && getDocument()
            && !isRestoring() && !getDocument()->isPerformingTransaction()
            && !testStatus(App::Remove)
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
