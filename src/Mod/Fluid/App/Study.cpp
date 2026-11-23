// SPDX-License-Identifier: LGPL-2.1-or-later

#include <algorithm>
#include <atomic>
#include <cmath>
#include <thread>

#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <gp_Mat.hxx>

#include <App/Document.h>
#include <Base/Console.h>
#include <Base/Exception.h>
#include <Base/FileInfo.h>
#include <Mod/Part/App/PartFeature.h>
#include <Mod/Part/App/TopoShape.h>

#include "Solver/Registry.h"
#include "Study.h"
#include "StudyPy.h"

using namespace Fluid;

namespace
{
// What the model and the kept results name an object by: its name in the
// study's document, "Document#Object" in another -- a document saved under
// another name still finds its own.
std::string sourceOf(const App::DocumentObject* obj, const App::Document* home)
{
    if (obj->getDocument() == home || !obj->getDocument()) {
        return obj->getNameInDocument();
    }
    return std::string(obj->getDocument()->getName()) + "#" + obj->getNameInDocument();
}

// The object's shape where it stands, which must hold a solid.
Part::TopoShape solidOf(const App::DocumentObject* obj, const char* role)
{
    Part::TopoShape shape = Part::Feature::getTopoShape(obj);
    if (shape.isNull() || shape.countSubShapes("Solid") == 0) {
        FC_THROWM(Base::ValueError,
                  role << " " << obj->getFullLabel() << " has no solid shape");
    }
    return shape;
}

// mm to m
Solver::Surface surfaceOf(const Part::TopoShape& shape, double accuracy)
{
    std::vector<Base::Vector3d> points;
    std::vector<Data::ComplexGeoData::Facet> facets;
    shape.getFaces(points, facets, accuracy);
    Solver::Surface surface;
    surface.points.reserve(points.size());
    for (const auto& p : points) {
        surface.points.push_back({p.x * 1.0e-3, p.y * 1.0e-3, p.z * 1.0e-3});
    }
    surface.triangles.reserve(facets.size());
    for (const auto& f : facets) {
        surface.triangles.push_back(
            {static_cast<int>(f.I1), static_cast<int>(f.I2), static_cast<int>(f.I3)});
    }
    return surface;
}

// What a shape of that density weighs and how: mm and kg/mm^3 to the
// model's units.
Solver::MassProps massOf(const Part::TopoShape& shape, double density)
{
    GProp_GProps props;
    BRepGProp::VolumeProperties(shape.getShape(), props);
    // a reversed solid's volume and moments come negative
    const double sign = props.Mass() < 0.0 ? -1.0 : 1.0;
    Solver::MassProps mass;
    mass.mass = sign * props.Mass() * density;
    const gp_Pnt centre = props.CentreOfMass();
    mass.centre = {centre.X() * 1.0e-3, centre.Y() * 1.0e-3, centre.Z() * 1.0e-3};
    // about the centre of mass, per unit density
    const gp_Mat m = props.MatrixOfInertia();
    const double scale = sign * density * 1.0e-6;
    mass.inertia = {m(1, 1) * scale,
                    m(2, 2) * scale,
                    m(3, 3) * scale,
                    m(1, 2) * scale,
                    m(1, 3) * scale,
                    m(2, 3) * scale};
    return mass;
}

const char* statusText(Solver::Status status)
{
    switch (status) {
        case Solver::Status::Ok:
            return "Done";
        case Solver::Status::Unsupported:
            return "Unsupported";
        case Solver::Status::NotFound:
            return "Not found";
        case Solver::Status::Empty:
            return "Nothing to simulate";
        case Solver::Status::Cancelled:
            return "Cancelled";
        default:
            return "Failed";
    }
}
}  // namespace

// The run in its thread: the frames go straight into the archive.
struct Study::Run: public Solver::FrameSink
{
    void frame(const Solver::Frame& frame) override
    {
        writer->frame(frame);
        count.store(writer->frames());
        time.store(frame.time);
    }
    void sample(double at, const std::vector<double>& values) override
    {
        writer->sample(at, values);
    }
    bool cancelled() const override
    {
        return cancel.load();
    }
    void body()
    {
        try {
            status = session->prepare(model, why);
            if (status == Solver::Status::Ok) {
                status = session->run(*this, why);
            }
            writer->close(index);
        }
        catch (const Base::Exception& e) {
            status = Solver::Status::Failed;
            why = e.what();
        }
        catch (const std::exception& e) {
            status = Solver::Status::Failed;
            why = e.what();
        }
        done.store(true);
    }

