// SPDX-License-Identifier: LGPL-2.1-or-later

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentObjectPy.h>

#include "Solver/Model.h"

// inclusion of the generated files (generated out of Study.pyi)
#include "StudyPy.h"
#include "StudyPy.cpp"

using namespace Fluid;

namespace
{
Py::Tuple tupleOf(const Solver::Vec3& v)
{
    Py::Tuple t(3);
    t.setItem(0, Py::Float(v[0]));
    t.setItem(1, Py::Float(v[1]));
    t.setItem(2, Py::Float(v[2]));
    return t;
}

// The object a model's source names (Study.cpp sourceOf()), or nullptr.
App::DocumentObject* objectOf(const std::string& source, const App::Document* home)
{
    const auto hash = source.find('#');
    if (hash == std::string::npos) {
        return home ? home->getObject(source.c_str()) : nullptr;
    }
    auto* doc = App::GetApplication().getDocument(source.substr(0, hash).c_str());
    return doc ? doc->getObject(source.substr(hash + 1).c_str()) : nullptr;
}
}  // namespace

std::string StudyPy::representation() const
{
    return {"<Fluid Study>"};
}

PyObject* StudyPy::buildModel(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    PY_TRY
    {
        const Solver::Model model = getStudyPtr()->buildModel();
        std::size_t triangles = 0;
        for (const auto& wall : model.walls) {
            triangles += wall.surface.triangles.size();
        }
        for (const auto& fluid : model.fluids) {
            triangles += fluid.surface.triangles.size();
        }
        Py::Dict ret;
        ret.setItem("Walls", Py::Long(static_cast<long>(model.walls.size())));
        ret.setItem("Fluids", Py::Long(static_cast<long>(model.fluids.size())));
        ret.setItem("Triangles", Py::Long(static_cast<long>(triangles)));
        ret.setItem("Monitors", Py::Long(static_cast<long>(model.monitors.size())));
        ret.setItem("Resolution", Py::Float(model.resolution));
        ret.setItem("SpaceMin", tupleOf(model.space.min));
        ret.setItem("SpaceMax", tupleOf(model.space.max));
        ret.setItem("Gravity", tupleOf(model.gravity));
        ret.setItem("TimeEnd", Py::Float(model.timeEnd));
        ret.setItem("OutputInterval", Py::Float(model.outputInterval));
        ret.setItem("Signature", Py::String(std::to_string(Solver::signature(model))));
        return Py::new_reference_to(ret);
    }
    PY_CATCH
}

PyObject* StudyPy::run(PyObject* args)
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    PY_TRY
    {
        std::string why;
        return Py::new_reference_to(Py::Long(static_cast<long>(getStudyPtr()->run(why))));
    }
    PY_CATCH
}

PyObject* StudyPy::start(PyObject* args)
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    PY_TRY
    {
        std::string why;
        return Py::new_reference_to(Py::Long(static_cast<long>(getStudyPtr()->start(why))));
    }
    PY_CATCH
}

PyObject* StudyPy::getProgress(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    const auto progress = getStudyPtr()->progress();
    Py::Dict ret;
    ret.setItem("Running", Py::Boolean(progress.running));
    ret.setItem("Done", Py::Boolean(progress.done));
    ret.setItem("Time", Py::Float(progress.time));
    ret.setItem("Frames", Py::Long(static_cast<long>(progress.frames)));
    return Py::new_reference_to(ret);
}

PyObject* StudyPy::cancel(PyObject* args)
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    getStudyPtr()->cancel();
    Py_Return;
}

PyObject* StudyPy::finish(PyObject* args)
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    PY_TRY
    {
        std::string why;
        return Py::new_reference_to(Py::Long(static_cast<long>(getStudyPtr()->finish(why))));
    }
    PY_CATCH
}

PyObject* StudyPy::getResults(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    PY_TRY
    {
        const auto* results = getStudyPtr()->results();
        if (!results) {
            Py_Return;
        }
        const auto& index = results->index();
        const std::size_t frames = index.times.size();
        const std::size_t samples = index.samples.size();
        const std::size_t width = index.monitors.size();
        Py::Dict ret;
        ret.setItem("Backend", Py::String(index.backend));
        ret.setItem("Version", Py::String(index.version));
        ret.setItem("Kind",
                    Py::String(index.kind == Solver::ResultKind::Surface  ? "Surface"
                               : index.kind == Solver::ResultKind::Fields ? "Fields"
                                                                          : "Particles"));
        ret.setItem("Frames", Py::Long(static_cast<long>(frames)));
        Py::List times;
        for (double t : index.times) {
            times.append(Py::Float(t));
        }
        ret.setItem("Times", times);
        Py::List sampleTimes;
        for (double t : index.samples) {
            sampleTimes.append(Py::Float(t));
        }
        ret.setItem("MonitorTimes", sampleTimes);
        Py::List monitors;
        for (std::size_t m = 0; m < width; ++m) {
            Py::Dict monitor;
            auto* obj = objectOf(index.monitors[m].source, getStudyPtr()->getDocument());
            monitor.setItem("Object", obj ? Py::asObject(obj->getPyObject()) : Py::None());
            monitor.setItem("Source", Py::String(index.monitors[m].source));
            monitor.setItem("Kind",
                            Py::String(index.monitors[m].kind == Solver::MonitorKind::Pressure
                                           ? "Pressure"
                                           : "Height"));
            monitor.setItem("Unit", Py::String(unitOf(index.monitors[m].kind)));
            Py::List values;
            for (std::size_t f = 0; f < samples; ++f) {
                values.append(Py::Float(index.values[f * width + m]));
            }
            monitor.setItem("Values", values);
            monitors.append(monitor);
        }
        ret.setItem("Monitors", monitors);
        return Py::new_reference_to(ret);
    }
    PY_CATCH
}

PyObject* StudyPy::getFrame(PyObject* args) const
{
    long at = 0;
    if (!PyArg_ParseTuple(args, "l", &at)) {
        return nullptr;
    }
    PY_TRY
    {
        const auto* results = getStudyPtr()->results();
        if (!results) {
            THROWM(Base::RuntimeError, "the study keeps no results");
        }
        if (at < 0) {
            THROWM(Base::IndexError, "a frame's index is not negative");
        }
        const auto frame = results->frame(static_cast<std::size_t>(at));
        Py::Dict ret;
        ret.setItem("Time", Py::Float(frame.time));
        ret.setItem("Count", Py::Long(static_cast<long>(frame.points.size() / 3)));
        ret.setItem("Points",
                    Py::asObject(PyBytes_FromStringAndSize(
                        reinterpret_cast<const char*>(frame.points.data()),
                        static_cast<Py_ssize_t>(frame.points.size() * sizeof(float)))));
        if (!frame.triangles.empty()) {
            ret.setItem("Triangles",
                        Py::asObject(PyBytes_FromStringAndSize(
                            reinterpret_cast<const char*>(frame.triangles.data()),
                            static_cast<Py_ssize_t>(frame.triangles.size()
                                                    * sizeof(std::int32_t)))));
        }
        return Py::new_reference_to(ret);
    }
    PY_CATCH
}

PyObject* StudyPy::isStale(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    PY_TRY
    {
        return Py::new_reference_to(Py::Boolean(getStudyPtr()->isStale()));
    }
    PY_CATCH
}

PyObject* StudyPy::getCustomAttributes(const char* /*attr*/) const
{
    return nullptr;
}

int StudyPy::setCustomAttributes(const char* /*attr*/, PyObject* /*obj*/)
{
    return 0;
}
