/***************************************************************************
 *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com>*
 *                                                                         *
 *   This file is part of the FreeCAD CAx development system.              *
 *                                                                         *
 *   This library is free software; you can redistribute it and/or         *
 *   modify it under the terms of the GNU Library General Public           *
 *   License as published by the Free Software Foundation; either          *
 *   version 2 of the License, or (at your option) any later version.      *
 *                                                                         *
 *   This library  is distributed in the hope that it will be useful,      *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this library; see the file COPYING.LIB. If not,    *
 *   write to the Free Software Foundation, Inc., 59 Temple Place,         *
 *   Suite 330, Boston, MA  02111-1307, USA                                *
 *                                                                         *
 ***************************************************************************/

// A recompute made by worker processes (docs/WorkerProcess.md sec 19): the
// kind "recompute" of the document's WorkerPool, and the client that hands
// it out and puts its result in. Both sides go through RecomputeHook, the
// worker to note what each execute() wrote, this process to put that in
// instead of executing.

#include "PreCompiled.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <unordered_map>

#include <QTimer>

#include <nlohmann/json.hpp>

#include <Base/Console.h>
#include <Base/FileInfo.h>
#include <Base/Tools.h>

#include "Animation.h"
#include "Application.h"
#include "Document.h"
#include "DocumentObject.h"
#include "LogMoveKeep.h"
#include "Property.h"
#include "RecomputeHook.h"
#include "TransactionLog.h"
#include "StringHasher.h"
#include "TransactionValue.h"
#include "WorkerPool.h"
#include "WorkerRecompute.h"
#include "WriteScope.h"

FC_LOG_LEVEL_INIT("Workers", true, true)

using namespace App;
using json = nlohmann::json;

// --------------------------------------------------------------------------
// RecomputeHook

namespace
{
thread_local RecomputeHook* innermost = nullptr;
thread_local unsigned long mintedCount = 0;
thread_local int puttingInDepth = 0;
thread_local LogMoveKeep* innermostKeep = nullptr;

/// What this process's last recompute job sent, by object and property
/// name (WorkerRecompute::sent)
std::set<std::pair<std::string, std::string>>& sentWrites()
{
    static std::set<std::pair<std::string, std::string>> set;
    return set;
}
}  // namespace

LogMoveKeep::LogMoveKeep(const Document& doc, Keep keep)
    : doc(doc)
    , keep(std::move(keep))
    , outer(innermostKeep)
{
    innermostKeep = this;
}

LogMoveKeep::~LogMoveKeep()
{
    if (innermostKeep == this) {
        innermostKeep = outer;
        return;
    }
    for (auto k = innermostKeep; k; k = k->outer) {
        if (k->outer == this) {
            k->outer = outer;
            return;
        }
    }
}

LogMoveKeep* LogMoveKeep::of(const Document& doc)
{
    for (auto k = innermostKeep; k; k = k->outer) {
        if (&k->doc == &doc) {
            return k;
        }
    }
    return nullptr;
}

RecomputeHook::RecomputeHook(const Document& doc)
    : _doc(&doc)
    , _outer(innermost)
{
    innermost = this;
}

RecomputeHook::~RecomputeHook()
{
    if (innermost == this) {
        innermost = _outer;
        return;
    }
    for (auto hook = innermost; hook; hook = hook->_outer) {
        if (hook->_outer == this) {
            hook->_outer = _outer;
            return;
        }
    }
}

bool RecomputeHook::make(DocumentObject&, Made&)
{
    return false;
}

void RecomputeHook::executing(DocumentObject&)
{}

void RecomputeHook::executed(DocumentObject&, int, double)
{}

RecomputeHook* RecomputeHook::of(const Document& doc)
{
    for (auto hook = innermost; hook; hook = hook->_outer) {
        if (hook->_doc == &doc) {
            return hook;
        }
    }
    return nullptr;
}

unsigned long RecomputeHook::minted()
{
    return mintedCount;
}

void RecomputeHook::noteMinted()
{
    ++mintedCount;
}

RecomputeHook::PuttingIn::PuttingIn()
{
    ++puttingInDepth;
}

RecomputeHook::PuttingIn::~PuttingIn()
{
    --puttingInDepth;
}

bool RecomputeHook::puttingIn()
{
    return puttingInDepth > 0;
}

// --------------------------------------------------------------------------
// The result's file

