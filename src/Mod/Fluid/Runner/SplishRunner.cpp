// SPDX-License-Identifier: LGPL-2.1-or-later

// FluidSplish: SPlisHSPlasH (MIT) run on a case the Fluid workbench wrote
// (docs/FluidSimulation.md sec 12 S3). A program of its own, built against
// the solver with the solver's own compiler settings and its own Eigen, so
// that none of either is in FreeCAD's process.
//
//   FluidSplish --version
//   FluidSplish <folder>
//
// The folder holds case.txt, fluid.points, boundary.points, a
// body.<k>.points for each wall the fluid moves, a forced.<k>.points for
// each wall a force is asked of and an inlet.<k>.points for each inlet
// (three floats a point, metres). Lines on standard output say what happened:
//
//   version <the solver's>
//   body <k> <x y z> <qx qy qz qw> where the k-th moved wall is at the
//                                  frame that follows: its motion from the
//                                  start, a translation and a rotation
//   frame <index> <time> <count>   frame.<index> is written: count points
//   sample <time> <value> ...      the monitors, in the case's order
//   error <why>
//
// Anything else is the solver's own talk.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _OPENMP
# include <omp.h>
#endif

#include "SPlisHSPlasH/BoundaryModel_Akinci2012.h"
#include "SPlisHSPlasH/Simulation.h"
#include "SPlisHSPlasH/StaticRigidBody.h"
#include "SPlisHSPlasH/TimeManager.h"
#include "SPlisHSPlasH/TimeStep.h"
#include "SPlisHSPlasH/SurfaceTension/SurfaceTension_Akinci2013.h"
#include "SPlisHSPlasH/Viscosity/Viscosity_Standard.h"
#include "Utilities/Counting.h"
#include "Utilities/Logger.h"
#include "Utilities/Timing.h"
#include "Utilities/Version.h"

INIT_LOGGING
INIT_TIMING
INIT_COUNTING

using namespace SPH;

namespace
{

// The pull between the fluid's particles of Akinci et al. 2013, as the
// solver has it, less its pull towards the walls. The solver's own cannot
// be used with that part at nothing: it still works out the wall's kernel,
// a fourth root that is of a number a rounding below zero when a particle
// is at the kernel's edge, and nothing times that is not a number -- a
// run was all such from that step on, one run in four.
class Cohesion: public SurfaceTension_Akinci2013
{
public:
    static constexpr const char* Name = "FluidSplish cohesion";

    explicit Cohesion(FluidModel* model)
        : SurfaceTension_Akinci2013(model)
    {}
    static NonPressureForceBase* creator(FluidModel* model)
    {
        return new Cohesion(model);
    }
    std::string getMethodName() override
    {
        return Name;
    }
    void setStrength(Real strength)
    {
        m_surfaceTension = strength;
        m_surfaceTensionBoundary = 0;
    }

    void step() override
    {
        Simulation* sim = Simulation::getCurrent();
        FluidModel* model = m_model;
        const Real density0 = model->getDensity0();
        const Real k = m_surfaceTension;
        const int count = (int)model->numActiveParticles();
        const unsigned int fluidModelIndex = model->getPointSetIndex();

        computeNormals();

        #pragma omp parallel for schedule(static) default(shared)
        for (int i = 0; i < count; i++) {
            const Vector3r& xi = model->getPosition(i);
            const Vector3r& ni = getNormal(i);
            const Real rhoi = model->getDensity(i);
            Vector3r& ai = model->getAcceleration(i);
            forall_fluid_neighbors_in_same_phase(
                const Real rhoj = model->getDensity(neighborIndex);
                Vector3r accel = Vector3r::Zero();
                // towards each other
                Vector3r between = xi - xj;
                const Real length2 = between.squaredNorm();
                if (length2 > (Real)1.0e-9) {
                    between *= (Real)1.0 / std::sqrt(length2);
                    accel -= k * model->getMass(neighborIndex) * between
                        * CohesionKernel::W(xi - xj);
                }
                // and towards a flatter surface
                accel -= k * (ni - getNormal(neighborIndex));
                ai += (Real)2.0 * density0 / (rhoi + rhoj) * accel;
            )
        }
    }
};

// A wall the fluid moves (docs/FluidSimulation.md sec 12 S4): a rigid body
// held by nothing, under the fluid's push on its particles and its own
// weight. The solver adds up the push each step; the body is stepped here,
// after the fluid, with the step the fluid took.
class Moved: public RigidBodyObject
{
public:
    Real mass = 0;
    Vector3r start = Vector3r::Zero();  // its centre of mass when the run starts
    Matrix3r inertia = Matrix3r::Identity();  // about the centre, as it stood then
    Vector3r x = Vector3r::Zero();
    Quaternionr q = Quaternionr::Identity();
    Vector3r v = Vector3r::Zero();
    Vector3r w = Vector3r::Zero();
    Vector3r force = Vector3r::Zero();
    Vector3r torque = Vector3r::Zero();

    bool isDynamic() const override
    {
        return true;
    }
    Real const getMass() const override
    {
        return mass;
    }
    Vector3r const& getPosition() const override
    {
        return x;
    }
    void setPosition(const Vector3r& value) override
    {
        x = value;
    }
    Vector3r getWorldSpacePosition() const override
    {
        return x;
    }
    Vector3r const& getVelocity() const override
    {
        return v;
    }
    void setVelocity(const Vector3r& value) override
    {
        v = value;
    }
    Quaternionr const& getRotation() const override
    {
        return q;
    }
    void setRotation(const Quaternionr& value) override
    {
        q = value;
    }
    Matrix3r getWorldSpaceRotation() const override
    {
        return q.matrix();
    }
    Vector3r const& getAngularVelocity() const override
    {
        return w;
    }
    void setAngularVelocity(const Vector3r& value) override
    {
        w = value;
    }
    void addForce(const Vector3r& f) override
    {
        force += f;
    }
    void addTorque(const Vector3r& t) override
    {
        torque += t;
    }
    void updateMeshTransformation() override
    {}
    const std::vector<Vector3r>& getVertices() const override
    {
        return none;
    }
    const std::vector<Vector3r>& getVertexNormals() const override
    {
        return none;
    }
    const std::vector<unsigned int>& getFaces() const override
    {
        return noFaces;
    }

