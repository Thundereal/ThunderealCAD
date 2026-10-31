// SPDX-License-Identifier: LGPL-2.1-or-later
/****************************************************************************
 *                                                                          *
 *   Copyright (c) 2023 Ondsel <development@ondsel.com>                     *
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


#include <App/Application.h>
#include <App/Document.h>
#include <Base/Console.h>
#include <Base/Interpreter.h>
#include <Base/PyObjectBase.h>

#include "AssemblyObject.h"
#include "AssemblyLink.h"
#include "BomObject.h"
#include "Groups.h"
#include "AssemblyGroup.h"
#include "Grounding.h"
#include "Joint.h"
#include "Physics.h"
#include "Simulation.h"
#include "Views.h"
#include "Solver/OndselBackend.h"
#include "Solver/ReferenceBackend.h"
#ifdef ASSEMBLY_WITH_CHRONO
#include "Solver/ChronoBackend.h"
#endif
#include "Solver/Registry.h"


namespace Assembly
{
extern PyObject* initModule();
}

/* Python entry */
PyMOD_INIT_FUNC(AssemblyApp)
{
    // load dependent module
    try {
        Base::Interpreter().runString("import Part");
        Base::Interpreter().runString("import Spreadsheet");
    }
    catch (const Base::Exception& e) {
        PyErr_SetString(PyExc_ImportError, e.what());
        PyMOD_Return(nullptr);
    }

    PyObject* mod = Assembly::initModule();
    Base::Console().log("Loading Assembly module... done\n");

    // Upstream's Python joints become C++ ones when a file is opened
    // (docs/AssemblyObjectModel.md sec 8): each document restored is noted,
    // and converted once the whole open is over -- the external documents
    // its parts link to are in then, and in the GUI the view providers.
    static std::vector<std::string> restored;
    App::GetApplication().signalFinishRestoreDocument.connect([](const App::Document& doc) {
        restored.emplace_back(doc.getName());
    });
    App::GetApplication().signalFinishOpenDocument.connect([]() {
        auto names = std::move(restored);
        restored.clear();
        for (const auto& name : names) {
            auto* doc = App::GetApplication().getDocument(name.c_str());
            if (!doc || doc->getObjectsOfType(Assembly::AssemblyObject::getClassTypeId()).empty()) {
                continue;
            }
            try {
                Base::PyGILStateLocker lock;
                Base::Interpreter().runString(
                    ("import AssemblyMigrate\nAssemblyMigrate.convertDocument("
                     "App.getDocument('" + name + "'))").c_str()
                );
            }
            catch (const Base::Exception& e) {
                e.ReportException();
            }
        }
    });


    // NOTE: To finish the initialization of our own type objects we must
    // call PyType_Ready, otherwise we run into a segmentation fault, later on.
    // This function is responsible for adding inherited slots from a type's base class.

    Assembly::AssemblyObject::init();
    Assembly::AssemblyLink::init();
    Assembly::BomObject::init();

    Assembly::BomGroup::init();
    Assembly::JointGroup::init();
    Assembly::ViewGroup::init();
    Assembly::SimulationGroup::init();
    Assembly::SnapshotGroup::init();
    Assembly::Joint::init();
    Assembly::JointPython::init();
    Assembly::AssemblyGroup::init();
    Assembly::PartsGroup::init();
    Assembly::GroundedJoint::init();
    Assembly::GroundedJointPython::init();
    Assembly::RigidGroup::init();
    Assembly::RigidGroupPython::init();
    Assembly::Simulation::init();
    Assembly::SimulationPython::init();
    Assembly::Motion::init();
    Assembly::MotionPython::init();
    Assembly::InitialCondition::init();
    Assembly::InitialConditionPython::init();
    Assembly::Sensor::init();
    Assembly::SensorPython::init();
    Assembly::ExplodedView::init();
    Assembly::ExplodedViewPython::init();
    Assembly::ExplodedViewStep::init();
    Assembly::ExplodedViewStepPython::init();
    Assembly::Snapshot::init();
    Assembly::SnapshotPython::init();
    Assembly::PhysicsItem::init();
    Assembly::MassOverride::init();
    Assembly::MassOverridePython::init();
    Assembly::SpringDamper::init();
    Assembly::SpringDamperPython::init();
    Assembly::FramedItem::init();
    Assembly::AppliedLoad::init();
    Assembly::AppliedLoadPython::init();
    Assembly::Bushing::init();
    Assembly::BushingPython::init();
    Assembly::ContactSet::init();
    Assembly::ContactSetPython::init();
    Assembly::CamContact::init();
    Assembly::TorsionSpring::init();
    Assembly::TorsionSpringPython::init();
    Assembly::JointStop::init();
    Assembly::JointStopPython::init();

    // The solver backends (docs/AssemblySolverInterface.md sec 4.3).
    Assembly::Solver::Registry::instance().add(std::make_unique<Assembly::Solver::OndselBackend>());
    // The oracle the others are tested against (sec 6.13).
    Assembly::Solver::Registry::instance().add(std::make_unique<Assembly::Solver::ReferenceBackend>());
#ifdef ASSEMBLY_WITH_CHRONO
    // Project Chrono, when built (BUILD_ASSEMBLY_CHRONO; sec 6.15).
    Assembly::Solver::Registry::instance().add(std::make_unique<Assembly::Solver::ChronoBackend>());
#endif

    PyMOD_Return(mod);
}
