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

#include <App/DocumentObjectPy.h>
#include <Base/Console.h>
#include <Base/Interpreter.h>
#include <Base/PyObjectBase.h>
#include <Mod/Part/App/BodyBase.h>

#include "Annotation.h"
#include "AnnotationSet.h"
#include "Check.h"
#include "Datum.h"
#include "Dimension.h"
#include "GeometricTolerance.h"
#include "Note.h"
#include "TolerancedFeature.h"

namespace PMI
{
class Module: public Py::ExtensionModule<Module>
{
public:
    Module()
        : Py::ExtensionModule<Module>("PMI")
    {
        add_varargs_method("check", &Module::check,
                           "check(set) -> [(object, 'Error' or 'Warning', message), ...]\n"
                           "The rules of docs/GDandT.md sec 8 over a PMI.AnnotationSet");
        add_varargs_method("findFeature", &Module::findFeature,
                           "findFeature(set, part, subs) -> the set's toleranced feature on "
                           "exactly these elements of part, or None");
        add_varargs_method("findSet", &Module::findSet,
                           "findSet(part) -> the annotation set of part, or None");
        initialize("PMI: product and manufacturing information -- GD&T on a part");
    }

private:
    static App::DocumentObject* toObject(PyObject* py)
    {
        if (!PyObject_TypeCheck(py, &App::DocumentObjectPy::Type)) {
            throw Py::TypeError("a document object is expected");
        }
        return static_cast<App::DocumentObjectPy*>(py)->getDocumentObjectPtr();
    }

    static AnnotationSet* toSet(PyObject* py)
    {
        auto set = freecad_cast<AnnotationSet*>(toObject(py));
        if (!set) {
            throw Py::TypeError("a PMI::AnnotationSet is expected");
        }
        return set;
    }

    Py::Object check(const Py::Tuple& args)
    {
        PyObject* pyset;
        if (!PyArg_ParseTuple(args.ptr(), "O", &pyset)) {
            throw Py::Exception();
        }
        Py::List list;
        for (auto& issue : Check::set(toSet(pyset))) {
            auto obj = const_cast<App::DocumentObject*>(issue.object);
            list.append(Py::TupleN(obj ? Py::asObject(obj->getPyObject()) : Py::None(),
                                   Py::String(issue.severity == Check::Error ? "Error" : "Warning"),
                                   Py::String(issue.message)));
        }
        return list;
    }

    Py::Object findFeature(const Py::Tuple& args)
    {
        PyObject* pyset;
        PyObject* pypart;
        PyObject* pysubs;
        if (!PyArg_ParseTuple(args.ptr(), "OOO", &pyset, &pypart, &pysubs)) {
            throw Py::Exception();
        }
        auto set = toSet(pyset);
        auto part = toObject(pypart);
        std::vector<std::string> subs;
        Py::Sequence seq(pysubs);
        for (Py::Sequence::iterator it = seq.begin(); it != seq.end(); ++it) {
            subs.push_back(Py::String(*it).as_std_string());
        }
        for (auto feat : set->features()) {
            if (feat->refersTo(part, subs)) {
                return Py::asObject(feat->getPyObject());
            }
        }
        return Py::None();
    }

    Py::Object findSet(const Py::Tuple& args)
    {
        PyObject* pypart;
        if (!PyArg_ParseTuple(args.ptr(), "O", &pypart)) {
            throw Py::Exception();
        }
        if (auto set = AnnotationSet::find(toObject(pypart))) {
            return Py::asObject(set->getPyObject());
        }
        return Py::None();
    }
};

PyObject* initModule()
{
    return Base::Interpreter().addModule(new Module);
}
}  // namespace PMI

PyMOD_INIT_FUNC(PMI)
{
    try {
        Base::Interpreter().runString("import Part");
    }
    catch (const Base::Exception& e) {
        PyErr_SetString(PyExc_ImportError, e.what());
        PyMOD_Return(nullptr);
    }
    PyObject* mod = PMI::initModule();

    // clang-format off
    PMI::Annotation          ::init();
    PMI::AnnotationSet       ::init();
    PMI::TolerancedFeature   ::init();
    PMI::Datum               ::init();
    PMI::DatumReferenceFrame ::init();
    PMI::Dimension           ::init();
    PMI::GeometricTolerance  ::init();
    PMI::Note                ::init();
    // clang-format on

    // In a PartDesign Body, as its Elements group is: never solid, never its
    // tip. A body holding a set gives even a lone solid a solid body, which
    // is what the annotations' faces are named through (docs/GDandT.md sec 3)
    for (Base::Type type : {PMI::AnnotationSet::getClassTypeId(),
                            PMI::Annotation::getClassTypeId()}) {
        Part::BodyBase::allowType(type, "Annotation");
    }
    Part::BodyBase::requireSolidBody(PMI::AnnotationSet::getClassTypeId());

    Base::Console().Log("Loading PMI module... done\n");
    PyMOD_Return(mod);
}
