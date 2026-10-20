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

#include "PreCompiled.h"

#ifndef _PreComp_
# include <cstdlib>
# include <functional>
# include <map>
# include <set>
# include <tuple>
#endif

#include <QDir>
#include <QFileInfo>

#include <nlohmann/json.hpp>

#include <Base/Console.h>
#include <Base/Exception.h>
#include <Base/FileInfo.h>
#include <Base/Stream.h>
#include <Base/Uuid.h>

#include "Application.h"
#include "AutoTransaction.h"
#include "Document.h"
#include "DocumentObject.h"
#include "ExportSet.h"
#include "FileBlobManager.h"
#include "FileHistory.h"
#include "ImportSet.h"
#include "LibraryPaths.h"
#include "PropertyLinks.h"
#include "TransactionLog.h"
#include "TransactionStore.h"

FC_LOG_LEVEL_INIT("App", true, true)

using namespace App;

namespace
{

QString qs(const std::string& s)
{
    return QString::fromUtf8(s.c_str());
}

std::string ss(const QString& s)
{
    return s.toUtf8().constData();
}

/// A full path, cleaned, the one key of a file here.
std::string keyOf(const std::string& path)
{
    const QFileInfo info(qs(path));
    const QString canonical = info.canonicalFilePath();
    return ss(canonical.isEmpty() ? QDir::cleanPath(info.absoluteFilePath()) : canonical);
}

/// An attribute's value as Persistence::encodeAttribute() wrote it, read.
std::string decodeAttribute(const std::string& s)
{
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out += s[i];
            continue;
        }
        const std::size_t end = s.find(';', i);
        if (end == std::string::npos) {
            out += s.substr(i);
            break;
        }
        const std::string name = s.substr(i + 1, end - i - 1);
        if (name == "lt")
            out += '<';
        else if (name == "gt")
            out += '>';
        else if (name == "amp")
            out += '&';
        else if (name == "quot")
            out += '"';
        else if (name == "apos")
            out += '\'';
        else if (!name.empty() && name[0] == '#')
            out += static_cast<char>(std::atoi(name.c_str() + 1));
        else
            out += s.substr(i, end - i + 1);
        i = end;
    }
    return out;
}

/// The attributes of every `<XLink .../>` of a Document.xml: every external
/// link is saved as one, alone or inside a list.
std::vector<std::map<std::string, std::string>> xlinksOf(const std::string& xml)
{
    std::vector<std::map<std::string, std::string>> out;
    const std::string open = "<XLink ";
    for (std::size_t at = xml.find(open); at != std::string::npos; at = xml.find(open, at + 1)) {
        const std::size_t end = xml.find('>', at);
        if (end == std::string::npos)
            break;
        std::map<std::string, std::string> attrs;
        std::size_t i = at + open.size();
        while (i < end) {
            const std::size_t eq = xml.find("=\"", i);
            if (eq == std::string::npos || eq >= end)
                break;
            std::size_t from = xml.find_last_of(" \t\n", eq);
            const std::string name = xml.substr(from + 1, eq - from - 1);
            const std::size_t quote = xml.find('"', eq + 2);
            if (quote == std::string::npos || quote > end)
                break;
            attrs[name] = decodeAttribute(xml.substr(eq + 2, quote - eq - 2));
            i = quote + 1;
        }
        out.push_back(std::move(attrs));
    }
    return out;
}

/// The document's `Uid` in a Document.xml; empty with none.
std::string uidOf(const std::string& xml)
{
    const std::size_t at = xml.find("<Property name=\"Uid\"");
    if (at == std::string::npos)
        return {};
    const std::size_t end = xml.find("</Property>", at);
    const std::string open = "<Uuid value=\"";
    const std::size_t value = xml.find(open, at);
    if (value == std::string::npos || value > end)
        return {};
    const std::size_t from = value + open.size();
    const std::size_t to = xml.find('"', from);
    return to == std::string::npos ? std::string() : xml.substr(from, to - from);
}

