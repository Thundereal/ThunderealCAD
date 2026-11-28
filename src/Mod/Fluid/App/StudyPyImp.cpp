// SPDX-License-Identifier: LGPL-2.1-or-later

#include <cstring>

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentObjectPy.h>
#include <Base/PlacementPy.h>
#include <Base/VectorPy.h>

#include "ParticleSurface.h"
#include "Section.h"
#include "Tracers.h"
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

// Where the moved walls are at a frame, each the motion from where it
// stood: a Placement, mm.
Py::List motionsOf(const std::vector<Solver::Motion>& bodies)
{
    Py::List list;
    for (const auto& motion : bodies) {
        const Base::Placement placement(
            Base::Vector3d(motion.translation[0], motion.translation[1], motion.translation[2]),
            Base::Rotation(motion.rotation[0],
                           motion.rotation[1],
                           motion.rotation[2],
                           motion.rotation[3]));
        list.append(Py::asObject(new Base::PlacementPy(new Base::Placement(placement))));
    }
    return list;
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
        Py::List bodies;
        for (std::size_t wall : Solver::freeWalls(model)) {
            const auto& moved = model.walls[wall];
            Py::Dict body;
            body.setItem("Source", Py::String(moved.source));
            body.setItem("Driven", Py::Boolean(moved.motion == Solver::WallMotion::Driven));
            body.setItem("Mass", Py::Float(moved.mass.mass));
            body.setItem("Centre", tupleOf(moved.mass.centre));
            Py::Tuple inertia(6);
            for (std::size_t i = 0; i < 6; ++i) {
                inertia.setItem(i, Py::Float(moved.mass.inertia[i]));
            }
            body.setItem("Inertia", inertia);
            bodies.append(body);
        }
        ret.setItem("Bodies", bodies);
        Py::List inlets;
        for (std::size_t at = 0; at < model.inlets.size(); ++at) {
            const auto& fed = model.inlets[at];
            Py::Dict inlet;
            double area = 0.0;
            double slant = 0.0;
            Solver::Vec3 middle {};
            Solver::faceOf(fed, area, middle, slant);
            inlet.setItem("Source", Py::String(fed.source));
            inlet.setItem("Direction", tupleOf(fed.direction));
            inlet.setItem("Speed", Py::Float(fed.speed));
            inlet.setItem("Area", Py::Float(area));
            inlet.setItem("Reynolds", Py::Float(Solver::reynolds(model, at)));
            inlet.setItem("Middle", tupleOf(middle));
            inlet.setItem("Start", Py::Float(fed.start));
            inlet.setItem("End", Py::Float(fed.end));
            inlets.append(inlet);
        }
        ret.setItem("Inlets", inlets);
        Py::Tuple open(6);
        for (std::size_t side = 0; side < 6; ++side) {
            open.setItem(side, Py::Boolean(model.space.open[side]));
        }
        ret.setItem("Open", open);
        const bool steady = Solver::fillsItsSpace(model);
        ret.setItem("Analysis",
                    Py::String(model.analysis == Solver::Analysis::Steady
                                   ? "Steady"
                                   : (steady ? "Unsteady" : "FreeSurface")));
        if (steady) {
            ret.setItem("Iterations", Py::Long(static_cast<long>(model.iterations)));
            ret.setItem("Turbulence",
                        Py::String(model.turbulence == Solver::Turbulence::KOmegaSST
                                       ? "KOmegaSST"
                                       : "Laminar"));
            Py::List outlets;
            for (const auto& let : model.outlets) {
                Py::Dict outlet;
                outlet.setItem("Source", Py::String(let.source));
                outlet.setItem("Pressure", Py::Float(let.pressure));
                outlet.setItem("Area", Py::Float(Solver::patchAreas(let.surface, 1)[0]));
                outlets.append(outlet);
            }
            ret.setItem("Outlets", outlets);
            Py::Dict domain;
            domain.setItem("Triangles",
                           Py::Long(static_cast<long>(model.domain.triangles.size())));
            domain.setItem("Volume", Py::Float(Solver::volumeOf(model.domain)));
            const std::size_t crossed = 1 + model.inlets.size() + model.outlets.size();
            const auto areas = Solver::patchAreas(model.domain, crossed + 1);
            Py::Tuple each(static_cast<int>(crossed));
            for (std::size_t i = 0; i < crossed; ++i) {
                each.setItem(i, Py::Float(areas[i]));
            }
            domain.setItem("Areas", each);
            domain.setItem("Slides", Py::Float(areas[crossed]));
            ret.setItem("Domain", domain);
        }
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
                            Py::String(index.monitors[m].kind == Solver::MonitorKind::Force
                                           ? "Force"
                                           : index.monitors[m].kind
                                                   == Solver::MonitorKind::Pressure
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
        Py::List bodies;
        for (const auto& source : index.bodies) {
            Py::Dict body;
            auto* obj = objectOf(source, getStudyPtr()->getDocument());
            body.setItem("Object", obj ? Py::asObject(obj->getPyObject()) : Py::None());
            body.setItem("Source", Py::String(source));
            bodies.append(body);
        }
        ret.setItem("Bodies", bodies);
        ret.setItem("Steady", Py::Boolean(index.steady));
        ret.setItem("Settled", Py::Boolean(index.settled));
        ret.setItem("Turbulence",
                    Py::String(index.turbulence.empty() ? "Laminar" : index.turbulence));
        ret.setItem("Reynolds", Py::Float(index.reynolds));
        Py::List steps;
        for (std::size_t k = 0; k + 1 < index.steps.size(); k += 2) {
            steps.append(Py::TupleN(Py::Float(index.steps[k]), Py::Float(index.steps[k + 1])));
        }
        ret.setItem("Steps", steps);
        if (index.wallUnits[1] > 0.0) {
            Py::Tuple units(3);
            for (std::size_t k = 0; k < 3; ++k) {
                units.setItem(k, Py::Float(index.wallUnits[k]));
            }
            ret.setItem("WallUnits", units);
        }
        else {
            ret.setItem("WallUnits", Py::None());
        }
        Py::List inside;
        for (std::size_t frame : index.inside) {
            inside.append(Py::Long(static_cast<long>(frame)));
        }
        ret.setItem("Inside", inside);
        if (!index.openings.empty()) {
            Py::List openings;
            const std::size_t count = index.openings.size();
            for (std::size_t k = 0; k < count; ++k) {
                Py::Dict opening;
                auto* obj = objectOf(index.openings[k].source, getStudyPtr()->getDocument());
                opening.setItem("Object", obj ? Py::asObject(obj->getPyObject()) : Py::None());
                opening.setItem("Source", Py::String(index.openings[k].source));
                opening.setItem("Kind", Py::String(index.openings[k].outlet ? "Outlet" : "Inlet"));
                Py::List pressures;
                Py::List flows;
                for (std::size_t f = 0; f < frames; ++f) {
                    pressures.append(Py::Float(index.flows[(f * count + k) * 2]));
                    flows.append(Py::Float(index.flows[(f * count + k) * 2 + 1]));
                }
                opening.setItem("Pressure", pressures);
                opening.setItem("Flow", flows);
                openings.append(opening);
            }
            ret.setItem("Openings", openings);
        }
        if (!index.energy.empty()) {
            Py::Dict energy;
            const auto column = [&](double Solver::Energy::* which) {
                Py::List values;
                for (const auto& at : index.energy) {
                    values.append(Py::Float(at.*which));
                }
                return values;
            };
            energy.setItem("Times", column(&Solver::Energy::time));
            energy.setItem("Kinetic", column(&Solver::Energy::kinetic));
            energy.setItem("Potential", column(&Solver::Energy::potential));
            energy.setItem("Work", column(&Solver::Energy::work));
            energy.setItem("Created", column(&Solver::Energy::created));
            energy.setItem("Exchanged", column(&Solver::Energy::exchanged));
            energy.setItem("Fed", Py::Boolean(index.fed));
            const EnergyVerdict verdict = judge(index);
            energy.setItem("Judged", Py::Boolean(verdict.kept));
            energy.setItem("BudgetJudged", Py::Boolean(verdict.kept && verdict.budget));
            energy.setItem("Rise", Py::Float(verdict.rise));
            energy.setItem("RiseLimit", Py::Float(EnergyVerdict::riseLimit));
            energy.setItem("Rose", Py::Boolean(verdict.rose));
            energy.setItem("HandOverJudged", Py::Boolean(verdict.handOver));
            energy.setItem("Made", Py::Float(verdict.made));
            energy.setItem("MadeLimit", Py::Float(EnergyVerdict::madeLimit));
            energy.setItem("Lagged", Py::Boolean(verdict.lagged));
            ret.setItem("Energy", energy);
        }
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
        ret.setItem("Bodies", motionsOf(frame.bodies));
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
        if (!frame.values.empty()) {
            ret.setItem("Values",
                        Py::asObject(PyBytes_FromStringAndSize(
                            reinterpret_cast<const char*>(frame.values.data()),
                            static_cast<Py_ssize_t>(frame.values.size() * sizeof(float)))));
        }
        return Py::new_reference_to(ret);
    }
    PY_CATCH
}

