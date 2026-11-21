// SPDX-License-Identifier: LGPL-2.1-or-later

#include <Base/Interpreter.h>
#include <CXX/Extensions.hxx>
#include <CXX/Objects.hxx>

#include "Solver/Registry.h"

namespace Fluid
{
class Module: public Py::ExtensionModule<Module>
{
public:
    Module()
        : Py::ExtensionModule<Module>("FluidApp")
    {
        add_varargs_method(
            "getBackends",
            &Module::getBackends,
            "getBackends() -> list of dict\n\n"
            "The registered fluid solver backends, in registration order. Each: Name\n"
            "(what a study's Backend property takes), Label, Available and, when it\n"
            "is not, Reason; Version; Results ('Particles', 'Surface', 'Fields');\n"
            "FreeSurface, Inlets, MovingWalls, FreeBodies, Heat, Steady, Repeatable,\n"
            "InProcess; TestData (test data, never chosen for a study by itself)."
        );
        initialize("The fluid simulation workbench's application module.");
    }

private:
    Py::Object getBackends(const Py::Tuple& args)
    {
        if (!PyArg_ParseTuple(args.ptr(), "")) {
            throw Py::Exception();
        }
        Py::List list;
        const auto& registry = Solver::Registry::instance();
        for (const auto& name : registry.names()) {
            const auto* backend = registry.find(name);
            const auto can = backend->capabilities();
            const auto availability = backend->availability();
            Py::Dict entry;
            entry.setItem("Name", Py::String(name));
            entry.setItem("Label", Py::String(backend->label()));
            entry.setItem("Available", Py::Boolean(availability.available));
            entry.setItem("Reason", Py::String(availability.reason));
            entry.setItem("Version", Py::String(availability.version));
            entry.setItem("Results",
                          Py::String(can.results == Solver::ResultKind::Surface  ? "Surface"
                                     : can.results == Solver::ResultKind::Fields ? "Fields"
                                                                                 : "Particles"));
            entry.setItem("FreeSurface", Py::Boolean(can.freeSurface));
            entry.setItem("Inlets", Py::Boolean(can.inlets));
            entry.setItem("MovingWalls", Py::Boolean(can.movingWalls));
            entry.setItem("FreeBodies", Py::Boolean(can.freeBodies));
            entry.setItem("Heat", Py::Boolean(can.heat));
            entry.setItem("Steady", Py::Boolean(can.steady));
            entry.setItem("Repeatable", Py::Boolean(can.repeatable));
            entry.setItem("InProcess", Py::Boolean(can.inProcess));
            entry.setItem("TestData", Py::Boolean(can.testData));
            list.append(entry);
        }
        return list;
    }
};

PyObject* initModule()
{
    return Base::Interpreter().addModule(new Module);
}

}  // namespace Fluid