/// The file a saved link of the file `owner` names: through its library
/// when this machine has it, else its path, relative to the owner's
/// folder. Empty for a link to the owner's own file.
std::string targetOf(const std::map<std::string, std::string>& attrs, const std::string& owner)
{
    auto get = [&attrs](const char* name) {
        auto it = attrs.find(name);
        return it == attrs.end() ? std::string() : it->second;
    };
    const std::string lib = get("lib");
    const std::string libFile = get("libfile");
    if (!lib.empty() && !libFile.empty()) {
        const std::string full = LibraryPaths::resolveToken(LibraryPaths::makeToken(lib, libFile));
        if (!full.empty() && QFileInfo::exists(qs(full)))
            return keyOf(full);
    }
    const std::string file = get("file");
    if (file.empty())
        return {};
    if (QFileInfo(qs(file)).isAbsolute())
        return keyOf(file);
    return keyOf(ss(QFileInfo(qs(owner)).absoluteDir().absoluteFilePath(qs(file))));
}

/// One of their files, read.
struct Theirs
{
    std::string uid;
    std::vector<std::string> links;
};

/// Our documents `top`'s links reach, by their file: the document of the
/// file itself, not a version's or a branch's, which an import goes to.
std::map<std::string, Document*> oursOf(Document& top)
{
    // One read in part -- a plain file opened for links -- read again whole
    // first: it is what an import goes to, and a reload gives another
    // document.
    for (int pass = 0; pass < 4; ++pass) {
        std::vector<std::string> partial;
        std::vector<Document*> queue {&top};
        std::set<Document*> seen {&top};
        for (std::size_t i = 0; i < queue.size(); ++i) {
            Document* doc = queue[i];
            if (doc->testStatus(Document::PartialDoc) && !doc->testStatus(Document::VersionDoc))
                partial.push_back(doc->FileName.getStrValue());
            auto outList = PropertyXLink::getDocumentOutList(doc);
            for (auto linked : outList[doc]) {
                if (linked && seen.insert(linked).second)
                    queue.push_back(linked);
            }
        }
        if (partial.empty())
            break;
        for (const auto& file : partial)
            GetApplication().openDocument(file.c_str(), false);
    }
    std::map<std::string, Document*> ours;
    std::vector<Document*> queue {&top};
    std::set<Document*> seen {&top};
    for (std::size_t i = 0; i < queue.size(); ++i) {
        Document* doc = queue[i];
        const std::string file = doc->FileName.getStrValue();
        if (!doc->testStatus(Document::VersionDoc) && !file.empty())
            ours.emplace(keyOf(file), doc);
        // The map held: a range over an element of a temporary dangles.
        auto outList = PropertyXLink::getDocumentOutList(doc);
        for (auto linked : outList[doc]) {
            if (linked && seen.insert(linked).second)
                queue.push_back(linked);
        }
    }
    return ours;
}

/// The manifest beside their top, when our export wrote it: their path,
/// relative to the folder -> the entry.
std::map<std::string, nlohmann::json> manifestOf(const std::string& theirTop)
{
    std::map<std::string, nlohmann::json> out;
    const QString path =
        QFileInfo(qs(theirTop)).absoluteDir().absoluteFilePath(qs(exportSetManifestName()));
    if (!QFileInfo::exists(path))
        return out;
    try {
        Base::ifstream in(Base::FileInfo(ss(path)), std::ios::in);
        const auto j = nlohmann::json::parse(in);
        for (const auto& e : j.value("files", nlohmann::json::array())) {
            const std::string rel = e.value("path", std::string());
            if (!rel.empty())
                out[ss(QDir::cleanPath(qs(rel)))] = e;
        }
    }
    catch (const std::exception& e) {
        FC_WARN("set manifest " << ss(path) << " not read: " << e.what());
    }
    return out;
}

}  // namespace

