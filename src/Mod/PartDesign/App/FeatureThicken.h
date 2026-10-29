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
#include <App/PropertyUnits.h>

#include "FeatureAddSub.h"

namespace PartDesign
{

/** Faces made into a solid of a thickness (docs/Surface.md): a surface's --
 * a network in the body, or one bound in -- or a solid's own. The solid is
 * the faces and their offset, walled along their free edges (OCCT's
 * thickening), and is added to the body, or taken from it or kept where it
 * crosses it (AddSubType).
 */
class PartDesignExport Thicken: public PartDesign::FeatureAddSub
{
    PROPERTY_HEADER_WITH_OVERRIDE(PartDesign::Thicken);

public:
    Thicken();

    /// Faces, or objects whole (their faces); several are sewn into one skin
    App::PropertyLinkSubList Faces;
    App::PropertyLength Value;
    /// The other side of the faces: against their normals
    App::PropertyBool Reversed;
    /// Half the thickness each side
    App::PropertyBool Midplane;
    App::PropertyEnumeration Join;
    /// The OCCT fork's offset fix level the object recomputes at
    /// (Part::OCCTOffsetFixLevel): the latest for an object made now, 0 for
    /// one a file from before the level brings, which so recomputes as it did
    App::PropertyInteger _OffsetFixLevel;

    App::DocumentObjectExecReturn* execute() override;
    short mustExecute() const override;
    void setupObject() override;
    const char* getViewProviderName() const override
    {
        return "PartDesignGui::ViewProviderThicken";
    }
};

}  // namespace PartDesign