namespace
{

constexpr const char* RecomputeKind = "recompute";

/// Not of the design: a value no save writes -- a shape's cache, made on
/// demand by whoever asks -- and the document's bookkeeping in a name of
/// upstream's (`_ObjectUUID`, Property::Legacy) are each process's own
bool notSaved(const Property& prop)
{
    return prop.testStatus(Property::Transient) || prop.testStatus(Property::PropNoPersist)
        || prop.testStatus(Property::PropTransient) || prop.testStatus(Property::Legacy);
}

/// The start of a worker's recompute: then its string table's hash, the
/// seconds it took, the strings its values name (a count plus one, then
/// the rows; 0 for none sent, docs/WorkerProcess.md sec 15), the dynamic
/// properties it added (object, name, type, group, doc, attributes, read
/// only, hidden), and each object it executed, in order: its name, whether it came out, its error,
/// its seconds, and what its execute() wrote -- each value's object and
/// property by name, its fragment and its attachments
const char RecomputeMagic[8] = {'F', 'C', 'W', 'R', 'C', 'P', '0', '1'};

/** Made here, never from a worker's values: an object whose result is not
 * all in its properties. A TechDraw view keeps its geometry in members
 * and finds its hidden lines in threads that end after its execute(): a
 * worker's values would put in a view with nothing to draw. By type name,
 * each a prefix (RecomputeHereTypes of Preferences/Workers, words
 * separated by spaces)
 */
bool madeHere(const DocumentObject& obj)
{
    static std::string kept;
    static std::vector<std::string> prefixes;
    std::string list = GetApplication()
                           .GetParameterGroupByPath("User parameter:BaseApp/Preferences/Workers")
                           ->GetASCII("RecomputeHereTypes", "TechDraw::");
    if (list != kept) {
        kept = list;
        prefixes.clear();
        std::istringstream words(list);
        for (std::string w; words >> w;) {
            prefixes.push_back(w);
        }
    }
    const char* type = obj.getTypeId().getName();
    for (auto& p : prefixes) {
        if (std::strncmp(type, p.c_str(), p.size()) == 0) {
            return true;
        }
    }
    return false;
}

double now()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void putSize(std::ostream& out, std::uint64_t n)
{
    out.write(reinterpret_cast<const char*>(&n), sizeof(n));
}

void putBytes(std::ostream& out, const std::string& bytes)
{
    putSize(out, bytes.size());
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void putDouble(std::ostream& out, double v)
{
    std::uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(v));
    std::memcpy(&bits, &v, sizeof(bits));
    putSize(out, bits);
}

bool getSize(std::istream& in, std::uint64_t& n)
{
    in.read(reinterpret_cast<char*>(&n), sizeof(n));
    return in.good();
}

bool getBytes(std::istream& in, std::string& bytes, std::uint64_t most)
{
    std::uint64_t n = 0;
    if (!getSize(in, n) || n > most) {
        return false;
    }
    bytes.resize(n);
    in.read(bytes.data(), static_cast<std::streamsize>(n));
    return in.good() || (in.eof() && static_cast<std::uint64_t>(in.gcount()) == n);
}

bool getDouble(std::istream& in, double& v)
{
    std::uint64_t bits = 0;
    if (!getSize(in, bits)) {
        return false;
    }
    std::memcpy(&v, &bits, sizeof(bits));
    return true;
}

bool writeAtomic(const std::string& name, const std::function<bool(std::ostream&)>& body)
{
    std::string part = name + ".part";
    {
        std::ofstream file(part, std::ios::binary | std::ios::trunc);
        if (!file || !body(file)) {
            file.close();
            Base::FileInfo(part).deleteFile();
            return false;
        }
        file.flush();
        if (!file) {
            file.close();
            Base::FileInfo(part).deleteFile();
            return false;
        }
    }
    Base::FileInfo(name).deleteFile();
    if (!Base::FileInfo(part).renameFile(name.c_str())) {
        Base::FileInfo(part).deleteFile();
        return false;
    }
    return true;
}

// --------------------------------------------------------------------------
// The worker's side

/// Notes, for each object the recompute executes, what its execute() wrote
class Recorder: public RecomputeHook
{
public:
    struct Entry
    {
        DocumentObject* obj = nullptr;
        int res = 0;
        std::string error;
        double seconds = 0;
        std::vector<std::pair<DocumentObject*, Property*>> writes;
        std::map<DocumentObject*, bool> touched;  ///< the other owners, when it was over
        bool here = false;  ///< made by the other process itself: not sent
    };

    explicit Recorder(Document& doc)
        : RecomputeHook(doc)
        , doc(doc)
    {
        conns.emplace_back(doc.signalChangedObject.connect(
            [this](const DocumentObject& obj, const Property& prop) {
                // Each value once, with the first object that wrote it: its
                // value as the recompute leaves it is what goes back
                if (!running.empty() && !entries[running.back()].here && !notSaved(prop)
                    && seen.insert(&prop).second) {
                    entries[running.back()].writes.emplace_back(
                        const_cast<DocumentObject*>(&obj), const_cast<Property*>(&prop));
                }
            }));
        // What the other process cannot be given by values: it names
        // objects and properties it has
        auto object = [this](const DocumentObject& obj) {
            if (spoiled.empty()) {
                spoiled = std::string(obj.getFullName()) + " was made or removed by the recompute";
            }
        };
        conns.emplace_back(doc.signalNewObject.connect(object));
        conns.emplace_back(doc.signalDeletedObject.connect(object));
        // One added is looked at when the recompute is over: what marks it
        // as not saved may come after it is added
        conns.emplace_back(GetApplication().signalAppendDynamicProperty.connect(
            [this](const Property& prop) {
                auto obj = freecad_cast<DocumentObject*>(prop.getContainer());
                if (obj && obj->getDocument() == &this->doc && prop.getName()) {
                    added.emplace_back(obj, prop.getName());
                }
            }));
        conns.emplace_back(GetApplication().signalRemoveDynamicProperty.connect(
            [this](const Property& prop) {
                auto obj = freecad_cast<DocumentObject*>(prop.getContainer());
                if (obj && obj->getDocument() == &this->doc && spoiled.empty()
                    && !notSaved(prop)) {
                    spoiled = prop.getFullName() + " was removed by the recompute";
                }
            }));
    }

    /// Why the other process cannot take it in by values, empty if it can
    std::string why()
    {
        if (!spoiled.empty()) {
            return spoiled;
        }
        return {};
    }

    /// What it added that the design keeps: the other process adds them
    /// as the execute() that added them here did
    std::vector<Property*> addedKept()
    {
        std::vector<Property*> props;
        for (auto& [obj, name] : added) {
            if (!obj->isAttachedToDocument()) {
                continue;
            }
            auto prop = obj->getDynamicPropertyByName(name.c_str());
            if (prop && !notSaved(*prop)
                && std::find(props.begin(), props.end(), prop) == props.end()) {
                props.push_back(prop);
            }
        }
        return props;
    }

    /// Not made here: the other process makes it itself
    bool make(DocumentObject& obj, Made& made) override
    {
        if (!madeHere(obj)) {
            return false;
        }
        made.ok = true;
        ++left;
        return true;
    }

