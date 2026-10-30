/***************************************************************************
 *   Copyright (c) 2014 Nathan Miller <Nathan.A.Mill[at]gmail.com>         *
 *   Copyright (c) 2014 Balázs Bámer                                       *
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

#include "PreCompiled.h"

#include <Base/Console.h>
#include <Base/Interpreter.h>
#include <Base/PyObjectBase.h>
#include <Gui/ActiveObjectList.h>
#include <Gui/Application.h>
#include <Gui/ToolContext.h>
#include <Mod/Part/App/BodyBase.h>

#include "Blending/ViewProviderBlendCurve.h"

#include "TaskFilling.h"
#include "TaskGeomFillSurface.h"
#include "TaskNetwork.h"
#include "TaskTools.h"
#include "TaskTrim.h"
#include "TaskSections.h"
#include "ViewProviderExtend.h"
#include "Workbench.h"


// use a different name to CreateCommand()
void CreateSurfaceCommands();


namespace SurfaceGui
{
class Module: public Py::ExtensionModule<Module>
{
public:
    Module()
        : Py::ExtensionModule<Module>("SurfaceGui")
    {
        initialize("This module is the SurfaceGui module.");  // register with Python
    }

private:
};

PyObject* initModule()
{
    return Base::Interpreter().addModule(new Module);
}

}  // namespace SurfaceGui

/* Python entry */
PyMOD_INIT_FUNC(SurfaceGui)
{
    if (!Gui::Application::Instance) {
        PyErr_SetString(PyExc_ImportError, "Cannot load Gui module in console application.");
        PyMOD_Return(nullptr);
    }

    Base::Interpreter().runString("import Surface");
    Base::Interpreter().runString("import PartGui");

    // clang-format off
    // instantiating the commands
    CreateSurfaceCommands();

    SurfaceGui::Workbench::init();
    SurfaceGui::ViewProviderGeomFillSurface ::init();
    SurfaceGui::ViewProviderFilling         ::init();
    SurfaceGui::ViewProviderSections        ::init();
    SurfaceGui::ViewProviderNetwork         ::init();
    SurfaceGui::ViewProviderTrim            ::init();
    SurfaceGui::ViewProviderBlendSurface    ::init();
    SurfaceGui::ViewProviderTwoRailSweep    ::init();
    SurfaceGui::ViewProviderPipeShell       ::init();
    SurfaceGui::ViewProviderExtend          ::init();
    SurfaceGui::ViewProviderBlendCurve      ::init();
    // SurfaceGui::ViewProviderCut::init();
    // clang-format on

    // The Surface tool bar in PartDesign, offered while a surface group is
    // active or a Surface feature is in edit (docs/SheetMetal.md sec 8)
    Gui::ToolContext::Spec surfaces;
    surfaces.name = "Surface";
    surfaces.workbenches = {"PartDesignWorkbench"};
    surfaces.toolbars = {{"Surface", &SurfaceGui::Workbench::addTools}};
    surfaces.activeKey = PDSURFACEKEY;
    surfaces.kind = "Surface";
    surfaces.applies = [](const App::DocumentObject* obj) {
        return Part::BodyBase::typeKind(obj->getTypeId()) == "Surface";
    };
    Gui::ToolContext::add(std::move(surfaces));

    PyObject* mod = SurfaceGui::initModule();
    Base::Console().Log("Loading GUI of Surface module... done\n");
    PyMOD_Return(mod);
}
