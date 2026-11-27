// SPDX-License-Identifier: LGPL-2.1-or-later

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

#ifndef _WIN32
# include <csignal>
# include <unistd.h>
#endif

#include <QCoreApplication>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include <App/Application.h>
#include <Base/Console.h>
#include <Base/Parameter.h>

#include "OpenFoamBackend.h"
#include "OpenFoamCase.h"

using namespace Fluid::Solver;
namespace fs = std::filesystem;

namespace
{
ParameterGrp::handle preferences()
{
    return App::GetApplication().GetParameterGroupByPath(
        "User parameter:BaseApp/Preferences/Mod/Fluid");
}

std::string readFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

std::string quoted(const std::string& text)
{
    std::string out = "'";
    for (char c : text) {
        if (c == '\'') {
            out += "'\\''";
        }
        else {
            out += c;
        }
    }
    return out + "'";
}

// This process's environment without what points programs at its own
// libraries: an OpenFOAM of another prefix must load its own.
QProcessEnvironment cleanEnvironment()
{
    QProcessEnvironment env;
    const QProcessEnvironment mine = QProcessEnvironment::systemEnvironment();
    for (const char* name : {"HOME", "USER", "LOGNAME", "LANG", "TMPDIR", "SHELL"}) {
        if (mine.contains(QString::fromLatin1(name))) {
            env.insert(QString::fromLatin1(name), mine.value(QString::fromLatin1(name)));
        }
    }
    env.insert(QStringLiteral("PATH"), QStringLiteral("/usr/local/bin:/usr/bin:/bin"));
    return env;
}

// A shell command run to its end, or until stop says so: its exit code, -1
// when it could not start, -7 when stopped. The command is the head of a
// process group of its own, and stopping ends the whole group -- mpirun and
// the solvers under it.
int runShell(const std::string& command,
             bool clean,
             const std::function<bool()>& stop,
             const std::function<void()>& idle,
             std::string* output = nullptr)
{
#ifdef _WIN32
    (void)command;
    (void)clean;
    (void)stop;
    (void)idle;
    (void)output;
    return -1;
#else
    QProcess process;
    process.setProcessEnvironment(clean ? cleanEnvironment()
                                        : QProcessEnvironment::systemEnvironment());
    process.setChildProcessModifier([] {
        ::setsid();
    });
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(QStringLiteral("bash"),
                  QStringList() << QStringLiteral("-c") << QString::fromStdString(command));
    if (!process.waitForStarted(10000)) {
        return -1;
    }
    const auto pid = static_cast<pid_t>(process.processId());
    while (!process.waitForFinished(200)) {
        if (output) {
            *output += process.readAll().toStdString();
        }
        if (stop && stop()) {
            ::kill(-pid, SIGTERM);
            if (!process.waitForFinished(5000)) {
                ::kill(-pid, SIGKILL);
                process.waitForFinished(5000);
            }
            return -7;
        }
        if (idle) {
            idle();
        }
    }
    if (output) {
        *output += process.readAll().toStdString();
    }
    return process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
#endif
}

// How a folder's OpenFOAM is made callable; empty when the folder holds
// none.
std::string preambleOf(const fs::path& place)
{
    std::error_code ec;
    if (fs::is_regular_file(place, ec)) {
        return "source " + quoted(place.string());
    }
    // a conda prefix: its activation needs no conda
    const fs::path conda = place / "etc" / "conda" / "activate.d" / "openfoam_activate.sh";
    if (fs::exists(conda, ec)) {
        return "export CONDA_PREFIX=" + quoted(place.string()) + "; export PATH="
            + quoted((place / "bin").string()) + ":$PATH; source " + quoted(conda.string());
    }
    if (fs::exists(place / "etc" / "bashrc", ec)) {
        return "source " + quoted((place / "etc" / "bashrc").string());
    }
    return {};
}

// Whether that preamble gives an interFoam we can drive; what it is.
bool probe(const std::string& preamble, bool clean, OpenFoamInstall& found, std::string& whyNot)
{
    std::string out;
    const std::string script =
        (preamble.empty() ? std::string() : preamble + " >/dev/null 2>&1; ")
        + "command -v interFoam >/dev/null 2>&1 || { echo NONE; exit 0; }; "
          "echo \"API ${FOAM_API:-none}\"; "
          "if ldd \"$(command -v interFoam)\" 2>/dev/null | grep -q '/dummy/libPstream'; "
          "then echo SERIAL; else echo PARALLEL; fi";
    if (runShell(script, clean, nullptr, nullptr, &out) != 0 || out.find("NONE") != std::string::npos
        || out.find("API ") == std::string::npos) {
        whyNot = "no interFoam";
        return false;
    }
    std::istringstream in(out.substr(out.find("API ") + 4));
    std::string api;
    in >> api;
    if (api == "none") {
        // The Foundation's releases have no FOAM_API; their dictionaries
        // differ from what this backend writes
        whyNot = "an OpenFOAM of the Foundation's line; the backend writes for the ESI "
                 "releases (v2012 and later)";
        return false;
    }
    found.found = true;
    found.preamble = preamble;
    found.cleanEnvironment = clean;
    found.version = api;
    found.parallel = out.find("PARALLEL") != std::string::npos;
    return true;
}

// One duct of the check, run in a folder of its own under base: what its
// flow lost, NaN when it did not run.
double ductRun(const std::string& preamble, bool clean, const fs::path& base, int axis)
{
    const double none = std::numeric_limits<double>::quiet_NaN();
    const OpenFoam::Case made = OpenFoam::ductCase(axis);
    const fs::path folder = base / ("duct" + std::to_string(axis));
    try {
        for (const auto& [name, text] : made.files) {
            const fs::path path = folder / name;
            fs::create_directories(path.parent_path());
            std::ofstream out(path, std::ios::binary);
            out << text;
            if (!out) {
                return none;
            }
        }
    }
    catch (const std::exception&) {
        return none;
    }
    for (const auto& step : made.steps) {
        const std::string command = (preamble.empty() ? std::string() : preamble + " >/dev/null 2>&1; ")
            + "cd " + quoted(folder.string()) + " && " + step.command + " > log." + step.name
            + " 2>&1";
        if (runShell(command, clean, nullptr, nullptr) != 0) {
            return none;
        }
    }
    return OpenFoam::ductLoss(readFile(folder / "postProcessing" / "pressure" / "0" / "p"));
}

// The verdicts of the check kept from one session to the next, by what
// was tested: the solver's stress library, its size and its date.
fs::path verdictsFile()
{
    return fs::path(App::Application::getUserCachePath()) / "Fluid" / "openfoam-viscous.txt";
}

std::string testedOf(const std::string& preamble, bool clean)
{
    std::string out;
    const std::string script = (preamble.empty() ? std::string() : preamble + " >/dev/null 2>&1; ")
        + "lib=$(ldd \"$(command -v simpleFoam)\" 2>/dev/null | "
          "awk '/libincompressibleTurbulenceModels/ {print $3; exit}'); "
          "[ -n \"$lib\" ] && stat -Lc 'TESTED %n %s %Y' \"$lib\"";
    if (runShell(script, clean, nullptr, nullptr, &out) != 0) {
        return {};
    }
    const auto at = out.find("TESTED ");
    if (at == std::string::npos) {
        return {};
    }
    std::string key = out.substr(at + 7);
    key.erase(key.find_last_not_of("\r\n") + 1);
    return key;
}

std::vector<fs::path> globbed(const fs::path& folder, const std::string& prefix)
{
    std::vector<fs::path> found;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(folder, ec)) {
        if (entry.path().filename().string().rfind(prefix, 0) == 0) {
            found.push_back(entry.path());
        }
    }
    // the newest release first
    std::ranges::sort(found, std::greater<>());
    return found;
}

