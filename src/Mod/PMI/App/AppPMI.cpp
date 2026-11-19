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
#include "Advisor.h"
#include "Api.h"
#include "Callout.h"
#include "AnnotationSet.h"
#include "Check.h"
#include "Datum.h"
#include "Dimension.h"
#include "GeometricTolerance.h"
#include "Note.h"
#include "TolerancedFeature.h"
#include "View.h"

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
        add_varargs_method("solidReference", &Module::solidReference,
                           "solidReference(obj, subname) -> (solid, 'FaceN'): a picked face as an "
                           "annotation refers to it -- through the solid body of a Body");
        add_varargs_method("getSet", &Module::getSet,
                           "getSet(part, create=True) -> the annotation set of part, made in its "
                           "Body or beside it");
        add_varargs_method("feature", &Module::feature,
                           "feature(set, solid, faces, kind=None) -> the set's toleranced feature "
                           "on these faces, made if there is none");
        add_varargs_method("nextDatumLetter", &Module::nextDatumLetter,
                           "nextDatumLetter(set, solid=None) -> the first letter not used");
        add_varargs_method("frame", &Module::frame,
                           "frame(set, compartments) -> the set's datum reference frame with "
                           "these compartments, e.g. ['A', 'B(M)'], made if there is none");
        add_varargs_method("stackUnder", &Module::stackUnder,
                           "stackUnder(callout) -> the callout it is stacked under by default: "
                           "the set's last one on its feature, or None");
        add_varargs_method("query", &Module::query,
                           "query(obj, subname) -> [(callout, how)]: what applies to a face, "
                           "picked or of a solid -- 'on' it, or 'datum A' citing a datum on it");
        add_varargs_method("advise", &Module::advise,
                           "advise(feature, characteristic=None) -> {'characteristics': [...], "
                           "and with a characteristic 'zoneShapes': [...], 'materialCondition': "
                           "bool, 'datums': 1 needed / 0 optional / -1 none}");
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

    Py::Object solidReference(const Py::Tuple& args)
    {
        PyObject* pyobj;
        const char* sub;
        if (!PyArg_ParseTuple(args.ptr(), "Os", &pyobj, &sub)) {
            throw Py::Exception();
        }
        App::DocumentObject* solid = nullptr;
        std::string face, why;
        if (!Api::solidReference(toObject(pyobj), sub, solid, face, why)) {
            throw Py::ValueError(why);
        }
        return Py::TupleN(Py::asObject(solid->getPyObject()), Py::String(face));
    }

    Py::Object getSet(const Py::Tuple& args)
    {
        PyObject* pypart;
        PyObject* create = Py_True;
        if (!PyArg_ParseTuple(args.ptr(), "O|O", &pypart, &create)) {
            throw Py::Exception();
        }
        if (auto set = Api::getSet(toObject(pypart), PyObject_IsTrue(create))) {
            return Py::asObject(set->getPyObject());
        }
        return Py::None();
    }

    Py::Object feature(const Py::Tuple& args)
    {
        PyObject* pyset;
        PyObject* pysolid;
        PyObject* pyfaces;
        const char* kind = nullptr;
        if (!PyArg_ParseTuple(args.ptr(), "OOO|z", &pyset, &pysolid, &pyfaces, &kind)) {
            throw Py::Exception();
        }
        std::vector<std::string> faces;
        Py::Sequence seq(pyfaces);
        for (Py::Sequence::iterator it = seq.begin(); it != seq.end(); ++it) {
            faces.push_back(Py::String(*it).as_std_string());
        }
        auto feat = Api::feature(toSet(pyset), toObject(pysolid), faces, kind);
        if (!feat) {
            throw Py::ValueError("no feature: a set, a solid and faces are needed");
        }
        return Py::asObject(feat->getPyObject());
    }

    Py::Object nextDatumLetter(const Py::Tuple& args)
    {
        PyObject* pyset;
        PyObject* pysolid = Py_None;
        if (!PyArg_ParseTuple(args.ptr(), "O|O", &pyset, &pysolid)) {
            throw Py::Exception();
        }
        App::DocumentObject* solid = pysolid == Py_None ? nullptr : toObject(pysolid);
        return Py::String(Api::nextDatumLetter(toSet(pyset), solid));
    }

    Py::Object frame(const Py::Tuple& args)
    {
        PyObject* pyset;
        PyObject* pycomps;
        if (!PyArg_ParseTuple(args.ptr(), "OO", &pyset, &pycomps)) {
            throw Py::Exception();
        }
        std::vector<std::string> comps;
        Py::Sequence seq(pycomps);
        for (Py::Sequence::iterator it = seq.begin(); it != seq.end(); ++it) {
            comps.push_back(Py::String(*it).as_std_string());
        }
        auto drf = Api::frame(toSet(pyset), comps);
        if (!drf) {
            return Py::None();
        }
        return Py::asObject(drf->getPyObject());
    }

    Py::Object stackUnder(const Py::Tuple& args)
    {
        PyObject* pyobj;
        if (!PyArg_ParseTuple(args.ptr(), "O", &pyobj)) {
            throw Py::Exception();
        }
        auto callout = freecad_cast<Callout*>(toObject(pyobj));
        if (!callout) {
            throw Py::TypeError("a PMI callout is wanted");
        }
        auto under = Api::stackUnder(callout);
        if (!under) {
            return Py::None();
        }
        return Py::asObject(under->getPyObject());
    }

    Py::Object query(const Py::Tuple& args)
    {
        PyObject* pyobj;
        const char* sub;
        if (!PyArg_ParseTuple(args.ptr(), "Os", &pyobj, &sub)) {
            throw Py::Exception();
        }
        App::DocumentObject* solid = nullptr;
        std::string face, why;
        if (!Api::solidReference(toObject(pyobj), sub, solid, face, why)) {
            throw Py::ValueError(why);
        }
        Py::List out;
        for (auto& a : Api::query(solid, face)) {
            out.append(Py::TupleN(Py::asObject(a.callout->getPyObject()), Py::String(a.how)));
        }
        return out;
    }

    Py::Object advise(const Py::Tuple& args)
    {
        PyObject* pyfeat;
        const char* name = nullptr;
        if (!PyArg_ParseTuple(args.ptr(), "O|z", &pyfeat, &name)) {
            throw Py::Exception();
        }
        auto feat = freecad_cast<TolerancedFeature*>(toObject(pyfeat));
        if (!feat) {
            throw Py::TypeError("a PMI::TolerancedFeature is expected");
        }
        Py::Dict res;
        Py::List chars;
        for (int c : Advisor::characteristics(feat)) {
            chars.append(Py::String(GeometricTolerance::CharacteristicEnums[c]));
        }
        res.setItem("characteristics", chars);
        if (name) {
            int c = -1;
            for (int i = 0; GeometricTolerance::CharacteristicEnums[i]; ++i) {
                if (std::string(GeometricTolerance::CharacteristicEnums[i]) == name) {
                    c = i;
                }
            }
            if (c < 0) {
                throw Py::ValueError(std::string("no characteristic ") + name);
            }
            Py::List zones;
            for (int z : Advisor::zoneShapes(feat, c)) {
                zones.append(Py::String(GeometricTolerance::ZoneShapeEnums[z]));
            }
            res.setItem("zoneShapes", zones);
            res.setItem("materialCondition", Py::Boolean(Advisor::materialCondition(feat, c)));
            res.setItem("datums", Py::Long(Advisor::datums(c)));
        }
        return res;
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
    PMI::Callout             ::init();
    PMI::TolerancedFeature   ::init();
    PMI::Datum               ::init();
    PMI::DatumReferenceFrame ::init();
    PMI::Dimension           ::init();
    PMI::GeometricTolerance  ::init();
    PMI::Note                ::init();
    PMI::View                ::init();
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
