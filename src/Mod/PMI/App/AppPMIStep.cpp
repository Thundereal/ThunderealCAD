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

/** PMIStep: the GD&T of STEP files, read after their shapes and written into
 * an export (StepImport, StepExport; docs/GDandT.md sec 5.1, 5.2). It is the
 * one part of PMI that calls the OCCT fork's GD&T extensions of XCAF by their
 * C++ names, so it is a module of its own, which PMI loads only where the
 * fork's marker is found (AppPMI.cpp, loadStepExchange): PMI itself loads on
 * upstream's OCCT.
 */

#include "PreCompiled.h"

#include <Base/Console.h>
#include <Base/Interpreter.h>
#include <Base/PyObjectBase.h>

#include "AnnotationSet.h"
#include "StepExport.h"
#include "StepImport.h"

namespace PMI
{
namespace Step
{
class Module: public Py::ExtensionModule<Module>
{
public:
    Module()
        : Py::ExtensionModule<Module>("PMIStep")
    {
        initialize("PMIStep: the GD&T of STEP files, read and written (loaded by PMI)");
    }
};
}  // namespace Step
}  // namespace PMI

PyMOD_INIT_FUNC(PMIStep)
{
    // Loaded by PMI, whose types its passes make: alone the types are not there
    if (PMI::AnnotationSet::getClassTypeId().isBad()) {
        PyErr_SetString(PyExc_ImportError, "PMIStep is loaded by PMI: import PMI");
        PyMOD_Return(nullptr);
    }
    PyObject* mod = Base::Interpreter().addModule(new PMI::Step::Module);

    // The GD&T of a STEP file, read after its shapes (docs/GDandT.md sec 5.1)
    PMI::Step::registerImport();
    // ... and written into one exported, AP242 (sec 5.2)
    PMI::Step::registerExport();

    Base::Console().log("Loading PMIStep module... done\n");
    PyMOD_Return(mod);
}
