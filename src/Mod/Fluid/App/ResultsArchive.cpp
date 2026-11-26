// SPDX-License-Identifier: LGPL-2.1-or-later

#include <algorithm>
#include <bit>
#include <cstdio>
#include <limits>
#include <regex>
#include <sstream>

#include <zipios++/zipios-config.h>
#include <zipios++/zipfile.h>
#include <zipios++/zipoutputstream.h>
#include <zipios++/meta-iostreams.h>

#include <Base/Exception.h>
#include <Base/FileInfo.h>
#include <Base/Stream.h>

#include "json.hpp"

#include "ResultsArchive.h"

using namespace Fluid;
using json = nlohmann::json;

namespace
{
constexpr int schema = 1;
constexpr const char* format = "ThunderealCAD fluid results";

static_assert(std::endian::native == std::endian::little,
              "the archive's blocks are written as the machine holds them, little endian");

void writeNpy(std::ostream& out,
              const char* descr,
              const std::vector<std::size_t>& shape,
              const void* data,
              std::size_t bytes)
{
    std::string dict = std::string("{'descr': '") + descr + "', 'fortran_order': False, 'shape': (";
    for (std::size_t i = 0; i < shape.size(); ++i) {
        dict += std::to_string(shape[i]);
        if (shape.size() == 1 || i + 1 < shape.size()) {
            dict += shape.size() == 1 ? "," : ", ";
        }
    }
    dict += "), }";
    const std::size_t total = 10 + dict.size() + 1;
    dict.append((64 - total % 64) % 64, ' ');
    dict += '\n';
    out.write("\x93NUMPY\x01\x00", 8);
    const auto length = static_cast<std::uint16_t>(dict.size());
    out.put(static_cast<char>(length & 0xff));
    out.put(static_cast<char>(length >> 8));
    out.write(dict.data(), static_cast<std::streamsize>(dict.size()));
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
}

// Reads a block of that element type: its shape, and its bytes into data.
template<typename T>
std::vector<T> readNpy(std::istream& in, const char* descr, std::vector<std::size_t>& shape)
{
    char magic[8];
    if (!in.read(magic, 8) || std::string(magic, 6) != "\x93NUMPY") {
        THROWM(Base::RuntimeError, "a block of the fluid results is not a .npy");
    }
    std::size_t length = 0;
    const int lengthBytes = magic[6] == 1 ? 2 : 4;
    for (int i = 0; i < lengthBytes; ++i) {
        length |= static_cast<std::size_t>(static_cast<unsigned char>(in.get())) << (8 * i);
    }
    std::string header(length, '\0');
    if (!in.read(header.data(), static_cast<std::streamsize>(length))) {
        THROWM(Base::RuntimeError, "a block of the fluid results is cut short");
    }
    std::smatch m;
    if (header.find(std::string("'") + descr + "'") == std::string::npos
        || header.find("'fortran_order': False") == std::string::npos
        || !std::regex_search(header, m, std::regex("'shape': \\(([0-9, ]*)\\)"))) {
        THROWM(Base::RuntimeError, "a block of the fluid results is not of its type in C order");
    }
    shape.clear();
    std::size_t count = 1;
    std::istringstream dims(m[1].str());
    for (std::string dim; std::getline(dims, dim, ',');) {
        if (dim.find_first_not_of(' ') == std::string::npos) {
            continue;
        }
        shape.push_back(std::stoull(dim));
        count *= shape.back();
    }
    std::vector<T> data(count);
    if (count > 0
        && !in.read(reinterpret_cast<char*>(data.data()),
                    static_cast<std::streamsize>(count * sizeof(T)))) {
        THROWM(Base::RuntimeError, "a block of the fluid results is cut short");
    }
    return data;
}

std::string frameName(std::size_t at, const char* what)
{
    char name[64];
    std::snprintf(name, sizeof(name), "frames/%06zu.%s.npy", at, what);
    return name;
}

const char* kindName(Solver::ResultKind kind)
{
    switch (kind) {
        case Solver::ResultKind::Surface:
            return "Surface";
        case Solver::ResultKind::Fields:
            return "Fields";
        default:
            return "Particles";
    }
}

const char* monitorName(Solver::MonitorKind kind)
{
    return kind == Solver::MonitorKind::Force      ? "Force"
           : kind == Solver::MonitorKind::Pressure ? "Pressure"
                                                   : "Height";
}
}  // namespace

const char* Fluid::unitOf(Solver::MonitorKind kind)
{
    return kind == Solver::MonitorKind::Force ? "N" : kind == Solver::MonitorKind::Pressure ? "Pa" : "mm";
}