OpenFoamInstall search()
{
    OpenFoamInstall install;
#ifdef _WIN32
    install.searched = "on Windows OpenFOAM runs under WSL, which this backend does not "
                       "drive yet";
    return install;
#else
    std::vector<std::string> looked;
    std::string refused;
    auto attempt = [&](const std::string& where, const std::string& preamble, bool clean) {
        std::string whyNot;
        if (probe(preamble, clean, install, whyNot)) {
            const std::string fault = OpenFoamBackend::viscousFault(preamble, clean);
            if (fault.empty()) {
                install.where = where;
                install.refused = refused;
                return true;
            }
            // found, and not to be used: the search goes on
            install = OpenFoamInstall {};
            whyNot = "not used";
            if (refused.empty()) {
                refused = "The OpenFOAM at " + where + " is not used: " + fault + ".";
            }
        }
        looked.push_back(where + " (" + whyNot + ")");
        return false;
    };
    const std::string set = preferences()->GetASCII("OpenFoamPath", "");
    if (!set.empty()) {
        const std::string preamble = preambleOf(set);
        if (preamble.empty()) {
            looked.push_back(set + " (the preference OpenFoamPath: no OpenFOAM there)");
        }
        else if (attempt(set, preamble, true)) {
            return install;
        }
    }
    // the environment's own word for where it is, before its PATH
    if (const char* named = std::getenv("FLUID_OPENFOAM_PATH"); named && *named) {
        const std::string preamble = preambleOf(named);
        if (preamble.empty()) {
            looked.push_back(std::string(named) + " (FLUID_OPENFOAM_PATH: no OpenFOAM there)");
        }
        else if (attempt(named, preamble, true)) {
            return install;
        }
    }
    if (attempt("the environment", "", false)) {
        return install;
    }
    for (const auto& place : OpenFoamBackend::usualPlaces()) {
        const std::string preamble = preambleOf(place);
        if (!preamble.empty() && attempt(place, preamble, true)) {
            return install;
        }
    }
    std::string text = refused.empty() ? std::string("OpenFOAM was not found. ")
                                       : refused + " No other was found. ";
    text += "Looked in: ";
    for (std::size_t i = 0; i < looked.size(); ++i) {
        text += (i ? "; " : "") + looked[i];
    }
    text += "; and the usual places (" + OpenFoamBackend::downloadFolder()
        + ", /usr/lib/openfoam, /opt, ~/OpenFOAM). Set the preference Mod/Fluid/OpenFoamPath "
          "to its folder or its etc/bashrc.";
    install.searched = text;
    install.refused = refused;
    return install;
#endif
}

