// SPDX-License-Identifier: LGPL-2.1-or-later
/****************************************************************************
 *                                                                          *
 *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
 *                                                                          *
 *   This file is part of FreeCAD.                                          *
 *                                                                          *
 *   FreeCAD is free software: you can redistribute it and/or modify it     *
 *   under the terms of the GNU Lesser General Public License as            *
 *   published by the Free Software Foundation, either version 2.1 of the   *
 *   License, or (at your option) any later version.                        *
 *                                                                          *
 *   FreeCAD is distributed in the hope that it will be useful, but         *
 *   WITHOUT ANY WARRANTY; without even the implied warranty of             *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU       *
 *   Lesser General Public License for more details.                        *
 *                                                                          *
 *   You should have received a copy of the GNU Lesser General Public       *
 *   License along with FreeCAD. If not, see                                *
 *   <https://www.gnu.org/licenses/>.                                       *
 *                                                                          *
 ***************************************************************************/

// The study objects as C++ objects (docs/AssemblyObjectModel.md sec 7.2):
// the data upstream's Python Simulation and Motion carry, typed, under the
// same names, so that their Python proxies, view providers and task panel
// attach to SimulationPython and MotionPython unchanged.

#pragma once

#include <App/DocumentObject.h>
#include <App/FeaturePython.h>
#include <App/GroupExtension.h>
#include <App/PropertyLinks.h>
#include <App/PropertyStandard.h>
#include <App/PropertyUnits.h>

#include <Mod/Assembly/AssemblyGlobal.h>

namespace Assembly
{

/// A kinematic study: owns its motions in Group
class AssemblyExport Simulation: public App::DocumentObject, public App::GroupExtension
{
    PROPERTY_HEADER_WITH_OVERRIDE(Assembly::Simulation);

public:
    Simulation();

    // Upstream's names, letter prefixes and all: they order the property
    // editor, and files and scripts use them
    App::PropertyTime aTimeStart;
    App::PropertyTime bTimeEnd;
    App::PropertyTime cTimeStepOutput;
    App::PropertyFloat fGlobalErrorTolerance;
    App::PropertyInteger jFramesPerSecond;

    const char* getViewProviderName() const override
    {
        return "Gui::ViewProviderDocumentObjectGroup";
    }
};

using SimulationPython = App::FeaturePythonT<Simulation>;


/// A motion driving one joint of the assembly, a child of a Simulation
class AssemblyExport Motion: public App::DocumentObject
{
    PROPERTY_HEADER_WITH_OVERRIDE(Assembly::Motion);

public:
    Motion();

    /// The joint the motion drives (upstream: a hidden link with subnames)
    App::PropertyLink Joint;
    /// The same enumeration, in the same order, as
    /// CommandCreateSimulation.MotionTypes
    App::PropertyEnumeration MotionType;
    /// The motion as a formula of time, as '1.0*time'
    App::PropertyString Formula;

    static const char* MotionTypeEnums[];

    const char* getViewProviderName() const override
    {
        return "Gui::ViewProviderDocumentObject";
    }
};

using MotionPython = App::FeaturePythonT<Motion>;

}  // namespace Assembly
