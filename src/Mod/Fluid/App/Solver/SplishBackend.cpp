// SPDX-License-Identifier: LGPL-2.1-or-later

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

#include <QCoreApplication>
#include <QProcess>
#include <QString>
#include <QStringList>

#include <App/Application.h>
#include <Base/Parameter.h>

#include "SplishBackend.h"
#include "SplishCase.h"

using namespace Fluid::Solver;
namespace fs = std::filesystem;

namespace
{
ParameterGrp::handle preferences()
{
    return App::GetApplication().GetParameterGroupByPath(
        "User parameter:BaseApp/Preferences/Mod/Fluid");
}

#ifdef _WIN32
constexpr const char* ProgramName = "FluidSplish.exe";
#else
constexpr const char* ProgramName = "FluidSplish";
#endif

std::string readFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// What the program at a path says its solver's version is; empty when it
// is not there or does not answer as the runner does.
std::string versionOf(const std::string& program)
{
    std::error_code ec;
    if (!fs::is_regular_file(program, ec)) {
        return {};
    }
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(QString::fromStdString(program), QStringList() << QStringLiteral("--version"));
    if (!process.waitForStarted(10000) || !process.waitForFinished(20000)) {
        process.kill();
        process.waitForFinished(2000);
        return {};
    }
    std::istringstream lines(process.readAll().toStdString());
    std::string line;
    while (std::getline(lines, line)) {
        const Splish::Event event = Splish::parseLine(line);
        if (event.kind == Splish::Event::Version) {
            return event.text;
        }
    }
    return {};
}

SplishRunner search()
{
    SplishRunner runner;
    std::string looked;
    for (const std::string& place : SplishBackend::places()) {
        const std::string version = versionOf(place);
        if (!version.empty()) {
            runner.found = true;
            runner.program = place;
            runner.version = version;
            return runner;
        }
        looked += (looked.empty() ? "" : ", ") + place;
    }
    runner.searched = std::string(ProgramName)
        + ", the program that runs SPlisHSPlasH, was not found. It is built with FreeCAD when "
          "BUILD_FLUID_SPLISHSPLASH is on. Looked for "
        + looked + ".";
    return runner;
}

std::mutex runnerMutex;
bool runnerKnown = false;
SplishRunner runnerFound;

class SplishSession: public Session
{
public:
    explicit SplishSession(SplishRunner runner)
        : runner(std::move(runner))
    {}

    ~SplishSession() override
    {
        if (!folder.empty() && !keep) {
            std::error_code ec;
            fs::remove_all(folder, ec);
        }
    }

