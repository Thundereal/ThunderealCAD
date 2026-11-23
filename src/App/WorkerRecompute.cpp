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
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <tuple>
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
#include "TransactionStore.h"
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
thread_local RecomputeHook::Minting* minting = nullptr;
RecomputeHook::Handover handover = nullptr;
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

namespace
{
RecomputeHook::Refusal refusal = nullptr;
}

void RecomputeHook::setRefusal(Refusal r)
{
    refusal = r;
}

bool RecomputeHook::refuses(const DocumentObject& obj, std::string& why)
{
    auto doc = obj.getDocument();
    return refusal && doc && !of(*doc) && refusal(obj, why);
}

void RecomputeHook::setHandover(Handover h)
{
    handover = h;
}

bool RecomputeHook::handOver(Document& doc, const std::vector<DocumentObject*>& objs)
{
    return handover && !of(doc) && handover(doc, objs);
}

unsigned long RecomputeHook::minted()
{
    return mintedCount;
}

void RecomputeHook::noteMinted(const DocumentObject& obj, int kind, long before, long after)
{
    ++mintedCount;
    if (minting) {
        auto key = std::make_pair(&obj, kind);
        auto it = minting->counts.find(key);
        if (it == minting->counts.end()) {
            minting->counts[key] = {before, after};
        }
        else {
            it->second.to = std::max(it->second.to, after);
        }
    }
}

RecomputeHook::Minting::Minting()
    : _outer(minting)
{
    minting = this;
}

RecomputeHook::Minting::~Minting()
{
    minting = _outer;
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
/// its seconds, the one whose execute() ran it (its place plus one, 0 for
/// none; sec 46), and what its execute() wrote -- each value's object and
/// property by name, its fragment and its attachments
const char RecomputeMagic[8] = {'F', 'C', 'W', 'R', 'C', 'P', '0', '2'};

/** Made here, never from a worker's values: an object whose result is not
 * all in its properties (DocumentObject::resultInProperties), which values
 * put in would leave with nothing to draw -- the rule an undo goes by. A
 * TechDraw view is one where its projection is not stored, or while it is
 * still being made; a worker, a console process, makes it inline and
 * stores it, and the view installs it as it is put in (docs/WorkerProcess.md
 * sec 39; until then every TechDraw type was made here). And any by type
 * name, each a prefix (RecomputeHereTypes of Preferences/Workers, words
 * separated by spaces; none by default -- "TechDraw::" for as before)
 */
bool madeHere(const DocumentObject& obj)
{
    if (!obj.resultInProperties()) {
        return true;
    }
    static std::string kept;
    static std::vector<std::string> prefixes;
    std::string list = GetApplication()
                           .GetParameterGroupByPath("User parameter:BaseApp/Preferences/Workers")
                           ->GetASCII("RecomputeHereTypes", "");
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
        /// The entry whose execute() ran this one, plus one; 0 for none
        std::size_t outer = 0;
    };

    Recorder(Document& doc, std::function<void(const std::string&)> at)
        : RecomputeHook(doc)
        , doc(doc)
        , at(std::move(at))
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

    /// Not made here: the other process makes it itself; or refused there,
    /// left in the error it is in there
    bool make(DocumentObject& obj, Made& made) override
    {
        if (!refused.empty() && obj.getNameInDocument()) {
            auto it = refused.find(obj.getNameInDocument());
            if (it != refused.end()) {
                made.ok = false;
                made.error = it->second;
                return true;
            }
        }
        if (!madeHere(obj)) {
            return false;
        }
        // One that others read is made here too, for them, and its values
        // not sent (entry.here): a spreadsheet, whose cells' values are not
        // saved, is read by the whole model (docs/WorkerProcess.md sec 44)
        for (auto reader : obj.getInList()) {
            if (reader && reader != &obj && reader->getDocument() == &doc) {
                return false;
            }
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
        entry.outer = running.empty() ? 0 : running.back() + 1;
        // Where it is, should this process die in it (docs/WorkerProcess.md
        // sec 34)
        if (at && !entry.here) {
            at(obj.getNameInDocument());
        }
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
            if (at) {
                // Back in the one that recomputed it, or between objects
                at(running.empty() ? std::string()
                                   : std::string(entries[running.back()].obj->getNameInDocument()));
            }
        }
    }

    Document& doc;
    std::vector<Entry> entries;
    std::vector<std::size_t> running;  ///< the entries being executed, innermost last
    std::set<const Property*> seen;
    std::string spoiled;
    std::vector<std::pair<DocumentObject*, std::string>> added;
    long left = 0;  ///< objects left for the other process to make
    std::function<void(const std::string&)> at;
    /// What the other process does not execute, each with its error
    std::map<std::string, std::string> refused;
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
    // What the asking process has to recompute: an object it marked to be
    // recomputed (enforceRecompute) has nothing in the log that says so
    // (docs/WorkerProcess.md sec 31)
    auto marked = inputs.find("touched");
    if (marked != inputs.end() && marked->is_array()) {
        for (auto& item : *marked) {
            if (auto obj = item.is_string() ? doc->getObject(item.get<std::string>().c_str())
                                            : nullptr) {
                if (!obj->mustRecompute()) {
                    obj->enforceRecompute();
                }
            }
        }
    }
    long touched = 0;
    for (auto obj : doc->getObjects()) {
        if (obj->mustRecompute()) {
            ++touched;
        }
    }
    // What the other process gave so far: counted on from here as it would
    // count on, the numbers given told back (docs/WorkerProcess.md sec 36)
    auto given = inputs.find("numbers");
    if (given != inputs.end() && given->is_object()) {
        for (auto& [name, counts] : given->items()) {
            auto obj = doc->getObject(name.c_str());
            if (!obj || !counts.is_array()) {
                continue;
            }
            for (auto& c : counts) {
                if (c.is_array() && c.size() == 2) {
                    doc->noteGivenNumber(*obj, c[0].get<int>(), c[1].get<long>());
                }
            }
        }
    }
    RecomputeHook::Minting minting;
    const double began = now();
    Recorder recorder(*doc, request.at);
    auto refusedIn = inputs.find("refused");
    if (refusedIn != inputs.end() && refusedIn->is_object()) {
        for (auto& [name, why] : refusedIn->items()) {
            if (why.is_string()) {
                recorder.refused[name] = why.get<std::string>();
            }
        }
    }
    doc->recompute(objs);
    const double seconds = now() - began;
    if (auto why = recorder.why(); !why.empty()) {
        return unfit(why);
    }
    // What the other process makes itself is not sent. The entry each
    // inner one names as its outer is renumbered for the entries left; one
    // whose outer is made there has none, that execute() recomputing it
    // there itself (sec 46)
    {
        constexpr std::size_t gone = std::numeric_limits<std::size_t>::max();
        std::vector<std::size_t> place(recorder.entries.size(), gone);
        std::size_t kept = 0;
        for (std::size_t i = 0; i < recorder.entries.size(); ++i) {
            if (!recorder.entries[i].here) {
                place[i] = kept++;
            }
        }
        for (auto& e : recorder.entries) {
            if (e.outer) {
                const std::size_t to = place[e.outer - 1];
                e.outer = to == gone ? 0 : to + 1;
            }
        }
    }
    recorder.entries.erase(std::remove_if(recorder.entries.begin(), recorder.entries.end(),
                                          [](const Recorder::Entry& e) { return e.here; }),
                           recorder.entries.end());
    // The numbers objects gave things here, each from what the other
    // process had given: taken there as they are if it gave none since
    // (sec 36). Only those of an object whose values go back -- one made
    // there gives its own
    json numbers = json::array();
    for (auto& [key, count] : minting.counts) {
        const DocumentObject* obj = key.first;
        const bool sent = std::any_of(recorder.entries.begin(), recorder.entries.end(),
                                      [obj](const Recorder::Entry& e) { return e.obj == obj; });
        if (!sent || !obj->isAttachedToDocument()) {
            return unfit("the recompute gave things new numbers in "
                         + std::string(obj->getFullName()) + ", which is not sent");
        }
        numbers.push_back(json::array({obj->getNameInDocument(), key.second, count.from, count.to}));
    }
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
            putSize(s, entry.outer);
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
                         {"byId", byId},
                         {"numbers", numbers}}
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
    /// In a recompute of the document: an object recomputed on its own
    /// (Document::recomputeFeature) has no start to time it from
    bool recomputing = false;
    /// From the log's recompute records, for an object not made since
    /// (docs/WorkerProcess.md sec 30)
    std::unordered_map<long, double> recorded;
    bool read = false;
};

