// SPDX-License-Identifier: LGPL-2.1-or-later
/****************************************************************************
 *                                                                          *
 *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
 *                                                                          *
 *   This file is part of FreeCAD.                                          *
 *                                                                          *
 *   FreeCAD is free software: you can redistribute it and/or modify it     *
 *   under the terms of the GNU Lesser General Public License as            *
 *   published by the Free Software Foundation, either version 2.1 of the   *
 *   License, or (at your option) any later version.                        *
 *                                                                          *
 *   FreeCAD is distributed in the hope that it will be useful, but         *
 *   WITHOUT ANY WARRANTY; without even the implied warranty of             *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU       *
 *   Lesser General Public License for more details.                        *
 *                                                                          *
 *   You should have received a copy of the GNU Lesser General Public       *
 *   License along with FreeCAD. If not, see                                *
 *   <https://www.gnu.org/licenses/>.                                       *
 *                                                                          *
 ***************************************************************************/

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
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

#include "SimulationArchive.h"

using namespace Assembly;
using json = nlohmann::json;

namespace
{
// The archive's schema: a reader refuses a later one
constexpr int schema = 1;
constexpr const char* format = "ThunderealCAD simulation results";

static_assert(std::endian::native == std::endian::little,
              "the archive's blocks are written as the machine holds them, little endian");

// A float64 block as numpy's .npy, version 1.0: the magic, the header's
// length, a dict of the type, the order and the shape, padded to 64 bytes
void writeNpy(std::ostream& out,
              const std::vector<std::size_t>& shape,
              const std::vector<double>& data)
{
    std::string dict = "{'descr': '<f8', 'fortran_order': False, 'shape': (";
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
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size() * sizeof(double)));
}

// Reads a block writeNpy() wrote, any version numpy writes; its shape
std::vector<double> readNpy(std::istream& in, std::vector<std::size_t>& shape)
{
    char magic[8];
    if (!in.read(magic, 8) || std::string(magic, 6) != "\x93NUMPY") {
        THROWM(Base::RuntimeError, "a block of the simulation results is not a .npy");
    }
    std::size_t length = 0;
    const int lengthBytes = magic[6] == 1 ? 2 : 4;
    for (int i = 0; i < lengthBytes; ++i) {
        length |= static_cast<std::size_t>(static_cast<unsigned char>(in.get())) << (8 * i);
    }
    std::string header(length, '\0');
    if (!in.read(header.data(), static_cast<std::streamsize>(length))) {
        THROWM(Base::RuntimeError, "a block of the simulation results is cut short");
    }
    std::smatch m;
    if (header.find("'<f8'") == std::string::npos
        || header.find("'fortran_order': False") == std::string::npos
        || !std::regex_search(header, m, std::regex("'shape': \\(([0-9, ]*)\\)"))) {
        THROWM(Base::RuntimeError, "a block of the simulation results is not float64 in C order");
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
    std::vector<double> data(count);
    if (!in.read(reinterpret_cast<char*>(data.data()),
                 static_cast<std::streamsize>(count * sizeof(double)))) {
        THROWM(Base::RuntimeError, "a block of the simulation results is cut short");
    }
    return data;
}

json placementJson(const Base::Placement& p)
{
    double q[4];
    p.getRotation().getValue(q[0], q[1], q[2], q[3]);
    const Base::Vector3d& v = p.getPosition();
    return json::array({v.x, v.y, v.z, q[0], q[1], q[2], q[3]});
}

Base::Placement placementOf(const double* v)
{
    return {Base::Vector3d(v[0], v[1], v[2]), Base::Rotation(v[3], v[4], v[5], v[6])};
}

// A number, or NaN for what JSON could not hold (null)
double number(const json& value)
{
    return value.is_number() ? value.get<double>() : std::numeric_limits<double>::quiet_NaN();
}

const char* kindName(Solver::Event::Kind kind)
{
    switch (kind) {
        case Solver::Event::Kind::ContactBegin:
            return "ContactBegin";
        case Solver::Event::Kind::ContactEnd:
            return "ContactEnd";
        default:
            return "Sensor";
    }
}
}  // namespace