EnergyVerdict Fluid::judge(const ResultsIndex& index)
{
    EnergyVerdict verdict;
    const auto& energy = index.energy;
    if (energy.size() < 4) {
        return verdict;
    }
    // past the start: the first record at or after a quarter of the time
    const double from = energy.front().time + 0.25 * (energy.back().time - energy.front().time);
    std::size_t first = 0;
    while (first + 1 < energy.size() && energy[first].time < from) {
        ++first;
    }
    if (first + 1 >= energy.size()) {
        return verdict;
    }
    verdict.kept = true;
    double fastest = 0.0;
    for (const auto& at : energy) {
        fastest = std::max(fastest, at.kinetic);
    }
    verdict.budget = !index.fed;
    if (verdict.budget && fastest > 0.0) {
        double lowest = std::numeric_limits<double>::max();
        double rise = 0.0;
        for (std::size_t k = first; k < energy.size(); ++k) {
            const double all = energy[k].kinetic + energy[k].potential + energy[k].work;
            lowest = std::min(lowest, all);
            rise = std::max(rise, all - lowest);
        }
        verdict.rise = rise / fastest;
        verdict.rose = verdict.rise > EnergyVerdict::riseLimit;
    }
    const double over = energy.back().exchanged - energy[first].exchanged;
    const double created = energy.back().created - energy[first].created;
    // against the fluid's own motion: a mechanism that stands passes
    // nothing worth a share
    if (over > 0.01 * fastest) {
        verdict.handOver = true;
        verdict.made = created / over;
        verdict.lagged = verdict.made > EnergyVerdict::madeLimit;
    }
    return verdict;
}

struct ArchiveWriter::Private
{
    explicit Private(const std::string& path)
        : file(path)
        , out(file, std::ios::out | std::ios::binary | std::ios::trunc)
    {}

    Base::FileInfo file;
    Base::ofstream out;
    std::unique_ptr<zipios::ZipOutputStream> zip;
    std::vector<double> times;
    std::vector<double> samples;
    std::vector<double> values;
    std::vector<double> motions;
    std::vector<double> energy;
    std::size_t width = 0;
    std::size_t bodies = 0;  // a frame, as the first frame has them
    bool sampled = false;
};

ArchiveWriter::ArchiveWriter(const std::string& path)
    : d(std::make_unique<Private>(path))
{
    if (!d->out) {
        THROWM(Base::FileException, "cannot write the fluid results", d->file);
    }
    d->zip = std::make_unique<zipios::ZipOutputStream>(d->out);
    d->zip->setComment(format);
    // a frame's coordinates barely deflate, and a run has thousands
    d->zip->setLevel(1);
}

ArchiveWriter::~ArchiveWriter() = default;

void ArchiveWriter::frame(const Solver::Frame& frame)
{
    const std::size_t at = d->times.size();
    std::vector<float> points;
    points.reserve(frame.points.size() * 3);
    for (const auto& p : frame.points) {
        for (double v : p) {
            points.push_back(static_cast<float>(v * 1000.0));
        }
    }
    d->zip->putNextEntry(frameName(at, "points"));
    writeNpy(*d->zip, "<f4", {frame.points.size(), 3}, points.data(), points.size() * sizeof(float));
    if (!frame.triangles.empty()) {
        std::vector<std::int32_t> triangles;
        triangles.reserve(frame.triangles.size() * 3);
        for (const auto& t : frame.triangles) {
            triangles.insert(triangles.end(), t.begin(), t.end());
        }
        d->zip->putNextEntry(frameName(at, "triangles"));
        writeNpy(*d->zip,
                 "<i4",
                 {frame.triangles.size(), 3},
                 triangles.data(),
                 triangles.size() * sizeof(std::int32_t));
    }
    if (at == 0) {
        d->bodies = frame.bodies.size();
    }
    for (std::size_t b = 0; b < d->bodies; ++b) {
        const Solver::Motion motion = b < frame.bodies.size() ? frame.bodies[b] : Solver::Motion();
        for (double v : motion.translation) {
            d->motions.push_back(v * 1000.0);
        }
        d->motions.insert(d->motions.end(), motion.rotation.begin(), motion.rotation.end());
    }
    d->times.push_back(frame.time);
}

void ArchiveWriter::sample(double time, const std::vector<double>& values)
{
    if (!d->sampled) {
        d->width = values.size();
        d->sampled = true;
    }
    d->samples.push_back(time);
    for (std::size_t i = 0; i < d->width; ++i) {
        d->values.push_back(i < values.size() ? values[i]
                                              : std::numeric_limits<double>::quiet_NaN());
    }
}