    Status prepare(const Model& given, std::string& why) override
    {
        if (!runner.found) {
            why = runner.searched;
            return Status::NotFound;
        }
        Splish::CaseOptions options;
        options.threads = threads();
        // 0 unless asked for: it moves what the monitors read
        options.cohesion = std::max(preferences()->GetFloat("Cohesion", 0.0), 0.0);
        Splish::Case made;
        if (!Splish::makeCase(given, options, made, why)) {
            return given.fluids.empty() ? Status::Empty : Status::Unsupported;
        }
        monitors = given.monitors.size();
        driven = anyDriven(given);
        centres.clear();
        if (driven) {
            for (std::size_t wall : freeWalls(given)) {
                centres.push_back(given.walls[wall].mass.centre);
            }
        }
        bodies = made.bodies.size();
        motions.assign(bodies, Motion());
        keep = preferences()->GetBool("KeepCase", false);
        try {
            const fs::path base = fs::path(App::Application::getTempPath());
            for (int attempt = 0; attempt < 1000 && folder.empty(); ++attempt) {
                const fs::path candidate = base
                    / ("FluidSplish_" + std::to_string(QCoreApplication::applicationPid()) + "_"
                       + std::to_string(attempt));
                if (fs::create_directory(candidate)) {
                    folder = candidate;
                }
            }
            if (folder.empty()) {
                why = "no folder for the case could be made in " + base.string();
                return Status::Failed;
            }
            for (const auto& [name, bytes] : made.files) {
                std::ofstream out(folder / name, std::ios::binary);
                out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                if (!out) {
                    why = "cannot write " + (folder / name).string();
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

    void drive(WallDriver* given) override
    {
        driver = given;
    }

    Status run(FrameSink& sink, std::string& why) override
    {
        // Whatever way it ends, the walls' mover is told: it may be waiting
        struct Told
        {
            WallDriver* driver;
            std::string* why;
            ~Told()
            {
                if (driver) {
                    driver->ended(*why);
                }
            }
        } told {driven ? driver : nullptr, &why};
        if (driven && !driver) {
            why = "the study's parts are a mechanism's, and nothing moves them";
            return Status::Unsupported;
        }
        QProcess process;
        process.setProcessChannelMode(QProcess::MergedChannels);
        process.setWorkingDirectory(QString::fromStdString(folder.string()));
        process.start(QString::fromStdString(runner.program),
                      QStringList() << QString::fromStdString(folder.string()));
        if (!process.waitForStarted(10000)) {
            why = runner.program + " could not be started";
            return Status::Failed;
        }
        std::string pending;
        std::string said;  // the solver's last talk, for when it fails
        std::string error;
        const auto take = [&](bool all) {
            pending += process.readAll().toStdString();
            std::size_t from = 0;
            for (;;) {
                const std::size_t end = pending.find('\n', from);
                if (end == std::string::npos) {
                    break;
                }
                handle(pending.substr(from, end - from), sink, said, error);
                from = end + 1;
            }
            pending.erase(0, from);
            if (all && !pending.empty()) {
                handle(pending, sink, said, error);
                pending.clear();
            }
        };
        while (!process.waitForFinished(driven ? 2 : 100)) {
            take(false);
            // The runner waits for its driven walls: the fluid's answer to
            // their mover, and the mover's next stretch to the runner
            while (asked && error.empty()) {
                asked = false;
                if (!loads.empty()) {
                    driver->pushed(loads);
                    loads.clear();
                }
                double time = 0.0;
                double length = 0.0;
                std::vector<WallState> states;
                std::string line = "end\n";
                if (!sink.cancelled() && driver->next(time, length, states)) {
                    if (states.size() != centres.size()) {
                        error = "the mechanism gave the states of another count of parts";
                        break;
                    }
                    line = Splish::moveLine(time + length, centres, states);
                }
                process.write(line.data(), static_cast<qint64>(line.size()));
                process.waitForBytesWritten(10000);
                if (process.waitForReadyRead(driven ? 2 : 100)) {
                    take(false);
                }
            }
            if (sink.cancelled()) {
                process.kill();
                process.waitForFinished(5000);
                return Status::Cancelled;
            }
            if (process.state() == QProcess::NotRunning) {
                break;
            }
        }
        take(true);
        if (!error.empty()) {
            why = error;
            return Status::Failed;
        }
        if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
            keep = true;
            why = "the solver stopped (code " + std::to_string(process.exitCode())
                + "); the case is kept in " + folder.string() + ". " + said;
            return Status::Failed;
        }
        return Status::Ok;
    }

private:
    // The solver on every thread is far slower than on a few (sec 12.1):
    // the count is always said.
    static int threads()
    {
        // FLUID_SPLISH_THREADS before the preference: a machine's own
        // limit, for every run started there -- the tests' too, whose
        // preferences are their own
        if (const char* given = std::getenv("FLUID_SPLISH_THREADS")) {
            const long asked = std::strtol(given, nullptr, 10);
            if (asked > 0) {
                return static_cast<int>(asked);
            }
        }
        long wanted = preferences()->GetInt("Threads", 0);
        if (wanted <= 0) {
            wanted = std::min(8L, static_cast<long>(std::thread::hardware_concurrency() / 2));
        }
        return static_cast<int>(std::max(wanted, 1L));
    }

    void handle(const std::string& line, FrameSink& sink, std::string& said, std::string& error)
    {
        const Splish::Event event = Splish::parseLine(line);
        switch (event.kind) {
            case Splish::Event::Frame: {
                const fs::path file = folder / ("frame." + std::to_string(event.index));
                Frame frame;
                frame.time = event.time;
                frame.bodies = motions;
                if (!Splish::pointsOf(readFile(file), event.count, frame.points)) {
                    error = "the frame at " + std::to_string(event.time) + " s could not be read";
                    return;
                }
                sink.frame(frame);
                std::error_code ec;
                fs::remove(file, ec);
                break;
            }
            case Splish::Event::Sample: {
                std::vector<double> values = event.values;
                values.resize(monitors, std::numeric_limits<double>::quiet_NaN());
                sink.sample(event.time, values);
                break;
            }
            case Splish::Event::Body:
                // of the frame that follows
                if (static_cast<std::size_t>(event.index) < motions.size()) {
                    Motion& motion = motions[static_cast<std::size_t>(event.index)];
                    motion.translation = {event.values[0], event.values[1], event.values[2]};
                    motion.rotation = {event.values[3],
                                       event.values[4],
                                       event.values[5],
                                       event.values[6]};
                }
                break;
            case Splish::Event::Ask:
                asked = true;
                if (driver) {
                    driver->ready(event.values[0]);
                }
                break;
            case Splish::Event::Pushed:
                asked = true;
                loads.clear();
                for (std::size_t k = 0; k + 5 < event.values.size(); k += 6) {
                    WallLoad load;
                    load.force = {event.values[k], event.values[k + 1], event.values[k + 2]};
                    load.torque = {event.values[k + 3], event.values[k + 4], event.values[k + 5]};
                    loads.push_back(load);
                }
                break;
            case Splish::Event::Error:
                error = event.text;
                break;
            case Splish::Event::Version:
                break;
            case Splish::Event::Other:
                if (!line.empty()) {
                    said = line.size() > 600 ? line.substr(0, 600) : line;
                }
                break;
        }
    }

    SplishRunner runner;
    fs::path folder;
    std::size_t monitors = 0;
    std::size_t bodies = 0;
    std::vector<Motion> motions;  // where the moved walls are, as last said
    // Walls moved from outside: who moves them, where each one's centre
    // was at the start, and what the runner last said of them
    WallDriver* driver = nullptr;
    bool driven = false;
    std::vector<Vec3> centres;
    bool asked = false;
    std::vector<WallLoad> loads;
    bool keep = false;
};
}  // namespace

Capabilities SplishBackend::capabilities() const
{
    Capabilities can;
    can.results = ResultKind::Particles;
    can.freeSurface = true;
    can.freeBodies = true;
    can.drivenWalls = true;
    can.inlets = true;
    can.forces = true;
    can.inProcess = false;
    return can;
}

Availability SplishBackend::availability() const
{
    const SplishRunner& found = runner();
    Availability availability;
    availability.available = found.found;
    availability.reason = found.searched;
    availability.version = found.version;
    return availability;
}

std::unique_ptr<Session> SplishBackend::createSession() const
{
    return std::make_unique<SplishSession>(runner());
}

const SplishRunner& SplishBackend::runner()
{
    const std::lock_guard lock(runnerMutex);
    if (!runnerKnown) {
        runnerFound = search();
        runnerKnown = true;
    }
    return runnerFound;
}

void SplishBackend::refresh()
{
    const std::lock_guard lock(runnerMutex);
    runnerKnown = false;
}

std::vector<std::string> SplishBackend::places()
{
    std::vector<std::string> places;
    const std::string preferred = preferences()->GetASCII("SplishPath", "");
    if (!preferred.empty()) {
        places.push_back(preferred);
    }
    if (const char* given = std::getenv("FLUID_SPLISH_PATH")) {
        if (*given) {
            places.emplace_back(given);
        }
    }
    places.push_back((fs::path(App::Application::getHomePath()) / "bin" / ProgramName).string());
    return places;
}
