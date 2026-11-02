/***************************************************************************
 *   Copyright (c) 2019 WandererFan <wandererfan@gmail.com>                *
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
# include <cstdlib>
# include <cstring>
# include <sstream>
#endif

#include <QCoreApplication>

#include <App/Document.h>
#include <Base/Console.h>
#include <Base/Exception.h>
#include <Mod/TechDraw/App/DrawViewDimExtentPy.h>  // generated from DrawViewDimExtentPy.xml

#include "DrawViewDimExtent.h"
#include "DrawDimHelper.h"
#include "DrawViewPart.h"

using namespace TechDraw;

//===========================================================================
// DrawViewDimExtent
//===========================================================================

PROPERTY_SOURCE(TechDraw::DrawViewDimExtent, TechDraw::DrawViewDimension)

DrawViewDimExtent::DrawViewDimExtent(void)
{
    App::PropertyLinkSubList       Source;                       //DrawViewPart & SubElements(Edges)
    App::PropertyLinkSubList       Source3d;                     //Part::Feature(s) & SubElements

    ADD_PROPERTY_TYPE(Source, (nullptr, nullptr), "", (App::PropertyType)(App::Prop_None), "View containing the  dimension");
    Source.setScope(App::LinkScope::Global);

    //Source3d is a candidate for deprecation as References3D contains the same information
    ADD_PROPERTY_TYPE(Source3d, (nullptr, nullptr), "", (App::PropertyType)(App::Prop_None), "3d geometry to be dimensioned");
    Source3d.setScope(App::LinkScope::Global);
    ADD_PROPERTY_TYPE(DirExtent ,(0), "", App::Prop_None, "Horizontal / Vertical");

    //CosmeticTags is a candidate for deprecation
    ADD_PROPERTY_TYPE(CosmeticTags ,(""), "", App::Prop_None, "Id of cosmetic endpoints");

}

App::DocumentObjectExecReturn *DrawViewDimExtent::execute(void)
{
//    Base::Console().Message("DVDE::execute() - %s\n", getNameInDocument());
    if (!keepUpdated()) {
        return App::DocumentObject::StdReturn;
    }
    // nothing to measure is the dimension's error, and a failed measurement
    // says on what (docs/TechDrawErrors.md 3.3)
    App::DocumentObject* docObj = Source.getValue();
    if (!docObj) {
        return new App::DocumentObjectExecReturn(
            QCoreApplication::translate("TechDraw", "The extent dimension has no view to measure")
                .toStdString(),
            this);
    }
    DrawViewPart* dvp = dynamic_cast<DrawViewPart*>(docObj);
    if (!dvp) {
        return new App::DocumentObjectExecReturn(
            QCoreApplication::translate("TechDraw",
                                        "The extent dimension's view %1 is not a part view")
                .arg(QString::fromUtf8(docObj->Label.getValue()))
                .toStdString(),
            this);
    }

    ReferenceVector references = getEffectiveReferences();
    if (references.empty()) {
        return new App::DocumentObjectExecReturn(
            QCoreApplication::translate("TechDraw", "The extent dimension on %1 has no references")
                .arg(QString::fromUtf8(dvp->Label.getValue()))
                .toStdString(),
            this);
    }

    resetLinear();
    resetAngular();
    resetArc();

    try {
        measure(references);
    }
    catch (const Base::Exception& e) {
        keepMeasurement();
        return new App::DocumentObjectExecReturn(
            QCoreApplication::translate("TechDraw", "The extent dimension on %1 failed: %2")
                .arg(QString::fromUtf8(dvp->Label.getValue()), QString::fromUtf8(e.what()))
                .toStdString(),
            this);
    }
    keepMeasurement();

    overrideKeepUpdated(false);
    return DrawView::execute();
}

//! the extent of the references, along the direction asked for
void DrawViewDimExtent::measure(const ReferenceVector& references)
{
    if (Type.isValue("Distance") || Type.isValue("DistanceX") || Type.isValue("DistanceY")) {
        setLinearPoints(getPointsExtent(references));
    }
}

//! validate 2D references - only checks if the target exists
bool DrawViewDimExtent::checkReferences2D() const
{
//    Base::Console().Message("DVDE::checkReferences2d() - %s\n", getNameInDocument());
    const std::vector<App::DocumentObject*> &objects = References2D.getValues();
    if (objects.empty()) {
        return false;
    }

    const std::vector<std::string> &subElements = References2D.getSubValues();
    //extent dims are the only dims allowed to have no subElements
    if (subElements.empty() || subElements.front().empty()) {
        return true;
    }

    //if we have an object and a non-empty subelement list, then extent dims are the same as other dims
    return DrawViewDimension::checkReferences2D();
}
pointPair DrawViewDimExtent::getPointsExtent(ReferenceVector references)
{
//    Base::Console().Message("DVD::getPointsExtent() - %s\n", getNameInDocument());
    App::DocumentObject* refObject = references.front().getObject();
    int direction = DirExtent.getValue();
    if (refObject->isDerivedFrom(TechDraw::DrawViewPart::getClassTypeId())) {
        auto dvp = static_cast<TechDraw::DrawViewPart*>(refObject);

        std::vector<std::string> edgeNames;     //empty list means we are using all the edges
        if (!references.at(0).getSubName().empty()) {
            //this is the usual case of selected edges in a dvp
            for (auto& ref : references) {
                if (ref.getSubName().empty()) {
                    continue;
                }
                std::string geomType = DrawUtil::getGeomTypeFromName(ref.getSubName());
                if (geomType == "Edge") {
                    edgeNames.push_back(ref.getSubName());
                }
            }
        }
        std::pair<Base::Vector3d, Base::Vector3d> endPoints =
            DrawDimHelper::minMax(dvp,
                                  edgeNames,
                                  direction);
        return pointPair(endPoints.first, endPoints.second);
    }

    //this is a 3d reference
    std::pair<Base::Vector3d, Base::Vector3d> endPoints =
            DrawDimHelper::minMax3d(getViewPart(),
                                    references,
                                    direction);
    return pointPair(endPoints.first, endPoints.second);
}

PyObject *DrawViewDimExtent::getPyObject(void)
{
    if (PythonObject.is(Py::_None())) {
        // ref counter is set to 1
        PythonObject = Py::Object(new DrawViewDimExtentPy(this), true);
    }
    return Py::new_reference_to(PythonObject);
}