void ArchiveWriter::energy(const Solver::Energy& energy)
{
    d->energy.insert(d->energy.end(),
                     {energy.time,
                      energy.kinetic,
                      energy.potential,
                      energy.work,
                      energy.created,
                      energy.exchanged});
}

std::size_t ArchiveWriter::frames() const
{
    return d->times.size();
}

double ArchiveWriter::lastTime() const
{
    return d->times.empty() ? 0.0 : d->times.back();
}

void ArchiveWriter::close(ResultsIndex index)
{
    const std::size_t frames = d->times.size();
    const std::size_t samples = d->samples.size();
    // Height is kept in mm, as the points are
    for (std::size_t m = 0; m < d->width && m < index.monitors.size(); ++m) {
        if (index.monitors[m].kind == Solver::MonitorKind::Height) {
            for (std::size_t f = 0; f < samples; ++f) {
                d->values[f * d->width + m] *= 1000.0;
            }
        }
    }
    json out;
    out["format"] = format;
    out["schema"] = schema;
    out["units"] = {{"time", "s"}, {"length", "mm"}, {"pressure", "Pa"}, {"force", "N"}};
    out["backend"] = index.backend;
    out["version"] = index.version;
    out["kind"] = kindName(index.kind);
    out["signature"] = std::to_string(index.signature);
    out["spacing"] = index.spacing;
    out["frames"] = frames;
    out["samples"] = samples;
    json monitors = json::array();
    for (const auto& monitor : index.monitors) {
        monitors.push_back({{"source", monitor.source},
                            {"kind", monitorName(monitor.kind)},
                            {"unit", unitOf(monitor.kind)}});
    }
    out["monitors"] = monitors;
    // as many as the frames have: a run that failed before it named them
    // keeps none
    index.bodies.resize(d->bodies);
    out["bodies"] = index.bodies;
    out["fed"] = index.fed;

    d->zip->setLevel(6);
    d->zip->putNextEntry("times.npy");
    writeNpy(*d->zip, "<f8", {frames}, d->times.data(), frames * sizeof(double));
    d->zip->putNextEntry("samples.npy");
    writeNpy(*d->zip, "<f8", {samples}, d->samples.data(), samples * sizeof(double));
    d->zip->putNextEntry("monitors.npy");
    writeNpy(*d->zip,
             "<f8",
             {samples, d->width},
             d->values.data(),
             d->values.size() * sizeof(double));
    if (d->bodies > 0) {
        d->zip->putNextEntry("bodies.npy");
        writeNpy(*d->zip,
                 "<f8",
                 {frames, d->bodies, 7},
                 d->motions.data(),
                 d->motions.size() * sizeof(double));
    }
    if (!d->energy.empty()) {
        d->zip->putNextEntry("energy.npy");
        writeNpy(*d->zip,
                 "<f8",
                 {d->energy.size() / 6, 6},
                 d->energy.data(),
                 d->energy.size() * sizeof(double));
    }
    d->zip->putNextEntry("index.json");
    *d->zip << out.dump(1);
    d->zip->close();
    d->zip.reset();
    d->out.close();
    if (!d->out) {
        THROWM(Base::FileException, "cannot write the fluid results", d->file);
    }
}

struct ArchiveReader::Private
{
    std::unique_ptr<zipios::ZipFile> zip;
    ResultsIndex index;
};

