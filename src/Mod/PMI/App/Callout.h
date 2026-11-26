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

#ifndef PMI_CALLOUT_H
#define PMI_CALLOUT_H

#include <string>
#include <vector>

#include <gp_Ax3.hxx>
#include <gp_Pnt.hxx>
#include <TopoDS_Shape.hxx>

#include <App/PropertyGeo.h>
#include <App/PropertyLinks.h>
#include <App/PropertyStandard.h>
#include <App/PropertyUnits.h>
#include <Mod/Part/App/PropertyTopoShape.h>

#include "Annotation.h"

namespace PMI
{

class Drawing;
class TolerancedFeature;
class View;

/** What an annotation looks like (docs/GDandT.md sec 6): drawn from its
 * semantics on every recompute, never the other way round.
 *
 * The drawing lies in Plane -- x the reading direction, y up, z toward the
 * reader -- with a leader to the feature. Plane is placed from the feature
 * (AutoPlace) until the user moves it. Presentation is the result: strokes as
 * edges and fills as faces, in the part's coordinates, which the view draws,
 * STEP writes tessellated and a drawing reads. EquivalentText is the CAx-IF
 * "equivalent unicode string" of the annotation, for checking.
 */
class PMIExport Callout: public Annotation
{
    PROPERTY_HEADER_WITH_OVERRIDE(PMI::Callout);

public:
    Callout();

    const char* getViewProviderName() const override
    {
        return "PMIGui::ViewProviderCallout";
    }

    App::PropertyPlacement Plane;
    App::PropertyLength TextHeight;
    App::PropertyBool AutoPlace;
    App::PropertyBool ShowLeader;
    /// Another callout this one is stacked under (docs/GDandT.md sec 6.4):
    /// its plane, below its drawing, on its leader
    App::PropertyLink AttachedTo;
    /// The PMI views showing it (sec 6.3); the first whose AnnotationPlane is
    /// set gives it its plane
    App::PropertyLinkList Views;
    Part::PropertyPartShape Presentation;
    /// Where each part of Presentation the view turns to read upright turns
    /// about, in Plane's coordinates: Presentation's sub-shapes after the
    /// first, which is what stays -- leaders, extension lines, marks on the
    /// part (docs/GDandT.md sec 6.5). Empty: nothing turns (a file's graphic)
    App::PropertyVectorList UprightAt;
    App::PropertyString EquivalentText;
    /// The graphic a file drew for it (docs/GDandT.md sec 5.1 step 3), in
    /// the set's coordinates: what is drawn while UseImported is set. None
    /// for a placeholder, where the file leaves the drawing to its reader
    Part::PropertyPartShape ImportedGraphic;
    /// The kind of graphic the file named it ('label', 'linear dimension':
    /// AP242's recommended practice, 8.4), written again with it
    App::PropertyString ImportedGraphicType;
    /// The name the file gave its graphic or placeholder ('Linear Size.13'),
    /// written again with it: many PMI may share one (STC-07's 31 radii)
    App::PropertyString ImportedGraphicName;
    /// The annotation placeholder the file gave it (AP242 ed2), empty for
    /// none: its kind ('linear dimension', 'general tolerance', 'note': the
    /// name of its geometric set) and its name, which need not be the
    /// graphic's; a third 'shared' when it is another PMI's placeholder too,
    /// one the file links both to (STC-06's hole callout, its diameter's and
    /// its depth's); 'hidden' after them when the file hid it (STC-06's 19).
    /// Written again with one, where its text is
    App::PropertyStringList ImportedPlaceholder;
    /// The file's graphic was curves (STEP: an annotation curve occurrence of
    /// polylines and circles, NIST CTC-05's), not tessellated: written so again
    /// while drawn as the file draws it
    App::PropertyBool ImportedAsCurves;
    /// The file hid it by its callout (an INVISIBILITY of the draughting
    /// callout, NIST FTC-09's 44), not its graphic: written so again, hidden
    App::PropertyBool ImportedHiddenCallout;
    /// The callout whose graphic from the file draws this one too, while
    /// both are as the file has them: a composite frame's upper segment, the
    /// PMI a graphic presents with it, a hole's size for its depth
    App::PropertyLink ImportedWith;
    /// Draw the file's graphic, or ours where its placeholder is, written
    /// named as the file named it; off once the callout is edited, and laid
    /// out from its semantics from then on
    App::PropertyBool UseImported;

    enum LeaderStyle
    {
        NoLeader,
        /// Ending in an arrowhead: frames, dimensions, notes
        ArrowLeader,
        /// Ending in a filled triangle on the feature: a datum (ISO 5459)
        TriangleLeader,
        /// Ending at the target, no terminator: a datum target
        PlainLeader,
    };