    std::unique_ptr<Solver::Session> session;
    Solver::Model model;
    std::unique_ptr<ArchiveWriter> writer;
    ResultsIndex index;
    std::string path;
    std::thread thread;
    std::atomic<bool> cancel {false};
    std::atomic<bool> done {false};
    std::atomic<std::size_t> count {0};
    std::atomic<double> time {0.0};
    Solver::Status status = Solver::Status::Failed;
    std::string why;
};

// ================================ Study ================================

PROPERTY_SOURCE_WITH_EXTENSIONS(Fluid::Study, App::DocumentObject)

Study::Study()
{
    GroupExtension::initExtension(this);

    ADD_PROPERTY_TYPE(Walls, (nullptr), "Study", App::Prop_None,
                      "The parts the fluid cannot pass");
    ADD_PROPERTY_TYPE(TimeEnd, (1.0), "Study", App::Prop_None, "How long the run is");
    ADD_PROPERTY_TYPE(OutputInterval, (1.0e-2), "Study", App::Prop_None,
                      "The time between the frames the run keeps");
    ADD_PROPERTY_TYPE(Gravity, (Base::Vector3d(0.0, 0.0, -9806.65)), "Study", App::Prop_None,
                      "Gravity, in the document's coordinates, mm/s^2");
    ADD_PROPERTY_TYPE(Resolution, (0.0), "Study", App::Prop_None,
                      "The size of what is to be resolved: the spacing of particles, or the "
                      "base size of cells. 0: a twentieth of the fluid's smallest side");
    ADD_PROPERTY_TYPE(OpenTop, (true), "Study", App::Prop_None,
                      "The space is the box around the walls and the fluid. Its side "
                      "against gravity is open to the air, and what leaves through it is "
                      "gone; or it is closed like the others");
    ADD_PROPERTY_TYPE(OpenSides, (false), "Study", App::Prop_None,
                      "The space's other sides are open as well, the one gravity points "
                      "to with them: what leaves the space anywhere is gone");
    ADD_PROPERTY_TYPE(Backend, (""), "Study", App::Prop_None,
                      "The solver the study runs on, by name; empty: the first that can run "
                      "on this machine");
    ADD_PROPERTY_TYPE(Results, (nullptr), "Results", App::Prop_Hidden,
                      "The last run's results, as they are kept in the document");
    ADD_PROPERTY_TYPE(Status, (""), "Results",
                      static_cast<App::PropertyType>(App::Prop_ReadOnly | App::Prop_Output),
                      "How the last run ended");
}

Study::~Study()
{
    if (active) {
        active->cancel.store(true);
        if (active->thread.joinable()) {
            active->thread.join();
        }
    }
}

PyObject* Study::getPyObject()
{
    if (PythonObject.is(Py::_None())) {
        // ref counter is set to 1
        PythonObject = Py::Object(new StudyPy(this), true);
    }
    return Py::new_reference_to(PythonObject);
}

void Study::unsetupObject()
{
    if (active) {
        std::string why;
        cancel();
        finish(why);
    }
    App::DocumentObject::unsetupObject();
}

void Study::onChanged(const App::Property* prop)
{
    if (prop == &Results) {
        reader.reset();
        readerPath.clear();
    }
    App::DocumentObject::onChanged(prop);
}