ArchiveReader::ArchiveReader(const std::string& path)
    : d(std::make_unique<Private>())
{
    try {
        d->zip = std::make_unique<zipios::ZipFile>(path);
        auto entry = [&](const char* name) {
            return std::unique_ptr<std::istream>(d->zip->getInputStream(name));
        };
        auto indexIn = entry("index.json");
        if (!indexIn) {
            THROWM(Base::RuntimeError, "the fluid results have no index");
        }
        const json index = json::parse(*indexIn);
        if (index.value("format", std::string()) != format) {
            THROWM(Base::RuntimeError, "not fluid results");
        }
        if (index.value("schema", 0) > schema) {
            FC_THROWM(Base::RuntimeError,
                      "the fluid results are of a later version, schema "
                          << index.value("schema", 0));
        }
        d->index.backend = index.value("backend", std::string());
        d->index.version = index.value("version", std::string());
        const auto kind = index.value("kind", std::string("Particles"));
        d->index.kind = kind == "Surface"  ? Solver::ResultKind::Surface
                        : kind == "Fields" ? Solver::ResultKind::Fields
                                           : Solver::ResultKind::Particles;
        d->index.signature = std::stoull(index.value("signature", std::string("0")));
        d->index.spacing = index.value("spacing", 0.0);
        for (const auto& monitor : index.at("monitors")) {
            d->index.monitors.push_back(
                {monitor.value("source", std::string()),
                 monitor.value("kind", std::string()) == "Force"
                     ? Solver::MonitorKind::Force
                     : monitor.value("kind", std::string()) == "Pressure"
                           ? Solver::MonitorKind::Pressure
                           : Solver::MonitorKind::Height});
        }
        std::vector<std::size_t> shape;
        auto timesIn = entry("times.npy");
        auto samplesIn = entry("samples.npy");
        auto valuesIn = entry("monitors.npy");
        if (!timesIn || !samplesIn || !valuesIn) {
            THROWM(Base::RuntimeError, "the fluid results have no times");
        }
        d->index.times = readNpy<double>(*timesIn, "<f8", shape);
        d->index.samples = readNpy<double>(*samplesIn, "<f8", shape);
        d->index.values = readNpy<double>(*valuesIn, "<f8", shape);
        if (d->index.values.size() != d->index.samples.size() * d->index.monitors.size()) {
            THROWM(Base::RuntimeError, "the fluid results' monitors do not match its samples");
        }
        if (index.contains("bodies")) {
            d->index.bodies = index.at("bodies").get<std::vector<std::string>>();
        }
        if (!d->index.bodies.empty()) {
            auto motionsIn = entry("bodies.npy");
            if (!motionsIn) {
                THROWM(Base::RuntimeError, "the fluid results have no place for the walls moved");
            }
            d->index.motions = readNpy<double>(*motionsIn, "<f8", shape);
            if (d->index.motions.size() != d->index.times.size() * d->index.bodies.size() * 7) {
                THROWM(Base::RuntimeError,
                       "the fluid results' moved walls do not match its frames");
            }
        }
        d->index.fed = index.value("fed", false);
        if (auto energyIn = entry("energy.npy")) {
            const auto kept = readNpy<double>(*energyIn, "<f8", shape);
            if (shape.size() != 2 || shape[1] != 6) {
                THROWM(Base::RuntimeError, "the fluid results' energy is not six to a time");
            }
            for (std::size_t k = 0; k + 5 < kept.size(); k += 6) {
                d->index.energy.push_back(
                    {kept[k], kept[k + 1], kept[k + 2], kept[k + 3], kept[k + 4], kept[k + 5]});
            }
        }
    }
    catch (const Base::Exception&) {
        throw;
    }
    catch (const std::exception& e) {
        FC_THROWM(Base::FileException, "cannot read the fluid results: " << e.what());
    }
}

ArchiveReader::~ArchiveReader() = default;

const ResultsIndex& ArchiveReader::index() const
{
    return d->index;
}

std::vector<Solver::Motion> ArchiveReader::bodies(std::size_t at) const
{
    if (at >= d->index.times.size()) {
        FC_THROWM(Base::IndexError,
                  "frame " << at << " of fluid results with " << d->index.times.size());
    }
    std::vector<Solver::Motion> found;
    const std::size_t count = d->index.bodies.size();
    for (std::size_t b = 0; b < count; ++b) {
        const double* kept = d->index.motions.data() + (at * count + b) * 7;
        found.push_back({{kept[0], kept[1], kept[2]}, {kept[3], kept[4], kept[5], kept[6]}});
    }
    return found;
}

ArchivedFrame ArchiveReader::frame(std::size_t at) const
{
    if (at >= d->index.times.size()) {
        FC_THROWM(Base::IndexError,
                  "frame " << at << " of fluid results with " << d->index.times.size());
    }
    ArchivedFrame frame;
    frame.time = d->index.times[at];
    frame.bodies = bodies(at);
    try {
        std::vector<std::size_t> shape;
        std::unique_ptr<std::istream> in(d->zip->getInputStream(frameName(at, "points")));
        if (!in) {
            THROWM(Base::RuntimeError, "a frame of the fluid results is missing");
        }
        frame.points = readNpy<float>(*in, "<f4", shape);
        if (d->index.kind == Solver::ResultKind::Surface) {
            std::unique_ptr<std::istream> tin(d->zip->getInputStream(frameName(at, "triangles")));
            if (tin) {
                frame.triangles = readNpy<std::int32_t>(*tin, "<i4", shape);
            }
        }
    }
    catch (const Base::Exception&) {
        throw;
    }
    catch (const std::exception& e) {
        FC_THROWM(Base::FileException, "cannot read the fluid results: " << e.what());
    }
    return frame;
}
