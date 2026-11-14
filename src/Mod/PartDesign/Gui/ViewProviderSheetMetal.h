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

#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <Gui/InventorBase.h>
#include <Mod/Part/Gui/ViewProvider.h>

class SoSwitch;

#include "ViewProviderAddSub.h"
#include "ViewProviderSketchBased.h"

namespace PartDesignGui
{

/// A base wall, its panel its values (docs/SheetMetal.md sec 5.1)
class PartDesignGuiExport ViewProviderSheetMetalBaseWall: public ViewProviderSketchBased
{
    PROPERTY_HEADER_WITH_OVERRIDE(PartDesignGui::ViewProviderSheetMetalBaseWall);

public:
    ViewProviderSheetMetalBaseWall();

protected:
    TaskDlgFeatureParameters* getEditDialog() override;
};

/// A lofted flange, its panel its values; it holds its two sketches
class PartDesignGuiExport ViewProviderSheetMetalLoftedFlange: public ViewProviderSketchBased
{
    PROPERTY_HEADER_WITH_OVERRIDE(PartDesignGui::ViewProviderSheetMetalLoftedFlange);

public:
    ViewProviderSheetMetalLoftedFlange();

protected:
    std::vector<App::DocumentObject*> _claimChildren() const override;
    TaskDlgFeatureParameters* getEditDialog() override;
};

/// A flange, its panel its edges and values
class PartDesignGuiExport ViewProviderSheetMetalFlange: public ViewProviderAddSub
{
    PROPERTY_HEADER_WITH_OVERRIDE(PartDesignGui::ViewProviderSheetMetalFlange);

public:
    ViewProviderSheetMetalFlange();

protected:
    TaskDlgFeatureParameters* getEditDialog() override;
};

/// A contour flange, its panel its edges, sketch, gaps and relief
class PartDesignGuiExport ViewProviderSheetMetalContourFlange: public ViewProviderSheetMetalFlange
{
    PROPERTY_HEADER_WITH_OVERRIDE(PartDesignGui::ViewProviderSheetMetalContourFlange);

public:
    ViewProviderSheetMetalContourFlange();

protected:
    TaskDlgFeatureParameters* getEditDialog() override;
};

/// A join by bend, its panel its two edges and relief
class PartDesignGuiExport ViewProviderSheetMetalJoin: public ViewProviderAddSub
{
    PROPERTY_HEADER_WITH_OVERRIDE(PartDesignGui::ViewProviderSheetMetalJoin);

public:
    ViewProviderSheetMetalJoin();

protected:
    TaskDlgFeatureParameters* getEditDialog() override;
};

/// Bend cuts, their panel their bends and pattern
class PartDesignGuiExport ViewProviderSheetMetalBendCuts: public ViewProviderAddSub
{
    PROPERTY_HEADER_WITH_OVERRIDE(PartDesignGui::ViewProviderSheetMetalBendCuts);

public:
    ViewProviderSheetMetalBendCuts();

protected:
    TaskDlgFeatureParameters* getEditDialog() override;
};

/// An extend wall, its panel its faces and length
class PartDesignGuiExport ViewProviderSheetMetalExtendWall: public ViewProviderAddSub
{
    PROPERTY_HEADER_WITH_OVERRIDE(PartDesignGui::ViewProviderSheetMetalExtendWall);

public:
    ViewProviderSheetMetalExtendWall();

protected:
    TaskDlgFeatureParameters* getEditDialog() override;
};

/// A hem, its panel its edges and values
class PartDesignGuiExport ViewProviderSheetMetalHem: public ViewProviderAddSub
{
    PROPERTY_HEADER_WITH_OVERRIDE(PartDesignGui::ViewProviderSheetMetalHem);

public:
    ViewProviderSheetMetalHem();

protected:
    TaskDlgFeatureParameters* getEditDialog() override;
};

/// A corner closure, its panel its two faces and its type
class PartDesignGuiExport ViewProviderSheetMetalCornerClosure: public ViewProviderAddSub
{
    PROPERTY_HEADER_WITH_OVERRIDE(PartDesignGui::ViewProviderSheetMetalCornerClosure);

public:
    ViewProviderSheetMetalCornerClosure();

protected:
    TaskDlgFeatureParameters* getEditDialog() override;
};

/// A rip, its panel the faces of the bends and walls it opens
class PartDesignGuiExport ViewProviderSheetMetalRip: public ViewProviderAddSub
{
    PROPERTY_HEADER_WITH_OVERRIDE(PartDesignGui::ViewProviderSheetMetalRip);

public:
    ViewProviderSheetMetalRip();

protected:
    TaskDlgFeatureParameters* getEditDialog() override;
};

/// A cutout, its panel its sketch, its way and how far
class PartDesignGuiExport ViewProviderSheetMetalCutout: public ViewProviderSketchBased
{
    PROPERTY_HEADER_WITH_OVERRIDE(PartDesignGui::ViewProviderSheetMetalCutout);

public:
    ViewProviderSheetMetalCutout();

protected:
    TaskDlgFeatureParameters* getEditDialog() override;
};

/// A fold, its panel its sketch, its values and the face on the side that moves
class PartDesignGuiExport ViewProviderSheetMetalFold: public ViewProviderSketchBased
{
    PROPERTY_HEADER_WITH_OVERRIDE(PartDesignGui::ViewProviderSheetMetalFold);

public:
    ViewProviderSheetMetalFold();

protected:
    TaskDlgFeatureParameters* getEditDialog() override;
};

/// A jog, its panel its sketch, its values and the face on the side that moves
class PartDesignGuiExport ViewProviderSheetMetalJog: public ViewProviderSketchBased
{
    PROPERTY_HEADER_WITH_OVERRIDE(PartDesignGui::ViewProviderSheetMetalJog);

public:
    ViewProviderSheetMetalJog();

protected:
    TaskDlgFeatureParameters* getEditDialog() override;
};

/// A tab, its panel its sketch
class PartDesignGuiExport ViewProviderSheetMetalTab: public ViewProviderSketchBased
{
    PROPERTY_HEADER_WITH_OVERRIDE(PartDesignGui::ViewProviderSheetMetalTab);

public:
    ViewProviderSheetMetalTab();

protected:
    TaskDlgFeatureParameters* getEditDialog() override;
};

/// Forms pressed into a wall, its panel the face, the kind, the points'
/// sketch, the angle and the way, and a dimple's sizes
class PartDesignGuiExport ViewProviderSheetMetalForm: public ViewProvider
{
    PROPERTY_HEADER_WITH_OVERRIDE(PartDesignGui::ViewProviderSheetMetalForm);

public:
    ViewProviderSheetMetalForm();

protected:
    TaskDlgFeatureParameters* getEditDialog() override;
};

/// Locating tabs on a wall's end, its panel the face, the count and the sizes
class PartDesignGuiExport ViewProviderSheetMetalLocatingTabs: public ViewProvider
{
    PROPERTY_HEADER_WITH_OVERRIDE(PartDesignGui::ViewProviderSheetMetalLocatingTabs);

public:
    ViewProviderSheetMetalLocatingTabs();

protected:
    TaskDlgFeatureParameters* getEditDialog() override;
};

/// Slots for another part's tabs, its panel the clearance
class PartDesignGuiExport ViewProviderSheetMetalSlots: public ViewProvider
{
    PROPERTY_HEADER_WITH_OVERRIDE(PartDesignGui::ViewProviderSheetMetalSlots);

public:
    ViewProviderSheetMetalSlots();

protected:
    TaskDlgFeatureParameters* getEditDialog() override;
};

/// A part converted, its panel its fixed face, open faces, rip edges and side
class PartDesignGuiExport ViewProviderSheetMetalConvert: public ViewProvider
{
    PROPERTY_HEADER_WITH_OVERRIDE(PartDesignGui::ViewProviderSheetMetalConvert);

public:
    ViewProviderSheetMetalConvert();

protected:
    TaskDlgFeatureParameters* getEditDialog() override;
};

/** Cross breaks on walls, its panel the faces, the angle and the way; while
 * the panel is open it draws each wall's diagonals on the face the break
 * rises from (PartDesign::crossBreakLines()), dash-dotted. Shown, the walls
 * wear the break as a finish, as every feature after it does
 * (ViewProvider::crossBreakFinishes())
 */
class PartDesignGuiExport ViewProviderSheetMetalCrossBreak: public ViewProvider
{
    PROPERTY_HEADER_WITH_OVERRIDE(PartDesignGui::ViewProviderSheetMetalCrossBreak);

public:
    ViewProviderSheetMetalCrossBreak();
    ~ViewProviderSheetMetalCrossBreak() override;