std::mutex installMutex;
bool installKnown = false;
OpenFoamInstall installFound;

// The last lines of a step's log that say what went wrong.
std::string tailOf(const fs::path& log)
{
    const std::string text = readFile(log);
    const auto fatal = text.find("FOAM FATAL");
    std::string tail = fatal != std::string::npos ? text.substr(fatal)
                                                  : text.substr(text.size() > 600
                                                                    ? text.size() - 600
                                                                    : 0);
    if (tail.size() > 900) {
        tail.resize(900);
    }
    return tail;
}

class OpenFoamSession: public Session
{
public:
    explicit OpenFoamSession(OpenFoamInstall install)
        : install(std::move(install))
    {}

    ~OpenFoamSession() override
    {
        if (!folder.empty() && !keep) {
            std::error_code ec;
            fs::remove_all(folder, ec);
        }
    }

    Status prepare(const Model& given, std::string& why) override
    {
        model = given;
        if (!install.found) {
            why = install.searched;
            return Status::NotFound;
        }
        OpenFoam::CaseOptions options;
        options.processes = processesFor(given);
        if (!OpenFoam::makeCase(model, options, made, why)) {
            return model.fluids.empty() && !steady() ? Status::Empty : Status::Unsupported;
        }
        keep = preferences()->GetBool("KeepCase", false);
        try {
            const fs::path base = fs::path(App::Application::getTempPath());
            for (int attempt = 0; attempt < 1000 && folder.empty(); ++attempt) {
                const fs::path candidate = base / ("FluidCase_" + std::to_string(QCoreApplication::applicationPid()) + "_"
                                                   + std::to_string(attempt));
                if (fs::create_directory(candidate)) {
                    folder = candidate;
                }
            }
            if (folder.empty()) {
                why = "no folder for the case could be made in " + base.string();
                return Status::Failed;
            }
            for (const auto& [name, text] : made.files) {
                const fs::path path = folder / name;
                fs::create_directories(path.parent_path());
                std::ofstream out(path, std::ios::binary);
                out << text;
                if (!out) {
                    why = "cannot write " + path.string();
                    return Status::Failed;
                }
            }
        }
        catch (const std::exception& e) {
            why = e.what();
            return Status::Failed;
        }
        return Status::Ok;
    }