    // A step of that length under what was added up, gravity, and a
    // force and torque besides; the speeds first, then the place from the
    // new speeds. What was added up stays: it is the fluid's for the whole
    // of the fluid's step, however many of these that is.
    void step(Real h, const Vector3r& gravity, const Vector3r& more, const Vector3r& turning)
    {
        v += h * ((force + more) / mass + gravity);
        x += h * v;
        const Matrix3r turn = q.matrix();
        const Matrix3r now = turn * inertia * turn.transpose();
        w += h * now.inverse() * (torque + turning - w.cross(now * w));
        const Quaternionr spin((Real)0, w[0], w[1], w[2]);
        q.coeffs() += (Real)0.5 * h * (spin * q).coeffs();
        q.normalize();
    }

private:
    std::vector<Vector3r> none;
    std::vector<unsigned int> noFaces;
};

struct Body
{
    unsigned long count = 0;
    double mass = 0.0;
    Vector3r centre = Vector3r::Zero();
    double inertia[6] = {0, 0, 0, 0, 0, 0};  // xx yy zz xy xz yz
};

// A flat face the fluid comes in through: a layer of particles on it,
// sent off along n at the speed, another each time the last has gone a
// particle's width.
struct Inlet
{
    unsigned long count = 0;
    Vector3r origin = Vector3r::Zero();  // a particle of the layer
    Vector3r n = Vector3r::Zero();       // the way the fluid goes
    Vector3r across = Vector3r::Zero();  // the layer's grid's first axis
    double speed = 0.0;
    double start = 0.0;
    double end = 0.0;
};

struct Monitor
{
    bool pressure = false;
    // the fluid's force on the wall of that place among the forced ones,
    // along normal; -1, not a force
    int force = -1;
    Vector3r point = Vector3r::Zero();
    // a pressure on a wall: the way the wall faces there; zero in the open
    // fluid
    Vector3r normal = Vector3r::Zero();
};

struct Case
{
    double radius = 0.0;
    Vector3r gravity = Vector3r(0, 0, -9.80665f);
    double end = 1.0;
    double interval = 0.01;
    double density = 1000.0;
    double viscosity = 1e-6;
    double cohesion = 0.0;
    int threads = 1;
    Vector3r spaceMin = Vector3r::Zero();
    Vector3r spaceMax = Vector3r::Zero();
    unsigned long boundary = 0;
    unsigned long fluid = 0;
    std::vector<Monitor> monitors;
    std::vector<Body> bodies;
    std::vector<unsigned long> forced;
    std::vector<Inlet> inlets;
};

[[noreturn]] void fail(const std::string& why)
{
    std::printf("error %s\n", why.c_str());
    std::fflush(stdout);
    std::exit(2);
}

Case readCase(const std::string& folder)
{
    const std::string path = folder + "/case.txt";
    std::FILE* in = std::fopen(path.c_str(), "r");
    if (!in) {
        fail("cannot read " + path);
    }
    Case made;
    char line[512];
    bool ours = false;
    while (std::fgets(line, sizeof(line), in)) {
        char word[64] = "";
        double a[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        if (std::sscanf(line, "%63s", word) != 1) {
            continue;
        }
        const std::string key = word;
        const char* rest = line + std::strlen(word);
        if (key == "fluidsplish") {
            ours = std::atoi(rest) == 1;
        }
        else if (key == "radius" && std::sscanf(rest, "%lf", &a[0]) == 1) {
            made.radius = a[0];
        }
        else if (key == "gravity" && std::sscanf(rest, "%lf %lf %lf", &a[0], &a[1], &a[2]) == 3) {
            made.gravity = Vector3r((Real)a[0], (Real)a[1], (Real)a[2]);
        }
        else if (key == "end" && std::sscanf(rest, "%lf", &a[0]) == 1) {
            made.end = a[0];
        }
        else if (key == "interval" && std::sscanf(rest, "%lf", &a[0]) == 1) {
            made.interval = a[0];
        }
        else if (key == "density" && std::sscanf(rest, "%lf", &a[0]) == 1) {
            made.density = a[0];
        }
        else if (key == "viscosity" && std::sscanf(rest, "%lf", &a[0]) == 1) {
            made.viscosity = a[0];
        }
        else if (key == "cohesion" && std::sscanf(rest, "%lf", &a[0]) == 1) {
            made.cohesion = a[0];
        }
        else if (key == "threads") {
            made.threads = std::max(1, std::atoi(rest));
        }
        else if (key == "space"
                 && std::sscanf(rest, "%lf %lf %lf %lf %lf %lf", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5])
                     == 6) {
            made.spaceMin = Vector3r((Real)a[0], (Real)a[1], (Real)a[2]);
            made.spaceMax = Vector3r((Real)a[3], (Real)a[4], (Real)a[5]);
        }
        else if (key == "boundary") {
            made.boundary = std::strtoul(rest, nullptr, 10);
        }
        else if (key == "fluid") {
            made.fluid = std::strtoul(rest, nullptr, 10);
        }
        else if (key == "body") {
            Body body;
            if (std::sscanf(rest, "%lu %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf", &body.count, &a[0],
                            &a[1], &a[2], &a[3], &a[4], &a[5], &a[6], &a[7], &a[8], &a[9])
                    != 11
                || body.count == 0 || !(a[0] > 0.0)) {
                fail("a wall the fluid moves has no particles, or no mass");
            }
            body.mass = a[0];
            body.centre = Vector3r((Real)a[1], (Real)a[2], (Real)a[3]);
            for (int k = 0; k < 6; k++) {
                body.inertia[k] = a[4 + k];
            }
            made.bodies.push_back(body);
        }
        else if (key == "forced") {
            made.forced.push_back(std::strtoul(rest, nullptr, 10));
            if (made.forced.back() == 0) {
                fail("a wall a force is asked of has no particles");
            }
        }
        else if (key == "inlet") {
            Inlet inlet;
            if (std::sscanf(rest, "%lu %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf", &inlet.count,
                            &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6], &a[7], &a[8], &a[9],
                            &a[10], &a[11])
                    != 13
                || inlet.count == 0 || !(a[9] > 0.0)) {
                fail("an inlet has no particles, or no speed");
            }
            inlet.origin = Vector3r((Real)a[0], (Real)a[1], (Real)a[2]);
            inlet.n = Vector3r((Real)a[3], (Real)a[4], (Real)a[5]);
            inlet.across = Vector3r((Real)a[6], (Real)a[7], (Real)a[8]);
            inlet.speed = a[9];
            inlet.start = a[10];
            inlet.end = a[11];
            made.inlets.push_back(inlet);
        }
        else if (key == "monitor") {
            char kind[32] = "";
            const int got = std::sscanf(rest, "%31s %lf %lf %lf %lf %lf %lf", kind, &a[0], &a[1], &a[2],
                                        &a[3], &a[4], &a[5]);
            if (std::string(kind) == "force") {
                // which forced wall, and the way the force is taken
                Monitor monitor;
                monitor.force = (int)a[0];
                monitor.normal = Vector3r((Real)a[1], (Real)a[2], (Real)a[3]);
                if (got != 5 || monitor.force < 0) {
                    fail("a force monitor names no wall");
                }
                made.monitors.push_back(monitor);
            }
            else if (got == 4 || got == 7) {
                Monitor monitor;
                monitor.pressure = std::string(kind) == "pressure";
                monitor.point = Vector3r((Real)a[0], (Real)a[1], (Real)a[2]);
                if (got == 7) {
                    monitor.normal = Vector3r((Real)a[3], (Real)a[4], (Real)a[5]);
                }
                made.monitors.push_back(monitor);
            }
        }
    }
    std::fclose(in);
    if (!ours) {
        fail(path + " is not a case of this program's");
    }
    if (!(made.radius > 0.0) || (made.fluid == 0 && made.inlets.empty()) || !(made.interval > 0.0)
        || !(made.density > 0.0)) {
        fail("the case has no fluid, or no size");
    }
    for (const Monitor& monitor : made.monitors) {
        if (monitor.force >= (int)made.forced.size()) {
            fail("a force monitor names a wall the case has not");
        }
    }
    return made;
}