    // An execute() may recompute another object itself (a split its
    // pieces): that one is an entry of its own, made while the outer one
    // runs, and what it writes is its own
    void executing(DocumentObject& obj) override
    {
        Entry entry;
        entry.obj = &obj;
        entry.here = madeHere(obj);
        running.push_back(entries.size());
        entries.push_back(std::move(entry));
    }

    void executed(DocumentObject& obj, int res, double seconds) override
    {
        if (!running.empty() && entries[running.back()].obj == &obj) {
            auto& entry = entries[running.back()];
            entry.res = res;
            entry.seconds = seconds;
            // How it left the other objects it wrote: a value put in may
            // touch where the write here did not, or not where it did
            for (auto& [owner, prop] : entry.writes) {
                if (owner != &obj && owner->isAttachedToDocument()) {
                    entry.touched[owner] = owner->isTouched();
                }
            }
            if (res) {
                const char* msg = doc.getErrorDescription(&obj);
                entry.error = msg && msg[0] ? msg : "Error";
            }
            running.pop_back();
        }
    }

    Document& doc;
    std::vector<Entry> entries;
    std::vector<std::size_t> running;  ///< the entries being executed, innermost last
    std::set<const Property*> seen;
    std::string spoiled;
    std::vector<std::pair<DocumentObject*, std::string>> added;
    long left = 0;  ///< objects left for the other process to make
    std::vector<fastsignals::scoped_connection> conns;
};

WorkerPool::Reply unfit(const std::string& why)
{
    WorkerPool::Reply reply;
    reply.ok = false;
    reply.unfit = true;
    reply.error = why;
    return reply;
}

/// In a worker: recompute what the job names, and write what each object's
/// execute() wrote into the pool's file
WorkerPool::Reply makeRecompute(const WorkerPool::Request& request)
{
    WorkerPool::Reply reply;
    Document* doc = request.document;
    if (!doc) {
        return unfit("no copy of the document");
    }
    json inputs = json::parse(request.inputs, nullptr, false);
    std::vector<DocumentObject*> objs;
    auto names = inputs.find("objects");
    if (names != inputs.end() && names->is_array()) {
        for (auto& item : *names) {
            if (!item.is_string()) {
                continue;
            }
            auto name = item.get<std::string>();
            auto obj = doc->getObject(name.c_str());
            if (!obj) {
                return unfit("no " + name + " in the copy");
            }
            objs.push_back(obj);
        }
    }
    std::map<std::string, std::string> display;
    auto hints = inputs.find("display");
    if (hints != inputs.end() && hints->is_object()) {
        for (auto& [name, hint] : hints->items()) {
            if (hint.is_string()) {
                display[name] = hint.get<std::string>();
            }
        }
    }
    long touched = 0;
    for (auto obj : doc->getObjects()) {
        if (obj->mustRecompute()) {
            ++touched;
        }
    }
    const unsigned long minted = RecomputeHook::minted();
    const double began = now();
    Recorder recorder(*doc);
    doc->recompute(objs);
    const double seconds = now() - began;
    if (auto why = recorder.why(); !why.empty()) {
        return unfit(why);
    }
    // A number an object gave a thing here is a number the other process
    // may give another (docs/WorkerProcess.md sec 5): not taken in as it is
    if (RecomputeHook::minted() != minted) {
        return unfit("the recompute gave things new numbers");
    }
    // What the other process makes itself is not sent
    recorder.entries.erase(std::remove_if(recorder.entries.begin(), recorder.entries.end(),
                                          [](const Recorder::Entry& e) { return e.here; }),
                           recorder.entries.end());
    for (auto& entry : recorder.entries) {
        if (entry.res < 0) {
            reply.ok = false;
            reply.error = "the recompute stopped at " + std::string(entry.obj->getFullName());
            return reply;
        }
    }
    // The string table the values name their strings in, for the other
    // process to read them by content if it cannot take them in by id
    std::string hash;
    auto hasher = doc->getStringHasher();
    const std::string tables = inputs.value("tables", std::string());
    if (hasher && !tables.empty()) {
        hash = hasher->contentHash();
        std::string name = tables + "/" + hash + ".table";
        if (!Base::FileInfo(name).exists()) {
            std::string bytes = hasher->saveTable();
            if (!writeAtomic(name, [&bytes](std::ostream& s) {
                    s.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                    return bool(s);
                })) {
                reply.ok = false;
                reply.error = "cannot write " + name;
                return reply;
            }
        }
    }
    CaptureConfig config(*doc);
    config.blobs = nullptr;  // whole, in the result's own file
    config.preferBinary = true;
    sentWrites().clear();
    std::vector<std::vector<CapturedValue>> values(recorder.entries.size());
    std::vector<long> ids;
    std::size_t count = 0;
    for (std::size_t i = 0; i < recorder.entries.size(); ++i) {
        for (auto& [obj, prop] : recorder.entries[i].writes) {
            if (!obj->isAttachedToDocument()) {
                return unfit(std::string(obj->getFullName()) + " is gone");
            }
            // Meshed as the view of the other process meshes it, the mesh
            // kept with the value (docs/WorkerProcess.md sec 14)
            CapturedValue value;
            auto hint = display.find(obj->getNameInDocument());
            if (hint == display.end() || !Animation::captureForDisplay
                || !Animation::captureForDisplay(*prop, hint->second, config, value)) {
                value = captureValue(config, *prop);
            }
            if (!value.ok || !value.blobs.empty()) {
                return unfit(prop->getFullName() + " cannot be written");
            }
            ids.insert(ids.end(), value.stringIds.begin(), value.stringIds.end());
            values[i].push_back(std::move(value));
            sentWrites().emplace(obj->getNameInDocument(), prop->getName());
            ++count;
        }
    }
    const bool byId = hasher && hasher->lease();
    std::vector<StringHasher::Row> rows;
    if (byId) {
        rows = hasher->rowsOf(ids);
    }
    const std::vector<Property*> added = recorder.addedKept();
    std::size_t bytes = 0;
    bool ok = writeAtomic(request.out, [&](std::ostream& s) {
        s.write(RecomputeMagic, sizeof(RecomputeMagic));
        putBytes(s, hash);
        putDouble(s, seconds);
        putSize(s, byId ? rows.size() + 1 : 0);
        if (byId) {
            StringHasher::writeRows(s, rows);
        }
        putSize(s, added.size());
        for (auto prop : added) {
            auto obj = static_cast<DocumentObject*>(prop->getContainer());
            auto data = obj->getDynamicPropertyData(prop);
            const char* doc = prop->getDocumentation();
            putBytes(s, obj->getNameInDocument());
            putBytes(s, prop->getName());
            putBytes(s, prop->getTypeId().getName());
            putBytes(s, data.group);
            putBytes(s, doc ? doc : "");
            putSize(s, static_cast<std::uint16_t>(data.attr));
            putSize(s, data.readonly ? 1 : 0);
            putSize(s, data.hidden ? 1 : 0);
        }
        putSize(s, recorder.entries.size());
        for (std::size_t i = 0; i < recorder.entries.size(); ++i) {
            auto& entry = recorder.entries[i];
            putBytes(s, entry.obj->getNameInDocument());
            putSize(s, entry.res ? 0 : 1);
            putBytes(s, entry.error);
            putDouble(s, entry.seconds);
            putSize(s, entry.writes.size());
            for (std::size_t k = 0; k < entry.writes.size(); ++k) {
                auto& [obj, prop] = entry.writes[k];
                auto& value = values[i][k];
                putBytes(s, obj->getNameInDocument());
                putBytes(s, prop->getName());
                auto t = entry.touched.find(obj);
                putSize(s, t == entry.touched.end() ? 0 : (t->second ? 2 : 1));
                putBytes(s, value.fragment);
                putSize(s, value.attachments.size());
                for (auto& a : value.attachments) {
                    putBytes(s, a.name);
                    putBytes(s, a.bytes);
                }
            }
        }
        bytes = static_cast<std::size_t>(s.tellp());
        return bool(s);
    });
    if (!ok) {
        reply.ok = false;
        reply.error = "cannot write " + request.out;
        return reply;
    }
    reply.output = json {{"executed", recorder.entries.size()},
                         {"values", count},
                         {"bytes", bytes},
                         {"seconds", seconds},
                         {"touched", touched},
                         {"left", recorder.left},
                         {"added", added.size()},
                         {"table", hash},
                         {"byId", byId}}
                       .dump();
    return reply;
}

const bool registered = WorkerPool::registerKind(RecomputeKind, makeRecompute);

}  // namespace