/// The most each object took in the recompute records of the rows that
/// lead to the document's head, the newest 2000: what an open knows of
/// them before any recompute here. A result put in from a worker records
/// its putting in, not its making; the largest of an object's records is
/// the one that made it
void readRecorded(const Document& doc, Times& slot)
{
    slot.read = true;
    TransactionLog* log = doc.getTransactionLog();
    if (!log || log->head() <= 0) {
        return;
    }
    try {
        const int64_t head = log->head();
        for (const auto& t : log->store().chain(head, std::max<int64_t>(0, head - 2000))) {
            if (t.kind != "recompute" || t.script.empty()) {
                continue;
            }
            json record = json::parse(t.script, nullptr, false);
            if (!record.is_object()) {
                continue;
            }
            for (const auto& o : record.value("objects", json::array())) {
                auto s = o.find("s");
                if (!o.is_object() || s == o.end() || !s->is_number()) {
                    continue;
                }
                const long id = o.value("id", 0L);
                double& most = slot.recorded[id];
                most = std::max(most, s->get<double>());
            }
        }
    }
    catch (Base::Exception& e) {
        FC_LOG(doc.getName() << ": the recompute records not read: " << e.what());
    }
}

std::map<const Document*, Times>& times()
{
    static std::map<const Document*, Times> map;
    return map;
}

