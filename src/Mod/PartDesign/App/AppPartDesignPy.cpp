/***************************************************************************
 *   Copyright (c) 2008 Jürgen Riegel <juergen.riegel@web.de>              *
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

#include <numbers>

#include <Base/GeometryPyCXX.h>
#include <Base/Interpreter.h>
#include <Base/VectorPy.h>
#include <Base/Tools.h>
#include <App/DocumentObjectPy.h>
#include <App/DocumentPy.h>
#include <Standard_Failure.hxx>

#include "SheetMetalCheck.h"
#include "SheetMetalFlat.h"
#include "SolidBody.h"
#ifdef PARTDESIGN_WITH_SPREADSHEET
#include "FeatureSheetMetalBendTable.h"
#endif


namespace PartDesign {
class Module : public Py::ExtensionModule<Module>
{
public:
    Module() : Py::ExtensionModule<Module>("_PartDesign")
    {
        add_varargs_method("makeFilletArc",&Module::makeFilletArc,
            "makeFilletArc(...) -- Fillet arc."
        );
        add_varargs_method("exportFlat",&Module::exportFlat,
            "exportFlat(obj, path) -- the flat of obj, a feature of a sheet's solid or the\n"
            "solid (its tip's), written with the sheet's settings as DXF or SVG by the\n"
            "suffix of path."
        );
        add_varargs_method("exportFlats",&Module::exportFlats,
            "exportFlats(doc, folder, suffix='dxf') -> list -- the flat of every sheet in doc\n"
            "written into folder, one file each, as DXF or SVG by suffix, named after its body\n"
            "(and its solid where the body holds more than one sheet): a dict for each sheet,\n"
            "{'Sheet': its solid, 'File': the file written or '', 'Error': why not or ''}."
        );
        add_varargs_method("checkSheet",&Module::checkSheet,
            "checkSheet(obj) -> dict -- the bend table of obj, a feature of a sheet's solid\n"
            "or the solid (its tip), and what is wrong with its flat: {'Bends': [...],\n"
            "'Problems': [...]}, each a dict. Asked for, never kept."
        );
        add_varargs_method("crossBreakFaces",&Module::crossBreakFaces,
            "crossBreakFaces(obj) -> list -- the faces of obj's shape its sheet's cross breaks\n"
            "bow, obj a feature of a sheet or a body: a dict a face, {'Face', 'Wall', 'Center',\n"
            "'Direction', 'Normal', 'Length', 'Width', 'Angle', 'Rises'} -- the rectangle the\n"
            "wall's diagonals cross, its long side's way, the way the break rises, its size,\n"
            "the angle (deg) and whether it rises out of that face, as obj's shape lies."
        );
        add_varargs_method("sheetBends",&Module::sheetBends,
            "sheetBends(obj) -> list -- the bend table alone of obj, as checkSheet() has it\n"
            "but for the faces: a dict a bend, its values, their sources and its direction."
        );
#ifdef PARTDESIGN_WITH_SPREADSHEET
        add_varargs_method("syncBendTable",&Module::syncBendTable,
            "syncBendTable(table, force=False) -- fill a bend table from its sheet now; a\n"
            "cell the user changed stays theirs unless force."
        );
#endif
        initialize("This module is the PartDesign module."); // register with Python
    }

private:
    Py::Object exportFlat(const Py::Tuple& args)
    {
        PyObject *pyObj;
        char *path;
        if (!PyArg_ParseTuple(args.ptr(), "O!et", &(App::DocumentObjectPy::Type), &pyObj,
                              "utf-8", &path))
            throw Py::Exception();
        const std::string file(path);
        PyMem_Free(path);
        PY_TRY {
            try {
                PartDesign::exportFlat(
                        static_cast<App::DocumentObjectPy*>(pyObj)->getDocumentObjectPtr(),
                        file.c_str());
            }
            catch (const Standard_Failure &e) {
                THROWM(Base::CADKernelError, e.GetMessageString());
            }
        } _PY_CATCH(throw Py::Exception())
        return Py::None();
    }

    Py::Object exportFlats(const Py::Tuple& args)
    {
        PyObject *pyDoc;
        char *folder;
        const char *suffix = "dxf";
        if (!PyArg_ParseTuple(args.ptr(), "O!et|s", &(App::DocumentPy::Type), &pyDoc,
                              "utf-8", &folder, &suffix))
            throw Py::Exception();
        const std::string dir(folder);
        PyMem_Free(folder);
        std::vector<PartDesign::FlatExport> written;
        PY_TRY {
            try {
                written = PartDesign::exportFlats(
                        static_cast<App::DocumentPy*>(pyDoc)->getDocumentPtr(),
                        dir.c_str(), suffix);
            }
            catch (const Standard_Failure &e) {
                THROWM(Base::CADKernelError, e.GetMessageString());
            }
        } _PY_CATCH(throw Py::Exception())
        Py::List res;
        for (const auto &item : written) {
            Py::Dict entry;
            auto sheet = const_cast<PartDesign::SolidBody*>(item.sheet);
            entry.setItem("Sheet", sheet ? Py::asObject(sheet->getPyObject()) : Py::None());
            entry.setItem("File", Py::String(item.path));
            entry.setItem("Error", Py::String(item.error));
            res.append(entry);
        }
        return res;
    }

    Py::Object crossBreakFaces(const Py::Tuple& args)
    {
        PyObject *pyObj;
        if (!PyArg_ParseTuple(args.ptr(), "O!", &(App::DocumentObjectPy::Type), &pyObj))
            throw Py::Exception();
        auto obj = static_cast<App::DocumentObjectPy*>(pyObj)->getDocumentObjectPtr();
        std::vector<PartDesign::SheetMetal::CrossBreakPatch> patches;
        SheetMetal::Model model;
        PY_TRY {
            try {
                patches = PartDesign::crossBreakPatches(obj);
                if (!patches.empty())
                    model = PartDesign::sheetModelOf(obj);
            }
            catch (const Standard_Failure &e) {
                THROWM(Base::CADKernelError, e.GetMessageString());
            }
        } _PY_CATCH(throw Py::Exception())
        auto vector = [](double x, double y, double z) {
            return Py::Vector(Base::Vector3d(x, y, z));
        };
        Py::List res;
        for (const auto &patch : patches) {
            Py::Dict entry;
            entry.setItem("Face", Py::String("Face" + std::to_string(patch.face + 1)));
            entry.setItem("Wall", Py::String(model.elements[patch.wall].name));
            entry.setItem("Center", vector(patch.center.X(), patch.center.Y(),
                                           patch.center.Z()));
            entry.setItem("Direction", vector(patch.direction.X(), patch.direction.Y(),
                                              patch.direction.Z()));
            entry.setItem("Normal", vector(patch.normal.X(), patch.normal.Y(),
                                           patch.normal.Z()));
            entry.setItem("Length", Py::Float(2 * patch.halfLength));
            entry.setItem("Width", Py::Float(2 * patch.halfWidth));
            entry.setItem("Angle", Py::Float(patch.angle * 180 / std::numbers::pi));
            entry.setItem("Rises", Py::Boolean(patch.rises));
            res.append(entry);
        }
        return res;
    }

    Py::Object checkSheet(const Py::Tuple& args)
    {
        PyObject *pyObj;
        if (!PyArg_ParseTuple(args.ptr(), "O!", &(App::DocumentObjectPy::Type), &pyObj))
            throw Py::Exception();
        PartDesign::SheetCheck found;
        PY_TRY {
            try {
                found = PartDesign::checkSheet(
                        static_cast<App::DocumentObjectPy*>(pyObj)->getDocumentObjectPtr());
            }
            catch (const Standard_Failure &e) {
                THROWM(Base::CADKernelError, e.GetMessageString());
            }
        } _PY_CATCH(throw Py::Exception())

        auto names = [](const std::vector<std::string> &values) {
            Py::Tuple res(values.size());
            for (std::size_t i = 0; i < values.size(); ++i)
                res.setItem(i, Py::String(values[i]));
            return res;
        };
        Py::List bends;
        for (const auto &bend : found.bends) {
            Py::Dict item = bendItem(bend);
            item.setItem("Elements", names(bend.elements));
            item.setItem("FlatElements", names(bend.flatElements));
            bends.append(item);
        }
        Py::List problems;
        for (const auto &problem : found.problems) {
            Py::Dict item;
            item.setItem("Kind", Py::String(problem.kind));
            item.setItem("Message", Py::String(problem.message));
            item.setItem("Feature", Py::String(problem.feature));
            item.setItem("Value", Py::Float(problem.value));
            item.setItem("Limit", Py::Float(problem.limit));
            item.setItem("Elements", names(problem.elements));
            item.setItem("FlatElements", names(problem.flatElements));
            problems.append(item);
        }
        Py::Dict res;
        res.setItem("Bends", bends);
        res.setItem("Problems", problems);
        return res;
    }

    static Py::Dict bendItem(const PartDesign::SheetBendRow &bend)
    {
        Py::Dict item;
        item.setItem("Name", Py::String(bend.name));
        item.setItem("Feature", Py::String(bend.feature));
        item.setItem("Angle", Py::Float(bend.angle));
        item.setItem("Radius", Py::Float(bend.radius));
        item.setItem("RadiusSource", Py::String(bend.radiusSource));
        item.setItem("KFactor", Py::Float(bend.kFactor));
        item.setItem("KFactorSource", Py::String(bend.kFactorSource));
        item.setItem("Method", Py::String(bend.method));
        item.setItem("MethodSource", Py::String(bend.methodSource));
        item.setItem("Allowance", Py::Float(bend.allowance));
        item.setItem("Direction", Py::String(bend.direction));
        return item;
    }

    Py::Object sheetBends(const Py::Tuple& args)
    {
        PyObject *pyObj;
        if (!PyArg_ParseTuple(args.ptr(), "O!", &(App::DocumentObjectPy::Type), &pyObj))
            throw Py::Exception();
        std::vector<PartDesign::SheetBendRow> rows;
        PY_TRY {
            try {
                rows = PartDesign::sheetBends(
                        static_cast<App::DocumentObjectPy*>(pyObj)->getDocumentObjectPtr());
            }
            catch (const Standard_Failure &e) {
                THROWM(Base::CADKernelError, e.GetMessageString());
            }
        } _PY_CATCH(throw Py::Exception())
        Py::List res;
        for (const auto &row : rows)
            res.append(bendItem(row));
        return res;
    }

#ifdef PARTDESIGN_WITH_SPREADSHEET
    Py::Object syncBendTable(const Py::Tuple& args)
    {
        PyObject *pyObj;
        PyObject *force = Py_False;
        if (!PyArg_ParseTuple(args.ptr(), "O!|O", &(App::DocumentObjectPy::Type), &pyObj,
                              &force))
            throw Py::Exception();
        auto table = Base::freecad_dynamic_cast<PartDesign::SheetMetalBendTable>(
                static_cast<App::DocumentObjectPy*>(pyObj)->getDocumentObjectPtr());
        if (!table)
            throw Py::TypeError("Expect a PartDesign::SheetMetalBendTable");
        PY_TRY {
            try {
                table->sync(PyObject_IsTrue(force) == 1);
            }
            catch (const Standard_Failure &e) {
                THROWM(Base::CADKernelError, e.GetMessageString());
            }
        } _PY_CATCH(throw Py::Exception())
        return Py::None();
    }
#endif

    Py::Object makeFilletArc(const Py::Tuple& args)
    {
        PyObject *pM1;
        PyObject *pP;
        PyObject *pQ;
        PyObject *pN;
        double r2;
        int ccw;
        if (!PyArg_ParseTuple(args.ptr(), "O!O!O!O!di",
                &(Base::VectorPy::Type), &pM1,
                &(Base::VectorPy::Type), &pP,
                &(Base::VectorPy::Type), &pQ,
                &(Base::VectorPy::Type), &pN,
                &r2, &ccw))
            throw Py::Exception();

        Base::Vector3d M1 = Py::Vector(pM1, false).toVector();
        Base::Vector3d P  = Py::Vector(pP,  false).toVector();
        Base::Vector3d Q  = Py::Vector(pQ,  false).toVector();
        Base::Vector3d N  = Py::Vector(pN,  false).toVector();

        Base::Vector3d u = Q - P;
        Base::Vector3d v = P - M1;
        Base::Vector3d b;
        if (ccw)
            b = u % N;
        else
            b = N % u;
        b.Normalize();

        double uu = u * u;
        double uv = u * v;
        double r1 = v.Length();

        // distinguish between internal and external fillets
        r2 *= Base::sgn(uv);

        double cc = 2.0 * r2 * (b * v - r1);
        double d = uv * uv - uu * cc;
        if (d < 0) {
            throw Py::RuntimeError("Unable to calculate intersection points");
        }

        double t;
        double t1 = (-uv + sqrt(d)) / uu;
        double t2 = (-uv - sqrt(d)) / uu;

        if (fabs(t1) < fabs(t2))
            t = t1;
        else
            t = t2;

        Base::Vector3d M2 = P + (u*t) + (b*r2);
        Base::Vector3d S1 = (r2 * M1 + r1 * M2)/(r1+r2);
        Base::Vector3d S2 = M2 - (b*r2);

        Py::Tuple tuple(3);
        tuple.setItem(0, Py::Vector(S1));
        tuple.setItem(1, Py::Vector(S2));
        tuple.setItem(2, Py::Vector(M2));

        return tuple;
    }
};

PyObject* initModule()
{
    return Base::Interpreter().addModule(new Module);
}

} // namespace PartDesign