    Status run(FrameSink& sink, std::string& why) override
    {
        // The frame at the start is the model's own: the volumes as given,
        // or the faces of the fluid's space with no pressure on them yet
        Frame first;
        first.time = 0.0;
        if (steady()) {
            first.points = model.domain.points;
            first.triangles = model.domain.triangles;
            first.values.assign(first.points.size(), 0.0);
            first.openings.resize(model.inlets.size() + model.outlets.size());
            liquidOf(model, liquid);
        }
        for (const auto& fluid : model.fluids) {
            const int offset = static_cast<int>(first.points.size());
            first.points.insert(first.points.end(),
                                fluid.surface.points.begin(),
                                fluid.surface.points.end());
            for (const auto& t : fluid.surface.triangles) {
                first.triangles.push_back({t[0] + offset, t[1] + offset, t[2] + offset});
            }
        }
        sink.frame(first);

        for (const auto& step : made.steps) {
            const std::string command = (install.preamble.empty() ? std::string()
                                                                   : install.preamble + "; ")
                + "cd " + quoted(folder.string()) + " && " + step.command + " > log." + step.name
                + " 2>&1";
            const int code = runShell(
                command,
                install.cleanEnvironment,
                [&] {
                    return sink.cancelled();
                },
                [&] {
                    if (step.solver) {
                        emit(sink, false);
                    }
                });
            if (code == -7) {
                return Status::Cancelled;
            }
            if (code != 0) {
                keep = true;
                why = step.name + " failed; the case is kept in " + folder.string() + ". "
                    + tailOf(folder / ("log." + step.name));
                return Status::Failed;
            }
            if (step.solver) {
                emit(sink, true);
            }
        }
        return Status::Ok;
    }

private:
    bool steady() const
    {
        return model.analysis == Analysis::Steady;
    }

    void emit(FrameSink& sink, bool finished)
    {
        if (steady()) {
            emitProbes(sink, finished);
            emitSkins(sink, finished);
        }
        else {
            emitSamples(sink, finished);
            emitFrames(sink, finished);
        }
    }

    // A steady flow's pressure monitors, a row a step: the solver's
    // pressure is over the density.
    void emitProbes(FrameSink& sink, bool finished)
    {
        if (model.monitors.empty()) {
            return;
        }
        const auto rows = OpenFoam::readTable(readFile(folder / "postProcessing/pressure/0/p"));
        const std::size_t ready = finished || rows.empty() ? rows.size() : rows.size() - 1;
        const double nan = std::numeric_limits<double>::quiet_NaN();
        for (; sampled < ready; ++sampled) {
            const auto& row = rows[sampled];
            std::vector<double> values;
            for (std::size_t m = 0; m < model.monitors.size(); ++m) {
                values.push_back(m + 1 < row.size() ? row[m + 1] * liquid.density : nan);
            }
            sink.sample(row.front(), values);
        }
    }

