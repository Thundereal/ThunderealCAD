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

#ifndef PMI_DATUM_H
#define PMI_DATUM_H

#include <string>
#include <vector>

#include <App/PropertyLinks.h>
#include <App/PropertyStandard.h>
#include <Mod/Part/App/PropertyTopoShape.h>

#include "Callout.h"

namespace PMI
{

class TolerancedFeature;
class DatumTarget;

/** A datum feature and the datum simulated from it (docs/GDandT.md
 * sec 4.3): the faces are the feature's, the exact plane, axis or point in
 * Shape is the datum. A datum may be established by targets instead, or
 * as well: points, lines and areas of the part, each a DatumTarget with
 * its own symbol. Upstream's PMI_Datum.
 */
class PMIExport Datum: public Callout
{
    PROPERTY_HEADER_WITH_OVERRIDE(PMI::Datum);

public:
    Datum();

    App::PropertyString Letter;
    App::PropertyLink Feature;
    /// The targets establishing it, if any
    App::PropertyLinkList Targets;
    /// The simulated datum: a plane, an axis or a point
    Part::PropertyPartShape Shape;

    App::DocumentObjectExecReturn* execute() override;
    TolerancedFeature* calloutFeature() const override;
    void layout(Drawing& d, double h) const override;
    std::u32string equivalentText() const override;
    LeaderStyle leaderStyle() const override
    {
        return TriangleLeader;
    }

    TolerancedFeature* getFeature() const;
    std::vector<DatumTarget*> getTargets() const;
    /// The solid it is of: its feature's, else its first target's
    const App::DocumentObject* solid() const;

    /// Whether \a letter is a legal datum letter under the set's standard
    /// (ASME refuses I, O and Q); \a why says what is wrong
    static bool isLegalLetter(const std::string& letter, bool asme, std::string& why);

protected:
    void onChanged(const App::Property* prop) override;
    void onDocumentRestored() override;

private:
    /// The simulated datum Shape is: of its feature, else of its targets
    TopoDS_Shape simulated() const;
    /// The datum its targets simulate: a plane through three or more
    /// points, a line through two, a point
    TopoDS_Shape fromTargets() const;
};

/** A datum target (docs/GDandT.md sec 4.3): a point, a line or an area of
 * the part that a datum is established from (ASME Y14.5 sec 7.24, ISO
 * 5459). Drawn as the target symbol -- a circle, the area's size above its
 * line, the datum's letter and the target's number below -- with a leader
 * to the target, and the target on the part: an X for a point, a phantom
 * line for a line, an outline hatched for an area. The Datum that lists it
 * in its Targets gives the letter. Upstream's datum target of PMI_Datum.
 */
class PMIExport DatumTarget: public Callout
{
    PROPERTY_HEADER_WITH_OVERRIDE(PMI::DatumTarget);

public:
    DatumTarget();

    enum TargetTypes
    {
        Point,
        Line,
        Rectangle,
        Circle,
        Area,
        /// A circle that is a curve, not an area (AP242's 'circular curve':
        /// NIST FTC-06): its diameter the Length, as a circle's
        CircularCurve,
    };

    App::PropertyInteger Number;
    App::PropertyEnumeration TargetType;
    /// Where it is: the origin its centre (a line's middle), z out of the
    /// part, x along a line or a rectangle's length
    App::PropertyPlacement Target;
    /// A line's length, a rectangle's length, a circle's or a circular
    /// curve's diameter
    App::PropertyLength Length;
    /// A rectangle's width
    App::PropertyLength Width;
    /// A movable target (ASME): its simulator moves with the feature
    App::PropertyBool Movable;
    /// The faces it lies on; an area target is these faces
    App::PropertyLink Feature;
    /// The target: a vertex, an edge or a face
    Part::PropertyPartShape Shape;

    App::DocumentObjectExecReturn* execute() override;
    TolerancedFeature* calloutFeature() const override;
    bool calloutTarget(gp_Pnt& target) const override;
    bool autoPlane(double h, gp_Ax3& plane, gp_Pnt& target) const override;
    void layout(Drawing& d, double h) const override;
    TopoDS_Shape mark(double h) const override;
    std::u32string equivalentText() const override;
    LeaderStyle leaderStyle() const override
    {
        return PlainLeader;
    }

    TargetTypes getTargetType() const
    {
        return static_cast<TargetTypes>(TargetType.getValue());
    }
    /// The Datum listing it, or null
    Datum* getDatum() const;
    /// The datum's letter and its number: "A1"
    std::string label() const;
    /// Whether it has a size: a line or an area
    bool hasSize() const;

    static const char* TargetTypeEnums[];

protected:
    void onDocumentRestored() override;

private:
    TopoDS_Shape makeShape() const;
};

/** A datum reference frame (docs/GDandT.md sec 4.3): up to three ordered
 * compartments, each a datum or a common datum with its modifiers, in the
 * frame's own notation: "A", "B(M)", "C-D", "A-B(L,F)". Upstream's
 * PMI_DatumReferenceFrame.
 */
class PMIExport DatumReferenceFrame: public Annotation
{
    PROPERTY_HEADER_WITH_OVERRIDE(PMI::DatumReferenceFrame);

public:
    DatumReferenceFrame();

    App::PropertyStringList Compartments;

    App::DocumentObjectExecReturn* execute() override;

    struct Compartment
    {
        /// One letter, or several for a common datum (A-B)
        std::vector<std::string> datums;
        std::vector<std::string> modifiers;
    };
    /// Parse the compartments; false with \a why on the first error
    bool parse(std::vector<Compartment>& result, std::string& why) const;

    /// The datum modifiers allowed in a compartment
    static const std::vector<std::string>& modifierNames();
};

}  // namespace PMI

#endif  // PMI_DATUM_H