bool WorkerRecompute::sent(const std::string& obj, const std::string& prop)
{
    return sentWrites().count({obj, prop}) > 0;
}

namespace
{

// --------------------------------------------------------------------------
// This process's side

/// Makes each object of a recompute here from what a worker's execute()
/// wrote, in place of executing it
/// What each object's last recompute took, by its id, per document
/// (WorkerRecompute::expectedSeconds)
struct Times
{
    std::unordered_map<long, double> seconds;
    std::chrono::steady_clock::time_point last;
};

std::map<const Document*, Times>& times()
{
    static std::map<const Document*, Times> map;
    return map;
}

class Supplier: public RecomputeHook
{
public:
    struct Value
    {
        std::string object;
        std::string prop;
        /// Of a value of another object: 1 it was left clean, 2 touched
        std::uint64_t left = 0;
        CapturedValue value;
    };
    struct Entry
    {
        bool ok = true;
        std::string error;
        double seconds = 0;
        std::vector<Value> values;
    };
    struct Added
    {
        std::string type;
        std::string group;
        std::string doc;
        short attr = 0;
        bool readonly = false;
        bool hidden = false;
    };

    explicit Supplier(Document& doc)
        : RecomputeHook(doc)
        , doc(doc)
    {}

    /// Read a worker's file; false, `why` set, when it does not read
    bool read(const std::string& file, const std::string& tables, std::string& why)
    {
        Base::FileInfo info(file);
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            why = "cannot read " + file;
            return false;
        }
        bytes = static_cast<std::size_t>(info.size());
        const std::uint64_t most = bytes;
        char magic[sizeof(RecomputeMagic)];
        in.read(magic, sizeof(magic));
        std::string hash;
        std::uint64_t rowCount = 0;
        if (!in || std::memcmp(magic, RecomputeMagic, sizeof(magic)) != 0
            || !getBytes(in, hash, most) || !getDouble(in, seconds)
            || !getSize(in, rowCount) || rowCount > most) {
            why = "not a worker's recompute";
            return false;
        }
        if (rowCount) {
            byId = true;
            if (!StringHasher::readRows(in, rows, most) || rows.size() != rowCount - 1) {
                why = "its strings do not read";
                return false;
            }
        }
        std::uint64_t addedCount = 0;
        if (!getSize(in, addedCount) || addedCount > most) {
            why = "its properties do not read";
            return false;
        }
        for (std::uint64_t i = 0; i < addedCount; ++i) {
            Added a;
            std::string object;
            std::string name;
            std::uint64_t attr = 0;
            std::uint64_t ro = 0;
            std::uint64_t hidden = 0;
            if (!getBytes(in, object, most) || !getBytes(in, name, most)
                || !getBytes(in, a.type, most) || !getBytes(in, a.group, most)
                || !getBytes(in, a.doc, most) || !getSize(in, attr) || !getSize(in, ro)
                || !getSize(in, hidden)) {
                why = "its properties do not read";
                return false;
            }
            a.attr = static_cast<short>(attr);
            a.readonly = ro != 0;
            a.hidden = hidden != 0;
            added[object + "." + name] = std::move(a);
        }
        if (!hash.empty()) {
            std::ifstream t(tables + "/" + hash + ".table", std::ios::binary);
            if (t) {
                table = StringHasherRef(new StringHasher);
                try {
                    table->restoreTable(t);
                }
                catch (Base::Exception& e) {
                    table = StringHasherRef();
                }
            }
        }
        std::uint64_t n = 0;
        if (!getSize(in, n) || n > most) {
            why = "its objects do not read";
            return false;
        }
        std::string name;
        for (std::uint64_t i = 0; i < n; ++i) {
            Entry entry;
            std::uint64_t ok = 0;
            std::uint64_t k = 0;
            if (!getBytes(in, name, most) || !getSize(in, ok) || !getBytes(in, entry.error, most)
                || !getDouble(in, entry.seconds) || !getSize(in, k) || k > most) {
                why = "its objects do not read";
                return false;
            }
            entry.ok = ok != 0;
            entry.values.resize(k);
            for (auto& v : entry.values) {
                std::uint64_t attachments = 0;
                if (!getBytes(in, v.object, most) || !getBytes(in, v.prop, most)
                    || !getSize(in, v.left) || !getBytes(in, v.value.fragment, most) || !getSize(in, attachments)
                    || attachments > most) {
                    why = "its values do not read";
                    return false;
                }
                v.value.attachments.resize(attachments);
                for (auto& a : v.value.attachments) {
                    if (!getBytes(in, a.name, most) || !getBytes(in, a.bytes, most)) {
                        why = "its values do not read";
                        return false;
                    }
                }
                v.value.ok = true;
            }
            entries[name].push_back(std::move(entry));
            ++count;
        }
        return true;
    }

