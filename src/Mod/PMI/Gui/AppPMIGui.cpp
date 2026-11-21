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

#include "PreCompiled.h"

#include <Base/Console.h>
#include <Base/Interpreter.h>
#include <Base/PyObjectBase.h>
#include <Gui/Application.h>
#include <Gui/ToolContext.h>
#include <Mod/Part/App/BodyBase.h>

#include <Gui/WidgetFactory.h>

#include "DlgSettingsPMI.h"
#include "ViewProviderCallout.h"
#include "ViewProviderView.h"
#include "Workbench.h"

void CreatePMICommands();

void loadPMIResource()
{
    Q_INIT_RESOURCE(PMI);
}

namespace PMIGui
{
class Module: public Py::ExtensionModule<Module>
{
public:
    Module()
        : Py::ExtensionModule<Module>("PMIGui")
    {
        initialize("The GUI of the PMI module (docs/GDandT.md)");
    }
};

PyObject* initModule()
{
    return Base::Interpreter().addModule(new Module);
}
}  // namespace PMIGui

PyMOD_INIT_FUNC(PMIGui)
{
    if (!Gui::Application::Instance) {
        PyErr_SetString(PyExc_ImportError, "Cannot load Gui module in console application.");
        PyMOD_Return(nullptr);
    }
    try {
        Base::Interpreter().runString("import PMI");
        Base::Interpreter().runString("import PartGui");
    }
    catch (const Base::Exception& e) {
        PyErr_SetString(PyExc_ImportError, e.what());
        PyMOD_Return(nullptr);
    }

    loadPMIResource();
    CreatePMICommands();
    (void)new Gui::PrefPageProducer<PMIGui::DlgSettingsPMI>(
        QT_TRANSLATE_NOOP("QObject", "Part/Part Design"));

    // clang-format off
    PMIGui::Workbench                 ::init();
    PMIGui::ViewProviderCallout       ::init();
    PMIGui::ViewProviderFeature       ::init();
    PMIGui::ViewProviderAnnotationSet ::init();
    PMIGui::ViewProviderView          ::init();
    // clang-format on

    // The PMI tool bar in PartDesign, offered while a Body's annotation set
    // is active or an annotation is in edit (docs/GDandT.md sec 3)
    Gui::ToolContext::Spec pmi;
    pmi.name = "PMI";
    pmi.workbenches = {"PartDesignWorkbench"};
    pmi.toolbars = {{"PMI", &PMIGui::Workbench::addTools}};
    pmi.activeKey = PMIGui::PMIKEY;
    pmi.kind = "Annotation";
    pmi.applies = [](const App::DocumentObject* obj) {
        return Part::BodyBase::typeKind(obj->getTypeId()) == "Annotation";
    };
    Gui::ToolContext::add(std::move(pmi));

    PyObject* mod = PMIGui::initModule();
    Base::Console().Log("Loading GUI of PMI module... done\n");
    PyMOD_Return(mod);
}