std::vector<Vector3r> readPoints(const std::string& path, unsigned long count)
{
    std::vector<Vector3r> points(count);
    if (count == 0) {
        return points;
    }
    std::FILE* in = std::fopen(path.c_str(), "rb");
    if (!in) {
        fail("cannot read " + path);
    }
    std::vector<float> raw(count * 3);
    const std::size_t got = std::fread(raw.data(), sizeof(float), raw.size(), in);
    std::fclose(in);
    if (got != raw.size()) {
        fail(path + " is short");
    }
    for (unsigned long i = 0; i < count; i++) {
        points[i] = Vector3r(raw[3 * i], raw[3 * i + 1], raw[3 * i + 2]);
    }
    return points;
}

// The walls that stay, as a moved wall meets them: their particles in
// cells of the distance a moved wall is slowed within.
//
// A stand-in for contact, until a mechanism's solver has the moved walls
// (sec 11.5). A moved wall stops a particle's width off a fixed one: its
// particle that comes nearer a fixed wall's than their two sheets are with
// that much between the faces is pushed off it by a spring, damped, with
// no friction. The springs are as stiff as it takes for the wall to rest
// a twentieth of a particle's radius deep under its own weight, and to
// stop within half a radius at the speed it comes in at; the wall is
// stepped as finely as such springs need, the fluid's push held
// meanwhile. Without it a block heavier than the water went through the
// tank's floor.
//
// The width between is the water's: one layer of particles stays under a
// block that lands, and it is no thinner than a particle. With the faces
// let touch, the layer was crushed between the two sheets, and one
// landing in five threw the block back up and over (45 degrees and more)
// and 118 to 213 particles of 6336 through the floor; twelve of twelve
// are quiet this way, with none lost.
//
// And a particle's width before that, the wall is slowed: to the speed at
// which it falls a tenth of a radius from rest, or to a stop within a
// radius when it comes faster. The water that is more than the one layer
// has to get out sideways, as it does from under anything that lands.
struct Held
{
    Real reach = 0;  // within this the wall is slowed
    Real film = 0;   // and this much nearer it is held off
    std::unordered_map<std::uint64_t, std::vector<Vector3r>> cells;

    static std::uint64_t keyOf(long i, long j, long k)
    {
        const auto part = [](long v) {
            return static_cast<std::uint64_t>(v + (1L << 20)) & ((1ULL << 21) - 1);
        };
        return (part(i) << 42) | (part(j) << 21) | part(k);
    }
    void add(const Vector3r& p)
    {
        cells[keyOf((long)std::floor(p[0] / reach),
                    (long)std::floor(p[1] / reach),
                    (long)std::floor(p[2] / reach))]
            .push_back(p);
    }