namespace
{
Py::Object bytesOf(const std::vector<float>& values)
{
    return Py::asObject(
        PyBytes_FromStringAndSize(reinterpret_cast<const char*>(values.data()),
                                  static_cast<Py_ssize_t>(values.size() * sizeof(float))));
}
}  // namespace

PyObject* StudyPy::getInside(PyObject* args) const
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
        const std::size_t count = frame.inside.size() / 7;
        std::vector<float> points;
        std::vector<float> velocity;
        std::vector<float> pressure;
        points.reserve(count * 3);
        velocity.reserve(count * 3);
        pressure.reserve(count);
        for (std::size_t c = 0; c < count; ++c) {
            const float* row = frame.inside.data() + c * 7;
            points.insert(points.end(), row, row + 3);
            velocity.insert(velocity.end(), row + 3, row + 6);
            pressure.push_back(row[6]);
        }
        Py::Dict ret;
        ret.setItem("Count", Py::Long(static_cast<long>(count)));
        ret.setItem("Points", bytesOf(points));
        ret.setItem("Velocity", bytesOf(velocity));
        ret.setItem("Pressure", bytesOf(pressure));
        return Py::new_reference_to(ret);
    }
    PY_CATCH
}

PyObject* StudyPy::getSection(PyObject* args) const
{
    long at = 0;
    PyObject* base = nullptr;
    PyObject* normal = nullptr;
    double step = 0.0;
    if (!PyArg_ParseTuple(args,
                          "lO!O!|d",
                          &at,
                          &Base::VectorPy::Type,
                          &base,
                          &Base::VectorPy::Type,
                          &normal,
                          &step)) {
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
        // results kept without the resolution: the study's own, as it is now
        double cell = results->index().spacing;
        if (!(cell > 0.0)) {
            cell = getStudyPtr()->buildModel().resolution * 1000.0;
        }
        if (!(step > 0.0)) {
            step = cell / 2.0;
        }
        const Base::Vector3d b = static_cast<Base::VectorPy*>(base)->value();
        const Base::Vector3d n = static_cast<Base::VectorPy*>(normal)->value();
        const auto section = Fluid::cut(results->frame(static_cast<std::size_t>(at)),
                                        {b.x, b.y, b.z},
                                        {n.x, n.y, n.z},
                                        step,
                                        cell);
        Py::Dict ret;
        ret.setItem("Count", Py::Long(static_cast<long>(section.pressures.size())));
        ret.setItem("Points", bytesOf(section.points));
        ret.setItem("Triangles",
                    Py::asObject(PyBytes_FromStringAndSize(
                        reinterpret_cast<const char*>(section.triangles.data()),
                        static_cast<Py_ssize_t>(section.triangles.size()
                                                * sizeof(std::int32_t)))));
        ret.setItem("Velocity", bytesOf(section.velocities));
        ret.setItem("Pressure", bytesOf(section.pressures));
        ret.setItem("Outline", bytesOf(section.outline));
        ret.setItem("Grid",
                    Py::asObject(PyBytes_FromStringAndSize(
                        reinterpret_cast<const char*>(section.grid.data()),
                        static_cast<Py_ssize_t>(section.grid.size() * sizeof(std::int32_t)))));
        ret.setItem("Step", Py::Float(step));
        return Py::new_reference_to(ret);
    }
    PY_CATCH
}