Solver::Model Study::buildModel() const
{
    Solver::Model model;
    const Base::Vector3d g = Gravity.getValue();
    model.gravity = {g.x * 1.0e-3, g.y * 1.0e-3, g.z * 1.0e-3};
    model.timeEnd = TimeEnd.getValue();
    model.outputInterval = OutputInterval.getValue();
    // the side gravity points away from most: the top
    const std::size_t axis = std::abs(g.x) >= std::abs(g.y) && std::abs(g.x) >= std::abs(g.z)
        ? 0
        : (std::abs(g.y) >= std::abs(g.z) ? 1 : 2);
    const std::size_t top = 2 * axis + (model.gravity[axis] < 0.0 ? 1 : 0);
    for (std::size_t side = 0; side < 6; ++side) {
        // without gravity no side is the top: they are all the others
        const bool isTop = g.Length() > 0.0 && side == top;
        model.space.open[side] = isTop ? OpenTop.getValue() : OpenSides.getValue();
    }

    // The shapes first: the resolution left to us comes from the fluid's box
    struct Taken
    {
        const App::DocumentObject* source;
        Part::TopoShape shape;
        Solver::Material material;
        double density = 0.0;  // of a part the fluid moves, kg/mm^3
    };
    std::vector<Taken> walls;
    std::vector<Taken> fluids;
    struct Fed
    {
        const Inlet* source;
        Part::TopoShape face;
    };
    std::vector<Fed> inlets;
    struct Asked
    {
        const Monitor* source;
        const App::DocumentObject* part;
    };
    std::vector<Asked> forces;
    Base::BoundBox3d space;
    double smallest = 0.0;
    for (auto* wall : Walls.getValues()) {
        if (wall) {
            walls.push_back({wall, solidOf(wall, "the wall"), {}});
            space.Add(walls.back().shape.getBoundBox());
        }
    }
    for (auto* child : Group.getValues()) {
        if (auto* fluid = freecad_cast<InitialFluid*>(child)) {
            auto* volume = fluid->Volume.getValue();
            if (!volume) {
                FC_THROWM(Base::ValueError, fluid->getFullLabel() << " has no volume");
            }
            // kg/mm^3 and kg/(mm s), the document's units
            Solver::Material material {fluid->Density.getValue() * 1.0e9,
                                       fluid->Viscosity.getValue() * 1.0e3};
            fluids.push_back({fluid, solidOf(volume, "the fluid volume"), material});
            const Base::BoundBox3d box = fluids.back().shape.getBoundBox();
            space.Add(box);
            const double side = std::min({box.LengthX(), box.LengthY(), box.LengthZ()});
            smallest = smallest == 0.0 ? side : std::min(smallest, side);
        }
        else if (auto* body = freecad_cast<FreeBody*>(child)) {
            auto* part = body->Part.getValue();
            if (!part) {
                FC_THROWM(Base::ValueError, body->getFullLabel() << " has no part");
            }
            for (const auto& wall : walls) {
                if (wall.source == part) {
                    FC_THROWM(Base::ValueError,
                              part->getFullLabel() << " is a wall of the study and "
                                                   << body->getFullLabel()
                                                   << "'s part: it is held, or it is moved");
                }
            }
            if (!(body->Density.getValue() > 0.0)) {
                FC_THROWM(Base::ValueError,
                          body->getFullLabel() << " has no density: what the fluid moves "
                                                  "has to weigh something");
            }
            // named by its part: that is what the results move
            walls.push_back({part, solidOf(part, "the part"), {}, body->Density.getValue()});
            space.Add(walls.back().shape.getBoundBox());
        }
        else if (auto* inlet = freecad_cast<Inlet*>(child)) {
            auto* owner = inlet->Face.getValue();
            const auto& subs = inlet->Face.getSubValues();
            if (!owner || subs.size() != 1) {
                FC_THROWM(Base::ValueError, inlet->getFullLabel() << " has no face, or several");
            }
            Part::TopoShape face = Part::Feature::getTopoShape(owner, subs.front().c_str(), true);
            if (face.isNull() || face.countSubShapes("Face") != 1) {
                FC_THROWM(Base::ValueError,
                          inlet->getFullLabel() << ": " << subs.front() << " of "
                                                << owner->getFullLabel() << " is not a face");
            }
            if (!(inlet->Velocity.getValue() > 0.0)) {
                FC_THROWM(Base::ValueError,
                          inlet->getFullLabel() << " has no speed: the fluid comes in at "
                                                   "more than nothing");
            }
            inlets.push_back({inlet, face});
            space.Add(face.getBoundBox());
        }
        else if (auto* monitor = freecad_cast<Monitor*>(child)) {
            if (monitor->Kind.getValue() == 2) {
                if (!monitor->Part.getValue()) {
                    FC_THROWM(Base::ValueError,
                              monitor->getFullLabel() << " has no part to report the force on");
                }
                if (!(monitor->Direction.getValue().Length() > 0.0)) {
                    FC_THROWM(Base::ValueError,
                              monitor->getFullLabel()
                                  << " has no direction to take the force along");
                }
                // which wall it is, once they all are known
                forces.push_back({monitor, monitor->Part.getValue()});
                model.monitors.push_back(
                    {sourceOf(monitor, getDocument()), Solver::MonitorKind::Force, {}});
                Base::Vector3d along = monitor->Direction.getValue();
                along.Normalize();
                model.monitors.back().direction = {along.x, along.y, along.z};
                continue;
            }
            const Base::Vector3d p = monitor->Point.getValue();
            model.monitors.push_back({sourceOf(monitor, getDocument()),
                                      monitor->Kind.getValue() == 1 ? Solver::MonitorKind::Pressure
                                                                    : Solver::MonitorKind::Height,
                                      {p.x * 1.0e-3, p.y * 1.0e-3, p.z * 1.0e-3}});
        }
    }
    double resolution = Resolution.getValue();  // mm
    if (resolution <= 0.0) {
        resolution = smallest / 20.0;
    }
    model.resolution = resolution * 1.0e-3;
    const double accuracy = std::max(resolution * 0.25, 1.0e-3);
    for (const auto& wall : walls) {
        model.walls.emplace_back();
        model.walls.back().source = sourceOf(wall.source, getDocument());
        model.walls.back().surface = surfaceOf(wall.shape, accuracy);
        if (wall.density > 0.0) {
            model.walls.back().motion = Solver::WallMotion::Free;
            model.walls.back().mass = massOf(wall.shape, wall.density);
        }
    }
    for (const auto& asked : forces) {
        int found = -1;
        for (std::size_t k = 0; k < walls.size(); ++k) {
            if (walls[k].source == asked.part && !(walls[k].density > 0.0)) {
                found = static_cast<int>(k);
            }
        }
        if (found < 0) {
            FC_THROWM(Base::ValueError,
                      asked.source->getFullLabel()
                          << " asks the force on " << asked.part->getFullLabel()
                          << ", which is not one of the study's walls");
        }
        for (auto& monitor : model.monitors) {
            if (monitor.source == sourceOf(asked.source, getDocument())) {
                monitor.wall = found;
            }
        }
    }
    for (const auto& fed : inlets) {
        Solver::Inlet inlet;
        inlet.source = sourceOf(fed.source, getDocument());
        inlet.surface = surfaceOf(fed.face, accuracy);
        // Out of the solid the face is of: the way its triangles face
        Base::Vector3d out;
        for (const auto& t : inlet.surface.triangles) {
            const auto& a = inlet.surface.points[static_cast<std::size_t>(t[0])];
            const auto& b = inlet.surface.points[static_cast<std::size_t>(t[1])];
            const auto& c = inlet.surface.points[static_cast<std::size_t>(t[2])];
            out += (Base::Vector3d(b[0] - a[0], b[1] - a[1], b[2] - a[2]))
                       .Cross(Base::Vector3d(c[0] - a[0], c[1] - a[1], c[2] - a[2]));
        }
        if (!(out.Length() > 0.0)) {
            FC_THROWM(Base::ValueError, fed.source->getFullLabel() << "'s face has no area");
        }
        out.Normalize();
        if (fed.source->Reversed.getValue()) {
            out = -out;
        }
        inlet.direction = {out.x, out.y, out.z};
        inlet.speed = fed.source->Velocity.getValue() * 1.0e-3;
        inlet.material = {fed.source->Density.getValue() * 1.0e9,
                          fed.source->Viscosity.getValue() * 1.0e3};
        inlet.start = fed.source->StartTime.getValue();
        inlet.end = fed.source->EndTime.getValue();
        double area = 0.0;
        double slant = 0.0;
        Solver::Vec3 middle {};
        if (!Solver::faceOf(inlet, area, middle, slant) || slant > 1.0e-3) {
            FC_THROWM(Base::ValueError,
                      fed.source->getFullLabel() << "'s face is not flat: the fluid comes "
                                                    "in through a flat face, square to it");
        }
        model.inlets.push_back(inlet);
    }
    for (const auto& fluid : fluids) {
        model.fluids.push_back(
            {sourceOf(fluid.source, getDocument()), surfaceOf(fluid.shape, accuracy), fluid.material});
    }
    if (space.IsValid()) {
        model.space.min = {space.MinX * 1.0e-3, space.MinY * 1.0e-3, space.MinZ * 1.0e-3};
        model.space.max = {space.MaxX * 1.0e-3, space.MaxY * 1.0e-3, space.MaxZ * 1.0e-3};
    }
    return model;
}