    struct Touch
    {
        Vector3r at;      // the moved wall's particle
        Vector3r normal;  // off the fixed wall
        Real depth;  // past where it is held off; less than 0 before
    };
    // Where the wall's particles are within reach of these, the wall as it
    // is now.
    void touches(const Moved& body, BoundaryModel_Akinci2012& skin, std::vector<Touch>& found) const
    {
        found.clear();
        if (cells.empty()) {
            return;
        }
        const Matrix3r turn = body.q.matrix();
        for (unsigned int j = 0; j < skin.numberOfParticles(); j++) {
            const Vector3r p = turn * skin.getPosition0(j) + body.x;
            const long c[3] = {(long)std::floor(p[0] / reach),
                               (long)std::floor(p[1] / reach),
                               (long)std::floor(p[2] / reach)};
            for (long a = -1; a <= 1; a++) {
                for (long b = -1; b <= 1; b++) {
                    for (long d = -1; d <= 1; d++) {
                        const auto cell = cells.find(keyOf(c[0] + a, c[1] + b, c[2] + d));
                        if (cell == cells.end()) {
                            continue;
                        }
                        for (const Vector3r& other : cell->second) {
                            const Vector3r off = p - other;
                            const Real apart = off.norm();
                            if (apart < reach && apart > (Real)1e-9 * reach) {
                                found.push_back({p, off / apart, reach - film - apart});
                            }
                        }
                    }
                }
            }
        }
    }
};

// A moved wall through one step of the fluid's, against the walls that
// stay.
void stepBody(Moved& body,
              BoundaryModel_Akinci2012& skin,
              const Held& held,
              Real h,
              const Vector3r& gravity,
              Real radius)
{
    std::vector<Held::Touch> touching;
    Real done = 0;
    const Real g = std::max(gravity.norm(), (Real)1e-3);
    while (done < h * ((Real)1 - (Real)1e-9)) {
        held.touches(body, skin, touching);
        if (touching.empty()) {
            // free: one step to the end, or as far as it takes to come
            // within a quarter of the reach of where it is
            const Real speed = body.v.norm() + body.w.norm() * (Real)10 * held.reach;
            Real take = h - done;
            if (!held.cells.empty() && speed * take > (Real)0.25 * held.reach) {
                take = std::max((Real)0.25 * held.reach / speed, h * (Real)1e-3);
                take = std::min(take, h - done);
            }
            body.step(take, gravity, Vector3r::Zero(), Vector3r::Zero());
            done += take;
            continue;
        }
        // the fastest any of them comes in, and how many touch
        Real coming = 0;
        std::size_t pressed = 0;
        for (const Held::Touch& touch : touching) {
            const Vector3r speed = body.v + body.w.cross(touch.at - body.x);
            coming = std::max(coming, -speed.dot(touch.normal));
            pressed += touch.depth > 0 ? 1 : 0;
        }
        const Real rate = std::max(std::sqrt(g / ((Real)0.05 * radius)), coming / ((Real)0.5 * radius));
        const Real stiff = pressed > 0 ? body.mass * rate * rate / (Real)pressed : 0;
        const Real damp = pressed > 0 ? (Real)1.4 * body.mass * rate / (Real)pressed : 0;
        // the film: no spring, and a drag on what comes nearer
        const Real slow = std::max(g / std::sqrt((Real)0.1 * g * radius), coming / radius);
        const Real drag = body.mass * slow / (Real)touching.size();
        Vector3r force = Vector3r::Zero();
        Vector3r torque = Vector3r::Zero();
        for (const Held::Touch& touch : touching) {
            const Vector3r speed = body.v + body.w.cross(touch.at - body.x);
            const Real nearing = -speed.dot(touch.normal);
            Real push = std::max(nearing, (Real)0) * drag;
            if (touch.depth > 0) {
                // a push, never a pull
                push += std::max(stiff * touch.depth + damp * nearing, (Real)0);
            }
            force += push * touch.normal;
            torque += (touch.at - body.x).cross(push * touch.normal);
        }
        const Real take = std::min((Real)0.2 / std::max(rate, slow), h - done);
        body.step(take, gravity, force, torque);
        done += take;
    }
    body.force.setZero();
    body.torque.setZero();
}

// What a run is, once the solver is set up.
// An inlet as it runs.
struct Feed
{
    Inlet inlet;
    std::vector<Vector3r> layer;
    Vector3r up = Vector3r::Zero();  // the grid's second axis
    // the grid's places a particle of the layer is at, and the ones next
    // to those
    std::unordered_set<std::int64_t> cells;
    double next = 0.0;  // when the next layer is due
    unsigned long sent = 0;

    static std::int64_t key(long i, long j)
    {
        return ((std::int64_t)i << 32) ^ (std::int64_t)(j & 0xffffffffL);
    }
    // Whether a place is in the stretch before the face where the fluid
    // is carried at the inlet's speed, whatever presses on it: a kernel's
    // width, so that what is past it has fluid behind it and not a void.
    bool carries(const Vector3r& x, Real d, Real support) const
    {
        const Vector3r off = x - inlet.origin;
        const Real along = off.dot(inlet.n);
        if (along < -d || along > support) {
            return false;
        }
        return cells.count(key(std::lround(off.dot(inlet.across) / d), std::lround(off.dot(up) / d)))
            != 0;
    }
};

struct Run
{
    Case made;
    std::string folder;
    FluidModel* fluid = nullptr;
    BoundaryModel_Akinci2012* wall = nullptr;
    // the walls the fluid moves, and their particles
    std::vector<Moved*> moved;
    std::vector<BoundaryModel_Akinci2012*> skins;
    Held held;
    // the walls a force is asked of: held still, and bodies to the solver
    // so that it adds up what the fluid pushes them with
    std::vector<BoundaryModel_Akinci2012*> forced;
    std::vector<Vector3r> pushed;  // on each, in the last step, N
    std::vector<Feed> feeds;
    Real support = 0;
    Vector3r up = Vector3r::Zero();  // against gravity; zero without any
    Vector3r park = Vector3r::Zero();
    unsigned long parked = 0;
};