    // A steady flow's frames: the pressure on every patch as the solver
    // wrote it at a step, the patches joined, and what each opening's
    // tables have at that step. The latest is left while the solver runs.
    void emitSkins(FrameSink& sink, bool finished)
    {
        std::vector<std::pair<double, fs::path>> times;
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(folder / "postProcessing" / "skin", ec)) {
            try {
                std::size_t used = 0;
                const std::string name = entry.path().filename().string();
                const double time = std::stod(name, &used);
                if (used == name.size() && !emitted.contains(name)) {
                    times.emplace_back(time, entry.path());
                }
            }
            catch (const std::exception&) {
            }
        }
        std::ranges::sort(times);
        const std::size_t ready = finished ? times.size() : (times.empty() ? 0 : times.size() - 1);
        const std::size_t openings = model.inlets.size() + model.outlets.size();
        for (std::size_t i = 0; i < ready; ++i) {
            Frame frame;
            frame.time = times[i].first;
            bool whole = true;
            for (std::size_t k = 0; k <= openings && whole; ++k) {
                const std::string name = k == 0 ? "wall" : OpenFoam::openingName(model, k - 1);
                Frame patch;
                whole = OpenFoam::readFieldVtk(readFile(times[i].second / (name + ".vtk")),
                                               "p",
                                               patch);
                const int offset = static_cast<int>(frame.points.size());
                frame.points.insert(frame.points.end(), patch.points.begin(), patch.points.end());
                for (const auto& t : patch.triangles) {
                    frame.triangles.push_back({t[0] + offset, t[1] + offset, t[2] + offset});
                }
                for (double value : patch.values) {
                    frame.values.push_back(value * liquid.density);
                }
            }
            if (!whole) {
                if (!finished) {
                    return;  // not whole yet: the next look takes it
                }
                continue;
            }
            for (std::size_t k = 0; k < openings; ++k) {
                const std::string name = OpenFoam::openingName(model, k);
                OpeningFlow opening;
                const auto valueAt = [&](const char* what) {
                    const auto table = OpenFoam::readTable(readFile(
                        folder / "postProcessing" / (name + what) / "0" / "surfaceFieldValue.dat"));
                    const auto* row = OpenFoam::rowAt(table, frame.time, 0.5);
                    return row && row->size() > 1 ? (*row)[1]
                                                  : std::numeric_limits<double>::quiet_NaN();
                };
                opening.pressure = valueAt("P") * liquid.density;
                // the solver counts what leaves the mesh
                opening.flow = k < model.inlets.size() ? -valueAt("Q") : valueAt("Q");
                frame.openings.push_back(opening);
            }
            if (finished && i + 1 == ready) {
                takeInside(frame);
            }
            sink.frame(frame);
            emitted.insert(times[i].second.filename().string());
        }
    }

    // The fluid inside as the solver left it, for the run's last frame: the
    // function object writes it once, at the end, under the step it ended at.
    void takeInside(Frame& frame) const
    {
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(folder / "postProcessing" / "inside", ec)) {
            Frame inside;
            if (!OpenFoam::readInside(readFile(entry.path() / "cells_p_U.xy"), inside)) {
                continue;
            }
            frame.centres = std::move(inside.centres);
            frame.velocities = std::move(inside.velocities);
            frame.pressures = std::move(inside.pressures);
            for (double& pressure : frame.pressures) {
                pressure *= liquid.density;
            }
            return;
        }
    }

    // As many processes as the mesh is worth: a share of the cores, and no
    // fewer than some twenty thousand cells each.
    int processesFor(const Model& given) const
    {
        if (!install.parallel) {
            return 1;
        }
        long wanted = preferences()->GetInt("Processes", 0);
        if (wanted <= 0) {
            const long cores = std::max(1U, std::thread::hardware_concurrency() / 2);
            double cells = 1.0;
            for (std::size_t i = 0; i < 3; ++i) {
                cells *= std::max(
                    1.0,
                    (given.space.max[i] - given.space.min[i]) / given.resolution);
            }
            wanted = std::min({cores, 8L, static_cast<long>(cells / 20000.0)});
        }
        return static_cast<int>(std::max(wanted, 1L));
    }

    // Hands over the monitors' values the function objects have written
    // since the last look: a row of each table a step of the solver, the
    // last row left while it runs -- it may be half written.
    void emitSamples(FrameSink& sink, bool finished)
    {
        if (model.monitors.empty()) {
            return;
        }
        const auto pressures =
            OpenFoam::readTable(readFile(folder / "postProcessing/pressure/0/p"));
        const auto heights =
            OpenFoam::readTable(readFile(folder / "postProcessing/height/0/height.dat"));
        bool anyPressure = false;
        bool anyHeight = false;
        for (const auto& monitor : model.monitors) {
            (monitor.kind == MonitorKind::Pressure ? anyPressure : anyHeight) = true;
        }
        // both are written each step, so their rows go together
        std::size_t rows = anyPressure ? pressures.size() : heights.size();
        if (anyPressure && anyHeight) {
            rows = std::min(pressures.size(), heights.size());
        }
        if (!finished && rows > 0) {
            --rows;
        }
        const double nan = std::numeric_limits<double>::quiet_NaN();
        for (; sampled < rows; ++sampled) {
            const auto* pressure = anyPressure ? &pressures[sampled] : nullptr;
            const auto* height = anyHeight ? &heights[sampled] : nullptr;
            std::vector<double> values;
            std::size_t p = 0;
            std::size_t h = 0;
            for (const auto& monitor : model.monitors) {
                if (monitor.kind == MonitorKind::Pressure) {
                    // time, then a value a probe
                    const std::size_t column = 1 + p++;
                    values.push_back(column < pressure->size() ? (*pressure)[column] : nan);
                }
                else {
                    // time, then for each location its height above the
                    // boundary and above the location: the second
                    const std::size_t column = 2 + 2 * h++;
                    values.push_back(column < height->size() ? (*height)[column] : nan);
                }
            }
            sink.sample((pressure ? pressure : height)->front(), values);
        }
    }

    // Hands over the frames the solver has finished writing: every time's
    // surface but the latest while it runs -- that one may be half written
    // -- and all of them once it is through.
    void emitFrames(FrameSink& sink, bool finished)
    {
        std::vector<std::pair<double, fs::path>> times;
        std::error_code ec;
        for (const auto& entry :
             fs::directory_iterator(folder / "postProcessing" / "freeSurface", ec)) {
            try {
                std::size_t used = 0;
                const std::string name = entry.path().filename().string();
                const double time = std::stod(name, &used);
                if (used == name.size() && !emitted.contains(name)) {
                    times.emplace_back(time, entry.path());
                }
            }
            catch (const std::exception&) {
            }
        }
        std::ranges::sort(times);
        const std::size_t ready = finished ? times.size() : (times.empty() ? 0 : times.size() - 1);
        for (std::size_t i = 0; i < ready; ++i) {
            Frame frame;
            frame.time = times[i].first;
            if (!OpenFoam::readSurfaceVtk(readFile(times[i].second / "water.vtk"), frame)) {
                if (!finished) {
                    return;  // not whole yet: the next look takes it
                }
                frame.points.clear();
                frame.triangles.clear();
            }
            sink.frame(frame);
            emitted.insert(times[i].second.filename().string());
        }
    }

    OpenFoamInstall install;
    Model model;
    Material liquid;  // a steady flow's
    OpenFoam::Case made;
    fs::path folder;
    bool keep = false;
    std::set<std::string> emitted;
    std::size_t sampled = 0;
};
}  // namespace