bool refuseCrashed(const DocumentObject& obj, std::string& why);

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
        /// What its execute() recomputed itself, in order: a TechDraw view
        /// its dimensions. Not reached by this process's recompute when the
        /// outer one is put in rather than executed (docs/WorkerProcess.md
        /// sec 46)
        std::vector<std::string> inner;
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
        // In the file's order, which an entry's outer one is told by
        std::vector<std::pair<std::string, Entry>> read;
        for (std::uint64_t i = 0; i < n; ++i) {
            Entry entry;
            std::uint64_t ok = 0;
            std::uint64_t k = 0;
            std::uint64_t outer = 0;
            if (!getBytes(in, name, most) || !getSize(in, ok) || !getBytes(in, entry.error, most)
                || !getDouble(in, entry.seconds) || !getSize(in, outer) || outer > i
                || !getSize(in, k) || k > most) {
                why = "its objects do not read";
                return false;
            }
            if (outer) {
                read[outer - 1].second.inner.push_back(name);
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
            read.emplace_back(name, std::move(entry));
        }
        for (auto& [object, entry] : read) {
            entries[object].push_back(std::move(entry));
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

    /// The names `other` made that this one did not: what another part
    /// adds (docs/WorkerProcess.md secs 42, 44). What both made -- what
    /// both parts read -- is this one's
    std::set<std::string> addsOf(const Supplier& other) const
    {
        std::set<std::string> names;
        for (auto& [name, list] : other.entries) {
            auto it = entries.find(name);
            if (!list.empty() && (it == entries.end() || it->second.empty())) {
                names.insert(name);
            }
        }
        return names;
    }

    /// Take in another part's objects, `names` of addsOf(), the part
    /// `index` of the split: its strings were taken in under their ids
    /// (takeRows) before
    void merge(Supplier& other, const std::set<std::string>& names, std::size_t index)
    {
        for (auto& name : names) {
            auto& list = other.entries[name];
            count += static_cast<long>(list.size());
            entries[name] = std::move(list);
            part[name] = index;
        }
        for (auto& [key, a] : other.added) {
            added.emplace(key, a);
        }
        bytes += other.bytes;
        seconds = std::max(seconds, other.seconds);
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
        // What crashed a worker is not executed here, nor in a put-in: one
        // half of a split recompute is put in when the other died over it
        // (sec 34, 42)
        std::string crash;
        if (refuseCrashed(obj, crash)) {
            made.ok = false;
            made.error = crash;
            return true;
        }
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
        // What its execute() recomputed there, recomputed here in turn as
        // its execute() would have: each from its own entry, or executed
        // here where it must be. A TechDraw view's dimensions, otherwise
        // never reached -- they are not touched -- and left measuring the
        // view as it was (docs/WorkerProcess.md sec 46)
        for (auto& name : entry.inner) {
            auto inner = doc.getObject(name.c_str());
            if (inner && inner->isAttachedToDocument()) {
                // as the view asks it: an object none of whose inputs is
                // touched is otherwise skipped
                inner->enforceRecompute();
                inner->recomputeFeature();
                ++innerMade;
            }
        }
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
    long innerMade = 0;  ///< recomputed after their outer one (sec 46)
    long executedHere = 0;
    std::vector<std::string> hereNames;
    long touchSet = 0;
    /// Made from the worker's values, by name: what the worker holds as
    /// they were put in (docs/WorkerProcess.md sec 29)
    std::set<std::string> fromWorker;
    long madeByType = 0;  ///< executed here by their type (madeHere())
    /// Of a split recompute, the part each object's values came from;
    /// one not here is the first's
    std::map<std::string, std::size_t> part;

    std::size_t partOf(const std::string& name) const
    {
        auto it = part.find(name);
        return it == part.end() ? 0 : it->second;
    }
};

struct State
{
    explicit State(Document& doc)
        : doc(doc)
        , tag(std::string("recompute:") + doc.getName())
    {}

    Document& doc;
    std::string tag;
    std::vector<std::string> names;  ///< what it recomputes
    std::string tables;
    bool applying = false;
    bool timer = false;
    /// Taken by a recompute run here: the hook for as long as it runs
    std::unique_ptr<Supplier> borrowed;
    /// Objects whose execute() crashed a worker on what they read now, each
    /// with the error it is left in: not executed here (sec 34)
    std::map<std::string, std::string> crashed;
    /// Objects that crashed a worker once: a recompute reaching one is
    /// handed to a worker whatever it is expected to take
    std::set<std::string> risky;
    WorkerRecompute::Info info;
    std::vector<fastsignals::scoped_connection> conns;
    /// Each worker's part of the recompute: one, or more where it is split
    /// (secs 42, 44). Its job; its result, come and not put in yet; the job
    /// that result is of; and the numbers its objects gave things there,
    /// [name, kind, from, to] (sec 36). All put in at once, when all are
    /// over
    struct Part
    {
        WorkerPool::JobId job = 0;
        std::string file;
        WorkerPool::JobId fileJob = 0;
        json numbers;
        /// Its result's times (WorkerPool::Result::queued, ...)
        double queued = 0;
        double setup = 0;
        double handler = 0;
        double returned = 0;
    };
    std::vector<Part> parts;
    /// When the one with the workers was started, how long start() took,
    /// and when its last part's result was read here (sec 53)
    double startedAt = 0;
    double startSeconds = 0;
    double resultAt = 0;
};

/// A job with the workers, of any part
bool making(const State& s)
{
    for (auto& p : s.parts) {
        if (p.job) {
            return true;
        }
    }
    return false;
}

/// A result come and not put in yet, of any part
bool hasResult(const State& s)
{
    for (auto& p : s.parts) {
        if (!p.file.empty()) {
            return true;
        }
    }
    return false;
}

/// No job with the workers any more, of any part
void dropJobs(State& s)
{
    for (auto& p : s.parts) {
        p.job = 0;
    }
}

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
    s.info.running = making(s) || hasResult(s);
    s.info.crashed.clear();
    for (auto& [name, why] : s.crashed) {
        s.info.crashed.push_back(name);
    }
    WorkerRecompute::signalChanged(s.doc);
}

void dropFile(State& s)
{
    for (auto& p : s.parts) {
        if (!p.file.empty()) {
            Base::FileInfo(p.file).deleteFile();
            p.file.clear();
        }
    }
}

/** The numbers a worker's objects gave things, taken as this process's:
 * false, `why` said, when one of those objects gave numbers here since the
 * worker was told what it had given -- the two may be the same numbers for
 * different things (docs/WorkerProcess.md sec 36).
 */
bool takeNumbers(Document& doc, const json& numbers, std::string& why)
{
    if (!numbers.is_array()) {
        return true;
    }
    std::vector<std::tuple<DocumentObject*, int, long>> take;
    for (auto& n : numbers) {
        if (!n.is_array() || n.size() != 4) {
            why = "its numbers do not read";
            return false;
        }
        auto obj = doc.getObject(n[0].get<std::string>().c_str());
        if (!obj) {
            why = n[0].get<std::string>() + " is gone";
            return false;
        }
        const int kind = n[1].get<int>();
        long here = 0;
        for (auto& [k, last] : doc.givenNumbers(*obj)) {
            if (k == kind) {
                here = last;
            }
        }
        if (here > n[2].get<long>()) {
            why = std::string(obj->getFullName()) + " gave numbers here since";
            return false;
        }
        take.emplace_back(obj, kind, n[3].get<long>());
    }
    for (auto& [obj, kind, last] : take) {
        doc.noteGivenNumber(*obj, kind, last);
    }
    return true;
}

/// Fill a supplier from the result that came; false, the reason noted and
/// the result dropped, when it does not read
bool supplierFor(State& s, std::unique_ptr<Supplier>& supplier)
{
    supplier = std::make_unique<Supplier>(s.doc);
    std::string why;
    // Each part's result, taken out: the base is the first part's, or the
    // first that made one; the others are then added (docs/WorkerProcess.md
    // secs 42, 44)
    std::vector<std::pair<std::string, json>> results;
    for (auto& p : s.parts) {
        results.emplace_back(std::move(p.file), std::move(p.numbers));
        p.file.clear();
        p.numbers = json();
    }
    std::size_t base = 0;
    while (base < results.size() && results[base].first.empty()) {
        ++base;
    }
    auto dropRest = [&](std::size_t from) {
        for (std::size_t k = from; k < results.size(); ++k) {
            if (!results[k].first.empty()) {
                Base::FileInfo(results[k].first).deleteFile();
            }
        }
    };
    bool ok = base < results.size();
    if (!ok) {
        why = "no part made a result";
    }
    else {
        ok = supplier->read(results[base].first, s.tables, why)
            && takeNumbers(s.doc, results[base].second, why) && supplier->takeStrings(why);
        Base::FileInfo(results[base].first).deleteFile();
    }
    if (!ok) {
        dropRest(base + 1);
        supplier.reset();
        s.info.reason = "a worker's recompute not put in: " + why;
        ++s.info.failed;
        FC_WARN(s.doc.getName() << ": " << s.info.reason);
        return false;
    }
    if (base) {
        for (auto& [name, list] : supplier->entries) {
            supplier->part[name] = base;
        }
    }
    for (std::size_t k = base + 1; k < results.size(); ++k) {
        const std::string& other = results[k].first;
        if (other.empty()) {
            continue;
        }
        // What this part made that those before it did not: its numbers for
        // those only -- what both made counted the same numbers from the same
        // start -- and its strings under their ids. Not taken, its objects
        // are executed here, as any the worker did not make
        Supplier half(s.doc);
        std::string why2;
        bool taken = half.read(other, s.tables, why2);
        std::set<std::string> adds;
        if (taken) {
            adds = supplier->addsOf(half);
            json mine = json::array();
            if (results[k].second.is_array()) {
                for (auto& n : results[k].second) {
                    if (n.is_array() && !n.empty() && n[0].is_string()
                        && adds.count(n[0].get<std::string>())) {
                        mine.push_back(n);
                    }
                }
            }
            // Its strings by id only: one string table read by content is
            // the base's (Supplier::takeStrings)
            auto hasher = s.doc.getStringHasher();
            if (!half.byId || !hasher) {
                why2 = "it sent no strings by id";
                taken = false;
            }
            else {
                taken = takeNumbers(s.doc, mine, why2) && hasher->takeRows(half.rows, &why2);
            }
        }
        Base::FileInfo(other).deleteFile();
        if (taken) {
            supplier->merge(half, adds, k);
        }
        else {
            FC_WARN(s.doc.getName() << ": part " << k + 1 << " of a split recompute not put in: "
                                    << why2);
        }
    }
    return true;
}

void noteSupplied(State& s, const Supplier& supplier)
{
    s.info.suppliedSecond = 0;
    for (auto& name : supplier.fromWorker) {
        s.info.suppliedSecond += supplier.partOf(name) ? 1 : 0;
    }
    s.info.objects = static_cast<long>(s.names.size());
    s.info.supplied = supplier.supplied;
    s.info.executedHere = supplier.executedHere;
    s.info.madeHere = supplier.madeByType;
    s.info.executedHereNames = supplier.hereNames;
    s.info.unused = supplier.unused();
    s.info.inner = supplier.innerMade;
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
    // Every part of a split one over first (secs 42, 44)
    if (!hasResult(s) || s.applying || making(s)) {
        return;
    }
    if (!safeNow(s.doc)) {
        schedule(s);
        return;
    }
    const double reading = now();
    // The times of the part that came last, the one waited for
    const State::Part* last = nullptr;
    for (auto& p : s.parts) {
        if (!p.file.empty() && (!last || p.queued + p.returned > last->queued + last->returned)) {
            last = &p;
        }
    }
    WorkerRecompute::Info::Times times;
    times.start = s.startSeconds;
    if (last) {
        times.queued = last->queued;
        times.setup = last->setup;
        times.job = last->handler;
        times.returned = last->returned;
    }
    times.held = s.resultAt > 0 ? reading - s.resultAt : 0;
    std::unique_ptr<Supplier> supplier;
    if (!supplierFor(s, supplier)) {
        changed(s);
        return;
    }
    times.read = now() - reading;
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
    times.apply = s.info.applySeconds;
    times.total = s.startedAt > 0 ? now() - s.startedAt : 0;
    s.info.times = times;
    // The rows it was put in as, and what in them is the worker's: told to
    // the worker that made it, which keeps those values as it holds them
    // when it next moves across the rows (docs/WorkerProcess.md sec 29)
    if (log && log->lastSeq() > before && !supplier->fromWorker.empty()) {
        if (auto pool = WorkerPool::of(s.doc, false)) {
            // Each part's worker told what is its own
            std::vector<std::vector<std::string>> own(s.parts.size());
            for (auto& name : supplier->fromWorker) {
                const std::size_t k = supplier->partOf(name);
                if (k < own.size()) {
                    own[k].push_back(name);
                }
            }
            for (std::size_t k = 0; k < own.size(); ++k) {
                if (!own[k].empty() && s.parts[k].fileJob) {
                    pool->putIn(s.parts[k].fileJob, before + 1, log->lastSeq(), own[k]);
                }
            }
        }
    }
    for (auto& p : s.parts) {
        p.fileJob = 0;
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

/** A job whose workers died over it, each in the same object's execute():
 * that object is not executed here on what it reads now, which would take
 * this process with it (docs/WorkerProcess.md sec 34). It is left in an error
 * that says so; what depends on it stays touched. A death with nothing said,
 * or in different objects, is put down to nothing: a worker may be ended
 * for want of memory.
 */
void quarantine(State& s, const WorkerPool::Result& r)
{
    if (!r.died || r.diedAt.empty() || r.diedAt.back().empty()) {
        return;
    }
    const std::string& name = r.diedAt.back();
    for (auto& where : r.diedAt) {
        if (where != name) {
            return;
        }
    }
    DocumentObject* obj = s.doc.getObject(name.c_str());
    if (!obj) {
        return;
    }
    std::string why = "its recompute crashed a worker process";
    if (r.diedAt.size() == 2) {
        why += " twice";
    }
    else if (r.diedAt.size() > 2) {
        why += " " + std::to_string(r.diedAt.size()) + " times";
    }
    std::string how = r.how;
    if (how.rfind("crashed (", 0) == 0) {
        how = how.substr(9, how.size() - 10);
    }
    if (!how.empty()) {
        why += " (" + how + ")";
    }
    why += ". It is not recomputed in this process, which it would crash too: change it, or what "
           "it uses, to have it tried again";
    if (!r.crashLog.empty()) {
        why += ". What was on the worker's stack is in " + r.crashLog;
    }
    s.crashed[name] = why;
    s.risky.insert(name);
    s.doc.setErrorDescription(obj, why.c_str());
    FC_ERR(obj->getFullName() << ": " << why);
}

/// Whether `obj` is not to be executed here (RecomputeHook::setRefusal)
bool refuseCrashed(const DocumentObject& obj, std::string& why)
{
    auto doc = obj.getDocument();
    State* s = doc ? stateOf(*doc) : nullptr;
    if (!s || s->crashed.empty() || !obj.getNameInDocument()) {
        return false;
    }
    auto it = s->crashed.find(obj.getNameInDocument());
    if (it == s->crashed.end()) {
        return false;
    }
    why = it->second;
    return true;
}

void done(const std::string& name, WorkerPool::JobId id, WorkerPool::Result& r)
{
    State* s = stateByName(name);
    if (!s || !id) {
        return;
    }
    // Which part (secs 42, 44): each result is kept apart, and all are put
    // in together when all are over
    State::Part* part = nullptr;
    for (auto& p : s->parts) {
        if (p.job == id) {
            part = &p;
        }
    }
    if (!part) {
        return;
    }
    part->job = 0;
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
            {
                json output = json::parse(r.output, nullptr, false);
                json numbers = output.is_object() ? output.value("numbers", json::array())
                                                  : json::array();
                part->file = r.file;
                part->fileJob = id;
                part->numbers = std::move(numbers);
                part->queued = r.queued;
                part->setup = r.setup;
                part->handler = r.seconds;
                part->returned = r.returned;
                s->resultAt = now();
            }
            r.file.clear();  // ours now, in the pool's directory until put in
            break;
        case WorkerPool::Result::Status::Failed:
            ++s->info.failed;
            s->info.reason = r.error;
            FC_LOG(name << ": a worker did not recompute: " << r.error);
            quarantine(*s, r);
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
    if (!making(*s) && hasResult(*s)) {
        schedule(*s);
    }
    changed(*s);
}

std::set<DocumentObject*> reachOf(const Document& doc, const std::vector<DocumentObject*>& objs);

/** A recompute asked here that reaches an object which crashed a worker
 * before: made by a worker first, waited for (docs/WorkerProcess.md sec 37).
 * The GUI hands such a recompute to a worker itself; this is for the rest --
 * a script's doc.recompute(), the console -- and for the GUI's own calls
 * that recompute at once.
 */
bool handOverRisky(Document& doc, const std::vector<DocumentObject*>& objs)
{
    State* s = stateOf(doc);
    if (!s || s->risky.empty() || s->applying || making(*s) || hasResult(*s)) {
        return false;
    }
    if (!GetApplication()
             .GetParameterGroupByPath("User parameter:BaseApp/Preferences/Workers")
             ->GetBool("HandOverRisky", true)) {
        return false;
    }
    // One tried again since its crash: one still refused is not executed,
    // here or there, and needs no worker
    bool reaches = false;
    for (auto obj : reachOf(doc, objs)) {
        const char* name = obj->getNameInDocument();
        if (name && s->risky.count(name) && !s->crashed.count(name)) {
            reaches = true;
            break;
        }
    }
    if (!reaches) {
        return false;
    }
    std::string why;
    if (!WorkerRecompute::start(doc, objs, &why)) {
        FC_LOG(doc.getName() << ": a recompute reaching what crashed a worker made here: " << why);
        return false;
    }
    FC_LOG(doc.getName() << ": a recompute reaching what crashed a worker, made by one");
    ++s->info.handedOver;
    const long failed = s->info.failed;
    const long applied = s->info.applied;
    // Put in as it comes (wait() applies it), or the worker died again:
    // what crashed is then refused here (quarantine)
    WorkerRecompute::wait(doc);
    return stateOf(doc) == s && (s->info.applied > applied || s->info.failed > failed);
}

State& stateFor(Document& doc)
{
    auto& slot = states()[&doc];
    if (slot) {
        return *slot;
    }
    slot = std::make_unique<State>(doc);
    State& s = *slot;
    RecomputeHook::setRefusal(&refuseCrashed);
    RecomputeHook::setHandover(&handOverRisky);
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
        for (auto& p : s.parts) {
            WorkerPool::JobId* job = &p.job;
            if (!*job) {
                continue;
            }
            if (auto pool = WorkerPool::of(s.doc, false)) {
                const WorkerPool::JobId id = *job;
                if (pool->claim(id) != WorkerPool::Claim::Done && *job == id) {
                    *job = 0;
                    ++s.info.cancelled;
                    s.info.reason = "recomputed here";
                }
            }
            else {
                *job = 0;
            }
        }
        if (hasResult(s) && supplierFor(s, s.borrowed)) {
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
    // What a crashed object reads, or it, changed by hand (an edit, an undo):
    // it is tried again, in a worker first (sec 34)
    s.conns.emplace_back(doc.signalChangedObject.connect(
        [&s](const DocumentObject& obj, const Property& prop) {
            if (s.crashed.empty() || s.applying || s.doc.testStatus(Document::Recomputing)
                || (WriteScope::causes() & WriteCause::Animation)
                || !(s.doc.writeEffects(prop) & WriteEffect::Dirty)) {
                return;
            }
            const std::set<DocumentObject*> reached = writeReaches(obj, prop);
            bool lifted = false;
            for (auto it = s.crashed.begin(); it != s.crashed.end();) {
                auto crashed = s.doc.getObject(it->first.c_str());
                if (crashed && reached.count(crashed)) {
                    FC_LOG(crashed->getFullName() << ": " << obj.getFullName() << "."
                                                  << prop.getName()
                                                  << " changed, to be tried again");
                    it = s.crashed.erase(it);
                    // Its error was the crash's: what it says next is its
                    // recompute's. A task panel's OK refuses an object left
                    // in error
                    crashed->purgeError();
                    lifted = true;
                }
                else {
                    ++it;
                }
            }
            if (lifted) {
                changed(s);
            }
        }));
    s.conns.emplace_back(doc.signalChangedObject.connect(
        [&s, reaches](const DocumentObject& obj, const Property& prop) {
            if (!hasResult(s) || s.applying || s.doc.testStatus(Document::Recomputing)
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
    s.conns.emplace_back(doc.signalDeletedObject.connect([&s](const DocumentObject& obj) {
        if (obj.getNameInDocument()) {
            s.risky.erase(obj.getNameInDocument());
            if (s.crashed.erase(obj.getNameInDocument())) {
                changed(s);
            }
        }
    }));
    auto object = [&s](const DocumentObject&) {
        if (hasResult(s) && !s.applying) {
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

/** Two halves of the recompute of `all` for two workers, and the seconds
 * they are expected to save against one, by what each object took last
 * (expectedSeconds). The longest chain of `all` by those times is cut at
 * one of its objects: `first` is that object, the chain after it, all that
 * depends on them, and what they read in `all`; `second` the rest and what
 * it reads -- what both read, the chain before the cut among it, is in
 * both, made twice. The cut that saves most. 0, the halves empty, where
 * none leaves a second half (docs/WorkerProcess.md sec 42).
 */
double splitOf(const Document& doc, const std::set<DocumentObject*>& all,
               std::set<DocumentObject*>& first, std::set<DocumentObject*>& second)
{
    Times& slot = times()[&doc];
    if (!slot.read) {
        readRecorded(doc, slot);
    }
    std::map<DocumentObject*, double> cost;
    double total = 0;
    for (auto obj : all) {
        double c = 0;
        auto t = slot.seconds.find(obj->getID());
        if (t != slot.seconds.end()) {
            c = t->second;
        }
        else if (auto r = slot.recorded.find(obj->getID()); r != slot.recorded.end()) {
            c = r->second;
        }
        cost[obj] = c;
        total += c;
    }
    // What each reads in `all`, and what reads it
    std::map<DocumentObject*, std::vector<DocumentObject*>> reads;
    std::map<DocumentObject*, std::vector<DocumentObject*>> users;
    for (auto obj : all) {
        for (auto dep : obj->getOutList()) {
            if (dep != obj && all.count(dep)) {
                reads[obj].push_back(dep);
                users[dep].push_back(obj);
            }
        }
    }
    // The longest chain ending at each object, by cost. A cycle would have
    // stopped the recompute; one is cut where it closes
    std::map<DocumentObject*, double> upto;
    std::map<DocumentObject*, DocumentObject*> via;
    std::set<DocumentObject*> visiting;
    std::function<double(DocumentObject*)> longest = [&](DocumentObject* obj) -> double {
        auto it = upto.find(obj);
        if (it != upto.end()) {
            return it->second;
        }
        if (!visiting.insert(obj).second) {
            return 0;
        }
        double best = 0;
        DocumentObject* from = nullptr;
        for (auto dep : reads[obj]) {
            const double d = longest(dep);
            if (d > best) {
                best = d;
                from = dep;
            }
        }
        visiting.erase(obj);
        via[obj] = from;
        return upto[obj] = best + cost[obj];
    };
    DocumentObject* end = nullptr;
    double most = 0;
    for (auto obj : all) {
        const double d = longest(obj);
        if (d > most) {
            most = d;
            end = obj;
        }
    }
    if (!end) {
        return 0;
    }
    std::vector<DocumentObject*> chain;  // from the end back to its start
    for (auto obj = end; obj; obj = via[obj]) {
        chain.push_back(obj);
    }
    auto sum = [&](const std::set<DocumentObject*>& part) {
        double c = 0;
        for (auto obj : part) {
            c += cost[obj];
        }
        return c;
    };
    // `part` and what it reads in `all`
    auto withReads = [&](std::set<DocumentObject*> part) {
        std::vector<DocumentObject*> stack(part.begin(), part.end());
        while (!stack.empty()) {
            auto obj = stack.back();
            stack.pop_back();
            for (auto dep : reads[obj]) {
                if (part.insert(dep).second) {
                    stack.push_back(dep);
                }
            }
        }
        return part;
    };
    double best = 0;
    std::set<DocumentObject*> after;  // the chain from the cut on, and what uses it
    for (auto cut : chain) {
        // Cut one earlier: `cut` and what uses it join what comes after
        std::vector<DocumentObject*> stack {cut};
        after.insert(cut);
        while (!stack.empty()) {
            auto obj = stack.back();
            stack.pop_back();
            for (auto user : users[obj]) {
                if (after.insert(user).second) {
                    stack.push_back(user);
                }
            }
        }
        std::set<DocumentObject*> rest;
        for (auto obj : all) {
            if (!after.count(obj)) {
                rest.insert(obj);
            }
        }
        if (rest.empty()) {
            break;  // earlier cuts take more still
        }
        std::set<DocumentObject*> a = withReads(after);
        std::set<DocumentObject*> b = withReads(rest);
        const double saves = total - std::max(sum(a), sum(b));
        if (saves > best) {
            best = saves;
            first = std::move(a);
            second = std::move(b);
        }
    }
    return best;
}

/** The parts of the recompute of `all` for at most `limit` workers, and the
 * seconds they are expected to save against one (docs/WorkerProcess.md sec
 * 44). The costliest part is what the result waits for: it is split in two
 * (splitOf) while that brings the costliest of them all down by `least`
 * seconds at least. One part, `all`, where nothing does.
 */
std::vector<std::set<DocumentObject*>> splitParts(const Document& doc,
                                                  const std::set<DocumentObject*>& all,
                                                  long limit, double least, double& saving)
{
    saving = 0;
    std::vector<std::set<DocumentObject*>> parts {all};
    if (limit < 2 || !(least > 0)) {
        return parts;
    }
    Times& slot = times()[&doc];
    if (!slot.read) {
        readRecorded(doc, slot);
    }
    auto costOf = [&](const std::set<DocumentObject*>& part) {
        double c = 0;
        for (auto obj : part) {
            if (auto t = slot.seconds.find(obj->getID()); t != slot.seconds.end()) {
                c += t->second;
            }
            else if (auto r = slot.recorded.find(obj->getID()); r != slot.recorded.end()) {
                c += r->second;
            }
        }
        return c;
    };
    std::vector<double> costs {costOf(all)};
    const double whole = costs.front();
    while (static_cast<long>(parts.size()) < limit) {
        const std::size_t worst =
            static_cast<std::size_t>(std::max_element(costs.begin(), costs.end()) - costs.begin());
        std::set<DocumentObject*> a;
        std::set<DocumentObject*> b;
        if (!(splitOf(doc, parts[worst], a, b) > 0) || a.empty() || b.empty()) {
            break;
        }
        double others = 0;
        for (std::size_t k = 0; k < costs.size(); ++k) {
            if (k != worst) {
                others = std::max(others, costs[k]);
            }
        }
        const double costA = costOf(a);
        const double costB = costOf(b);
        if (costs[worst] - std::max({others, costA, costB}) < least) {
            break;
        }
        parts[worst] = std::move(a);
        costs[worst] = costA;
        parts.push_back(std::move(b));
        costs.push_back(costB);
    }
    saving = whole - *std::max_element(costs.begin(), costs.end());
    return parts;
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
            Times& slot = times()[&doc];
            slot.last = std::chrono::steady_clock::now();
            slot.recomputing = true;
        });
    static fastsignals::scoped_connection after =
        GetApplication().signalRecomputed.connect([](const Document& doc) {
            times()[&doc].recomputing = false;
        });
    static fastsignals::scoped_connection each = GetApplication().signalRecomputedObject.connect(
        [](const Document& doc, const DocumentObject& obj) {
            const auto t = std::chrono::steady_clock::now();
            Times& slot = times()[&doc];
            if (!slot.recomputing) {
                return;
            }
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

bool WorkerRecompute::risky(const Document& doc, const std::vector<DocumentObject*>& objs)
{
    State* s = stateOf(doc);
    if (!s || s->risky.empty()) {
        return false;
    }
    for (auto obj : reachOf(doc, objs)) {
        if (obj->getNameInDocument() && s->risky.count(obj->getNameInDocument())) {
            return true;
        }
    }
    return false;
}

bool WorkerRecompute::crashed(const DocumentObject& obj)
{
    std::string why;
    return refuseCrashed(obj, why);
}

void WorkerRecompute::noteSeconds(const DocumentObject& obj, double seconds)
{
    auto doc = obj.getDocument();
    if (doc && !RecomputeHook::of(*doc)) {
        times()[doc].seconds[obj.getID()] = seconds;
    }
}

double WorkerRecompute::expectedSeconds(const Document& doc,
                                        const std::vector<DocumentObject*>& objs, long* unknown)
{
    double total = 0;
    long missing = 0;
    Times& slot = times()[&doc];
    if (!slot.read) {
        readRecorded(doc, slot);
    }
    for (auto obj : reachOf(doc, objs)) {
        auto t = slot.seconds.find(obj->getID());
        if (t != slot.seconds.end()) {
            total += t->second;
            continue;
        }
        auto r = slot.recorded.find(obj->getID());
        if (r != slot.recorded.end()) {
            total += r->second;
        }
        else {
            ++missing;
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
    const double began = now();
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
    if (making(s) || hasResult(s)) {
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
    const bool meshes = GetApplication()
                            .GetParameterGroupByPath("User parameter:BaseApp/Preferences/Workers")
                            ->GetBool("RecomputeMeshes", true);
    // What this process does not execute is not executed there either: left
    // in the same error, not tried again on what crashed it (sec 34)
    json refused = json::object();
    for (auto& [crashed, why] : s.crashed) {
        refused[crashed] = why;
    }
    const std::string name = doc.getName();
    // One job of `part`: what it recomputes, what of it is touched here,
    // how the view meshes it, the numbers each gave so far (sec 36)
    auto jobOf = [&](const std::set<DocumentObject*>& part, std::vector<std::string>& names,
                     std::shared_ptr<WorkerPool::JobId> slot) {
        std::vector<std::string> touchedHere;
        json display = json::object();
        json numbers = json::object();
        for (auto obj : part) {
            names.emplace_back(obj->getNameInDocument());
            auto given = doc.givenNumbers(*obj);
            if (!given.empty()) {
                json counts = json::array();
                for (auto& [kind, last] : given) {
                    counts.push_back(json::array({kind, last}));
                }
                numbers[obj->getNameInDocument()] = std::move(counts);
            }
            if (obj->mustRecompute()) {
                touchedHere.emplace_back(obj->getNameInDocument());
            }
            // How the view here meshes it, for the worker to mesh it so
            if (Animation::displayHint) {
                std::string hint = Animation::displayHint(*obj);
                if (!hint.empty()) {
                    display[obj->getNameInDocument()] = hint;
                }
            }
        }
        WorkerPool::Job job;
        job.kind = RecomputeKind;
        job.tag = s.tag;
        job.reads = names;
        job.inputs = json {{"objects", names},
                           {"touched", touchedHere},
                           {"display", meshes ? display : json::object()},
                           {"tables", s.tables},
                           {"refused", refused},
                           {"numbers", numbers}}
                         .dump();
        job.done = [name, slot](WorkerPool::Result& r) { done(name, *slot, r); };
        return job;
    };
    // Parts where more workers save enough (secs 42, 44): one, the longest
    // chain of what it reaches and all after it; the others, the rest
    const double least = GetApplication()
                             .GetParameterGroupByPath("User parameter:BaseApp/Preferences/Workers")
                             ->GetFloat("SplitSeconds", 5.0);
    double saving = 0;
    const std::vector<std::set<DocumentObject*>> parts =
        splitParts(doc, all, pool->info().limit, least, saving);
    std::vector<std::string> names;
    std::vector<State::Part> made;
    if (parts.size() > 1) {
        std::vector<long> sizes;
        for (auto& part : parts) {
            std::vector<std::string> partNames;
            auto slot = std::make_shared<WorkerPool::JobId>(0);
            const WorkerPool::JobId id = pool->submit(jobOf(part, partNames, slot));
            if (!id) {
                if (!made.empty()) {
                    pool->cancel([&](const std::string& t) { return t == s.tag; });
                }
                return refuse(pool->reason().empty() ? std::string("not taken")
                                                     : pool->reason());
            }
            *slot = id;
            State::Part p;
            p.job = id;
            made.push_back(std::move(p));
            sizes.push_back(static_cast<long>(partNames.size()));
        }
        for (auto obj : all) {
            names.emplace_back(obj->getNameInDocument());
        }
        ++s.info.split;
        s.info.splitSaving = saving;
        s.info.splitObjects = sizes;
        std::ostringstream ss;
        for (std::size_t k = 0; k < sizes.size(); ++k) {
            ss << (k ? (k + 1 == sizes.size() ? " and " : ", ") : "") << sizes[k];
        }
        FC_LOG(doc.getName() << ": a recompute split over " << sizes.size() << " workers, "
                             << ss.str() << " objects, expected to save " << saving << " s");
    }
    else {
        auto slot = std::make_shared<WorkerPool::JobId>(0);
        const WorkerPool::JobId id = pool->submit(jobOf(all, names, slot));
        if (!id) {
            return refuse(pool->reason().empty() ? std::string("not taken") : pool->reason());
        }
        *slot = id;
        State::Part p;
        p.job = id;
        made.push_back(std::move(p));
    }
    s.parts = std::move(made);
    s.names = std::move(names);
    s.info.reason.clear();
    s.startedAt = began;
    s.startSeconds = now() - began;
    s.resultAt = 0;
    ++s.info.started;
    changed(s);
    return true;
}

bool WorkerRecompute::running(const Document& doc)
{
    auto s = stateOf(doc);
    return s && (making(*s) || hasResult(*s));
}

bool WorkerRecompute::wait(Document& doc, double seconds)
{
    const double began = now();
    for (;;) {
        State* s = stateOf(doc);
        if (!s) {
            return true;
        }
        if (hasResult(*s) && !making(*s) && safeNow(doc)) {
            apply(*s);
            continue;
        }
        if (!making(*s)) {
            return !hasResult(*s);
        }
        WorkerPool* pool = WorkerPool::of(doc, false);
        if (!pool) {
            dropJobs(*s);
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
    if (!s || (!making(*s) && !hasResult(*s))) {
        return;
    }
    if (making(*s)) {
        if (auto pool = WorkerPool::of(doc, false)) {
            const std::string tag = s->tag;
            pool->cancel([&tag](const std::string& t) { return t == tag; });
        }
        dropJobs(*s);
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