// A particle that left the space is gone: it is put far away, apart from
// the others there, and held. The solver has no way to take one out.
void parkTheGone(Run& run)
{
    FluidModel* fluid = run.fluid;
    // by more than a kernel: a particle against an open side's rim still
    // presses on the ones inside
    const Real margin = run.support;
    for (unsigned int i = 0; i < fluid->numActiveParticles(); i++) {
        if (fluid->getParticleState(i) == ParticleState::Fixed) {
            continue;
        }
        const Vector3r& x = fluid->getPosition(i);
        bool out = false;
        for (int k = 0; k < 3; k++) {
            out = out || x[k] < run.made.spaceMin[k] - margin || x[k] > run.made.spaceMax[k] + margin;
        }
        if (out) {
            fluid->getPosition(i) = run.park + Vector3r((Real)run.parked * 4 * run.support, 0, 0);
            fluid->getVelocity(i).setZero();
            fluid->setParticleState(i, ParticleState::Fixed);
            run.parked++;
        }
    }
}

// A moved wall's particles where the wall is now, moving as it does.
void place(const Moved& body, BoundaryModel_Akinci2012& skin)
{
    const Matrix3r turn = body.q.matrix();
    const int count = (int)skin.numberOfParticles();
    #pragma omp parallel for schedule(static) default(shared)
    for (int j = 0; j < count; j++) {
        skin.getPosition(j) = turn * skin.getPosition0(j) + body.x;
        skin.getVelocity(j) = body.w.cross(skin.getPosition(j) - body.x) + body.v;
    }
}

// The inlets after a step of that length that ended at that time.
//
// As the solver's own emitters go, which are rectangles and circles only:
// the fluid in the stretch before the face is carried at the inlet's
// speed -- the solver leaves a particle so marked where it is, and it is
// moved here -- and a layer is put on the face each time the last has gone
// a particle's width, placed as far on as it is late. A particle comes
// from the ones that left the space, or else from the ones kept for the
// inlets when the fluid was made.
void feedInlets(Run& run, double time, Real step)
{
    if (run.feeds.empty()) {
        return;
    }
    Simulation* sim = Simulation::getCurrent();
    FluidModel* fluid = run.fluid;
    const Real d = (Real)(2 * run.made.radius);
    const double before = time - step;
    const int active = (int)fluid->numActiveParticles();
    #pragma omp parallel for schedule(static) default(shared)
    for (int i = 0; i < active; i++) {
        const ParticleState state = fluid->getParticleState(i);
        if (state == ParticleState::Fixed) {
            continue;
        }
        fluid->setParticleState(i, ParticleState::Active);
        if (state == ParticleState::AnimatedByEmitter) {
            // The solver did not move it in this step: it is moved here,
            // as it was going, in the stretch still or just out of it.
            // (The solver's own emitters leave a particle where it is for
            // the step it comes out of the stretch in; the layer behind
            // then runs into it, and the jet came apart from there on, its
            // front half as fast again as it was sent.)
            fluid->getPosition(i) += step * fluid->getVelocity(i);
        }
        for (const Feed& feed : run.feeds) {
            if (before < feed.inlet.start || before > feed.inlet.end
                || !feed.carries(fluid->getPosition(i), d, run.support)) {
                continue;
            }
            fluid->getVelocity(i) = (Real)feed.inlet.speed * feed.inlet.n;
            fluid->setParticleState(i, ParticleState::AnimatedByEmitter);
            break;
        }
    }
    for (Feed& feed : run.feeds) {
        while (before >= feed.next && feed.next <= feed.inlet.end) {
            const Vector3r speed = (Real)feed.inlet.speed * feed.inlet.n;
            const Vector3r on = (Real)(before - feed.next + step) * speed;
            // the ones that left the space first; the solver sorts its
            // particles as it likes, so they are looked for each time
            std::vector<unsigned int> free;
            for (unsigned int i = 0; i < fluid->numActiveParticles() && free.size() < feed.layer.size();
                 i++) {
                if (fluid->getParticleState(i) == ParticleState::Fixed) {
                    free.push_back(i);
                }
            }
            const unsigned int had = fluid->numActiveParticles();
            unsigned int fresh = 0;
            for (std::size_t k = 0; k < feed.layer.size(); k++) {
                unsigned int index = 0;
                if (k < free.size()) {
                    index = free[k];
                }
                else if (had + fresh < fluid->numParticles()) {
                    index = had + fresh++;
                }
                else {
                    fail("the inlets ran out of particles");
                }
                fluid->getPosition(index) = feed.layer[k] + on;
                fluid->getVelocity(index) = speed;
                fluid->setParticleState(index, ParticleState::AnimatedByEmitter);
            }
            if (fresh != 0) {
                fluid->setNumActiveParticles(had + fresh);
                sim->emittedParticles(fluid, had);
                sim->getNeighborhoodSearch()->resizeSet(fluid->getPointSetIndex(),
                                                        &fluid->getPosition(0)[0],
                                                        fluid->numActiveParticles());
            }
            feed.sent++;
            feed.next += (double)d / feed.inlet.speed;
        }
    }
}

void writeFrame(Run& run, int index, double time)
{
    // where the moved walls are, before the frame they are of
    for (std::size_t k = 0; k < run.moved.size(); k++) {
        const Moved& body = *run.moved[k];
        const Vector3r shift = body.x - body.q.matrix() * body.start;
        std::printf("body %zu %.9g %.9g %.9g %.9g %.9g %.9g %.9g\n", k, (double)shift[0],
                    (double)shift[1], (double)shift[2], (double)body.q.x(), (double)body.q.y(),
                    (double)body.q.z(), (double)body.q.w());
    }
    FluidModel* fluid = run.fluid;
    std::vector<float> raw;
    raw.reserve(3 * (std::size_t)fluid->numActiveParticles());
    for (unsigned int i = 0; i < fluid->numActiveParticles(); i++) {
        if (fluid->getParticleState(i) == ParticleState::Fixed) {
            continue;
        }
        const Vector3r& x = fluid->getPosition(i);
        raw.push_back(x[0]);
        raw.push_back(x[1]);
        raw.push_back(x[2]);
    }
    const std::string name = run.folder + "/frame." + std::to_string(index);
    const std::string part = name + ".part";
    std::FILE* out = std::fopen(part.c_str(), "wb");
    if (!out || std::fwrite(raw.data(), sizeof(float), raw.size(), out) != raw.size()) {
        fail("cannot write " + part);
    }
    std::fclose(out);
    // whole before it has its name: the reader never sees half of one
    if (std::rename(part.c_str(), name.c_str()) != 0) {
        fail("cannot write " + name);
    }
    std::printf("frame %d %.9g %zu\n", index, time, raw.size() / 3);
    std::fflush(stdout);
}