const char* App::setPairingName(SetPairing pairing)
{
    switch (pairing) {
        case SetPairing::History:
            return "history";
        case SetPairing::SaveId:
            return "save";
        case SetPairing::Manifest:
            return "manifest";
        case SetPairing::Uid:
            return "uid";
        case SetPairing::Path:
            return "path";
        case SetPairing::User:
            return "user";
        case SetPairing::None:
            break;
    }
    return "none";
}

SetImportPlan App::planSetImport(Document& ourTop, const std::string& theirTop)
{
    // docs/TransactionLog.md sec 35 L2, L3, 35.6 Q1.
    const std::string topKey = keyOf(theirTop);
    if (!QFileInfo(qs(topKey)).isFile())
        THROWM(Base::FileException, "no such file", theirTop.c_str());
    const std::map<std::string, Document*> ours = oursOf(ourTop);
    if (ours.count(topKey))
        THROWM(Base::RuntimeError, "'" + theirTop + "' is a file of this set, not another's");
    const QDir theirDir = QFileInfo(qs(topKey)).absoluteDir();
    const std::string ourTopFile = ourTop.FileName.getStrValue();
    const QDir ourDir = QFileInfo(qs(ourTopFile)).absoluteDir();

    // L2: their set, from the files. Ours by the same file are shared.
    SetImportPlan plan;
    plan.ourTop = &ourTop;
    std::map<std::string, Theirs> theirs;
    std::vector<std::string> order;
    std::set<std::string> visiting;
    std::set<std::string> sharedSeen;
    std::set<std::string> missingSeen;
    std::function<void(const std::string&)> read = [&](const std::string& file) {
        if (theirs.count(file) || !visiting.insert(file).second)
            return;
        std::string xml;
        if (!FileBlobManager::readArchiveMember(file, "Document.xml", xml))
            THROWM(Base::FileException, "cannot read the document", file.c_str());
        Theirs t;
        t.uid = uidOf(xml);
        std::set<std::string> linked;
        for (const auto& attrs : xlinksOf(xml)) {
            const std::string target = targetOf(attrs, file);
            if (target.empty() || target == file || !linked.insert(target).second)
                continue;
            if (ours.count(target)) {
                if (sharedSeen.insert(target).second)
                    plan.shared.push_back(target);
                continue;
            }
            if (!QFileInfo(qs(target)).isFile()) {
                if (missingSeen.insert(target).second)
                    plan.missing.push_back(target);
                continue;
            }
            t.links.push_back(target);
            read(target);
        }
        theirs[file] = std::move(t);
        // Their links first: each after what it links.
        order.push_back(file);
    };
    read(topKey);

    // Ours' Uids, from the files: an open document's is changed when two
    // with one are open (sec 35.2 item 8).
    std::map<std::string, std::string> ourUids;
    std::map<std::string, std::string> ourRelative;
    for (const auto& [file, doc] : ours) {
        std::string xml;
        ourUids[file] = FileBlobManager::readArchiveMember(file, "Document.xml", xml)
            ? uidOf(xml)
            : doc->Uid.getValueStr();
        ourRelative[file] = ss(QDir::cleanPath(ourDir.relativeFilePath(qs(file))));
    }
    const auto manifest = manifestOf(topKey);

    // L3: the first signal that answers. Two of ours answering one alike
    // pair none: the user says.
    for (const auto& file : order) {
        SetImportFile f;
        f.theirs = file;
        f.relative = ss(QDir::cleanPath(theirDir.relativeFilePath(qs(file))));
        f.uid = theirs[file].uid;
        f.links = theirs[file].links;
        auto pairWith = [&f](const std::vector<std::string>& hits, SetPairing how,
                             const std::map<std::string, Document*>& ours) {
            if (hits.size() > 1)
                f.ambiguous = true;
            else if (hits.size() == 1) {
                f.ourFile = hits.front();
                f.ours = ours.at(f.ourFile);
                f.pairing = how;
            }
            return !hits.empty();
        };
        auto entry = manifest.find(f.relative);
        if (entry != manifest.end()) {
            f.branch = entry->second.value("branch", std::string());
            f.version = entry->second.value("version", int64_t(0));
        }

        // 1, 2: a history both hold, or a save of ours named.
        std::vector<std::string> hits;
        for (const auto& [ourFile, doc] : ours) {
            try {
                for (const auto& b : doc->forkBranches(file)) {
                    if (!b.independent) {
                        hits.push_back(ourFile);
                        break;
                    }
                }
            }
            catch (Base::Exception&) {
                // No log here, or nothing of theirs to read: no answer.
            }
            catch (std::exception&) {
            }
        }
        FileHistory::Saved saved;
        const bool history = FileHistory::savedAs(file, saved) && saved.history;
        if (pairWith(hits, history ? SetPairing::History : SetPairing::SaveId, ours)) {
            plan.files.push_back(std::move(f));
            continue;
        }
        // 3: our manifest.
        hits.clear();
        if (entry != manifest.end()) {
            const std::string named = entry->second.value("file", std::string());
            if (!named.empty() && ours.count(keyOf(named)))
                hits.push_back(keyOf(named));
        }
        if (pairWith(hits, SetPairing::Manifest, ours)) {
            plan.files.push_back(std::move(f));
            continue;
        }
        // 4: the Uid, read from the files.
        hits.clear();
        for (const auto& [ourFile, uid] : ourUids) {
            if (!f.uid.empty() && uid == f.uid)
                hits.push_back(ourFile);
        }
        if (pairWith(hits, SetPairing::Uid, ours)) {
            plan.files.push_back(std::move(f));
            continue;
        }
        // 5: the same path under the top.
        hits.clear();
        for (const auto& [ourFile, rel] : ourRelative) {
            if (rel == f.relative)
                hits.push_back(ourFile);
        }
        pairWith(hits, SetPairing::Path, ours);
        plan.files.push_back(std::move(f));
    }
    return plan;
}

