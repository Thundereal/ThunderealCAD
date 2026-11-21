// SPDX-License-Identifier: LGPL-2.1-or-later

#include <algorithm>
#include <atomic>
#include <cmath>
#include <thread>

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

    // The shapes first: the resolution left to us comes from the fluid's box
    struct Taken
    {
        const App::DocumentObject* source;
        Part::TopoShape shape;
        Solver::Material material;
    };
    std::vector<Taken> walls;
    std::vector<Taken> fluids;
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
        else if (auto* monitor = freecad_cast<Monitor*>(child)) {
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
        model.walls.push_back({sourceOf(wall.source, getDocument()), surfaceOf(wall.shape, accuracy)});
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
    if (run->model.fluids.empty()) {
        why = "the study has no fluid";
        Status.setValue(why);
        return Solver::Status::Empty;
    }
    run->session = backend->createSession();
    run->path = Results.getExchangeTempFile();
    run->writer = std::make_unique<ArchiveWriter>(run->path);
    run->index.backend = backend->name();
    run->index.version = availability.version;
    run->index.kind = backend->capabilities().results;
    run->index.signature = Solver::signature(run->model);
    for (const auto& monitor : run->model.monitors) {
        run->index.monitors.push_back({monitor.source, monitor.kind});
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

// =============================== Monitor ===============================

PROPERTY_SOURCE(Fluid::Monitor, App::DocumentObject)

const char* Monitor::KindEnums[] = {"Height", "Pressure", nullptr};

Monitor::Monitor()
{
    ADD_PROPERTY_TYPE(Kind, (0L), "Monitor", App::Prop_None,
                      "Height: of the fluid's surface above the point, against gravity. "
                      "Pressure: at the point");
    Kind.setEnums(KindEnums);
    ADD_PROPERTY_TYPE(Point, (Base::Vector3d()), "Monitor", App::Prop_None,
                      "Where, in the document's coordinates");
}