    void attach(App::DocumentObject* obj) override;
    void updateData(const App::Property* prop) override;
    void beforeDelete() override;
    void reattach(App::DocumentObject* obj) override;

protected:
    bool setEdit(int ModNum) override;
    void unsetEdit(int ModNum) override;
    TaskDlgFeatureParameters* getEditDialog() override;

private:
    /// The diagonals drawn as the object's shape has them now
    void refreshLines();

    std::unique_ptr<PartGui::ViewProviderPart> lines;
    /// The diagonals' view, drawn while the panel is open, not picked
    Gui::CoinPtr<SoSwitch> pcLines;
};

/// A sheet metal rule, its panel its card and values; a double-click opens it
class PartDesignGuiExport ViewProviderSheetMetalRule: public PartGui::ViewProviderPart
{
    PROPERTY_HEADER_WITH_OVERRIDE(PartDesignGui::ViewProviderSheetMetalRule);

public:
    ViewProviderSheetMetalRule();
    bool doubleClicked() override;

protected:
    bool setEdit(int ModNum) override;
    void unsetEdit(int ModNum) override;
};

/** A sheet's flat object (docs/SheetMetal.md sec 12, ruled 2026-11-04): its
 * solid's tip laid flat. Shown out of date, it has the flat laid by a
 * recompute of it alone.
 *
 * It draws the blank, and each kind of marking -- the bend lines up, those
 * down, the tangent lines, the grain arrow -- in a view of its own, with its
 * own colour, line style and width, shown or not; picked and selected, a
 * marking is the element of the object's shape it is (Edge40).
 */
class PartDesignGuiExport ViewProviderSheetMetalFlatPattern: public PartGui::ViewProviderPart
{
    PROPERTY_HEADER_WITH_OVERRIDE(PartDesignGui::ViewProviderSheetMetalFlatPattern);

public:
    ViewProviderSheetMetalFlatPattern();
    ~ViewProviderSheetMetalFlatPattern() override;

