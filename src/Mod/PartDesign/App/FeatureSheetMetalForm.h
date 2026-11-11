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
#pragma once

#include <App/PropertyLinks.h>
#include <App/PropertyStandard.h>
#include <App/PropertyUnits.h>

#include "Feature.h"
#include "SheetMetalExtension.h"

namespace PartDesign
{

/** Forms pressed into a wall (docs/SheetMetal.md sec 4.8, ruled 2026-11-06)
 *
 * One at each point of Positions, a sketch on Face, a wall's skin of the
 * feature this builds on: rising out of that face, or into it if Reversed,
 * turned by Angle about its normal. Kind is the form: "Tool", Tool's shape
 * set on its face, taken as the form's outside and hollowed to the sheet's
 * thickness, the hollow opened down through the wall; or a built-in one,
 * "Dimple", Diameter across where it leaves the face, Depth high, its slant
 * at FlankAngle, its bends of Radius inside, the thickness all through it;
 * "Louver", the sheet cut along one long side and the strip risen as a hood,
 * Length long along the form's X, Width from its hinge to the opening,
 * which faces its Y, a ramp up to the lip Depth above the face, its ends
 * drawn down to the face at FlankAngle; "Bead", a dimple drawn out along
 * the form's X, Length end to end; "Lance", a louver whose ends are cut,
 * not drawn, its tab free on three sides; "DrawnCutout", a dimple or a bead
 * open at its top; "FlangedHole", a hole Diameter across, its rim drawn up
 * into a straight collar Depth high; "Emboss", a flat-topped rounded
 * rectangle Length by Width, its corners CornerRadius round, where it
 * leaves the face; "Gusset", the one form across a bend, Face a bend's: a
 * rib pressed in from its outside at each point's place along its axis,
 * its face across the inside corner Length along each wall from their
 * sharp, Width wide, its ends sloping back at FlankAngle, owned by the bend.
 * A bead and an emboss are drawn too (2026-11-11): each open chain of lines
 * and arcs in Positions, running on into each other, is a bead's path, the
 * bead Diameter across along it and round past its ends; each closed chain
 * an emboss's profile, where it leaves the face, its corners of two lines
 * rounded by CornerRadius. Those lie as drawn, not turned by Angle.
 * The built-in ones' sizes start from the rule's thickness.
 * Each is a form of the sheet's model (SheetMetal::Form), owned by its wall.
 */
class PartDesignExport SheetMetalForm : public PartDesign::Feature, public SheetMetalExtension
{
    PROPERTY_HEADER_WITH_EXTENSIONS(PartDesign::SheetMetalForm);

public:
    SheetMetalForm();

    App::PropertyEnumeration Kind;
    /// The wall's skin the forms rise out of, of the feature this builds on
    App::PropertyLinkSub Face;
    /// A sketch whose points are where the forms are (a sketch alone, its
    /// subelements unused: the panel's sketch choice takes this kind); a
    /// bead's paths and an emboss's profiles are what is drawn in it
    App::PropertyLinkSub Positions;
    /// Each form turned about the face's normal
    App::PropertyAngle Angle;
    /// Rising into the face, not out of it
    App::PropertyBool Reversed;
    /// Kind Tool: the tool, and its face that sits on the wall's skin
    App::PropertyLinkSub Tool;
    /// Kind Dimple, Bead, DrawnCutout: across where it leaves the face;
    /// FlangedHole: the hole, inside its collar
    App::PropertyLength Diameter;
    /// Kind Dimple, Louver, Bead, Lance, DrawnCutout, FlangedHole: how high
    /// its top is above the face
    App::PropertyLength Depth;
    /// The built-in kinds: the inside radius of its bends
    App::PropertyLength Radius;
    /// Kind Dimple, Bead, DrawnCutout: the slant of its side, from the face;
    /// Louver: its ends'
    App::PropertyAngle FlankAngle;
    /// Kind Louver, Lance: along the form's X; Bead, DrawnCutout: along it
    /// end to end
    App::PropertyLength Length;
    /// Kind Louver, Lance: from its hinge to its opening, along the form's Y;
    /// Emboss: across it
    App::PropertyLength Width;
    /// Kind Emboss: its corners' radius where it leaves the face
    App::PropertyLength CornerRadius;

    App::DocumentObjectExecReturn* execute() override;
    short mustExecute() const override;
    /// its own faces are those tracing to it, as a pad's are, and those its
    /// rounding made of an edge: the form's too, whatever the edge was
    bool isElementGenerated(const Part::TopoShape& shape,
                            const Data::MappedName& name) const override
    {
        return isElementTraced(shape, name) || Feature::isElementGenerated(shape, name);
    }
    void onChanged(const App::Property* prop) override;
    const char* getViewProviderName() const override
    {
        return "PartDesignGui::ViewProviderSheetMetalForm";
    }
    bool shouldApplyPlacement() override
    {
        // the base's shape as it is, in place
        return false;
    }

    static const char* KindEnums[];

private:
    /// The built-in sizes set from the rule's thickness, or restored
    bool sized = false;
};

}  // namespace PartDesign