void App::pairByHand(SetImportPlan& plan, const std::string& theirs, Document* ours)
{
    const std::string key = keyOf(theirs);
    for (auto& f : plan.files) {
        if (f.theirs != key)
            continue;
        f.ambiguous = false;
        if (ours) {
            f.ours = ours;
            f.ourFile = keyOf(ours->FileName.getStrValue());
            f.pairing = SetPairing::User;
        }
        else {
            f.ours = nullptr;
            f.ourFile.clear();
            f.pairing = SetPairing::None;
        }
        return;
    }
    THROWM(Base::ValueError, "'" + theirs + "' is no file of their set");
}

namespace
{

/// Where a link of ours points: a file, and the version or the branch of
/// it, from the name of the document it links into (FileHistory::parseName).
using LinkKey = std::tuple<std::string, int64_t, std::string>;
using Target = PropertyXLink::ImportSetLinks::Target;

LinkKey keyOfDocument(const Document* doc)
{
    const std::string name = doc->FileName.getStrValue();
    FileHistory::NameParts parts;
    if (doc->testStatus(Document::VersionDoc) && FileHistory::parseName(name, parts))
        return {keyOf(parts.file), parts.branch.empty() ? parts.version : 0, parts.branch};
    return {keyOf(name), 0, std::string()};
}

std::string nameOf(const Target& t)
{
    if (!t.branch.empty())
        return t.file + "@" + t.branch + "@";
    if (t.version > 0)
        return t.file + "@v" + std::to_string(t.version);
    return t.file;
}

/** The links of branch `branch` of `ourFile` that still point at a file of
 * the set as it is here -- set before the copies parted, they are in no
 * row brought -- pointed at what that file came as: one row on the branch.
 * Returns how many. Only links held by a property of their own; one inside
 * a list is left as it is.
 */
int relinkBranch(const std::string& ourFile, const std::string& branch,
                 const std::map<LinkKey, Target>& targets,
                 const std::map<std::string, Target>& byHeld = {},
                 const char* rowName = "Link their set", bool plainly = false)
{
    auto& app = GetApplication();
    Document* doc = app.openDocument((ourFile + "@" + branch + "@").c_str(), false);
    if (!doc)
        return 0;
    std::vector<std::pair<PropertyXLink*, App::DocumentObject*>> changes;
    std::vector<std::pair<PropertyXLink*, std::string>> plain;
    for (auto obj : doc->getObjects()) {
        std::vector<App::Property*> props;
        obj->getPropertyList(props);
        for (auto prop : props) {
            auto link = Base::freecad_dynamic_cast<PropertyXLink>(prop);
            if (!link || prop->getContainer() != obj || !link->getValue())
                continue;
            const Document* linked = link->getValue()->getDocument();
            if (linked == doc)
                continue;
            const Target* found = nullptr;
            auto held = link->getHeldHash().empty() ? byHeld.end()
                                                    : byHeld.find(link->getHeldHash());
            if (held != byHeld.end())
                found = &held->second;
            else {
                auto it = targets.find(keyOfDocument(linked));
                if (it != targets.end())
                    found = &it->second;
            }
            if (!found || nameOf(*found) == linked->FileName.getStrValue())
                continue;
            if (plainly) {
                // By name: the object may be ours only once the file it is
                // in is merged.
                plain.emplace_back(link, found->file);
                continue;
            }
            Document* to = app.openDocument(nameOf(*found).c_str(), false);
            auto target = to ? to->getObject(link->getValue()->getNameInDocument()) : nullptr;
            if (!target) {
                FC_WARN("set import: " << link->getFullName() << " has no "
                        << link->getValue()->getNameInDocument() << " in " << nameOf(*found));
                continue;
            }
            changes.emplace_back(link, target);
        }
    }
    if (changes.empty() && plain.empty())
        return 0;
    const int mode = doc->getUndoMode();
    if (!mode)
        doc->setUndoMode(1);
    // One open already -- a set's merge's step, which a change of a file it
    // links opened here -- takes the row; a document holds one per id.
    const bool open = doc->hasPendingTransaction();
    if (!open)
        doc->openTransaction(rowName);
    try {
        for (auto& [link, target] : changes)
            link->setValue(target, std::vector<std::string>(link->getSubValues()));
        for (auto& [link, file] : plain)
            link->setPlainFile(file);
        if (!open)
            doc->commitTransaction();
    }
    catch (...) {
        if (!open)
            doc->abortTransaction();
        if (!mode)
            doc->setUndoMode(0);
        throw;
    }
    if (!mode)
        doc->setUndoMode(0);
    return static_cast<int>(changes.size() + plain.size());
}

/// Close what was opened since `before`, unless a document staying open
/// links to it.
void closeOpened(const std::set<Document*>& before)
{
    auto& app = GetApplication();
    for (bool closed = true; closed;) {
        closed = false;
        auto inList = PropertyXLink::getDocumentInList();
        for (auto doc : app.getDocuments()) {
            if (before.count(doc))
                continue;
            bool linked = false;
            for (auto from : inList[doc]) {
                if (from != doc) {
                    linked = true;
                    break;
                }
            }
            if (!linked) {
                app.closeDocument(doc->getName());
                closed = true;
                break;
            }
        }
    }
}

}  // namespace