Solver::Status Study::start(std::string& why)
{
    if (active) {
        why = "the study is running already";
        return Solver::Status::Failed;
    }
    const std::string name = Backend.getValue();
    auto* backend = Solver::Registry::instance().select(name);
    if (!backend) {
        why = name.empty() ? std::string("no solver that can run on this machine was found")
                           : "there is no solver named " + name;
        Status.setValue(why);
        return Solver::Status::NotFound;
    }
    const auto availability = backend->availability();
    if (!availability.available) {
        why = backend->label() + " cannot run here: " + availability.reason;
        Status.setValue(why);
        return Solver::Status::NotFound;
    }
    auto run = std::make_unique<Run>();
    run->model = buildModel();
    if (run->model.fluids.empty() && run->model.inlets.empty()) {
        why = "the study has no fluid";
        Status.setValue(why);
        return Solver::Status::Empty;
    }
    const auto moved = Solver::freeWalls(run->model);
    if (!moved.empty() && !backend->capabilities().freeBodies) {
        why = backend->label() + " cannot move a part by the fluid";
        Status.setValue(std::string(statusText(Solver::Status::Unsupported)) + ": " + why);
        return Solver::Status::Unsupported;
    }
    if (!run->model.inlets.empty() && !backend->capabilities().inlets) {
        why = backend->label() + " cannot run an inlet";
        Status.setValue(std::string(statusText(Solver::Status::Unsupported)) + ": " + why);
        return Solver::Status::Unsupported;
    }
    for (const auto& monitor : run->model.monitors) {
        if (monitor.kind == Solver::MonitorKind::Force && !backend->capabilities().forces) {
            why = backend->label() + " cannot report the force on a part";
            Status.setValue(std::string(statusText(Solver::Status::Unsupported)) + ": " + why);
            return Solver::Status::Unsupported;
        }
    }
    run->session = backend->createSession();
    run->path = Results.getExchangeTempFile();
    run->writer = std::make_unique<ArchiveWriter>(run->path);
    run->index.backend = backend->name();
    run->index.version = availability.version;
    run->index.kind = backend->capabilities().results;
    run->index.signature = Solver::signature(run->model);
    run->index.spacing = run->model.resolution * 1000.0;
    for (const auto& monitor : run->model.monitors) {
        run->index.monitors.push_back({monitor.source, monitor.kind});
    }
    for (std::size_t wall : moved) {
        run->index.bodies.push_back(run->model.walls[wall].source);
    }
    active = std::move(run);
    Status.setValue("Running");
    active->thread = std::thread([run = active.get()] {
        run->body();
    });
    return Solver::Status::Ok;
}