    /// Before the recompute: its strings under their ids, or else read by
    /// content from the worker's table while it runs
    bool takeStrings(std::string& why)
    {
        auto hasher = doc.getStringHasher();
        if (byId && hasher && hasher->takeRows(rows, &why)) {
            takenById = true;
            return true;
        }
        if (!table) {
            if (why.empty()) {
                why = "the worker's string table is not there";
            }
            return false;
        }
        if (byId) {
            FC_LOG(doc.getName() << ": a worker's recompute read by content: " << why);
            byContentWhy = why;
        }
        else {
            byContentWhy = "the worker sent no rows";
        }
        strings = std::make_unique<RestoreStrings>(table, hasher);
        return true;
    }

    void executing(DocumentObject& obj) override
    {
        answered.erase(&obj);  // a new recompute of it takes its next entry
    }

    /// What it took: the worker's execute() for one made from its values,
    /// this process's for one executed here (expectedSeconds)
    void executed(DocumentObject& obj, int res, double took) override
    {
        (void)res;
        auto it = answered.find(&obj);
        const bool supplied = it != answered.end() && it->second.first;
        times()[&doc].seconds[obj.getID()] = supplied ? it->second.second.seconds : took;
    }

    bool make(DocumentObject& obj, Made& made) override
    {
        // Its class retrying the execute within the one recompute of it (a
        // Part feature on copies of its inputs): the answer it was given
        auto again = answered.find(&obj);
        if (again != answered.end()) {
            made = again->second.second;
            return again->second.first;
        }
        Made got;
        const bool supplied = supply(obj, got);
        answered[&obj] = {supplied, got};
        if (supplied) {
            fromWorker.insert(obj.getNameInDocument());
        }
        made = got;
        return supplied;
    }

    bool supply(DocumentObject& obj, Made& made)
    {
        if (madeHere(obj)) {
            ++madeByType;
            return false;  // executed here, as it must be
        }
        auto it = entries.find(obj.getNameInDocument());
        if (it == entries.end() || it->second.empty()) {
            noteHere(obj);
            return false;
        }
        Entry entry = std::move(it->second.front());
        it->second.pop_front();
        // The other objects it writes, clean before: left as the worker's
        // execute() left them, not as putting the value in leaves them
        std::map<DocumentObject*, bool> others;
        for (auto& v : entry.values) {
            if (v.left && v.object != obj.getNameInDocument()) {
                auto owner = doc.getObject(v.object.c_str());
                if (owner && !owner->isTouched()) {
                    others[owner] = v.left == 2;
                }
            }
        }
        try {
            PuttingIn checked;
            RestoreBatch batch;
            for (auto& v : entry.values) {
                auto owner = doc.getObject(v.object.c_str());
                auto prop = owner ? owner->getPropertyByName(v.prop.c_str()) : nullptr;
                if (!prop && owner) {
                    // Added by the execute() there: added here as it did
                    auto a = added.find(v.object + "." + v.prop);
                    if (a != added.end()) {
                        prop = owner->addDynamicProperty(
                            a->second.type.c_str(), v.prop.c_str(), a->second.group.c_str(),
                            a->second.doc.c_str(), a->second.attr, a->second.readonly,
                            a->second.hidden);
                        if (prop) {
                            ++propertiesAdded;
                        }
                    }
                }
                if (!prop) {
                    FC_WARN(doc.getName() << ": a worker's recompute names " << v.object << "."
                                          << v.prop << ", which is not here");
                    noteHere(obj);
                    return false;  // executed here, which writes it all again
                }
                restoreValue(*prop, v.value);
            }
            batch.finish();
            for (auto& [owner, touched] : others) {
                if (!owner->isAttachedToDocument() || owner->isTouched() == touched) {
                    continue;
                }
                if (touched) {
                    owner->touch();
                }
                else {
                    owner->purgeTouched();
                }
                ++touchSet;
            }
        }
        catch (Base::Exception& e) {
            FC_WARN(obj.getFullName() << ": a worker's value not put in: " << e.what());
            noteHere(obj);
            return false;
        }
        made.ok = entry.ok;
        made.error = entry.error;
        made.seconds = entry.seconds;
        ++supplied;
        return true;
    }