PyObject* StudyPy::getPaths(PyObject* args) const
{
    long at = 0;
    Py_buffer given {};
    double interval = 0.0;
    long most = 2000;
    if (!PyArg_ParseTuple(args, "ly*|dl", &at, &given, &interval, &most)) {
        return nullptr;
    }
    std::vector<float> seeds(static_cast<std::size_t>(given.len) / sizeof(float));
    if (!seeds.empty()) {
        std::memcpy(seeds.data(), given.buf, seeds.size() * sizeof(float));
    }
    const bool whole = static_cast<std::size_t>(given.len) % (3 * sizeof(float)) == 0;
    PyBuffer_Release(&given);
    PY_TRY
    {
        if (!whole) {
            THROWM(Base::ValueError, "the seeds are float32, three a seed: x, y, z in mm");
        }
        const auto* results = getStudyPtr()->results();
        if (!results) {
            THROWM(Base::RuntimeError, "the study keeps no results");
        }
        if (at < 0) {
            THROWM(Base::IndexError, "a frame's index is not negative");
        }
        if (most < 0) {
            THROWM(Base::ValueError, "a path has no fewer than no points");
        }
        double cell = results->index().spacing;
        if (!(cell > 0.0)) {
            cell = getStudyPtr()->buildModel().resolution * 1000.0;
        }
        const auto paths = Fluid::trace(results->frame(static_cast<std::size_t>(at)),
                                        seeds,
                                        cell,
                                        interval,
                                        static_cast<std::size_t>(most));
        Py::Dict ret;
        ret.setItem("Count",
                    Py::Long(static_cast<long>(paths.starts.empty() ? 0 : paths.starts.size() - 1)));
        ret.setItem("Points", bytesOf(paths.points));
        ret.setItem("Speeds", bytesOf(paths.speeds));
        ret.setItem("Starts",
                    Py::asObject(PyBytes_FromStringAndSize(
                        reinterpret_cast<const char*>(paths.starts.data()),
                        static_cast<Py_ssize_t>(paths.starts.size() * sizeof(std::int32_t)))));
        ret.setItem("Interval", Py::Float(paths.interval));
        return Py::new_reference_to(ret);
    }
    PY_CATCH
}