Study::Progress Study::progress() const
{
    Progress progress;
    if (active) {
        progress.running = true;
        progress.done = active->done.load();
        progress.time = active->time.load();
        progress.frames = active->count.load();
    }
    return progress;
}

void Study::cancel()
{
    if (active) {
        active->cancel.store(true);
    }
}

Solver::Status Study::finish(std::string& why)
{
    if (!active) {
        why = "the study is not running";
        return Solver::Status::Failed;
    }
    if (active->thread.joinable()) {
        active->thread.join();
    }
    const auto run = std::move(active);
    active.reset();
    why = run->why;
    if (run->status == Solver::Status::Ok) {
        Results.setValue(run->path.c_str(), "FluidResults.zip");
        Status.setValue(statusText(run->status));
    }
    else {
        Base::FileInfo(run->path).deleteFile();
        Status.setValue(why.empty() ? std::string(statusText(run->status))
                                    : std::string(statusText(run->status)) + ": " + why);
    }
    return run->status;
}

Solver::Status Study::run(std::string& why)
{
    const auto started = start(why);
    return started == Solver::Status::Ok ? finish(why) : started;
}

const ArchiveReader* Study::results() const
{
    if (Results.isEmpty()) {
        return nullptr;
    }
    const std::string path = Results.getValue();
    if (!reader || readerPath != path) {
        reader = std::make_unique<ArchiveReader>(path);
        readerPath = path;
    }
    return reader.get();
}

