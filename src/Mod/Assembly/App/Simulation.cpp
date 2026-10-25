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

#include "Simulation.h"

using namespace Assembly;


PROPERTY_SOURCE_WITH_EXTENSIONS(Assembly::Simulation, App::DocumentObject)

Simulation::Simulation()
{
    GroupExtension::initExtension(this);

    // Upstream's defaults (CommandCreateSimulation.Simulation)
    ADD_PROPERTY_TYPE(aTimeStart, (0.0), "Simulation", App::Prop_None, "Simulation start time.");
    ADD_PROPERTY_TYPE(bTimeEnd, (1.0), "Simulation", App::Prop_None, "Simulation end time.");
    ADD_PROPERTY_TYPE(cTimeStepOutput, (1.0e-2), "Simulation", App::Prop_None,
                      "Simulation time step for output.");
    ADD_PROPERTY_TYPE(fGlobalErrorTolerance, (1.0e-6), "Simulation", App::Prop_None,
                      "Integration global error tolerance.");
    ADD_PROPERTY_TYPE(jFramesPerSecond, (30), "Simulation", App::Prop_None, "Frames Per Second.");
}


PROPERTY_SOURCE(Assembly::Motion, App::DocumentObject)

const char* Motion::MotionTypeEnums[] = {"Angular", "Linear", nullptr};

Motion::Motion()
{
    ADD_PROPERTY_TYPE(Joint, (nullptr), "Motion", App::Prop_None,
                      "The joint that is moved by the motion");
    ADD_PROPERTY_TYPE(MotionType, (0L), "Motion", App::Prop_None, "The type of the motion");
    MotionType.setEnums(MotionTypeEnums);
    ADD_PROPERTY_TYPE(Formula, (""), "Motion", App::Prop_None,
                      "This is the formula of the motion. For example '1.0*time'.");
}


// Python features --------------------------------------------------------

namespace App
{
/// @cond DOXERR
PROPERTY_SOURCE_TEMPLATE(Assembly::SimulationPython, Assembly::Simulation)
template<>
const char* Assembly::SimulationPython::getViewProviderName() const
{
    return "Gui::ViewProviderPythonFeature";
}
template class AssemblyExport FeaturePythonT<Assembly::Simulation>;

PROPERTY_SOURCE_TEMPLATE(Assembly::MotionPython, Assembly::Motion)
template<>
const char* Assembly::MotionPython::getViewProviderName() const
{
    return "Gui::ViewProviderPythonFeature";
}
template class AssemblyExport FeaturePythonT<Assembly::Motion>;
/// @endcond
}  // namespace App
