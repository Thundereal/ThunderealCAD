// SPDX-License-Identifier: LGPL-2.1-or-later

#include <Base/Console.h>
#include <Base/Interpreter.h>
#include <Base/PyObjectBase.h>

#include "Solver/FreeFallBackend.h"
#include "Solver/Registry.h"
#include "Study.h"

namespace Fluid
{
extern PyObject* initModule();
}

PyMOD_INIT_FUNC(FluidApp)
{
    try {
        Base::Interpreter().runString("import Part");
    }
    catch (const Base::Exception& e) {
        PyErr_SetString(PyExc_ImportError, e.what());
        PyMOD_Return(nullptr);
    }

    PyObject* mod = Fluid::initModule();
    Base::Console().log("Loading Fluid module... done\n");

    Fluid::Study::init();
    Fluid::InitialFluid::init();
    Fluid::Monitor::init();

    // The backends (docs/FluidSimulation.md sec 11.2). The solvers come with
    // S2 and S3; until then there is the test data alone, which a study
    // must name to run on.
    Fluid::Solver::Registry::instance().add(std::make_unique<Fluid::Solver::FreeFallBackend>());

    PyMOD_Return(mod);
}