Capabilities OpenFoamBackend::capabilities() const
{
    Capabilities can;
    can.results = ResultKind::Surface;
    can.freeSurface = true;
    can.steady = true;
    can.inProcess = false;
    return can;
}

Availability OpenFoamBackend::availability() const
{
    const OpenFoamInstall& found = install();
    Availability availability;
    availability.available = found.found;
    availability.reason = found.searched;
    availability.version = found.version;
    return availability;
}

std::unique_ptr<Session> OpenFoamBackend::createSession() const
{
    return std::make_unique<OpenFoamSession>(install());
}

std::string OpenFoamBackend::viscousFault(const std::string& preamble, bool cleanEnvironment)
{
    const auto said = [](double along, double across) {
        char text[320];
        std::snprintf(text,
                      sizeof(text),
                      "it computes viscous flow wrongly -- the same flow along a duct loses "
                      "%.1f%% more pressure laid along x than along y, as conda-forge's "
                      "build of v2412 does; a mended build is what Download OpenFOAM "
                      "installs",
                      100.0 * (along / across - 1.0));
        return std::string(text);
    };
    // what was tested before is not tested again
    const std::string tested = testedOf(preamble, cleanEnvironment);
    const fs::path kept = verdictsFile();
    if (!tested.empty()) {
        std::ifstream in(kept);
        for (std::string line; std::getline(in, line);) {
            const auto tab = line.find('\t');
            if (tab != std::string::npos && line.substr(0, tab) == tested) {
                double along = 0.0;
                double across = 0.0;
                std::istringstream values(line.substr(tab + 1));
                if (values >> along >> across) {
                    return OpenFoam::sameLoss(along, across) ? std::string() : said(along, across);
                }
            }
        }
    }
    fs::path base;
    try {
        const fs::path temp = fs::path(App::Application::getTempPath());
        for (int attempt = 0; attempt < 1000 && base.empty(); ++attempt) {
            const fs::path candidate = temp
                / ("FluidCheck_" + std::to_string(QCoreApplication::applicationPid()) + "_"
                   + std::to_string(attempt));
            if (fs::create_directory(candidate)) {
                base = candidate;
            }
        }
    }
    catch (const std::exception&) {
        return {};
    }
    if (base.empty()) {
        return {};
    }
    const double along = ductRun(preamble, cleanEnvironment, base, 0);
    const double across = ductRun(preamble, cleanEnvironment, base, 1);
    std::error_code ec;
    fs::remove_all(base, ec);
    if (!(along > 0.0) || !(across > 0.0)) {
        return {};  // not seen: not refused
    }
    if (!tested.empty()) {
        fs::create_directories(kept.parent_path(), ec);
        std::ofstream out(kept, std::ios::app);
        char values[80];
        std::snprintf(values, sizeof(values), "%.9g %.9g", along, across);
        out << tested << '\t' << values << '\n';
    }
    return OpenFoam::sameLoss(along, across) ? std::string() : said(along, across);
}

