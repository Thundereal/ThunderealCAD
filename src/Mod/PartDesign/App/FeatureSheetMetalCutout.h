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

#include <App/PropertyStandard.h>

#include "FeatureSketchBased.h"
#include "SheetMetalExtension.h"

namespace PartDesign
{

/** A closed sketch cut normal to the sheet on every wall and bend it
 * crosses (docs/SheetMetal.md sec 5.2, ruled 2026-11-02)
 *
 * The sketch is taken onto the skin facing it, along the cut's way, against
 * the sketch's normal as a pocket's (Reversed the other way), and cut
 * through the sheet from there: a wall's straight through, a bend's along
 * its radius, so the hole is what is cut in the blank. Through every wall
 * and bend in the sketch's way, UpToFirst the first it meets only.
 *
 * Wrap (ruled 2026-11-03): the sketch as if drawn on the flat instead, the
 * wall parallel to it and nearest it fixed, what lies on the flat cut
 * through it and folded back; the way and UpToFirst do not apply.
 */
class PartDesignExport SheetMetalCutout : public ProfileBased, public SheetMetalExtension
{
    PROPERTY_HEADER_WITH_EXTENSIONS(PartDesign::SheetMetalCutout);

public:
    SheetMetalCutout();

    /// Only the first wall or bend the sketch meets, not every one
    App::PropertyBool UpToFirst;
    /// The sketch as if drawn on the flat
    App::PropertyBool Wrap;
    /// Where a wrap's sketch is read: Folded, where the wall nearest it lies
    /// folded; Flat, in the frame of the feature's flat as the sheet lays it
    App::PropertyEnumeration WrapFrame;
    /// The flat settings a wrap in the flat's frame was cut by: cut again
    /// when they are others (mustExecute()), which an undo or a load is not
    App::PropertyString WrapSettings;

    App::DocumentObjectExecReturn* execute() override;
    short mustExecute() const override;
    bool skipRecompute() override;
    void onChanged(const App::Property* prop) override;
    const char* getViewProviderName() const override
    {
        return "PartDesignGui::ViewProviderSheetMetalCutout";
    }

private:
    /// A wrap read in the flat's frame, the flat's settings, which it does
    /// not link, others than it was cut by
    bool flatSettingsChanged() const;
};

}  // namespace PartDesign
