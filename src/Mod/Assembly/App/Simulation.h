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
// the data upstream's Python Simulation and Motion carry, typed, so that
// their Python proxies, view providers and task panel attach to
// SimulationPython and MotionPython. The motion's names are upstream's; the
// simulation's lose upstream's letter prefixes (upstreamNames()).

#pragma once

#include <map>
#include <string>

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

    // Upstream's aTimeStart, bTimeEnd, ...: the letter prefixes order them
    // in upstream's property editor, which sorts by name. This fork's shows
    // them in the order declared here
    App::PropertyTime TimeStart;
    App::PropertyTime TimeEnd;
    App::PropertyTime TimeStepOutput;
    App::PropertyFloat GlobalErrorTolerance;
    App::PropertyInteger FramesPerSecond;
    /// Kinematic: the motions drive the joints; Dynamic: the bodies move
    /// under gravity by their masses (docs/AssemblyDynamics.md, phase D1),
    /// on a backend that runs dynamics
    App::PropertyEnumeration AnalysisType;
    /// Dynamic: gravity, in the document's coordinates, mm/s^2
    App::PropertyVector Gravity;
    App::PropertyBool GravityEnabled;
    /// Dynamic: the Physics group's force elements this study uses (springs
    /// and dampers); a mass override counts whatever this lists
    App::PropertyLinkList Elements;
    /// Dynamic: the coefficient of restitution at the joints' limits, which
    /// a dynamic run takes as rigid stops: 0 stays at the stop, 1 bounces
    /// back as fast as it came
    App::PropertyFloatConstraint Restitution;

    static const char* AnalysisTypeEnums[];

    /// Upstream's name of each property above, and the property's name
    static const std::map<std::string, std::string>& upstreamNames();

    const char* getViewProviderName() const override
    {
        return "Gui::ViewProviderDocumentObjectGroup";
    }

protected:
    /// Upstream's names restore into the properties above
    void handleChangedPropertyName(Base::XMLReader& reader,
                                   const char* typeName,
                                   const char* propName) override;
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
    /// Not upstream (docs/AssemblyDynamics.md sec 5.2): what the formula
    /// gives -- Position: the angle (rad) or the distance (mm); Speed: their
    /// rate (rad/s, mm/s); Effort: the torque (N m) or the force (N). Speed
    /// and Effort need a Dynamic study.
    App::PropertyEnumeration Control;

    static const char* MotionTypeEnums[];
    static const char* ControlEnums[];

    const char* getViewProviderName() const override
    {
        return "Gui::ViewProviderDocumentObject";
    }
};

using MotionPython = App::FeaturePythonT<Motion>;


/** An initial velocity of a dynamic study (docs/AssemblyDynamics.md sec
 * 5.2's InitialCondition), a child of a Simulation as a motion is: a
 * joint's rate, about or along its axis as a Speed motion of it turns it;
 * or a component's velocity, its origin's and its turn's, in the
 * assembly's axes. The run starts from the solved assembly with these
 * velocities, made consistent with the joints by the least change: what
 * a joint does not allow is taken out. A static or kinematic study does
 * not use them.
 */
class AssemblyExport InitialCondition: public App::DocumentObject
{
    PROPERTY_HEADER_WITH_OVERRIDE(Assembly::InitialCondition);

public:
    InitialCondition();

    /// Joint: a joint's rate; Component: a component's velocity
    App::PropertyEnumeration Mode;
    /// Joint: a Revolute, Cylindrical or Slider joint
    App::PropertyLink Joint;
    /// Joint: Angular, the turn about the axis; Linear, the slide along it
    App::PropertyEnumeration MotionType;
    /// Joint: an angular speed or a speed, as MotionType is
    App::PropertyQuantity Rate;
    /// Component: the component given the velocity
    App::PropertyLink Component;
    /// Component: its origin's velocity, in the assembly's axes, mm/s
    App::PropertyVector LinearVelocity;
    /// Component: its angular velocity, in the assembly's axes, deg/s
    App::PropertyVector AngularVelocity;

    static const char* ModeEnums[];

    void onChanged(const App::Property* prop) override;
    void onDocumentRestored() override;

    const char* getViewProviderName() const override
    {
        return "Gui::ViewProviderDocumentObject";
    }

    bool isJointRate() const
    {
        return Mode.getValue() == 0;
    }

private:
    void updateUnit();
};

using InitialConditionPython = App::FeaturePythonT<InitialCondition>;

}  // namespace Assembly