std::string OpenFoamBackend::faultAt(const std::string& place)
{
    const std::string preamble = preambleOf(place);
    return preamble.empty() ? std::string() : viscousFault(preamble, true);
}

const OpenFoamInstall& OpenFoamBackend::install()
{
    const std::lock_guard lock(installMutex);
    if (!installKnown) {
        installFound = search();
        installKnown = true;
    }
    return installFound;
}

void OpenFoamBackend::refresh()
{
    const std::lock_guard lock(installMutex);
    installKnown = false;
}

std::string OpenFoamBackend::downloadFolder()
{
    return (fs::path(App::Application::getUserAppDataDir()) / "Fluid" / "openfoam").string();
}

std::vector<std::string> OpenFoamBackend::usualPlaces()
{
    std::vector<std::string> places {downloadFolder()};
    for (const auto& found : globbed("/usr/lib/openfoam", "openfoam")) {
        places.push_back(found.string());
    }
    for (const auto& found : globbed("/opt", "openfoam")) {
        places.push_back(found.string());
    }
    for (const auto& found : globbed("/opt/OpenFOAM", "OpenFOAM-")) {
        places.push_back(found.string());
    }
    if (const char* home = std::getenv("HOME")) {
        for (const auto& found : globbed(fs::path(home) / "OpenFOAM", "OpenFOAM-")) {
            places.push_back(found.string());
        }
    }
    return places;
}