SetImportResult App::importSet(const SetImportPlan& plan)
{
    // docs/TransactionLog.md sec 35 L4.
    SetImportResult result;
    std::set<Document*> before;
    for (auto doc : GetApplication().getDocuments())
        before.insert(doc);
    std::map<LinkKey, Target> targets;
    PropertyXLink::ImportSetLinks links;

    // L5, Q2: their files alone held in our top's log, each written under
    // the top's transient directory at its path in their set, so that links
    // between them hold; links to them are to that file, held.
    std::map<std::string, std::pair<int64_t, std::string>> held;   // theirs -> row, staged
    if (plan.ourTop && plan.ourTop->getTransactionLog()) {
        const std::string setId = Base::Uuid::createUuid().substr(0, 8);
        const std::string theirTop = plan.files.empty() ? std::string()
            : ss(QFileInfo(qs(plan.files.back().theirs)).fileName());
        for (const auto& f : plan.files) {
            if (f.ours || f.ambiguous)
                continue;
            // Their path, with no way out of the folder it is written in.
            QStringList parts = QDir::cleanPath(qs(f.relative)).split(QLatin1Char('/'));
            for (auto& part : parts) {
                if (part == QStringLiteral(".."))
                    part = QStringLiteral("_up");
            }
            const std::string heldPath = setId + "/" + ss(parts.join(QLatin1Char('/')));
            try {
                const int64_t seq = plan.ourTop->keepSentFileAt(
                    f.theirs, ss(QFileInfo(qs(f.theirs)).fileName()), theirTop, "set");
                std::string hash;
                for (const auto& sent : plan.ourTop->sentFiles()) {
                    if (sent.seq == seq)
                        hash = sent.hash;
                }
                const std::string staged =
                    plan.ourTop->TransientDir.getStrValue() + "/held/" + heldPath;
                QDir().mkpath(QFileInfo(qs(staged)).absolutePath());
                plan.ourTop->writeSentFile(seq, staged);
                Target target;
                target.file = staged;
                target.held = hash;
                target.heldPath = heldPath;
                PropertyXLink::ImportSetLinks::add(f.theirs, target);
                held[f.theirs] = {seq, staged};
            }
            catch (Base::Exception& e) {
                FC_ERR("set import: " << f.theirs << " not held: " << e.what());
            }
        }
    }

    const QDir ourDir = plan.ourTop
        ? QFileInfo(qs(plan.ourTop->FileName.getStrValue())).absoluteDir() : QDir();
    for (const auto& f : plan.files) {
        SetImportedFile r;
        r.theirs = f.theirs;
        r.action = defaultSetMergeAction(f);
        if (!f.ours && !f.ambiguous)
            r.writeTo = ss(QDir::cleanPath(ourDir.absoluteFilePath(qs(f.relative))));
        if (!f.ours) {
            auto it = held.find(f.theirs);
            if (it != held.end()) {
                r.held = it->second.first;
                r.staged = it->second.second;
            }
            result.files.push_back(std::move(r));
            continue;
        }
        r.ourFile = f.ourFile;
        try {
            const auto res = f.ours->importFork(f.theirs);
            if (res.seq)
                r.branch = res.branch;
            r.rows = res.rows;
            r.independent = res.independent;
            r.stoppedAt = res.stoppedAt;
            r.reason = res.reason;
        }
        catch (Base::Exception& e) {
            r.error = e.what();
        }
        catch (std::exception& e) {
            r.error = e.what();
        }
        // What their file is here, for the files linking it, imported after.
        PropertyXLink::ImportSetLinks::Target target;
        target.file = f.ourFile;
        if (!r.branch.empty())
            target.branch = r.branch;
        else if (!f.branch.empty())
            target.branch = f.branch;
        else
            target.version = f.version;
        PropertyXLink::ImportSetLinks::add(f.theirs, target);
        // And a link of ours to what their file was of, to what it came as.
        targets[LinkKey {f.ourFile, f.branch.empty() ? f.version : 0, f.branch}] = target;
        FC_LOG("set import " << f.theirs << " -> " << f.ourFile
               << (r.branch.empty() ? std::string() : " branch " + r.branch)
               << (r.error.empty() ? std::string() : " error " + r.error));
        result.files.push_back(std::move(r));
    }
    for (auto& r : result.files) {
        if (r.branch.empty() || !r.error.empty())
            continue;
        try {
            r.relinked = relinkBranch(r.ourFile, r.branch, targets);
        }
        catch (Base::Exception& e) {
            r.error = std::string("links not made the set's: ") + e.what();
        }
    }
    // Held for the top's branch: what links them, until it is merged or
    // deleted. With none, they wait as a sent file does.
    const SetImportedFile* top = result.files.empty() ? nullptr : &result.files.back();
    if (top && !top->branch.empty() && plan.ourTop) {
        for (const auto& r : result.files) {
            if (r.held)
                plan.ourTop->holdSentFileFor(r.held, top->branch);
        }
    }
    closeOpened(before);
    return result;
}