// The monitors now.
//
// A height is the fluid above the point in a column around the line against
// gravity, as a depth: the particles there, each a cube of the spacing,
// over the column's area. The column has no edge: a particle counts the
// less the farther it is from the line, to nothing at the column's radius,
// the weights adding up to one over the area. With an edge, particles on a
// grid of the spacing came to 4% more or less depth by where the line stood
// among them.
//
// A pressure on a wall is the fluid's push on the wall's particles around
// the point, along the wall's normal, over the area they stand for. The
// solver's own pressure at the particles against a wall is not the wall's:
// a wall's particle has none to answer with, and the fluid's there carries
// both halves (measured in still water: half as much again at the floor).
// The push is what holds the water up, whatever the particles' pressures.
//
// A pressure in the open fluid is the solver's own at the particles within
// a kernel of the point, weighted by the kernel; none there is none.
void writeSample(Run& run, double time, Real stepUsed)
{
    if (run.made.monitors.empty()) {
        return;
    }
    Simulation* sim = Simulation::getCurrent();
    FluidModel* fluid = run.fluid;
    const Real r = (Real)run.made.radius;
    const Real d = 2 * r;
    const Real column = (Real)2.5 * d;
    const Real patch = (Real)1.5 * d;
    const std::size_t n = run.made.monitors.size();
    std::vector<double> sum(n, 0.0);
    std::vector<double> weight(n, 0.0);
    // the wall's particles a monitor's push is taken on
    // of the walls held still, the ones a force is asked of with them
    struct Stand
    {
        BoundaryModel_Akinci2012* wall;
        unsigned int j;
    };
    std::vector<std::vector<Stand>> stands(n);
    std::vector<BoundaryModel_Akinci2012*> still = run.forced;
    if (run.wall) {
        still.push_back(run.wall);
    }
    for (BoundaryModel_Akinci2012* wall : still) {
        for (std::size_t m = 0; m < n; m++) {
            const Monitor& monitor = run.made.monitors[m];
            if (monitor.force >= 0 || !monitor.pressure || monitor.normal.squaredNorm() == 0) {
                continue;
            }
            for (unsigned int j = 0; j < wall->numberOfParticles(); j++) {
                const Vector3r off = wall->getPosition(j) - monitor.point;
                const Real along = off.dot(monitor.normal);
                // behind the face, on its own sheet or its crease's row
                if (along < 0 && along > -2 * r
                    && (off - along * monitor.normal).squaredNorm() <= patch * patch) {
                    stands[m].push_back({wall, j});
                }
            }
        }
    }
    const FieldDescription* field = nullptr;
    for (unsigned int k = 0; k < fluid->numberOfFields(); k++) {
        if (fluid->getField(k).name == "p / rho^2") {
            field = &fluid->getField(k);
        }
    }
    // The solver keeps the pressure over the density's square, times the
    // step's square, the density taken as 1 at rest.
    const double perStep = stepUsed > 0 ? 1.0 / ((double)stepUsed * stepUsed) : 0.0;
    for (unsigned int i = 0; i < fluid->numActiveParticles(); i++) {
        if (fluid->getParticleState(i) == ParticleState::Fixed) {
            continue;
        }
        const Vector3r& x = fluid->getPosition(i);
        const double kept = field ? *static_cast<Real*>(field->getFct(i)) * perStep : 0.0;
        for (std::size_t m = 0; m < n; m++) {
            const Monitor& monitor = run.made.monitors[m];
            if (monitor.force >= 0) {
                continue;
            }
            const Vector3r off = x - monitor.point;
            if (!monitor.pressure) {
                const Real along = off.dot(run.up);
                const double aside = (off - along * run.up).squaredNorm() / (column * column);
                if (along >= -r && aside < 1.0) {
                    sum[m] += (1.0 - aside) * (1.0 - aside);
                }
            }
            else if (!stands[m].empty()) {
                const Real reach = patch + run.support + 2 * r;
                if (off.squaredNorm() > reach * reach) {
                    continue;
                }
                for (const Stand& stand : stands[m]) {
                    const Vector3r between = x - stand.wall->getPosition(stand.j);
                    if (between.squaredNorm() < run.support * run.support) {
                        // on the wall's particle, towards it from the fluid's
                        const Vector3r push = (Real)(fluid->getMass(i) * kept)
                            * stand.wall->getVolume(stand.j)
                            * sim->gradW(between);
                        sum[m] -= push.dot(monitor.normal);
                    }
                }
            }
            else if (monitor.normal.squaredNorm() == 0 && off.squaredNorm() < run.support * run.support) {
                const double w = sim->W(off);
                sum[m] += w * kept * run.made.density;
                weight[m] += w;
            }
        }
    }
    std::string line = "sample ";
    char text[40];
    std::snprintf(text, sizeof(text), "%.9g", time);
    line += text;
    const double each = (1.5 * r) * (1.5 * r);  // the area one wall particle stands for
    for (std::size_t m = 0; m < n; m++) {
        double value = 0.0;
        if (run.made.monitors[m].force >= 0) {
            value = (double)run.pushed[(std::size_t)run.made.monitors[m].force].dot(
                run.made.monitors[m].normal);
        }
        else if (!run.made.monitors[m].pressure) {
            value = run.up.squaredNorm() > 0
                ? sum[m] * (double)d * d * d * 3.0 / (3.14159265358979323846 * column * column)
                : std::numeric_limits<double>::quiet_NaN();
        }
        else if (!stands[m].empty()) {
            value = sum[m] / (each * (double)stands[m].size());
        }
        else if (weight[m] > 0.0) {
            value = sum[m] / weight[m];
        }
        std::snprintf(text, sizeof(text), " %.9g", value);
        line += text;
    }
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && std::string(argv[1]) == "--version") {
        std::printf("version %s\n", SPLISHSPLASH_VERSION);
        return 0;
    }
    if (argc != 2) {
        std::printf("error usage: FluidSplish <folder> | --version\n");
        return 2;
    }
    Run run;
    run.folder = argv[1];
    run.made = readCase(run.folder);
    const Case& made = run.made;
    std::printf("version %s\n", SPLISHSPLASH_VERSION);