void Assembly::writeSimulationArchive(const std::string& path,
                                      const Solver::Trajectory& frames,
                                      const std::vector<ArchivedPart>& archivedParts,
                                      bool values)
{
    const std::size_t nFrames = frames.times.size();
    const std::size_t nBodies = frames.bodies.size();

    json index;
    index["format"] = format;
    index["schema"] = schema;
    index["units"] = {{"time", "s"},
                      {"length", "mm"},
                      {"angle", "rad"},
                      {"force", "mN"},
                      {"torque", "mN mm"},
                      {"velocity", "mm/s"}};
    index["kept"] = values ? "All" : "Motion";
    index["frames"] = nFrames;
    index["bodies"] = frames.bodies;
    json parts = json::array();
    for (const auto& part : archivedParts) {
        parts.push_back(
            {{"name", part.name}, {"body", part.body}, {"offset", placementJson(part.offset)}});
    }
    index["parts"] = parts;
    std::size_t width = 0;
    json channels = json::array();
    if (values) {
        for (const auto& channel : frames.channels) {
            json entry = {{"source", channel.source},
                          {"quantity", channel.quantity},
                          {"width", channel.width},
                          {"unique", channel.unique}};
            // The part a value is on, a contact set's: a reader before it
            // has none of those quantities, and reads past it
            if (!channel.on.empty()) {
                entry["on"] = channel.on;
            }
            channels.push_back(entry);
            width += static_cast<std::size_t>(channel.width);
        }
    }
    index["channels"] = channels;
    json events = json::array();
    for (const auto& event : frames.events) {
        json entry = {{"time", event.time},
                      {"kind", kindName(event.kind)},
                      {"source", event.source},
                      {"value", event.value},
                      {"stopped", event.stopped}};
        if (event.kind != Solver::Event::Kind::Sensor) {
            entry["bodies"] = json::array({event.bodyA, event.bodyB});
        }
        events.push_back(entry);
    }
    index["events"] = events;

    std::vector<double> poses;
    poses.reserve(nFrames * nBodies * 7);
    for (const auto& pose : frames.poses) {
        double q[4];
        pose.getRotation().getValue(q[0], q[1], q[2], q[3]);
        const Base::Vector3d& v = pose.getPosition();
        poses.insert(poses.end(), {v.x, v.y, v.z, q[0], q[1], q[2], q[3]});
    }

    Base::FileInfo file(path);
    Base::ofstream out(file, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!out) {
        THROWM(Base::FileException, "cannot write the simulation results", file);
    }
    if (frames.poses.size() != nFrames * nBodies) {
        THROWM(Base::RuntimeError, "the simulation's poses do not match its frames");
    }
    {
        zipios::ZipOutputStream zip(out);
        zip.setComment(format);
        zip.setLevel(6);
        zip.putNextEntry("index.json");
        zip << index.dump(1);
        zip.putNextEntry("times.npy");
        writeNpy(zip, {nFrames}, frames.times);
        zip.putNextEntry("poses.npy");
        writeNpy(zip, {nFrames, nBodies, 7}, poses);
        if (values && width > 0) {
            zip.putNextEntry("values.npy");
            writeNpy(zip, {nFrames, width}, frames.values);
        }
        zip.close();
    }
    out.close();
    if (!out) {
        THROWM(Base::FileException, "cannot write the simulation results", file);
    }
}

SimulationArchive Assembly::readSimulationArchive(const std::string& path)
{
    SimulationArchive archive;
    Solver::Trajectory& frames = archive.frames;
    try {
        zipios::ZipFile zip(path);
        auto entry = [&](const char* name) {
            std::unique_ptr<std::istream> in(zip.getInputStream(name));
            return in;
        };
        auto indexIn = entry("index.json");
        if (!indexIn) {
            THROWM(Base::RuntimeError, "the simulation results have no index");
        }
        const json index = json::parse(*indexIn);
        if (index.value("format", std::string()) != format) {
            THROWM(Base::RuntimeError, "not simulation results");
        }
        if (index.value("schema", 0) > schema) {
            FC_THROWM(Base::RuntimeError,
                      "the simulation results are of a later version, schema "
                          << index.value("schema", 0));
        }
        archive.values = index.value("kept", std::string("All")) == "All";
        frames.bodies = index.at("bodies").get<std::vector<Solver::Id>>();
        for (const auto& part : index.at("parts")) {
            const auto offset = part.at("offset").get<std::vector<double>>();
            if (offset.size() != 7) {
                THROWM(Base::RuntimeError, "a part of the simulation results has no offset");
            }
            archive.parts.push_back({part.at("name").get<std::string>(),
                                     part.at("body").get<Solver::Id>(),
                                     placementOf(offset.data())});
        }
        std::size_t width = 0;
        for (const auto& channel : index.at("channels")) {
            frames.channels.push_back({channel.at("source").get<std::string>(),
                                       channel.at("quantity").get<std::string>(),
                                       channel.at("width").get<int>(),
                                       channel.value("unique", true),
                                       channel.value("on", std::string())});
            width += static_cast<std::size_t>(frames.channels.back().width);
        }
        for (const auto& entry : index.at("events")) {
            Solver::Event event;
            event.time = number(entry.at("time"));
            event.source = entry.at("source").get<std::string>();
            event.value = number(entry.at("value"));
            event.stopped = entry.value("stopped", false);
            const std::string kind = entry.value("kind", std::string("Sensor"));
            if (kind == "ContactBegin" || kind == "ContactEnd") {
                event.kind = kind == "ContactBegin" ? Solver::Event::Kind::ContactBegin
                                                    : Solver::Event::Kind::ContactEnd;
                const auto bodies = entry.at("bodies").get<std::vector<std::string>>();
                event.bodyA = bodies.size() > 0 ? bodies[0] : std::string();
                event.bodyB = bodies.size() > 1 ? bodies[1] : std::string();
            }
            frames.events.push_back(std::move(event));
        }

        std::vector<std::size_t> shape;
        auto block = [&](const char* name) {
            auto in = entry(name);
            if (!in) {
                FC_THROWM(Base::RuntimeError, "the simulation results have no " << name);
            }
            return readNpy(*in, shape);
        };
        frames.times = block("times.npy");
        const std::size_t nFrames = frames.times.size();
        const std::size_t nBodies = frames.bodies.size();
        const std::vector<double> poses = block("poses.npy");
        if (poses.size() != nFrames * nBodies * 7) {
            THROWM(Base::RuntimeError, "the simulation results' poses do not match their frames");
        }
        frames.poses.reserve(nFrames * nBodies);
        for (std::size_t i = 0; i < nFrames * nBodies; ++i) {
            frames.poses.push_back(placementOf(&poses[7 * i]));
        }
        if (width > 0) {
            frames.values = block("values.npy");
            if (frames.values.size() != nFrames * width) {
                THROWM(Base::RuntimeError,
                       "the simulation results' values do not match their frames");
            }
        }
        zip.close();
    }
    catch (const json::exception& e) {
        FC_THROWM(Base::RuntimeError, "the simulation results' index is malformed: " << e.what());
    }
    catch (const std::exception& e) {
        // zipios' own, a file it cannot open or read
        if (dynamic_cast<const Base::Exception*>(&e)) {
            throw;
        }
        FC_THROWM(Base::FileException, "cannot read the simulation results: " << e.what());
    }
    return archive;
}
