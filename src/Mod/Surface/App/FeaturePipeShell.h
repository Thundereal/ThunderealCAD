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

#include <App/PropertyGeo.h>
#include <App/PropertyLinks.h>
#include <App/PropertyStandard.h>
#include <Mod/Part/App/PartFeature.h>
#include <Mod/Surface/SurfaceGlobal.h>

namespace Surface
{

/// Profiles swept along a spine with every mode of OCCT's pipe shell
/// (docs/Surface.md sec 5.1): Part's Sweep exposes some of them
class SurfaceExport PipeShell: public Part::Feature
{
    PROPERTY_HEADER_WITH_OVERRIDE(Surface::PipeShell);

public:
    PipeShell();

    App::PropertyLinkSubList Spine;
    App::PropertyLinkSubList Profiles;
    App::PropertyEnumeration Mode;
    App::PropertyVector Direction;
    App::PropertyLinkSubList AuxiliarySpine;
    App::PropertyBool CurvilinearEquivalence;
    App::PropertyEnumeration AuxiliaryContact;
    App::PropertyLinkSubList Support;
    App::PropertyEnumeration Transition;
    App::PropertyBool WithContact;
    App::PropertyBool WithCorrection;
    App::PropertyBool Solid;

    App::DocumentObjectExecReturn* execute() override;
    const char* getViewProviderName() const override
    {
        return "SurfaceGui::ViewProviderPipeShell";
    }
};

}  // namespace Surface