#ifdef _OPENMP
    omp_set_num_threads(made.threads);
#endif

    std::vector<Vector3r> x = readPoints(run.folder + "/fluid.points", made.fluid);
    std::vector<Vector3r> wall = readPoints(run.folder + "/boundary.points", made.boundary);

    Simulation* sim = Simulation::getCurrent();
    sim->init((Real)made.radius, false);
    // the walls are particles, sampled by the workbench where the fluid can
    // reach
    sim->setValue<int>(Simulation::BOUNDARY_HANDLING_METHOD, Simulation::ENUM_AKINCI2012);

    std::vector<Vector3r> v(x.size(), Vector3r::Zero());
    std::vector<unsigned int> ids(x.size(), 0);
    // before the fluid is made: it lists the methods there are then
    sim->addSurfaceTensionMethod(Cohesion::Name, Cohesion::creator);
    // The inlets' particles are kept from the start: a layer each time the
    // fluid has gone a particle's width, all the while an inlet runs.
    unsigned long kept = 0;
    for (const Inlet& inlet : made.inlets) {
        Feed feed;
        feed.inlet = inlet;
        feed.inlet.end = std::min(inlet.end, made.end);
        feed.next = inlet.start;
        feed.up = inlet.n.cross(inlet.across);
        feed.layer = readPoints(run.folder + "/inlet." + std::to_string(run.feeds.size()) + ".points",
                                inlet.count);
        for (const Vector3r& p : feed.layer) {
            const Vector3r off = p - inlet.origin;
            const long i = std::lround(off.dot(inlet.across) / (2 * made.radius));
            const long j = std::lround(off.dot(feed.up) / (2 * made.radius));
            for (long di = -1; di <= 1; di++) {
                for (long dj = -1; dj <= 1; dj++) {
                    feed.cells.insert(Feed::key(i + di, j + dj));
                }
            }
        }
        const double lasts = std::max(0.0, feed.inlet.end - inlet.start);
        kept += inlet.count * ((unsigned long)(lasts * inlet.speed / (2 * made.radius)) + 2);
        run.feeds.push_back(feed);
    }
    sim->addFluidModel("Fluid", (unsigned int)x.size(), x.data(), v.data(), ids.data(),
                       (unsigned int)kept);
    FluidModel* fluid = sim->getFluidModel(0);
    run.fluid = fluid;
    fluid->setDensity0((Real)made.density);
    fluid->setViscosityMethod("Standard");
    if (fluid->getViscosityBase()) {
        fluid->getViscosityBase()->setValue<Real>(Viscosity_Standard::VISCOSITY_COEFFICIENT,
                                                 (Real)made.viscosity);
    }
    // Cohesion between the fluid's particles, when the case asks for it:
    // none towards the walls, which stay as the validated runs have them.
    if (made.cohesion > 0.0) {
        fluid->setSurfaceTensionMethod(Cohesion::Name);
        if (auto* cohesion = dynamic_cast<Cohesion*>(fluid->getSurfaceTensionBase())) {
            cohesion->setStrength((Real)made.cohesion);
        }
        else {
            std::printf("error the solver has no cohesion\n");
            return 1;
        }
    }

    Vector3r g = made.gravity;
    sim->setVecValue<Real>(Simulation::GRAVITATION, g.data());
    sim->setValue<int>(Simulation::CFL_METHOD, Simulation::ENUM_CFL_STANDARD);
    sim->setSimulationMethod(Simulation::ENUM_SIMULATION_DFSPH);

    BoundaryModel_Akinci2012* bm = nullptr;
    if (!wall.empty()) {
        StaticRigidBody* body = new StaticRigidBody();
        body->setPosition0(Vector3r::Zero());
        body->setPosition(Vector3r::Zero());
        body->setRotation0(Quaternionr::Identity());
        body->setRotation(Quaternionr::Identity());
        bm = new BoundaryModel_Akinci2012();
        bm->initModel(body, (unsigned int)wall.size(), wall.data());
        sim->addBoundaryModel(bm);
        run.wall = bm;
    }

    // The walls the fluid moves, each a body of its own: its particles are
    // kept about its centre of mass and put where it is.
    for (std::size_t k = 0; k < made.bodies.size(); k++) {
        const Body& given = made.bodies[k];
        std::vector<Vector3r> skin
            = readPoints(run.folder + "/body." + std::to_string(k) + ".points", given.count);
        for (Vector3r& p : skin) {
            p -= given.centre;
        }
        Moved* body = new Moved();
        body->mass = (Real)given.mass;
        body->start = given.centre;
        body->x = given.centre;
        const double* i = given.inertia;
        body->inertia << (Real)i[0], (Real)i[3], (Real)i[4], (Real)i[3], (Real)i[1], (Real)i[5],
            (Real)i[4], (Real)i[5], (Real)i[2];
        if (!(std::abs(body->inertia.determinant()) > 0)) {
            fail("a wall the fluid moves has no inertia");
        }
        BoundaryModel_Akinci2012* model = new BoundaryModel_Akinci2012();
        model->initModel(body, (unsigned int)skin.size(), skin.data());
        place(*body, *model);
        sim->addBoundaryModel(model);
        run.moved.push_back(body);
        run.skins.push_back(model);
    }

    // The walls a force is asked of: the solver adds up the fluid's push
    // on a body's particles and not on a fixed wall's, so each is a body
    // to it, one that is never moved.
    for (std::size_t k = 0; k < made.forced.size(); k++) {
        std::vector<Vector3r> skin
            = readPoints(run.folder + "/forced." + std::to_string(k) + ".points", made.forced[k]);
        Moved* body = new Moved();
        body->mass = 1;
        BoundaryModel_Akinci2012* model = new BoundaryModel_Akinci2012();
        model->initModel(body, (unsigned int)skin.size(), skin.data());
        place(*body, *model);
        sim->addBoundaryModel(model);
        run.forced.push_back(model);
        run.pushed.push_back(Vector3r::Zero());
    }

    // the fluid's points are registered with the neighbour search here,
    // then the walls': in that order, once each
    sim->setSimulationInitialized(true);
    if (bm) {
        bm->deferredInit();
    }
    for (BoundaryModel_Akinci2012* skin : run.skins) {
        skin->deferredInit();
    }
    for (BoundaryModel_Akinci2012* skin : run.forced) {
        skin->deferredInit();
    }
    sim->performNeighborhoodSearchSort();
    if (bm) {
        for (unsigned int j = 0; j < bm->numberOfParticles(); j++) {
            bm->getPosition(j) = bm->getPosition0(j);
            bm->getVelocity(j).setZero();
        }
    }
    for (std::size_t k = 0; k < run.moved.size(); k++) {
        place(*run.moved[k], *run.skins[k]);
    }
    for (BoundaryModel_Akinci2012* skin : run.forced) {
        for (unsigned int j = 0; j < skin->numberOfParticles(); j++) {
            skin->getPosition(j) = skin->getPosition0(j);
            skin->getVelocity(j).setZero();
        }
    }
    // two sheets are each this far behind their faces (the workbench's
    // SplishCase sizesOf); a particle's width between the faces where the
    // moved wall is held off, and another before it where it is slowed
    run.held.film = (Real)(2 * made.radius);
    run.held.reach = (Real)(2 * 1.2 * made.radius) + 2 * run.held.film;
    if (bm && !run.moved.empty()) {
        for (unsigned int j = 0; j < bm->numberOfParticles(); j++) {
            run.held.add(bm->getPosition(j));
        }
    }
    if (!run.moved.empty()) {
        for (BoundaryModel_Akinci2012* skin : run.forced) {
            for (unsigned int j = 0; j < skin->numberOfParticles(); j++) {
                run.held.add(skin->getPosition(j));
            }
        }
    }
    if (bm || !run.skins.empty() || !run.forced.empty()) {
        sim->updateBoundaryVolume();
    }

    run.support = sim->getSupportRadius();
    if (g.norm() > 0) {
        run.up = -g / g.norm();
    }
    run.park = made.spaceMax + 100 * (made.spaceMax - made.spaceMin)
        + Vector3r(100 * run.support, 100 * run.support, 100 * run.support);

    TimeManager* tm = TimeManager::getCurrent();
    tm->setTime(0);
    int index = 0;
    writeFrame(run, index++, 0.0);
    writeSample(run, 0.0, 0);
    // the time is kept here: the solver's own is a float, and a long run's
    // steps would not add up in it
    double time = 0.0;
    // The solver sets its step itself in the middle of one, from the
    // particles' speeds, and what it took is known only afterwards. A
    // frame is the first step at or past its time, and says the time it is
    // of: cutting a step short to meet a frame's time exactly was tried,
    // and the pressure of such a sliver of a step is a spike of its own
    // making (the solver's pressure is what undoes a step's compression,
    // over the step's square).
    sim->setValue<Real>(Simulation::CFL_MIN_TIMESTEPSIZE, (Real)1e-5);
    // The longest step: the solver's own 5 ms, and no longer than gravity
    // takes to move a particle a hundredth of the spacing from rest. The
    // solver's limit from the speeds says nothing of water at rest, and at
    // 5 mm of spacing a tank of still water run at 5 ms flew apart: each
    // step's fall was more compression than the pressure solver undoes.
    double longest = std::min(0.005, made.interval);
    if (g.norm() > 0) {
        longest = std::min(longest, std::sqrt(0.01 * 2 * made.radius / (double)g.norm()));
    }
    sim->setValue<Real>(Simulation::CFL_MAX_TIMESTEPSIZE, (Real)longest);
    while (time < made.end - 1e-9) {
        sim->getTimeStep()->step();
        const Real step = tm->getTimeStepSize();
        time += step;
        // a run that has come apart says so and stops: every step after is
        // of nothing, and it was an hour of them once
        const unsigned int active = fluid->numActiveParticles();
        for (unsigned int i = 0; i < active; i++) {
            if (!fluid->getPosition(i).allFinite()) {
                std::printf("error the solver lost the fluid at %.9g s\n", time);
                return 1;
            }
        }
        // the moved walls, by what the fluid pushed them with in this step
        for (std::size_t k = 0; k < run.moved.size(); k++) {
            Moved& body = *run.moved[k];
            Vector3r force;
            Vector3r torque;
            run.skins[k]->getForceAndTorque(force, torque);
            run.skins[k]->clearForceAndTorque();
            body.addForce(force);
            body.addTorque(torque);
            stepBody(body, *run.skins[k], run.held, step, g, (Real)made.radius);
            if (!body.x.allFinite() || !body.q.coeffs().allFinite()) {
                std::printf("error the solver lost a moved wall at %.9g s\n", time);
                return 1;
            }
            place(body, *run.skins[k]);
        }
        for (std::size_t k = 0; k < run.forced.size(); k++) {
            Vector3r torque;
            run.forced[k]->getForceAndTorque(run.pushed[k], torque);
            run.forced[k]->clearForceAndTorque();
        }
        parkTheGone(run);
        feedInlets(run, time, step);
        writeSample(run, time, step);
        const double next = std::min(index * made.interval, made.end);
        if (time >= next - 1e-9) {
            writeFrame(run, index++, time);
        }
    }
    return 0;
}
