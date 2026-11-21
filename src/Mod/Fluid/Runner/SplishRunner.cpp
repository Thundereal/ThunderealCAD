// SPDX-License-Identifier: LGPL-2.1-or-later

// FluidSplish: SPlisHSPlasH (MIT) run on a case the Fluid workbench wrote
// (docs/FluidSimulation.md sec 12 S3). A program of its own, built against
// the solver with the solver's own compiler settings and its own Eigen, so
// that none of either is in FreeCAD's process.
//
//   FluidSplish --version
//   FluidSplish <folder>
//
// The folder holds case.txt, fluid.points and boundary.points (three floats
// a point, metres). Lines on standard output say what happened:
//
//   version <the solver's>
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
#include <string>
#include <vector>

#ifdef _OPENMP
# include <omp.h>
#endif

#include "SPlisHSPlasH/BoundaryModel_Akinci2012.h"
#include "SPlisHSPlasH/Simulation.h"
#include "SPlisHSPlasH/StaticRigidBody.h"
#include "SPlisHSPlasH/TimeManager.h"
#include "SPlisHSPlasH/TimeStep.h"
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

struct Monitor
{
    bool pressure = false;
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
    int threads = 1;
    Vector3r spaceMin = Vector3r::Zero();
    Vector3r spaceMax = Vector3r::Zero();
    unsigned long boundary = 0;
    unsigned long fluid = 0;
    std::vector<Monitor> monitors;
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
        double a[6] = {0, 0, 0, 0, 0, 0};
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
        else if (key == "monitor") {
            char kind[32] = "";
            const int got = std::sscanf(rest, "%31s %lf %lf %lf %lf %lf %lf", kind, &a[0], &a[1], &a[2],
                                        &a[3], &a[4], &a[5]);
            if (got == 4 || got == 7) {
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
    if (!(made.radius > 0.0) || made.fluid == 0 || !(made.interval > 0.0) || !(made.density > 0.0)) {
        fail("the case has no fluid, or no size");
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

// What a run is, once the solver is set up.
struct Run
{
    Case made;
    std::string folder;
    FluidModel* fluid = nullptr;
    BoundaryModel_Akinci2012* wall = nullptr;
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

void writeFrame(Run& run, int index, double time)
{
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
    std::vector<std::vector<unsigned int>> stands(n);
    if (run.wall) {
        for (std::size_t m = 0; m < n; m++) {
            const Monitor& monitor = run.made.monitors[m];
            if (!monitor.pressure || monitor.normal.squaredNorm() == 0) {
                continue;
            }
            for (unsigned int j = 0; j < run.wall->numberOfParticles(); j++) {
                const Vector3r off = run.wall->getPosition(j) - monitor.point;
                const Real along = off.dot(monitor.normal);
                // behind the face, on its own sheet or its crease's row
                if (along < 0 && along > -2 * r
                    && (off - along * monitor.normal).squaredNorm() <= patch * patch) {
                    stands[m].push_back(j);
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
                for (unsigned int j : stands[m]) {
                    const Vector3r between = x - run.wall->getPosition(j);
                    if (between.squaredNorm() < run.support * run.support) {
                        // on the wall's particle, towards it from the fluid's
                        const Vector3r push = (Real)(fluid->getMass(i) * kept) * run.wall->getVolume(j)
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
        if (!run.made.monitors[m].pressure) {
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
    sim->addFluidModel("Fluid", (unsigned int)x.size(), x.data(), v.data(), ids.data(), 0);
    FluidModel* fluid = sim->getFluidModel(0);
    run.fluid = fluid;
    fluid->setDensity0((Real)made.density);
    fluid->setViscosityMethod("Standard");
    if (fluid->getViscosityBase()) {
        fluid->getViscosityBase()->setValue<Real>(Viscosity_Standard::VISCOSITY_COEFFICIENT,
                                                 (Real)made.viscosity);
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

    // the fluid's points are registered with the neighbour search here,
    // then the walls': in that order, once each
    sim->setSimulationInitialized(true);
    if (bm) {
        bm->deferredInit();
    }
    sim->performNeighborhoodSearchSort();
    if (bm) {
        for (unsigned int j = 0; j < bm->numberOfParticles(); j++) {
            bm->getPosition(j) = bm->getPosition0(j);
            bm->getVelocity(j).setZero();
        }
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
        parkTheGone(run);
        writeSample(run, time, step);
        const double next = std::min(index * made.interval, made.end);
        if (time >= next - 1e-9) {
            writeFrame(run, index++, time);
        }
    }
    return 0;
}