    /// What make() said of each object in its current recompute
    std::map<const DocumentObject*, std::pair<bool, Made>> answered;

    void noteHere(const DocumentObject& obj)
    {
        FC_LOG(obj.getFullName() << ": not made by the worker, executed here");
        ++executedHere;
        if (hereNames.size() < 20) {
            hereNames.emplace_back(obj.getNameInDocument());
        }
    }

    long unused() const
    {
        long n = 0;
        for (auto& [name, list] : entries) {
            // One that carries nothing is no object made there: an object
            // whose recompute came to no execute() here (a link's element)
            for (auto& e : list) {
                n += !e.values.empty() || !e.ok ? 1 : 0;
            }
        }
        return n;
    }

    Document& doc;
    std::unordered_map<std::string, std::deque<Entry>> entries;
    std::map<std::string, Added> added;  ///< by object.property
    long propertiesAdded = 0;
    std::vector<StringHasher::Row> rows;
    bool byId = false;
    bool takenById = false;
    std::string byContentWhy;
    StringHasherRef table;
    std::unique_ptr<RestoreStrings> strings;
    double seconds = 0;
    std::size_t bytes = 0;
    long count = 0;
    long supplied = 0;
    long executedHere = 0;
    std::vector<std::string> hereNames;
    long touchSet = 0;
    /// Made from the worker's values, by name: what the worker holds as
    /// they were put in (docs/WorkerProcess.md sec 29)
    std::set<std::string> fromWorker;
    long madeByType = 0;  ///< executed here by their type (madeHere())  ///< objects whose touched state was set as the worker left it
};

struct State
{
    explicit State(Document& doc)
        : doc(doc)
        , tag(std::string("recompute:") + doc.getName())
    {}

    Document& doc;
    std::string tag;
    WorkerPool::JobId job = 0;
    std::vector<std::string> names;  ///< what it recomputes
    std::string file;  ///< its result, come and not put in yet
    WorkerPool::JobId fileJob = 0;  ///< the job it is of
    std::string tables;
    bool applying = false;
    bool timer = false;
    /// Taken by a recompute run here: the hook for as long as it runs
    std::unique_ptr<Supplier> borrowed;
    WorkerRecompute::Info info;
    std::vector<fastsignals::scoped_connection> conns;
};

std::map<const Document*, std::unique_ptr<State>>& states()
{
    static std::map<const Document*, std::unique_ptr<State>> map;
    return map;
}

State* stateOf(const Document& doc)
{
    auto it = states().find(&doc);
    return it == states().end() ? nullptr : it->second.get();
}

State* stateByName(const std::string& name)
{
    auto doc = GetApplication().getDocument(name.c_str());
    return doc ? stateOf(*doc) : nullptr;
}

void changed(State& s)
{
    s.info.running = s.job != 0 || !s.file.empty();
    WorkerRecompute::signalChanged(s.doc);
}

void dropFile(State& s)
{
    if (!s.file.empty()) {
        Base::FileInfo(s.file).deleteFile();
        s.file.clear();
    }
}

/// Fill a supplier from the result that came; false, the reason noted and
/// the result dropped, when it does not read
bool supplierFor(State& s, std::unique_ptr<Supplier>& supplier)
{
    supplier = std::make_unique<Supplier>(s.doc);
    std::string why;
    const std::string file = s.file;
    s.file.clear();
    bool ok = supplier->read(file, s.tables, why) && supplier->takeStrings(why);
    Base::FileInfo(file).deleteFile();
    if (!ok) {
        supplier.reset();
        s.info.reason = "a worker's recompute not put in: " + why;
        ++s.info.failed;
        FC_WARN(s.doc.getName() << ": " << s.info.reason);
    }
    return ok;
}

void noteSupplied(State& s, const Supplier& supplier)
{
    s.info.objects = static_cast<long>(s.names.size());
    s.info.supplied = supplier.supplied;
    s.info.executedHere = supplier.executedHere;
    s.info.madeHere = supplier.madeByType;
    s.info.executedHereNames = supplier.hereNames;
    s.info.unused = supplier.unused();
    s.info.byId = supplier.takenById;
    s.info.byContentWhy = supplier.byContentWhy;
    s.info.workerSeconds = supplier.seconds;
    s.info.bytes = supplier.bytes;
}

bool safeNow(const Document& doc)
{
    return !doc.testStatus(Document::Recomputing) && !doc.isPerformingTransaction()
        && !doc.testStatus(Document::Restoring);
}

void schedule(State& s);

/// Put in the result that came, as a recompute of what it recomputed
void apply(State& s)
{
    if (s.file.empty() || s.applying) {
        return;
    }
    if (!safeNow(s.doc)) {
        schedule(s);
        return;
    }
    std::unique_ptr<Supplier> supplier;
    if (!supplierFor(s, supplier)) {
        changed(s);
        return;
    }
    std::vector<DocumentObject*> objs;
    for (auto& name : s.names) {
        if (auto obj = s.doc.getObject(name.c_str())) {
            objs.push_back(obj);
        }
    }
    const double began = now();
    TransactionLog* log = s.doc.getTransactionLog();
    const int64_t before = log ? log->lastSeq() : 0;
    {
        Base::FlagToggler<bool> applying(s.applying, false);
        try {
            s.doc.recompute(objs);
        }
        catch (Base::Exception& e) {
            e.ReportException();
        }
    }
    s.info.applySeconds = now() - began;
    // The rows it was put in as, and what in them is the worker's: told to
    // the worker that made it, which keeps those values as it holds them
    // when it next moves across the rows (docs/WorkerProcess.md sec 29)
    if (log && log->lastSeq() > before && !supplier->fromWorker.empty()) {
        if (auto pool = WorkerPool::of(s.doc, false)) {
            pool->putIn(s.fileJob, before + 1, log->lastSeq(),
                        {supplier->fromWorker.begin(), supplier->fromWorker.end()});
        }
    }
    noteSupplied(s, *supplier);
    ++s.info.applied;
    s.info.reason.clear();
    supplier.reset();
    changed(s);
}

