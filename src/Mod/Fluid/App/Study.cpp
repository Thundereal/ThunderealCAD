// SPDX-License-Identifier: LGPL-2.1-or-later

#include <algorithm>
#include <atomic>
#include <cmath>
#include <thread>

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <GeomLib_IsPlanarSurface.hxx>
#include <Poly_Triangulation.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Mat.hxx>

#include <App/Document.h>
#include <Base/Console.h>
#include <Base/Exception.h>
#include <Base/FileInfo.h>
#include <Base/Rotation.h>
#include <Mod/Part/App/PartFeature.h>
#include <Mod/Part/App/TopoShape.h>

#include "Solver/Registry.h"
#include "Solver/WallExchange.h"
#include "Study.h"
#ifdef FLUID_WITH_ASSEMBLY
# include <Mod/Assembly/App/AssemblyObject.h>
# include <Mod/Assembly/App/Simulation.h>
# include <Mod/Assembly/App/Solver/Backend.h>
#endif
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

// mm to m. The triangles are of a copy of the shape: the shape's own are
// the 3D view's, as fine as it last drew them, and a model that took those
// would be another each time they changed -- its results read as stale.
Solver::Surface meshOf(const Part::TopoShape& shape, double accuracy)
{
    std::vector<Base::Vector3d> points;
    std::vector<Data::ComplexGeoData::Facet> facets;
    const Part::TopoShape own(BRepBuilderAPI_Copy(shape.getShape(), false, false).Shape());
    own.getFaces(points, facets, accuracy);
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

// The closed spaces inside the parts: what is left of a box around them
// when they are taken out of it, less the piece that reaches the box.
std::vector<TopoDS_Shape> spacesInside(const std::vector<Part::TopoShape>& parts)
{
    Bnd_Box all;
    TopTools_ListOfShape tools;
    for (const auto& part : parts) {
        BRepBndLib::Add(part.getShape(), all);
        tools.Append(part.getShape());
    }
    std::vector<TopoDS_Shape> spaces;
    if (all.IsVoid()) {
        return spaces;
    }
    double x0 = 0.0, y0 = 0.0, z0 = 0.0, x1 = 0.0, y1 = 0.0, z1 = 0.0;
    all.Get(x0, y0, z0, x1, y1, z1);
    const double margin = 0.1 * std::max({x1 - x0, y1 - y0, z1 - z0, 1.0});
    const gp_Pnt low(x0 - margin, y0 - margin, z0 - margin);
    const gp_Pnt high(x1 + margin, y1 + margin, z1 + margin);
    TopTools_ListOfShape arguments;
    arguments.Append(BRepPrimAPI_MakeBox(low, high).Shape());
    BRepAlgoAPI_Cut cut;
    cut.SetArguments(arguments);
    cut.SetTools(tools);
    cut.Build();
    if (!cut.IsDone()) {
        THROWM(Base::ValueError, "the space inside the walls could not be worked out");
    }
    for (TopExp_Explorer each(cut.Shape(), TopAbs_SOLID); each.More(); each.Next()) {
        Bnd_Box box;
        BRepBndLib::Add(each.Current(), box);
        double a0 = 0.0, b0 = 0.0, c0 = 0.0, a1 = 0.0, b1 = 0.0, c1 = 0.0;
        box.Get(a0, b0, c0, a1, b1, c1);
        // the piece outside the walls is the one the box bounds
        const double reach = 0.5 * margin;
        const bool outside = a0 < low.X() + reach || b0 < low.Y() + reach || c0 < low.Z() + reach
            || a1 > high.X() - reach || b1 > high.Y() - reach || c1 > high.Z() - reach;
        if (!outside) {
            // a copy: its faces are meshed anew, and those the cut left as
            // they were are the walls' own, with the 3D view's triangles
            spaces.push_back(BRepBuilderAPI_Copy(each.Current(), false, false).Shape());
        }
    }
    return spaces;
}

// A face of a space as triangles wound out of the space, mm to m, added to
// the surface under that patch. The space must be meshed already. Returns
// a point inside the face, in mm; false when the face has no triangles.
bool addFace(const TopoDS_Face& face, int patch, Solver::Surface& surface, gp_Pnt& within)
{
    TopLoc_Location location;
    const Handle(Poly_Triangulation) mesh = BRep_Tool::Triangulation(face, location);
    if (mesh.IsNull() || mesh->NbTriangles() == 0) {
        return false;
    }
    const gp_Trsf placed = location.Transformation();
    const int first = static_cast<int>(surface.points.size());
    for (int i = 1; i <= mesh->NbNodes(); ++i) {
        const gp_Pnt p = mesh->Node(i).Transformed(placed);
        surface.points.push_back({p.X() * 1.0e-3, p.Y() * 1.0e-3, p.Z() * 1.0e-3});
    }
    const bool reversed = face.Orientation() == TopAbs_REVERSED;
    double largest = -1.0;
    for (int i = 1; i <= mesh->NbTriangles(); ++i) {
        int a = 0, b = 0, c = 0;
        mesh->Triangle(i).Get(a, b, c);
        if (reversed) {
            std::swap(b, c);
        }
        surface.triangles.push_back({first + a - 1, first + b - 1, first + c - 1});
        surface.patches.push_back(patch);
        const gp_Pnt pa = mesh->Node(a).Transformed(placed);
        const gp_Pnt pb = mesh->Node(b).Transformed(placed);
        const gp_Pnt pc = mesh->Node(c).Transformed(placed);
        const double area = gp_Vec(pa, pb).Crossed(gp_Vec(pa, pc)).Magnitude();
        if (area > largest) {
            largest = area;
            within = gp_Pnt((pa.XYZ() + pb.XYZ() + pc.XYZ()) / 3.0);
        }
    }
    return true;
}

// Which way a flat face looks, from its triangles; false when it has no
// area or is not flat.
bool flatOf(const Solver::Surface& surface, Base::Vector3d& out, bool& hasArea)
{
    out = Base::Vector3d();
    for (const auto& t : surface.triangles) {
        const auto& a = surface.points[static_cast<std::size_t>(t[0])];
        const auto& b = surface.points[static_cast<std::size_t>(t[1])];
        const auto& c = surface.points[static_cast<std::size_t>(t[2])];
        out += (Base::Vector3d(b[0] - a[0], b[1] - a[1], b[2] - a[2]))
                   .Cross(Base::Vector3d(c[0] - a[0], c[1] - a[1], c[2] - a[2]));
    }
    hasArea = out.Length() > 0.0;
    if (!hasArea) {
        return false;
    }
    out.Normalize();
    Solver::Inlet probe;
    probe.surface = surface;
    probe.direction = {out.x, out.y, out.z};
    double area = 0.0;
    double slant = 0.0;
    Solver::Vec3 middle {};
    return Solver::faceOf(probe, area, middle, slant) && slant <= 1.0e-3;
}

Solver::Analysis analysisOf(long chosen)
{
    return chosen == 1 ? Solver::Analysis::Steady
                       : (chosen == 2 ? Solver::Analysis::Unsteady : Solver::Analysis::FreeSurface);
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
#ifdef FLUID_WITH_ASSEMBLY
namespace
{
// The fluid's run and an Assembly's simulation together
// (docs/FluidSimulation.md sec 12.9): to the Assembly a source of loads on
// its parts, to the fluid's session the mover of its driven walls. The
// Assembly's bodies in mm, mN and mN mm, a load at a body's frame's origin;
// the fluid's walls in SI, a load at the wall's centre.
struct Together: Assembly::AssemblyObject::PartLoads
{
    Solver::WallExchange exchange;
    Assembly::AssemblyObject* assembly = nullptr;
    App::DocumentObject* simulation = nullptr;
    std::vector<App::DocumentObject*> moved;
    std::vector<Solver::Vec3> centres;  // each wall's, at the start, m
    std::vector<Base::Placement> starts;  // each body's frame then
    bool told = false;

    std::vector<App::DocumentObject*> parts() const override
    {
        return moved;
    }
    double longestPiece() const override
    {
        double stretch = 0.0;
        std::string why;
        return const_cast<Solver::WallExchange&>(exchange).longest(stretch, why) ? stretch : 0.0;
    }
    bool piece(double time,
               double length,
               const std::vector<Assembly::Solver::BodyState>& states,
               std::vector<Assembly::Solver::Wrench>& loads,
               std::string& why) override
    {
        if (starts.empty()) {
            for (const auto& state : states) {
                starts.push_back(state.placement);
            }
        }
        std::vector<Solver::WallState> walls(states.size());
        std::vector<Base::Vector3d> arms(states.size());  // origin to centre, mm
        for (std::size_t k = 0; k < states.size(); ++k) {
            // where every point of the body is, from where it was
            const Base::Placement motion = states[k].placement * starts[k].inverse();
            double q[4];
            motion.getRotation().getValue(q[0], q[1], q[2], q[3]);
            const Base::Vector3d shift = motion.getPosition();
            Base::Vector3d centre;
            motion.multVec(Base::Vector3d(centres[k][0], centres[k][1], centres[k][2]) * 1.0e3,
                           centre);
            arms[k] = centre - states[k].placement.getPosition();
            const Base::Vector3d speed =
                states[k].velocity + states[k].angularVelocity.Cross(arms[k]);
            walls[k].motion.translation = {shift.x * 1.0e-3, shift.y * 1.0e-3, shift.z * 1.0e-3};
            walls[k].motion.rotation = {q[0], q[1], q[2], q[3]};
            walls[k].velocity = {speed.x * 1.0e-3, speed.y * 1.0e-3, speed.z * 1.0e-3};
            walls[k].spin = {states[k].angularVelocity.x,
                             states[k].angularVelocity.y,
                             states[k].angularVelocity.z};
        }
        std::vector<Solver::WallLoad> pushed;
        if (!exchange.piece(time, length, walls, pushed, why) || pushed.size() != states.size()) {
            if (why.empty()) {
                why = "the fluid's run gave no loads";
            }
            return false;
        }
        loads.resize(states.size());
        for (std::size_t k = 0; k < states.size(); ++k) {
            // N to mN; N m about the centre to mN mm about the frame's origin
            const Base::Vector3d force(pushed[k].force[0], pushed[k].force[1], pushed[k].force[2]);
            const Base::Vector3d torque(pushed[k].torque[0],
                                        pushed[k].torque[1],
                                        pushed[k].torque[2]);
            loads[k].force = force * 1.0e3;
            loads[k].torque = torque * 1.0e6 + arms[k].Cross(force * 1.0e3);
        }
        return true;
    }
};
}  // namespace
#endif

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
    void energy(const Solver::Energy& energy) override
    {
        writer->energy(energy);
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
            // a steady flow stops when it has settled: short of its steps
            index.settled = index.steady && status == Solver::Status::Ok
                && writer->lastTime() < model.iterations;
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
#ifdef FLUID_WITH_ASSEMBLY
        // whatever way it ended: the mechanism may be waiting for it
        if (together) {
            together->exchange.ended(why);
        }
#endif
        done.store(true);
    }

#ifdef FLUID_WITH_ASSEMBLY
    std::unique_ptr<Together> together;
#endif

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

const char* Study::AnalysisEnums[] = {"Free surface", "Steady flow", "Flow in time", nullptr};
const char* Study::TurbulenceEnums[] = {"Laminar", "k-omega SST", nullptr};

Study::Study()
{
    GroupExtension::initExtension(this);

    ADD_PROPERTY_TYPE(Walls, (nullptr), "Study", App::Prop_None,
                      "The parts the fluid cannot pass");
    ADD_PROPERTY_TYPE(Analysis, (0L), "Study", App::Prop_None,
                      "A liquid under the air, from how it stands at the start; or one "
                      "fluid filling the closed space inside the walls, from its inlets "
                      "to its outlets, as it settles to; or that fluid in that space "
                      "from rest, through time");
    Analysis.setEnums(AnalysisEnums);
    ADD_PROPERTY_TYPE(Slides, (nullptr), "Study", App::Prop_None,
                      "Of a fluid that fills its space: the faces of the walls it slides "
                      "along, held by nothing, and does not pass");
    ADD_PROPERTY_TYPE(Iterations, (2000), "Study", App::Prop_None,
                      "A steady flow's: how many steps it is given to settle in, at most");
    ADD_PROPERTY_TYPE(Turbulence, (0L), "Study", App::Prop_None,
                      "A steady flow's: in layers, with no eddies; or turbulent, its "
                      "eddies had by their mean effect (Menter's k-omega SST model, the "
                      "walls by wall functions)");
    Turbulence.setEnums(TurbulenceEnums);
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
    ADD_PROPERTY_TYPE(Mechanism, (nullptr), "Study", App::Prop_None,
                      "An Assembly's simulation the study runs together with: the parts "
                      "the fluid moves are then that mechanism's, moved with its joints "
                      "under the fluid's push");
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
    struct Let
    {
        const Outlet* source;
        Part::TopoShape face;
    };
    std::vector<Let> outlets;
    // one fluid filling the space inside the walls: settled, or through time
    const bool steady = Analysis.getValue() != 0;
    model.analysis = analysisOf(Analysis.getValue());
    model.iterations = static_cast<int>(Iterations.getValue());
    model.turbulence = Analysis.getValue() == 1 && Turbulence.getValue() == 1 ? Solver::Turbulence::KOmegaSST
                                                            : Solver::Turbulence::Laminar;
    // The one face a link names, where it stands.
    const auto faceOf = [](const App::DocumentObject* asker, const App::PropertyLinkSub& link) {
        auto* owner = link.getValue();
        const auto& subs = link.getSubValues();
        if (!owner || subs.size() != 1) {
            FC_THROWM(Base::ValueError, asker->getFullLabel() << " has no face, or several");
        }
        Part::TopoShape face = Part::Feature::getTopoShape(owner, subs.front().c_str(), true);
        if (face.isNull() || face.countSubShapes("Face") != 1) {
            FC_THROWM(Base::ValueError,
                      asker->getFullLabel() << ": " << subs.front() << " of "
                                            << owner->getFullLabel() << " is not a face");
        }
        return face;
    };
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
        if (steady && (freecad_cast<InitialFluid*>(child) || freecad_cast<FreeBody*>(child))) {
            FC_THROWM(Base::ValueError,
                      child->getFullLabel() << " is of a free surface's study: here "
                                               "the fluid fills the space inside the "
                                               "walls, and moves no part");
        }
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
            Part::TopoShape face = faceOf(inlet, inlet->Face);
            if (!(inlet->Velocity.getValue() > 0.0)) {
                FC_THROWM(Base::ValueError,
                          inlet->getFullLabel() << " has no speed: the fluid comes in at "
                                                   "more than nothing");
            }
            inlets.push_back({inlet, face});
            space.Add(face.getBoundBox());
        }
        else if (auto* outlet = freecad_cast<Outlet*>(child)) {
            if (!steady) {
                FC_THROWM(Base::ValueError,
                          outlet->getFullLabel() << " is of a study whose fluid fills its "
                                                    "space: a free surface's fluid leaves "
                                                    "by the open sides of its space");
            }
            outlets.push_back({outlet, faceOf(outlet, outlet->Face)});
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
    if (steady) {
        if (inlets.empty()) {
            THROWM(Base::ValueError, "a steady flow needs an inlet");
        }
        if (outlets.empty()) {
            THROWM(Base::ValueError, "a steady flow needs an outlet");
        }
        // The faces the fluid crosses, the inlets first: a patch each
        std::vector<std::pair<const App::DocumentObject*, TopoDS_Shape>> crossed;
        // and each flat: said here, before a round one is looked for among
        // the space's faces and not found
        const auto flat = [](const Part::TopoShape& face) {
            const TopoDS_Face one = TopoDS::Face(face.getSubShape(TopAbs_FACE, 1));
            return GeomLib_IsPlanarSurface(BRep_Tool::Surface(one), 1.0e-7).IsPlanar();
        };
        for (const auto& fed : inlets) {
            if (!flat(fed.face)) {
                FC_THROWM(Base::ValueError,
                          fed.source->getFullLabel() << "'s face is not flat: the fluid comes "
                                                        "in through a flat face, square to it");
            }
            crossed.emplace_back(fed.source, fed.face.getShape());
        }
        for (const auto& let : outlets) {
            if (!flat(let.face)) {
                FC_THROWM(Base::ValueError,
                          let.source->getFullLabel() << "'s face is not flat: the fluid "
                                                        "leaves through a flat face");
            }
            crossed.emplace_back(let.source, let.face.getShape());
        }
        std::vector<Part::TopoShape> parts;
        for (const auto& wall : walls) {
            parts.push_back(wall.shape);
        }
        std::vector<TopoDS_Shape> spaces = spacesInside(parts);
        const auto smallestOf = [](const std::vector<TopoDS_Shape>& shapes) {
            // the shapes' own box: not padded by their tolerances
            Bnd_Box box;
            for (const auto& shape : shapes) {
                BRepBndLib::AddOptimal(shape, box, false, false);
            }
            if (box.IsVoid()) {
                return 0.0;
            }
            double x0 = 0.0, y0 = 0.0, z0 = 0.0, x1 = 0.0, y1 = 0.0, z1 = 0.0;
            box.Get(x0, y0, z0, x1, y1, z1);
            return std::min({x1 - x0, y1 - y0, z1 - z0});
        };
        // The round faces as flat pieces a hundredth of the resolution off
        // them: a mesh is drawn onto these, and a pipe's pressure drop goes
        // with the fourth power of what they make its radius.
        const auto deflectionOf = [this](double side) {
            const double asked = Resolution.getValue();
            return std::max(0.01 * (asked > 0.0 ? asked : side / 20.0), 1.0e-4);
        };
        std::vector<int> spaceOf(crossed.size(), -1);
        // The faces the fluid slides along, each where it stands
        std::vector<TopoDS_Shape> slid;
        {
            const auto& owners = Slides.getValues();
            const auto& subs = Slides.getSubValues();
            for (std::size_t k = 0; k < owners.size() && k < subs.size(); ++k) {
                Part::TopoShape face = owners[k]
                    ? Part::Feature::getTopoShape(owners[k], subs[k].c_str(), true)
                    : Part::TopoShape();
                if (face.isNull() || face.countSubShapes("Face") != 1) {
                    FC_THROWM(Base::ValueError,
                              "the fluid is to slide along "
                                  << subs[k] << " of "
                                  << (owners[k] ? owners[k]->getFullLabel() : std::string("nothing"))
                                  << ", which is not a face");
                }
                slid.push_back(face.getShape());
            }
        }
        std::vector<bool> slidOnto(slid.size(), false);
        for (int pass = 0; pass < 2; ++pass) {
            const double deflection = deflectionOf(smallestOf(spaces));
            model.domain = Solver::Surface {};
            std::ranges::fill(spaceOf, -1);
            slidOnto.assign(slid.size(), false);
            std::vector<Solver::Surface> surfaces(spaces.size());
            for (std::size_t i = 0; i < spaces.size(); ++i) {
                BRepTools::Clean(spaces[i]);
                BRepMesh_IncrementalMesh mesher(spaces[i], deflection, false, 0.2, true);
                for (TopExp_Explorer each(spaces[i], TopAbs_FACE); each.More(); each.Next()) {
                    gp_Pnt within;
                    const std::size_t from = surfaces[i].triangles.size();
                    if (!addFace(TopoDS::Face(each.Current()), 0, surfaces[i], within)) {
                        continue;
                    }
                    const TopoDS_Vertex probe = BRepBuilderAPI_MakeVertex(within);
                    bool isCrossed = false;
                    for (std::size_t k = 0; k < crossed.size(); ++k) {
                        BRepExtrema_DistShapeShape gap(probe, crossed[k].second);
                        if (gap.IsDone() && gap.Value() < 1.0e-6) {
                            for (std::size_t t = from; t < surfaces[i].triangles.size(); ++t) {
                                surfaces[i].patches[t] = static_cast<int>(k) + 1;
                            }
                            spaceOf[k] = static_cast<int>(i);
                            isCrossed = true;
                            break;
                        }
                    }
                    for (std::size_t k = 0; k < slid.size() && !isCrossed; ++k) {
                        BRepExtrema_DistShapeShape gap(probe, slid[k]);
                        if (gap.IsDone() && gap.Value() < 1.0e-6) {
                            for (std::size_t t = from; t < surfaces[i].triangles.size(); ++t) {
                                surfaces[i].patches[t] = static_cast<int>(crossed.size()) + 1;
                            }
                            slidOnto[k] = true;
                            break;
                        }
                    }
                }
            }
            for (std::size_t k = 0; k < crossed.size(); ++k) {
                if (spaceOf[k] < 0) {
                    FC_THROWM(Base::ValueError,
                              crossed[k].first->getFullLabel()
                                  << "'s face does not open onto a closed space inside the "
                                     "walls: an opening without a lid, or a gap, lets the "
                                     "space out");
                }
                if (spaceOf[k] != spaceOf[0]) {
                    FC_THROWM(Base::ValueError,
                              crossed[0].first->getFullLabel()
                                  << " and " << crossed[k].first->getFullLabel()
                                  << " do not open onto the same space");
                }
            }
            for (std::size_t k = 0; k < slid.size(); ++k) {
                if (!slidOnto[k]) {
                    FC_THROWM(Base::ValueError,
                              "the fluid is to slide along "
                                  << Slides.getSubValues()[k] << " of "
                                  << Slides.getValues()[k]->getFullLabel()
                                  << ", which is no face of a closed space inside the walls");
                }
            }
            const auto which = static_cast<std::size_t>(spaceOf[0]);
            model.domain = std::move(surfaces[which]);
            // The spaces the fluid is not in may be smaller: once they are
            // out, the pieces are as fine as the fluid's own space asks
            const std::vector<TopoDS_Shape> filled {spaces[which]};
            const bool same = deflectionOf(smallestOf(filled)) == deflection;
            smallest = smallestOf(filled);
            spaces = filled;
            if (same) {
                break;
            }
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
        model.walls.back().surface = meshOf(wall.shape, accuracy);
        if (wall.density > 0.0) {
            // a mechanism's part where the study runs with one
            model.walls.back().motion = Mechanism.getValue() ? Solver::WallMotion::Driven
                                                             : Solver::WallMotion::Free;
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
        inlet.surface = meshOf(fed.face, accuracy);
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
    for (const auto& let : outlets) {
        Solver::Outlet outlet;
        outlet.source = sourceOf(let.source, getDocument());
        outlet.surface = meshOf(let.face, accuracy);
        Base::Vector3d out;
        bool hasArea = false;
        if (!flatOf(outlet.surface, out, hasArea)) {
            FC_THROWM(Base::ValueError,
                      let.source->getFullLabel() << "'s face is not flat: the fluid leaves "
                                                    "through a flat face");
        }
        // kPa, the document's unit
        outlet.pressure = let.source->Pressure.getValue() * 1.0e3;
        model.outlets.push_back(outlet);
    }
    for (const auto& fluid : fluids) {
        model.fluids.push_back(
            {sourceOf(fluid.source, getDocument()), meshOf(fluid.shape, accuracy), fluid.material});
    }
    if (space.IsValid()) {
        model.space.min = {space.MinX * 1.0e-3, space.MinY * 1.0e-3, space.MinZ * 1.0e-3};
        model.space.max = {space.MaxX * 1.0e-3, space.MaxY * 1.0e-3, space.MaxZ * 1.0e-3};
    }
    if (steady) {
        // the fluid's own box, not the walls'
        Solver::boxOf(model.domain, model.space.min, model.space.max);
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
    auto* backend = Solver::Registry::instance().select(
        name,
        analysisOf(Analysis.getValue()));
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
    if (run->model.analysis == Solver::Analysis::Steady && !backend->capabilities().steady) {
        why = backend->label() + " cannot run a steady flow";
        Status.setValue(std::string(statusText(Solver::Status::Unsupported)) + ": " + why);
        return Solver::Status::Unsupported;
    }
    if (run->model.analysis == Solver::Analysis::Unsteady && !backend->capabilities().unsteady) {
        why = backend->label() + " cannot run a flow that fills its space through time";
        Status.setValue(std::string(statusText(Solver::Status::Unsupported)) + ": " + why);
        return Solver::Status::Unsupported;
    }
    const bool driven = Solver::anyDriven(run->model);
    if (driven && !backend->capabilities().drivenWalls) {
        why = backend->label() + " cannot run with a mechanism";
        Status.setValue(std::string(statusText(Solver::Status::Unsupported)) + ": " + why);
        return Solver::Status::Unsupported;
    }
#ifndef FLUID_WITH_ASSEMBLY
    if (driven) {
        why = "this build has no Assembly module to run the mechanism";
        Status.setValue(std::string(statusText(Solver::Status::Unsupported)) + ": " + why);
        return Solver::Status::Unsupported;
    }
#else
    // The Assembly's simulation, the assembly it is of, and what of it the
    // fluid can move: refused here with the reason, rather than by a run
    // that does not start or moves nothing
    Assembly::AssemblyObject* assembly = nullptr;
    if (driven) {
        std::string refused;
        auto* simulation = Mechanism.getValue();
        for (auto* in : simulation->getInListRecursive()) {
            if (auto* found = freecad_cast<Assembly::AssemblyObject*>(in)) {
                assembly = found;
                break;
            }
        }
        if (!assembly) {
            refused = simulation->getFullLabel() + " is not an assembly's simulation";
        }
        else if (auto* kind = freecad_cast<Assembly::Simulation*>(simulation);
                 !kind || std::string(kind->AnalysisType.getValueAsString()) != "Dynamic") {
            // a Kinematic run moves the parts by the joints' motions alone
            refused = std::string(simulation->Label.getValue())
                + " is not a dynamic simulation: the fluid's push moves a mechanism "
                  "only under its forces";
        }
        else {
            for (auto* child : Group.getValues()) {
                auto* body = freecad_cast<FreeBody*>(child);
                auto* part = body ? body->Part.getValue() : nullptr;
                if (!part) {
                    continue;
                }
                if (!assembly->hasObject(part, true)) {
                    refused = std::string(part->Label.getValue()) + " is not a part of "
                        + assembly->Label.getValue() + ", the mechanism's assembly";
                }
                else if (assembly->isPartGrounded(part)) {
                    refused = std::string(part->Label.getValue()) + " is grounded in "
                        + assembly->Label.getValue() + ": nothing moves it";
                }
                if (!refused.empty()) {
                    break;
                }
            }
        }
        if (!refused.empty()) {
            why = refused;
            Status.setValue(std::string(statusText(Solver::Status::Unsupported)) + ": " + why);
            return Solver::Status::Unsupported;
        }
    }
#endif
    // a steady flow's inlets are the flow itself: who runs one has them
    const bool steadyRun = Solver::fillsItsSpace(run->model);
    if (!steadyRun && !run->model.inlets.empty() && !backend->capabilities().inlets) {
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
    run->index.kind = steadyRun ? Solver::ResultKind::Fields : backend->capabilities().results;
    run->index.steady = run->model.analysis == Solver::Analysis::Steady;
    if (run->model.turbulence == Solver::Turbulence::KOmegaSST) {
        run->index.turbulence = "KOmegaSST";
    }
    if (steadyRun) {
        for (std::size_t i = 0; i < run->model.inlets.size(); ++i) {
            run->index.reynolds = std::max(run->index.reynolds, Solver::reynolds(run->model, i));
        }
    }
    if (steadyRun) {
        for (const auto& inlet : run->model.inlets) {
            run->index.openings.push_back({inlet.source, false});
        }
        for (const auto& outlet : run->model.outlets) {
            run->index.openings.push_back({outlet.source, true});
        }
    }
    run->index.signature = Solver::signature(run->model);
    run->index.spacing = run->model.resolution * 1000.0;
    run->index.fed = !steadyRun && !run->model.inlets.empty();
    for (const auto& monitor : run->model.monitors) {
        run->index.monitors.push_back({monitor.source, monitor.kind});
    }
    for (std::size_t wall : moved) {
        run->index.bodies.push_back(run->model.walls[wall].source);
    }
#ifdef FLUID_WITH_ASSEMBLY
    if (driven) {
        run->together = std::make_unique<Together>();
        run->together->assembly = assembly;
        run->together->simulation = Mechanism.getValue();
        for (auto* child : Group.getValues()) {
            if (auto* body = freecad_cast<FreeBody*>(child)) {
                run->together->moved.push_back(body->Part.getValue());
            }
        }
        for (std::size_t wall : moved) {
            run->together->centres.push_back(run->model.walls[wall].mass.centre);
        }
        run->session->drive(&run->together->exchange);
    }
#endif
    active = std::move(run);
    Status.setValue("Running");
    active->thread = std::thread([run = active.get()] {
        run->body();
    });
#ifdef FLUID_WITH_ASSEMBLY
    if (active->together) {
        Together& together = *active->together;
        if (together.assembly->startSimulation(together.simulation, &together) != 0) {
            together.exchange.done();
            active->cancel.store(true);
            active->thread.join();
            active.reset();
            why = "the mechanism's simulation could not be started";
            Status.setValue(std::string(statusText(Solver::Status::Failed)) + ": " + why);
            return Solver::Status::Failed;
        }
    }
#endif
    return Solver::Status::Ok;
}

struct Study::WallCache
{
    double spacing = 0.0;
    std::uint64_t signature = 0;
    std::vector<WallPoint> fixed;
    std::vector<std::vector<WallPoint>> moved;  // as a frame's bodies come
};

std::vector<WallPoint> Study::wallPointsAt(const std::vector<Solver::Motion>& bodies,
                                           double spacing) const
{
    std::uint64_t signature = 0;
    try {
        if (const auto* kept = results()) {
            signature = kept->index().signature;
        }
    }
    catch (const Base::Exception&) {
        signature = 0;
    }
    if (!wallCache || wallCache->spacing != spacing || wallCache->signature != signature) {
        auto cache = std::make_shared<WallCache>();
        cache->spacing = spacing;
        cache->signature = signature;
        try {
            const Solver::Model model = buildModel();
            const auto moved = Solver::freeWalls(model);
            for (std::size_t i = 0; i < model.walls.size(); ++i) {
                // the model's metres to the particles' mm
                Solver::Surface surface = model.walls[i].surface;
                for (auto& point : surface.points) {
                    for (double& x : point) {
                        x *= 1000.0;
                    }
                }
                auto points = wallPointsOf(surface, 0.5 * spacing);
                if (std::ranges::find(moved, i) != moved.end()) {
                    cache->moved.push_back(std::move(points));
                }
                else {
                    cache->fixed.insert(cache->fixed.end(), points.begin(), points.end());
                }
            }
        }
        catch (const Base::Exception&) {
            cache->fixed.clear();
            cache->moved.clear();
        }
        wallCache = cache;
    }
    std::vector<WallPoint> points = wallCache->fixed;
    // a frame of another model's run moves other parts: then the fixed
    // walls alone
    if (bodies.size() != wallCache->moved.size()) {
        return points;
    }
    for (std::size_t k = 0; k < bodies.size(); ++k) {
        const Solver::Motion& motion = bodies[k];
        const Base::Rotation turn(motion.rotation[0],
                                  motion.rotation[1],
                                  motion.rotation[2],
                                  motion.rotation[3]);
        for (const WallPoint& point : wallCache->moved[k]) {
            Base::Vector3d at(point.at[0], point.at[1], point.at[2]);
            Base::Vector3d out(point.out[0], point.out[1], point.out[2]);
            at = turn.multVec(at);
            out = turn.multVec(out);
            points.push_back({{at.x + motion.translation[0],
                               at.y + motion.translation[1],
                               at.z + motion.translation[2]},
                              {out.x, out.y, out.z}});
        }
    }
    return points;
}

Study::Progress Study::progress() const
{
    Progress progress;
    if (active) {
#ifdef FLUID_WITH_ASSEMBLY
        // the mechanism's run over, the fluid's has no more to take
        if (active->together && active->together->assembly->simulationProgress().done) {
            active->together->exchange.done();
        }
#endif
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
#ifdef FLUID_WITH_ASSEMBLY
        if (active->together) {
            active->together->assembly->cancelSimulation();
            active->together->exchange.done();
        }
#endif
    }
}

Solver::Status Study::finish(std::string& why)
{
    if (!active) {
        why = "the study is not running";
        return Solver::Status::Failed;
    }
#ifdef FLUID_WITH_ASSEMBLY
    int mechanism = 0;
    if (active->together) {
        // the mechanism's first: it keeps its frames, and the fluid's run
        // ends where that did
        mechanism = active->together->assembly->finishSimulation();
        active->together->exchange.done();
    }
#endif
    if (active->thread.joinable()) {
        active->thread.join();
    }
#ifdef FLUID_WITH_ASSEMBLY
    if (mechanism != 0 && active->status == Solver::Status::Ok) {
        active->status = Solver::Status::Failed;
        active->why = "the mechanism's simulation failed";
    }
#endif
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

// ================================ Outlet ===============================

PROPERTY_SOURCE(Fluid::Outlet, App::DocumentObject)

Outlet::Outlet()
{
    ADD_PROPERTY_TYPE(Face, (nullptr), "Outlet", App::Prop_None,
                      "The flat face the fluid leaves through");
    ADD_PROPERTY_TYPE(Pressure, (0.0), "Outlet", App::Prop_None,
                      "The pressure the face is held at, over the surroundings'");
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
