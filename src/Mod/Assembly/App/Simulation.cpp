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

#include <cstring>

#include <Base/Reader.h>

#include "Simulation.h"

using namespace Assembly;


PROPERTY_SOURCE_WITH_EXTENSIONS(Assembly::Simulation, App::DocumentObject)

Simulation::Simulation()
{
    GroupExtension::initExtension(this);

    // Upstream's defaults (CommandCreateSimulation.Simulation)
    ADD_PROPERTY_TYPE(TimeStart, (0.0), "Simulation", App::Prop_None, "Simulation start time.");
    ADD_PROPERTY_TYPE(TimeEnd, (1.0), "Simulation", App::Prop_None, "Simulation end time.");
    ADD_PROPERTY_TYPE(TimeStepOutput, (1.0e-2), "Simulation", App::Prop_None,
                      "Simulation time step for output.");
    ADD_PROPERTY_TYPE(GlobalErrorTolerance, (1.0e-6), "Simulation", App::Prop_None,
                      "Integration global error tolerance.");
    ADD_PROPERTY_TYPE(FramesPerSecond, (30), "Simulation", App::Prop_None, "Frames Per Second.");
    // Not upstream: dynamics (docs/AssemblyDynamics.md); a file without
    // them is a kinematic study, as before
    ADD_PROPERTY_TYPE(AnalysisType,
                      (0L),
                      "Simulation",
                      App::Prop_None,
                      "Kinematic: the motions drive the joints. Dynamic: the parts move "
                      "under gravity by their masses. Static: where they settle, at each "
                      "output time.");
    AnalysisType.setEnums(AnalysisTypeEnums);
    ADD_PROPERTY_TYPE(Gravity,
                      (Base::Vector3d(0.0, 0.0, -9806.65)),
                      "Simulation",
                      App::Prop_None,
                      "Dynamic: gravity, in the document's coordinates, mm/s^2");
    ADD_PROPERTY_TYPE(GravityEnabled,
                      (true),
                      "Simulation",
                      App::Prop_None,
                      "Dynamic: whether gravity acts");
    ADD_PROPERTY_TYPE(Elements,
                      (nullptr),
                      "Simulation",
                      App::Prop_None,
                      "Dynamic: the Physics group's force elements this study uses");
    ADD_PROPERTY_TYPE(Restitution,
                      (0.0),
                      "Simulation",
                      App::Prop_None,
                      "Dynamic: the coefficient of restitution at the joints' limits, which "
                      "a dynamic run takes as rigid stops: 0 stays at the stop, 1 bounces "
                      "back as fast as it came");
    static const App::PropertyFloatConstraint::Constraints unit = {0.0, 1.0, 0.05};
    Restitution.setConstraints(&unit);
    ADD_PROPERTY_TYPE(Reactions,
                      (false),
                      "Simulation",
                      App::Prop_None,
                      "Kinematic: also the reactions and the motors' efforts the motions "
                      "take, from the masses, gravity and the Elements (inverse dynamics)");
    ADD_PROPERTY_TYPE(ResultsStale,
                      (false),
                      "Simulation",
                      App::PropertyType(App::Prop_ReadOnly | App::Prop_Output | App::Prop_Transient
                                        | App::Prop_NoRecompute),
                      "The run kept for this study is of an older mechanism: the assembly, its "
                      "motions or the study changed since. Run it again.");
}

const char* Simulation::AnalysisTypeEnums[] = {"Kinematic", "Dynamic", "Static", nullptr};

const std::map<std::string, std::string>& Simulation::upstreamNames()
{
    static const std::map<std::string, std::string> names {
        {"aTimeStart", "TimeStart"},
        {"bTimeEnd", "TimeEnd"},
        {"cTimeStepOutput", "TimeStepOutput"},
        {"fGlobalErrorTolerance", "GlobalErrorTolerance"},
        {"jFramesPerSecond", "FramesPerSecond"},
    };
    return names;
}

void Simulation::handleChangedPropertyName(Base::XMLReader& reader,
                                           const char* typeName,
                                           const char* propName)
{
    auto it = upstreamNames().find(propName);
    if (it != upstreamNames().end()) {
        auto* prop = getPropertyByName(it->second.c_str());
        if (prop && strcmp(prop->getTypeId().getName(), typeName) == 0) {
            prop->Restore(reader);
            return;
        }
    }
    DocumentObject::handleChangedPropertyName(reader, typeName, propName);
}