    /** @name How each kind of marking is drawn */
    //@{
    App::PropertyBool ShowBendUp;
    App::PropertyColor BendUpColor;
    App::PropertyEnumeration BendUpStyle;
    App::PropertyFloatConstraint BendUpWidth;
    App::PropertyBool ShowBendDown;
    App::PropertyColor BendDownColor;
    App::PropertyEnumeration BendDownStyle;
    App::PropertyFloatConstraint BendDownWidth;
    App::PropertyBool ShowTangentLines;
    App::PropertyColor TangentLineColor;
    App::PropertyEnumeration TangentLineStyle;
    App::PropertyFloatConstraint TangentLineWidth;
    App::PropertyBool ShowGrain;
    App::PropertyColor GrainColor;
    App::PropertyEnumeration GrainStyle;
    App::PropertyFloatConstraint GrainWidth;
    App::PropertyBool ShowMarkings;
    App::PropertyColor MarkingColor;
    App::PropertyEnumeration MarkingStyle;
    App::PropertyFloatConstraint MarkingWidth;
    App::PropertyBool ShowCrossBreaks;
    App::PropertyColor CrossBreakColor;
    App::PropertyEnumeration CrossBreakStyle;
    App::PropertyFloatConstraint CrossBreakWidth;
    App::PropertyBool ShowForms;
    App::PropertyColor FormColor;
    App::PropertyEnumeration FormStyle;
    App::PropertyFloatConstraint FormWidth;
    //@}

    void attach(App::DocumentObject* obj) override;
    void show() override;
    void updateData(const App::Property* prop) override;
    void beforeDelete() override;
    void reattach(App::DocumentObject* obj) override;
    bool getElementPicked(const SoPickedPoint* pp, std::string& subname) const override;
    bool getDetailPath(const char* subname, SoFullPath* path, bool append,
                       SoDetail*& det) const override;
    /// A drawing's look of a marking: its kind's style, colour, and shown
    /// or not; a bend line's label its note, "UP 90deg R1", a cross break's
    /// first diagonal's "CROSS BREAK UP 3deg"; the blank's edges are left to
    /// the drawing
    bool getEdgeInfo(const char* mappedName, EdgeInfo& info) const override;

    /** Draw \a shape, a flat as layFlat() makes it, each bend Up or Down by
     * \a directions: the object's own, or another's (a feature's, drawn by
     * its panel)
     */
    void showFlat(const Part::TopoShape& shape,
                  const std::map<std::string, std::string>& directions);
    /// The look of \a other: its lines' colour and width, and each kind of
    /// marking's style
    void copyLook(const ViewProviderSheetMetalFlatPattern& other);

protected:
    void onChanged(const App::Property* prop) override;

private:
    /// The object's shape drawn: the blank for this view, each kind's edges
    /// for its own
    void refreshMarkings();
    /// A kind's look on its view
    void applyLook(int kind);

    struct Kind
    {
        App::PropertyBool* show;
        App::PropertyColor* color;
        App::PropertyEnumeration* style;
        App::PropertyFloatConstraint* width;
        std::unique_ptr<PartGui::ViewProviderPart> view;
        /// Each of its edges' index in the object's shape, by its own
        std::vector<int> edges;
    };
    std::array<Kind, 7> kinds;
    /// The markings' views, drawn while this view is shown
    Gui::CoinPtr<SoSwitch> pcMarkings;
    /// How many edges the blank has: the shape's first ones
    int blankEdges = 0;
};

}  // namespace PartDesignGui