PyObject* StudyPy::getBodies(PyObject* args) const
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
        return Py::new_reference_to(motionsOf(results->bodies(static_cast<std::size_t>(at))));
    }
    PY_CATCH
}

PyObject* StudyPy::getSurface(PyObject* args) const
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
        const auto bytesOf = [](const void* data, std::size_t size) {
            return Py::asObject(PyBytes_FromStringAndSize(static_cast<const char*>(data),
                                                          static_cast<Py_ssize_t>(size)));
        };
        Py::Dict ret;
        ret.setItem("Time", Py::Float(frame.time));
        ret.setItem("Bodies", motionsOf(frame.bodies));
        if (!frame.triangles.empty()) {
            ret.setItem("Count", Py::Long(static_cast<long>(frame.points.size() / 3)));
            ret.setItem("Points", bytesOf(frame.points.data(), frame.points.size() * sizeof(float)));
            ret.setItem("Triangles",
                        bytesOf(frame.triangles.data(),
                                frame.triangles.size() * sizeof(std::int32_t)));
            return Py::new_reference_to(ret);
        }
        // results kept before the spacing was: the study's own, as it is now
        double spacing = results->index().spacing;
        if (!(spacing > 0.0)) {
            spacing = getStudyPtr()->buildModel().resolution * 1000.0;
        }
        std::vector<Solver::Vec3> particles(frame.points.size() / 3);
        for (std::size_t i = 0; i < particles.size(); ++i) {
            particles[i] = {frame.points[3 * i], frame.points[3 * i + 1], frame.points[3 * i + 2]};
        }
        ParticleSurface surface;
        // the walls given: beside one the field would hover about the level
        surfaceOf(particles,
                  spacing,
                  surface,
                  SprayBelow,
                  getStudyPtr()->wallPointsAt(frame.bodies, spacing));
        std::vector<float> spray(surface.spray.size() * 3);
        for (std::size_t i = 0; i < surface.spray.size(); ++i) {
            for (std::size_t x = 0; x < 3; ++x) {
                spray[3 * i + x] = static_cast<float>(surface.spray[i][x]);
            }
        }
        ret.setItem("Spray", bytesOf(spray.data(), spray.size() * sizeof(float)));
        std::vector<float> points(surface.points.size() * 3);
        std::vector<float> normals(surface.points.size() * 3);
        for (std::size_t i = 0; i < surface.points.size(); ++i) {
            for (std::size_t x = 0; x < 3; ++x) {
                points[3 * i + x] = static_cast<float>(surface.points[i][x]);
                normals[3 * i + x] = static_cast<float>(surface.normals[i][x]);
            }
        }
        std::vector<std::int32_t> triangles(surface.triangles.size() * 3);
        for (std::size_t i = 0; i < surface.triangles.size(); ++i) {
            for (std::size_t x = 0; x < 3; ++x) {
                triangles[3 * i + x] = surface.triangles[i][x];
            }
        }
        ret.setItem("Count", Py::Long(static_cast<long>(surface.points.size())));
        ret.setItem("Points", bytesOf(points.data(), points.size() * sizeof(float)));
        ret.setItem("Normals", bytesOf(normals.data(), normals.size() * sizeof(float)));
        ret.setItem("Triangles", bytesOf(triangles.data(), triangles.size() * sizeof(std::int32_t)));
        return Py::new_reference_to(ret);
    }
    PY_CATCH
}

PyObject* StudyPy::getWallPoints(PyObject* args) const
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
        double spacing = results->index().spacing;
        if (!(spacing > 0.0)) {
            spacing = getStudyPtr()->buildModel().resolution * 1000.0;
        }
        const auto walls = getStudyPtr()->wallPointsAt(results->bodies(static_cast<std::size_t>(at)),
                                                       spacing);
        std::vector<float> points(walls.size() * 3);
        std::vector<float> normals(walls.size() * 3);
        for (std::size_t i = 0; i < walls.size(); ++i) {
            for (std::size_t x = 0; x < 3; ++x) {
                points[3 * i + x] = static_cast<float>(walls[i].at[x]);
                normals[3 * i + x] = static_cast<float>(walls[i].out[x]);
            }
        }
        const auto bytesOf = [](const std::vector<float>& data) {
            return Py::asObject(PyBytes_FromStringAndSize(reinterpret_cast<const char*>(data.data()),
                                                          static_cast<Py_ssize_t>(data.size() * sizeof(float))));
        };
        Py::Dict ret;
        ret.setItem("Points", bytesOf(points));
        ret.setItem("Normals", bytesOf(normals));
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