    /// The toleranced feature the callout is drawn to, or null
    virtual TolerancedFeature* calloutFeature() const = 0;
    /// Draw the callout in its plane's coordinates at text height \a h
    virtual void layout(Drawing& d, double h) const = 0;
    /// The CAx-IF equivalent string
    virtual std::u32string equivalentText() const = 0;
    virtual LeaderStyle leaderStyle() const
    {
        return ArrowLeader;
    }
    /// The point a leader goes to; by default targetPoint() on the feature
    virtual bool calloutTarget(gp_Pnt& target) const;
    /// What is drawn on the part rather than in the plane, in the set's
    /// coordinates (a datum target's X): by default nothing
    virtual TopoDS_Shape mark(double h) const;

    /// Lay the callout out again: Plane (if AutoPlace), Presentation,
    /// EquivalentText
    void updatePresentation();
    /// Whether a change of \a prop is the user editing the callout: what
    /// it says, where it is, its size
    bool isEdit(const App::Property* prop) const;

    /// The plane the callout takes while AutoPlace is on, before stacking;
    /// by default defaultPlane() on its feature. \a target is where a
    /// leader goes.
    virtual bool autoPlane(double h, gp_Ax3& plane, gp_Pnt& target) const;

    /// The callout this one is stacked under, or null (also for a loop)
    Callout* attachedParent() const;
    /// The callout this one is drawn as one with, under it, when it is
    /// stacked under none, and how far below that one's bottom its own top
    /// is (\a below, at text height \a h): a composite frame's lower segment,
    /// flush under its upper (docs/GDandT.md sec 4.5). None by default
    virtual Callout* composedUnder(double h, double& below) const
    {
        (void)h;
        (void)below;
        return nullptr;
    }
    /// The rows of the frame it is one of, drawn as one (a composite frame's
    /// lower segments, counted from its upper or from one of them); 0 for none
    virtual int composedRows() const
    {
        return 0;
    }
    /// Whether the file's graphic of another callout (ImportedWith) draws
    /// this one too: nothing of its own is drawn then
    bool drawnByAnother() const;
    /// Whether, edited, the callout whose graphic drew it too (ImportedWith)
    /// is drawn from what it says as well: the two are one drawing (a
    /// composite frame). Else that graphic stays -- one of the 32 radii of a
    /// "32X R" edited does not make 32 callouts of it
    virtual bool editTakesHost() const
    {
        return false;
    }

    /// Whether \a view shows it
    bool inView(const View* view) const;
    /// The view whose plane it lies in, or null
    View* planeView() const;

    /// The plane a callout on \a feature takes: the face's own if planar,
    /// else the standard view looking most squarely at it; \a target is the
    /// point on the feature the leader goes to
    static bool defaultPlane(const TolerancedFeature* feature, double h, gp_Ax3& plane,
                             gp_Pnt& target);
    /// The point on the feature a leader goes to
    static bool targetPoint(const TolerancedFeature* feature, gp_Pnt& target);

    /// A number as a drawing writes it: \a decimals places, or (-1) as few
    /// as it needs
    static std::u32string number(double value, int decimals = -1);

protected:
    void onChanged(const App::Property* prop) override;
    void onDocumentRestored() override;
    /// How many callouts before this one in the set land near \a mine on
    /// a parallel plane
    int autoSlot(const gp_Ax3& mine, double h) const;
    /// Touches the auto-placed callouts after this one in its set: their
    /// slots are taken by where this lands, which they do not depend on
    void touchLaterSlots() const;

    /// Set while the callout changes its own properties (its plane as it is
    /// placed, a limits dimension's deviations about a nominal measured
    /// again): no edit by the user
    bool _placing = false;
private:
    /// Where it lands placed on its own (calloutTarget, then autoPlane),
    /// as another's autoSlot asks: kept through one recompute while none of
    /// its own properties changes and what it is placed from is the same
    /// shapes; else each placement asked of every one before it in the set
    /// (a set's recompute quadratic in callouts: 400 took 1.7 s)
    bool landing(double h, gp_Ax3& plane, gp_Pnt& target) const;
    /// The shapes a placement is made from: of the objects it links to, and
    /// of the parts their features refer to
    std::vector<TopoDS_Shape> landingInputs() const;
    struct Landing
    {
        bool valid = false;
        bool ok = false;
        double h = 0;
        unsigned long recompute = 0;
        std::vector<TopoDS_Shape> inputs;
        gp_Ax3 plane;
        gp_Pnt target;
    };
    mutable Landing _landing;
};

}  // namespace PMI

#endif  // PMI_CALLOUT_H