PROPERTY_SOURCE(Assembly::Motion, App::DocumentObject)

const char* Motion::MotionTypeEnums[] = {"Angular", "Linear", nullptr};
const char* Motion::ControlEnums[] = {"Position", "Speed", "Effort", nullptr};

Motion::Motion()
{
    ADD_PROPERTY_TYPE(Joint, (nullptr), "Motion", App::Prop_None,
                      "The joint that is moved by the motion");
    ADD_PROPERTY_TYPE(MotionType, (0L), "Motion", App::Prop_None, "The type of the motion");
    MotionType.setEnums(MotionTypeEnums);
    ADD_PROPERTY_TYPE(Formula, (""), "Motion", App::Prop_None,
                      "This is the formula of the motion. For example '1.0*time'.");
    ADD_PROPERTY_TYPE(Control,
                      (0L),
                      "Motion",
                      App::Prop_None,
                      "What the formula gives. Position: the angle (rad) or distance (mm). "
                      "Speed: their rate (rad/s, mm/s).\nEffort: the torque (N m) or force "
                      "(N). Speed and Effort need a Dynamic study.");
    Control.setEnums(ControlEnums);
}


PROPERTY_SOURCE(Assembly::InitialCondition, App::DocumentObject)

const char* InitialCondition::ModeEnums[] = {"Joint", "Component", nullptr};

InitialCondition::InitialCondition()
{
    ADD_PROPERTY_TYPE(Mode,
                      (0L),
                      "Initial velocity",
                      App::Prop_None,
                      "Joint: a joint's rate. Component: a component's velocity");
    Mode.setEnums(ModeEnums);
    ADD_PROPERTY_TYPE(Joint,
                      (nullptr),
                      "Initial velocity",
                      App::Prop_None,
                      "Joint: the Revolute, Cylindrical or Slider joint given the rate");
    ADD_PROPERTY_TYPE(MotionType,
                      (0L),
                      "Initial velocity",
                      App::Prop_None,
                      "Joint: Angular, its turn about its axis; Linear, its slide along it");
    MotionType.setEnums(Motion::MotionTypeEnums);
    ADD_PROPERTY_TYPE(Rate,
                      (0.0),
                      "Initial velocity",
                      App::Prop_None,
                      "Joint: the rate, the way a Speed motion of the joint turns or slides it");
    ADD_PROPERTY_TYPE(Component,
                      (nullptr),
                      "Initial velocity",
                      App::Prop_None,
                      "Component: the component given the velocity");
    ADD_PROPERTY_TYPE(LinearVelocity,
                      (Base::Vector3d()),
                      "Initial velocity",
                      App::Prop_None,
                      "Component: its origin's velocity, in the assembly's axes, in mm/s");
    ADD_PROPERTY_TYPE(AngularVelocity,
                      (Base::Vector3d()),
                      "Initial velocity",
                      App::Prop_None,
                      "Component: its angular velocity, in the assembly's axes, in deg/s");
    updateUnit();
}

void InitialCondition::updateUnit()
{
    // An angle over a time (the angle's internal unit is the degree), or a
    // length over a time
    Rate.setUnit(MotionType.getValue() == 0 ? Base::Unit(0, 0, -1, 0, 0, 0, 0, 1)
                                            : Base::Unit(1, 0, -1));
}

void InitialCondition::onChanged(const App::Property* prop)
{
    if (prop == &MotionType) {
        updateUnit();
    }
    DocumentObject::onChanged(prop);
}

void InitialCondition::onDocumentRestored()
{
    updateUnit();
    DocumentObject::onDocumentRestored();
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

PROPERTY_SOURCE_TEMPLATE(Assembly::InitialConditionPython, Assembly::InitialCondition)
template<>
const char* Assembly::InitialConditionPython::getViewProviderName() const
{
    return "Gui::ViewProviderPythonFeature";
}
template class AssemblyExport FeaturePythonT<Assembly::InitialCondition>;
/// @endcond
}  // namespace App
