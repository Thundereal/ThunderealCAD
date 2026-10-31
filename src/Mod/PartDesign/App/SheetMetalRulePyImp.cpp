// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PreCompiled.h"

#include <numbers>

#include "FeatureSheetMetalRule.h"
#include "SheetMetalRulePy.h"
#include "SheetMetalRulePy.cpp"

using namespace PartDesign;

std::string SheetMetalRulePy::representation() const
{
    return {"<PartDesign::SheetMetalRule>"};
}

PyObject* SheetMetalRulePy::hasGauge(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    return Py::new_reference_to(Py::Boolean(getSheetMetalRulePtr()->hasGauge()));
}

PyObject* SheetMetalRulePy::resetRuleValue(PyObject* args)
{
    const char* name;
    if (!PyArg_ParseTuple(args, "s", &name)) {
        return nullptr;
    }
    PY_TRY
    {
        getSheetMetalRulePtr()->resetValue(name);
        Py_Return;
    }
    PY_CATCH
}

PyObject* SheetMetalRulePy::bendAllowance(PyObject* args) const
{
    double angle;
    PyObject* pyRadius = Py_None;
    if (!PyArg_ParseTuple(args, "d|O", &angle, &pyRadius)) {
        return nullptr;
    }
    PY_TRY
    {
        double radius = -1;
        if (pyRadius != Py_None) {
            radius = Py::Float(pyRadius);
        }
        auto rule = getSheetMetalRulePtr();
        auto bend = rule->getBend(angle * std::numbers::pi / 180, radius);
        return Py::new_reference_to(
            Py::Float(SheetMetal::bendAllowance(bend, rule->getAllowance())));
    }
    PY_CATCH
}

PyObject* SheetMetalRulePy::getCustomAttributes(const char* /*attr*/) const
{
    return nullptr;
}

int SheetMetalRulePy::setCustomAttributes(const char* /*attr*/, PyObject* /*obj*/)
{
    return 0;
}