void schedule(State& s)
{
    if (s.timer) {
        return;
    }
    s.timer = true;
    const std::string name = s.doc.getName();
    QTimer::singleShot(0, [name]() {
        if (auto s = stateByName(name)) {
            s->timer = false;
            apply(*s);
        }
    });
}

void done(const std::string& name, WorkerPool::JobId id, WorkerPool::Result& r)
{
    State* s = stateByName(name);
    if (!s || s->job != id) {
        return;
    }
    s->job = 0;
    switch (r.status) {
        case WorkerPool::Result::Status::Done:
            // An object it failed to recompute is said to have failed here
            // as the result is put in: the worker's saying so is not said
            // again (docs/WorkerProcess.md sec 27)
            r.messages.erase(std::remove_if(r.messages.begin(), r.messages.end(),
                                            [](const WorkerPool::Message& m) {
                                                return m.level == Base::LogStyle::Error
                                                    && m.text.find("Failed to recompute ")
                                                    != std::string::npos;
                                            }),
                             r.messages.end());
            if (r.file.empty()) {
                ++s->info.failed;
                s->info.reason = "the worker wrote nothing";
                break;
            }
            s->file = r.file;
            s->fileJob = id;
            r.file.clear();  // ours now, in the pool's directory until put in
            schedule(*s);
            break;
        case WorkerPool::Result::Status::Failed:
            ++s->info.failed;
            s->info.reason = r.error;
            FC_LOG(name << ": a worker did not recompute: " << r.error);
            break;
        case WorkerPool::Result::Status::Unfit:
            ++s->info.unfit;
            s->info.reason = r.error;
            FC_LOG(name << ": a worker cannot recompute: " << r.error);
            break;
        case WorkerPool::Result::Status::Cancelled:
            ++s->info.cancelled;
            s->info.reason = r.error;
            break;
    }
    changed(*s);
}

State& stateFor(Document& doc)
{
    auto& slot = states()[&doc];
    if (slot) {
        return *slot;
    }
    slot = std::make_unique<State>(doc);
    State& s = *slot;
    static fastsignals::scoped_connection gone =
        GetApplication().signalDeleteDocument.connect([](const Document& d) {
            states().erase(&d);
        });
    // A recompute run here meanwhile: the result that came is put in by it;
    // one being made is waited for; one queued is taken back and done here
    s.conns.emplace_back(doc.signalBeforeRecompute.connect([&s](const Document&) {
        if (s.applying) {
            return;
        }
        s.borrowed.reset();
        if (s.job) {
            if (auto pool = WorkerPool::of(s.doc, false)) {
                const WorkerPool::JobId id = s.job;
                if (pool->claim(id) != WorkerPool::Claim::Done && s.job == id) {
                    s.job = 0;
                    ++s.info.cancelled;
                    s.info.reason = "recomputed here";
                }
            }
            else {
                s.job = 0;
            }
        }
        if (!s.file.empty() && supplierFor(s, s.borrowed)) {
            ++s.info.borrowed;
        }
        changed(s);
    }));
    s.conns.emplace_back(doc.signalRecomputed.connect(
        [&s](const Document&, const std::vector<DocumentObject*>&) {
            if (s.borrowed && !s.applying) {
                noteSupplied(s, *s.borrowed);
                s.borrowed.reset();
            }
        }));
    // A change of what the result is made of, between its coming and its
    // being put in: dropped, as the pool drops a job it reaches
    auto reaches = [&s](const std::set<DocumentObject*>& objs) {
        for (auto obj : objs) {
            if (obj && obj->getNameInDocument()
                && std::find(s.names.begin(), s.names.end(), obj->getNameInDocument())
                    != s.names.end()) {
                return true;
            }
        }
        return false;
    };
    s.conns.emplace_back(doc.signalChangedObject.connect(
        [&s, reaches](const DocumentObject& obj, const Property& prop) {
            if (s.file.empty() || s.applying || s.doc.testStatus(Document::Recomputing)
                || (WriteScope::causes() & WriteCause::Animation)
                || !(s.doc.writeEffects(prop) & WriteEffect::Dirty)) {
                return;
            }
            if (reaches(writeReaches(obj, prop))) {
                dropFile(s);
                ++s.info.cancelled;
                s.info.reason = "the design changed";
                changed(s);
            }
        }));
    auto object = [&s](const DocumentObject&) {
        if (!s.file.empty() && !s.applying) {
            dropFile(s);
            ++s.info.cancelled;
            s.info.reason = "the design changed";
            changed(s);
        }
    };
    s.conns.emplace_back(doc.signalNewObject.connect(object));
    s.conns.emplace_back(doc.signalDeletedObject.connect(object));
    return s;
}

}  // namespace

fastsignals::signal<void(const Document&)> WorkerRecompute::signalChanged;

namespace
{

/// What a recompute of `objs` -- none: of what is touched -- reaches: them,
/// and everything depending on them
std::set<DocumentObject*> reachOf(const Document& doc, const std::vector<DocumentObject*>& objs)
{
    std::set<DocumentObject*> seeds;
    if (objs.empty()) {
        for (auto obj : doc.getObjects()) {
            if (obj->mustRecompute()) {
                seeds.insert(obj);
            }
        }
    }
    else {
        for (auto obj : objs) {
            if (obj && obj->isAttachedToDocument() && obj->getDocument() == &doc) {
                seeds.insert(obj);
            }
        }
    }
    std::set<DocumentObject*> all;
    for (auto obj : seeds) {
        all.insert(obj);
        for (auto o : obj->getInListEx(true)) {
            if (o && o->isAttachedToDocument() && o->getDocument() == &doc) {
                all.insert(o);
            }
        }
    }
    return all;
}

}  // namespace