bool Study::isStale() const
{
    const auto* kept = results();
    return kept && kept->index().signature != Solver::signature(buildModel());
}

// ============================ InitialFluid =============================

PROPERTY_SOURCE(Fluid::InitialFluid, App::DocumentObject)

InitialFluid::InitialFluid()
{
    ADD_PROPERTY_TYPE(Volume, (nullptr), "Fluid", App::Prop_None,
                      "The solid that is full of fluid when the run starts");
    // water at 20 degrees: 998.2 kg/m^3, 1.002 mPa s
    ADD_PROPERTY_TYPE(Density, (998.2e-9), "Fluid", App::Prop_None, "The fluid's density");
    ADD_PROPERTY_TYPE(Viscosity, (1.002e-6), "Fluid", App::Prop_None,
                      "The fluid's dynamic viscosity");
}

// ============================== FreeBody ===============================

PROPERTY_SOURCE(Fluid::FreeBody, App::DocumentObject)

FreeBody::FreeBody()
{
    ADD_PROPERTY_TYPE(Part, (nullptr), "Body", App::Prop_None,
                      "The solid the fluid moves: a wall to it, held by nothing");
    ADD_PROPERTY_TYPE(Density, (0.0), "Body", App::Prop_None,
                      "The solid's density, the same all through");
}

// =============================== Monitor ===============================

PROPERTY_SOURCE(Fluid::Monitor, App::DocumentObject)

const char* Monitor::KindEnums[] = {"Height", "Pressure", "Force", nullptr};

Monitor::Monitor()
{
    ADD_PROPERTY_TYPE(Kind, (0L), "Monitor", App::Prop_None,
                      "Height: of the fluid's surface above the point, against gravity. "
                      "Pressure: at the point. Force: the fluid's on the part, along the "
                      "direction");
    Kind.setEnums(KindEnums);
    ADD_PROPERTY_TYPE(Point, (Base::Vector3d()), "Monitor", App::Prop_None,
                      "Where, in the document's coordinates");
    ADD_PROPERTY_TYPE(Part, (nullptr), "Monitor", App::Prop_None,
                      "A force: the wall of the study the fluid's force is on");
    ADD_PROPERTY_TYPE(Direction, (Base::Vector3d(0.0, 0.0, 1.0)), "Monitor", App::Prop_None,
                      "A force: the way it is taken along, in the document's coordinates");
}

// ================================ Inlet ================================

PROPERTY_SOURCE(Fluid::Inlet, App::DocumentObject)

Inlet::Inlet()
{
    ADD_PROPERTY_TYPE(Face, (nullptr), "Inlet", App::Prop_None,
                      "The flat face the fluid comes in through");
    ADD_PROPERTY_TYPE(Velocity, (0.0), "Inlet", App::Prop_None,
                      "The fluid's speed through the face, the same all over it");
    ADD_PROPERTY_TYPE(Reversed, (false), "Inlet", App::Prop_None,
                      "The fluid goes out of the solid the face is of; or the other way");
    ADD_PROPERTY_TYPE(Density, (998.2e-9), "Inlet", App::Prop_None, "The fluid's density");
    ADD_PROPERTY_TYPE(Viscosity, (1.002e-6), "Inlet", App::Prop_None,
                      "The fluid's dynamic viscosity");
    ADD_PROPERTY_TYPE(StartTime, (0.0), "Inlet", App::Prop_None, "From when the fluid comes");
    ADD_PROPERTY_TYPE(EndTime, (0.0), "Inlet", App::Prop_None,
                      "Until when: not after the start, to the run's end");
}
