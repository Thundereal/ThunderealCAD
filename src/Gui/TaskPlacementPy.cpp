// SPDX-License-Identifier: LGPL-2.1-or-later
/****************************************************************************
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

// FreeCADGui.TaskPlacement: upstream's Python type of that name (its
// Gui/Placement.cpp), which the Assembly workbench uses to edit a joint's
// offsets. Upstream's wraps a bare Placement dialog, not the TaskPlacement
// task dialog, and so does this one; the methods and their behaviour follow
// upstream's, over the fork's Placement dialog. The type is file-local, so
// Placement.h and Application.h carry nothing for it.

#include "PreCompiled.h"

#include <QDialogButtonBox>
#include <QPointer>

#include <App/DocumentObject.h>
#include <App/DocumentObjectPy.h>
#include <App/PropertyGeo.h>
#include <Base/Interpreter.h>
#include <Base/PlacementPy.h>
#include <CXX/Extensions.hxx>

#include "Placement.h"
#include "PythonWrapper.h"
#include "TaskPlacementPy.h"

using namespace Gui::Dialog;

namespace
{

class TaskPlacementPy: public Py::PythonExtension<TaskPlacementPy>
{
public:
    using BaseType = Py::PythonExtension<TaskPlacementPy>;

    static void init_type()
    {
        behaviors().name("TaskPlacement");
        behaviors().doc("TaskPlacement()\n"
                        "The placement dialog, for editing a placement property from Python.\n"
                        "'form' is the dialog widget.");
        behaviors().set_tp_new(PyMake);
        behaviors().supportRepr();
        behaviors().supportGetattr();
        behaviors().supportSetattr();
        // clang-format off
        add_varargs_method("setPropertyName", &TaskPlacementPy::setPropertyName,
                           "setPropertyName(string)");
        add_varargs_method("setPlacement", &TaskPlacementPy::setPlacement,
                           "setPlacement(Placement)");
        add_varargs_method("setSelection", &TaskPlacementPy::setSelection,
                           "setSelection(list)");
        add_varargs_method("bindObject", &TaskPlacementPy::bindObject,
                           "bindObject()");
        add_varargs_method("setPlacementAndBindObject", &TaskPlacementPy::setPlacementAndBindObject,
                           "setPlacementAndBindObject(obj, string)");
        add_varargs_method("setIgnoreTransactions", &TaskPlacementPy::setIgnoreTransactions,
                           "setIgnoreTransactions(bool)");
        add_varargs_method("showDefaultButtons", &TaskPlacementPy::showDefaultButtons,
                           "showDefaultButtons(bool)");
        add_varargs_method("accept", &TaskPlacementPy::accept, "accept()");
        add_varargs_method("reject", &TaskPlacementPy::reject, "reject()");
        add_varargs_method("clicked", &TaskPlacementPy::clicked, "clicked(int)");
        add_varargs_method("open", &TaskPlacementPy::open, "open()");
        add_varargs_method("isAllowedAlterDocument", &TaskPlacementPy::isAllowedAlterDocument,
                           "isAllowedAlterDocument()");
        add_varargs_method("isAllowedAlterView", &TaskPlacementPy::isAllowedAlterView,
                           "isAllowedAlterView()");
        add_varargs_method("isAllowedAlterSelection", &TaskPlacementPy::isAllowedAlterSelection,
                           "isAllowedAlterSelection()");
        add_varargs_method("getStandardButtons", &TaskPlacementPy::getStandardButtons,
                           "getStandardButtons()");
        // clang-format on
        behaviors().readyType();
    }

    TaskPlacementPy()
        : widget {new Placement}
    {}

    ~TaskPlacementPy() override
    {
        // Upstream leaks the dialog. Deferred, so a 'form' wrapper the caller
        // still holds does not point at a deleted widget mid-statement.
        if (widget) {
            widget->deleteLater();
        }
    }

    Py::Object repr() override
    {
        return Py::String("TaskPlacement");
    }

    Py::Object getattr(const char* name) override
    {
        if (strcmp(name, "form") == 0) {
            if (!widget) {
                return Py::None();
            }
            Gui::PythonWrapper wrap;
            wrap.loadWidgetsModule();
            return wrap.fromQWidget(widget, "QDialog");
        }
        return getattr_methods(name);
    }

    int setattr(const char* name, const Py::Object& attr) override
    {
        if (strcmp(name, "form") == 0 && attr.isNone()) {
            delete widget;
            return 0;
        }
        return BaseType::setattr(name, attr);
    }

    Py::Object setPropertyName(const Py::Tuple& args)
    {
        const char* propname {};
        if (!PyArg_ParseTuple(args.ptr(), "s", &propname)) {
            throw Py::Exception();
        }
        if (widget) {
            widget->setPropertyName(propname);
        }
        return Py::None();
    }

    Py::Object setPlacement(const Py::Tuple& args)
    {
        PyObject* plm {};
        if (!PyArg_ParseTuple(args.ptr(), "O!", &Base::PlacementPy::Type, &plm)) {
            throw Py::Exception();
        }
        if (widget) {
            widget->setPlacement(*static_cast<Base::PlacementPy*>(plm)->getPlacementPtr());
        }
        return Py::None();
    }

    Py::Object setSelection(const Py::Tuple& args)
    {
        PyObject* seq {};
        if (!PyArg_ParseTuple(args.ptr(), "O", &seq)) {
            throw Py::Exception();
        }
        std::vector<Gui::SelectionObject> sel;
        Py::Sequence list(seq);
        for (const auto& item : list) {
            if (PyObject_TypeCheck(item.ptr(), &App::DocumentObjectPy::Type)) {
                auto pyobj = static_cast<App::DocumentObjectPy*>(item.ptr());
                sel.emplace_back(pyobj->getDocumentObjectPtr());
            }
        }
        if (widget) {
            widget->setSelection(sel);
        }
        return Py::None();
    }

    Py::Object bindObject(const Py::Tuple& args)
    {
        if (!PyArg_ParseTuple(args.ptr(), "")) {
            throw Py::Exception();
        }
        if (widget) {
            widget->bindObject();
        }
        return Py::None();
    }

    // Upstream's Placement has a method of this name; here it is the same
    // steps done through the fork's setters
    Py::Object setPlacementAndBindObject(const Py::Tuple& args)
    {
        PyObject* pyobj {};
        const char* name {};
        if (!PyArg_ParseTuple(args.ptr(), "O!s", &App::DocumentObjectPy::Type, &pyobj, &name)) {
            throw Py::Exception();
        }
        App::DocumentObject* obj = static_cast<App::DocumentObjectPy*>(pyobj)->getDocumentObjectPtr();
        auto prop = obj ? dynamic_cast<App::PropertyPlacement*>(obj->getPropertyByName(name))
                        : nullptr;
        if (widget && prop) {
            widget->setPlacement(prop->getValue());
            widget->setPropertyName(name);
            widget->setSelection({Gui::SelectionObject(obj)});
            widget->bindObject();
        }
        return Py::None();
    }

    Py::Object setIgnoreTransactions(const Py::Tuple& args)
    {
        PyObject* value {};
        if (!PyArg_ParseTuple(args.ptr(), "O", &value)) {
            throw Py::Exception();
        }
        if (widget) {
            widget->setIgnoreTransactions(PyObject_IsTrue(value) != 0);
        }
        return Py::None();
    }

    Py::Object showDefaultButtons(const Py::Tuple& args)
    {
        PyObject* value {};
        if (!PyArg_ParseTuple(args.ptr(), "O", &value)) {
            throw Py::Exception();
        }
        if (widget) {
            widget->showDefaultButtons(PyObject_IsTrue(value) != 0);
        }
        return Py::None();
    }

    Py::Object accept(const Py::Tuple& args)
    {
        if (!PyArg_ParseTuple(args.ptr(), "")) {
            throw Py::Exception();
        }
        bool res = true;
        if (widget) {
            widget->accept();
            res = widget->result() == QDialog::Accepted;
        }
        return Py::Boolean(res);
    }

    Py::Object reject(const Py::Tuple& args)
    {
        if (!PyArg_ParseTuple(args.ptr(), "")) {
            throw Py::Exception();
        }
        bool res = true;
        if (widget) {
            widget->reject();
            res = widget->result() == QDialog::Rejected;
        }
        return Py::Boolean(res);
    }

    Py::Object clicked(const Py::Tuple& args)
    {
        int index {};
        if (!PyArg_ParseTuple(args.ptr(), "i", &index)) {
            throw Py::Exception();
        }
        if (widget && index == QDialogButtonBox::Apply) {
            widget->onApplyButtonClicked();
        }
        return Py::None();
    }

    Py::Object open(const Py::Tuple& args)
    {
        if (!PyArg_ParseTuple(args.ptr(), "")) {
            throw Py::Exception();
        }
        if (widget) {
            widget->open();
        }
        return Py::None();
    }

    Py::Object isAllowedAlterDocument(const Py::Tuple& args)
    {
        return allowed(args);
    }

    Py::Object isAllowedAlterView(const Py::Tuple& args)
    {
        return allowed(args);
    }

    Py::Object isAllowedAlterSelection(const Py::Tuple& args)
    {
        return allowed(args);
    }

    Py::Object getStandardButtons(const Py::Tuple& args)
    {
        if (!PyArg_ParseTuple(args.ptr(), "")) {
            throw Py::Exception();
        }
        auto buttons = QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply;
        return Py::Long(static_cast<int>(buttons));
    }

private:
    static PyObject* PyMake(struct _typeobject*, PyObject* args, PyObject*)
    {
        if (!PyArg_ParseTuple(args, "")) {
            return nullptr;
        }
        return new TaskPlacementPy();
    }

    static Py::Object allowed(const Py::Tuple& args)
    {
        if (!PyArg_ParseTuple(args.ptr(), "")) {
            throw Py::Exception();
        }
        return Py::Boolean(true);
    }

    QPointer<Placement> widget;
};

}  // namespace

void Gui::Dialog::registerTaskPlacementInPython(PyObject* module)
{
    TaskPlacementPy::init_type();
    Base::Interpreter().addType(TaskPlacementPy::type_object(), module, "TaskPlacement");
}
