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


#ifndef PARTDESIGN_FEATUREFILLET_H
#define PARTDESIGN_FEATUREFILLET_H

#include <App/PropertyStandard.h>
#include <App/PropertyUnits.h>
#include <Mod/Part/App/PropertyDressUp.h>
#include "FeatureDressUp.h"

namespace PartDesign
{

class PartDesignExport Fillet : public DressUp
{
    PROPERTY_HEADER_WITH_OVERRIDE(PartDesign::Fillet);

public:
    Fillet();

    App::PropertyQuantityConstraint Radius;
    Part::PropertyFilletSegments Segments;
    Part::PropertyFilletCorners Corners;
    App::PropertyBool UseAllEdges;
    /// A colour the user picked for an edge group's handles: the group id
    /// to "#rrggbb"; a group not here takes an automatic one (entry 202)
    App::PropertyMap GroupColors;
    App::PropertyBool RemoveSlivers;
    App::PropertyBool PastFeatures;
    App::PropertyBool FullRound;
    /// 1 from the first: a fillet's end that would cut into the body past
    /// its vertex is flat (Part::FilletFlatEnd); 0, a file's from before
    App::PropertyInteger _Version;
    /// The OCCT fork's fillet fix level the object recomputes at
    /// (Part::OCCTFilletFixLevel): the latest for an object made now, 0 for
    /// one a file from before brings
    App::PropertyInteger _FilletFixLevel;

    /** @name methods override feature */
    //@{
    /// recalculate the feature
    App::DocumentObjectExecReturn *execute() override;
    short mustExecute() const override;
    void setupObject() override;
    /// returns the type name of the view provider
    const char* getViewProviderName() const override {
        return "PartDesignGui::ViewProviderFillet";
    }
    //@}

protected:
    void onChanged(const App::Property *prop) override;
    void onDocumentRestored() override;
    void handleChangedPropertyType(Base::XMLReader &reader, const char * TypeName, App::Property * prop) override;

private:
    /// The setback corners of Corners in baseShape
    Part::TopoShape::FilletCorners getCorners(const Part::TopoShape &baseShape) const;

    std::vector<int> edgeIndices;
};

} //namespace Part


#endif // PARTDESIGN_FEATUREFILLET_H
