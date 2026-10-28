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

#include <vector>

#include <App/PropertyStandard.h>
#include <Gui/ViewProviderExtensionPython.h>
#include <Mod/Part/PartGlobal.h>

class SoSwitch;
class SoCoordinate3;
class SoLineSet;
class TopoDS_Shape;
class gp_Pnt;

namespace PartGui
{

/** A curvature comb over a shape's edges and its faces' isocurves
 * (docs/Surface.md sec 7): at points along each, a tooth away from the
 * centre of curvature, as long as the curvature there, and the envelope
 * through the teeth's ends. An overlay of the view, for any Part object,
 * off until asked for; never part of the shape.
 */
class PartGuiExport ViewProviderCurvatureCombExtension: public Gui::ViewProviderExtension
{
    EXTENSION_PROPERTY_HEADER_WITH_OVERRIDE(PartGui::ViewProviderCurvatureCombExtension);

public:
    ViewProviderCurvatureCombExtension();
    ~ViewProviderCurvatureCombExtension() override = default;

    App::PropertyBool CurvatureComb;
    App::PropertyFloatConstraint CombScale;
    App::PropertyIntegerConstraint CombDensity;
    App::PropertyIntegerConstraint CombIsocurves;

    void extensionUpdateData(const App::Property*) override;
    void extensionSetupContextMenu(QMenu*, QObject*, const char*) override;

protected:
    void extensionOnChanged(const App::Property* p) override;

private:
    /// One comb: points along a curve and their teeth's ends
    struct Comb
    {
        std::vector<gp_Pnt> points;
        std::vector<gp_Pnt> teeth;  // unscaled: point + direction * curvature
    };
    void rebuild();
    void combsOf(const TopoDS_Shape& shape, std::vector<Comb>& combs) const;

    SoSwitch* pcComb {nullptr};
    SoCoordinate3* teethCoords {nullptr};
    SoLineSet* teethLines {nullptr};
    SoCoordinate3* envelopeCoords {nullptr};
    SoLineSet* envelopeLines {nullptr};
};

using ViewProviderCurvatureCombExtensionPython =
    Gui::ViewProviderExtensionPythonT<PartGui::ViewProviderCurvatureCombExtension>;

}  // namespace PartGui