void WorkerRecompute::trackTimes()
{
    static bool tracking = false;
    if (tracking) {
        return;
    }
    tracking = true;
    // Each object's time is from the one before it was recomputed, or from
    // the recompute's start: the gaps add up to the recompute's. A recompute
    // under a hook is noted by the hook -- a worker's result put in, by the
    // worker's times
    static fastsignals::scoped_connection before =
        GetApplication().signalBeforeRecomputeDocument.connect([](const Document& doc) {
            times()[&doc].last = std::chrono::steady_clock::now();
        });
    static fastsignals::scoped_connection each = GetApplication().signalRecomputedObject.connect(
        [](const Document& doc, const DocumentObject& obj) {
            const auto t = std::chrono::steady_clock::now();
            Times& slot = times()[&doc];
            if (!RecomputeHook::of(doc)) {
                slot.seconds[obj.getID()] = std::chrono::duration<double>(t - slot.last).count();
            }
            slot.last = t;
        });
    static fastsignals::scoped_connection gone =
        GetApplication().signalDeleteDocument.connect([](const Document& doc) {
            times().erase(&doc);
        });
}

double WorkerRecompute::expectedSeconds(const Document& doc,
                                        const std::vector<DocumentObject*>& objs, long* unknown)
{
    double total = 0;
    long missing = 0;
    auto it = times().find(&doc);
    for (auto obj : reachOf(doc, objs)) {
        if (it == times().end()) {
            ++missing;
            continue;
        }
        auto t = it->second.seconds.find(obj->getID());
        if (t == it->second.seconds.end()) {
            ++missing;
        }
        else {
            total += t->second;
        }
    }
    if (unknown) {
        *unknown = missing;
    }
    return total;
}

bool WorkerRecompute::start(Document& doc, const std::vector<DocumentObject*>& objs,
                            std::string* why)
{
    (void)registered;
    State& s = stateFor(doc);
    auto refuse = [&](const std::string& reason) {
        s.info.reason = reason;
        if (why) {
            *why = reason;
        }
        return false;
    };
    std::string reason;
    if (!WorkerPool::supported(&reason)) {
        return refuse(reason);
    }
    if (doc.testStatus(Document::Recomputing) || doc.isPerformingTransaction()) {
        return refuse("the document is busy");
    }
    if (s.job || !s.file.empty()) {
        return refuse("a recompute is with the workers already");
    }
    // What a recompute would reach: what is touched, or asked for, and
    // everything depending on it
    const std::set<DocumentObject*> all = reachOf(doc, objs);
    if (all.empty()) {
        return refuse("nothing to recompute");
    }
    WorkerPool* pool = WorkerPool::of(doc);
    // The document as it is now, if the pool's copy is not
    if (!pool->start()) {
        return refuse(pool->reason());
    }
    s.tables = pool->info().directory + "/tables";
    if (!Base::FileInfo(s.tables).createDirectories()) {
        return refuse("cannot make " + s.tables);
    }
    std::vector<std::string> names;
    json display = json::object();
    for (auto obj : all) {
        names.emplace_back(obj->getNameInDocument());
        // How the view here meshes it, for the worker to mesh it so
        if (Animation::displayHint) {
            std::string hint = Animation::displayHint(*obj);
            if (!hint.empty()) {
                display[obj->getNameInDocument()] = hint;
            }
        }
    }
    const bool meshes = GetApplication()
                            .GetParameterGroupByPath("User parameter:BaseApp/Preferences/Workers")
                            ->GetBool("RecomputeMeshes", true);
    WorkerPool::Job job;
    job.kind = RecomputeKind;
    job.tag = s.tag;
    job.reads = names;
    job.inputs = json {{"objects", names},
                       {"display", meshes ? display : json::object()},
                       {"tables", s.tables}}
                     .dump();
    const std::string name = doc.getName();
    auto slot = std::make_shared<WorkerPool::JobId>(0);
    job.done = [name, slot](WorkerPool::Result& r) { done(name, *slot, r); };
    const WorkerPool::JobId id = pool->submit(std::move(job));
    if (!id) {
        return refuse(pool->reason().empty() ? std::string("not taken") : pool->reason());
    }
    *slot = id;
    s.job = id;
    s.names = std::move(names);
    s.info.reason.clear();
    ++s.info.started;
    changed(s);
    return true;
}

bool WorkerRecompute::running(const Document& doc)
{
    auto s = stateOf(doc);
    return s && (s->job || !s->file.empty());
}

bool WorkerRecompute::wait(Document& doc, double seconds)
{
    const double began = now();
    for (;;) {
        State* s = stateOf(doc);
        if (!s) {
            return true;
        }
        if (!s->file.empty() && safeNow(doc)) {
            apply(*s);
            continue;
        }
        if (!s->job) {
            return s->file.empty();
        }
        WorkerPool* pool = WorkerPool::of(doc, false);
        if (!pool) {
            s->job = 0;
            changed(*s);
            return true;
        }
        if (seconds >= 0 && now() - began >= seconds) {
            return false;
        }
        pool->poll(seconds == 0 ? 0 : 50);
    }
}

void WorkerRecompute::cancel(Document& doc, const std::string& why)
{
    State* s = stateOf(doc);
    if (!s || (!s->job && s->file.empty())) {
        return;
    }
    if (s->job) {
        if (auto pool = WorkerPool::of(doc, false)) {
            const std::string tag = s->tag;
            pool->cancel([&tag](const std::string& t) { return t == tag; });
        }
        s->job = 0;
    }
    dropFile(*s);
    ++s->info.cancelled;
    s->info.reason = why;
    changed(*s);
}

WorkerRecompute::Info WorkerRecompute::info(const Document& doc)
{
    auto s = stateOf(doc);
    return s ? s->info : Info();
}