const char* App::setMergeActionName(SetMergeAction action)
{
    switch (action) {
        case SetMergeAction::Merge:
            return "merge";
        case SetMergeAction::TakeTheirs:
            return "theirs";
        case SetMergeAction::KeepOurs:
            return "ours";
        case SetMergeAction::Write:
            return "write";
        case SetMergeAction::Drop:
            break;
    }
    return "drop";
}

SetMergeAction App::defaultSetMergeAction(const SetImportFile& file)
{
    if (!file.ours)
        return SetMergeAction::Write;
    return file.certain() ? SetMergeAction::Merge : SetMergeAction::TakeTheirs;
}

namespace
{

/// The branch `branch` of `doc`'s file taken in as merged, by a record with
/// no ops whose second parent is its head: not asked again, and what is held
/// for it let go. No undo step.
void recordTaken(Document& doc, const std::string& branch, const char* how)
{
    TransactionLog* log = doc.getTransactionLog();
    LogBranch b;
    if (!log || !log->store().findBranch(branch, b))
        return;
    nlohmann::json j;
    j["set"] = {{"branch", branch}, {"action", how}};
    log->record("merge", std::string(how) + " " + branch, j.dump(), b.head);
    log->flush();
}

}  // namespace

SetMergeResult App::mergeSet(Document& ourTop, const std::vector<SetMergeFile>& files,
                             const SetMergeOptions& options)
{
    // docs/TransactionLog.md sec 35 L6.
    auto& app = GetApplication();
    SetMergeResult result;
    for (const auto& f : files) {
        SetMergedFile r;
        r.theirs = f.theirs;
        r.ourFile = f.ourFile;
        r.action = f.action;
        result.files.push_back(std::move(r));
    }
    auto docOf = [&app](const std::string& file) -> Document* {
        for (auto doc : app.getDocuments()) {
            if (!doc->testStatus(Document::VersionDoc)
                    && keyOf(doc->FileName.getStrValue()) == keyOf(file))
                return doc;
        }
        return app.openDocument(file.c_str(), false);
    };

    // Nothing moves until all of it can: conflicts with no side, a file to
    // be written that is there.
    for (std::size_t i = 0; i < files.size(); ++i) {
        const auto& f = files[i];
        auto& r = result.files[i];
        if (f.action == SetMergeAction::Write) {
            if (!f.held)
                r.error = "no file is held for it";
            else if (f.writeTo.empty())
                r.error = "no place to write it";
            else if (!options.overwrite && QFileInfo::exists(qs(f.writeTo)))
                r.error = "'" + f.writeTo + "' is there";
        }
        else if (f.action == SetMergeAction::Merge && !f.branch.empty()) {
            Document* ours = docOf(f.ourFile);
            if (!ours) {
                r.error = "cannot open '" + f.ourFile + "'";
                continue;
            }
            try {
                for (const auto& c : ours->previewMerge(f.branch).changes) {
                    if ((c.kind == "conflict" || c.kind == "unit") && f.fallback.empty()
                            && !f.picks.count(c.key))
                        ++r.unresolved;
                }
                if (r.unresolved)
                    r.error = std::to_string(r.unresolved) + " conflicts with no side";
            }
            catch (Base::Exception& e) {
                r.error = e.what();
            }
        }
        if (!r.error.empty())
            result.refused = true;
    }
    if (result.refused)
        return result;

    std::set<Document*> before;
    for (auto doc : app.getDocuments())
        before.insert(doc);

    // Their files alone, where the user says; links to them, there.
    std::map<std::string, Target> byHeld;
    for (std::size_t i = 0; i < files.size(); ++i) {
        const auto& f = files[i];
        if (f.action != SetMergeAction::Write)
            continue;
        std::string hash;
        for (const auto& sent : ourTop.sentFiles()) {
            if (sent.seq == f.held)
                hash = sent.hash;
        }
        QDir().mkpath(QFileInfo(qs(f.writeTo)).absolutePath());
        ourTop.writeSentFile(f.held, f.writeTo);
        result.files[i].written = f.writeTo;
        Target t;
        t.file = f.writeTo;
        if (!hash.empty())
            byHeld[hash] = t;
    }
    // Each branch's links to the set's branches made plain links to ours,
    // which hold what they had once merged; one row of the branch, before
    // the step, so the merge takes them so. By name: an object of theirs is
    // ours only once its file is merged.
    std::map<LinkKey, Target> plain;
    for (const auto& f : files) {
        if (f.ourFile.empty() || f.branch.empty())
            continue;
        Target t;
        t.file = keyOf(f.ourFile);
        plain[LinkKey {keyOf(f.ourFile), 0, f.branch}] = t;
    }

    for (std::size_t i = 0; i < files.size(); ++i) {
        const auto& f = files[i];
        if (f.ourFile.empty() || f.branch.empty() || f.action == SetMergeAction::KeepOurs)
            continue;
        try {
            relinkBranch(keyOf(f.ourFile), f.branch, plain, byHeld, "Link our set", true);
        }
        catch (Base::Exception& e) {
            result.files[i].error = std::string("links not made ours: ") + e.what();
        }
    }

    // One undo step across the documents (Q3), held open as a command holds
    // its own: a branch's row committed on the way does not close it.
    {
        AutoTransaction step(options.name.c_str());
        int tid = 0;
        app.getActiveTransaction(&tid);
        result.transaction = tid;
        SetMergeScope scope;
        for (std::size_t i = 0; i < files.size(); ++i) {
            const auto& f = files[i];
            auto& r = result.files[i];
            if (f.ourFile.empty() || f.branch.empty() || !r.error.empty())
                continue;
            Document* ours = docOf(f.ourFile);
            if (!ours) {
                r.error = "cannot open '" + f.ourFile + "'";
                continue;
            }
            try {
                switch (f.action) {
                    case SetMergeAction::Merge: {
                        const auto m = ours->mergeBranch(f.branch, f.picks, f.fallback);
                        r.seq = m.seq;
                        r.failed = m.failed;
                        r.unresolved = m.unresolved.size();
                        if (r.unresolved)
                            r.error = "conflicts with no side";
                        break;
                    }
                    case SetMergeAction::TakeTheirs: {
                        // Q8: ours made what the branch's head is, one row as a
                        // restore is; then the branch is taken in.
                        Document* tip = app.openDocument(
                            (keyOf(f.ourFile) + "@" + f.branch + "@").c_str(), false);
                        const int64_t num = tip ? tip->snapshotToLog() : 0;
                        if (!num) {
                            r.error = "no version of the branch's head";
                            break;
                        }
                        ours->restoreVersion(num);
                        r.seq = ours->getTransactionLog() ? ours->getTransactionLog()->lastSeq() : 0;
                        recordTaken(*ours, f.branch, "Take theirs");
                        break;
                    }
                    case SetMergeAction::KeepOurs:
                        recordTaken(*ours, f.branch, "Keep ours");
                        break;
                    default:
                        break;
                }
                if (f.action == SetMergeAction::Merge || f.action == SetMergeAction::TakeTheirs)
                    r.undoId = ours->getTransactionID(true, 0);
            }
            catch (Base::Exception& e) {
                r.error = e.what();
            }
            catch (std::exception& e) {
                r.error = e.what();
            }
        }
    }
    closeOpened(before);
    return result;
}
