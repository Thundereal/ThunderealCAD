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
#include "GeoFeature.h"
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

/// A full path, cleaned, the one key of a file here: the history's key.
std::string keyOf(const std::string& path)
{
    return FileHistory::canonicalPath(path);
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
    std::set<std::string> objects;   ///< its objects' UIDs (sec 36 U1)
};

/// The UIDs of a Document.xml's objects: `uid` on each `<Object>` entry of
/// its `Objects` (sec 36 U1)
std::set<std::string> objectUidsOf(const std::string& xml)
{
    std::set<std::string> uids;
    const std::string tag = "<Object ";
    const std::string key = " uid=\"";
    for (auto at = xml.find(tag); at != std::string::npos; at = xml.find(tag, at + 1)) {
        const auto end = xml.find('>', at);
        const auto uid = xml.find(key, at);
        if (end == std::string::npos || uid == std::string::npos || uid > end)
            continue;
        const auto from = uid + key.size();
        const auto to = xml.find('"', from);
        if (to != std::string::npos && to > from)
            uids.insert(xml.substr(from, to - from));
    }
    return uids;
}

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
        case SetPairing::Objects:
            return "objects";
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
    // docs/TransactionLogJournal.md sec 35 L2, L3, 35.6 Q1.
    const std::string topKey = keyOf(theirTop);
    if (!QFileInfo(qs(topKey)).isFile())
        THROWM(Base::FileException, "no such file", theirTop.c_str());
    const std::map<std::string, Document*> ours = oursOf(ourTop);
    if (ours.count(topKey))
        THROWM(Base::RuntimeError, "'" + theirTop + "' is a file of this set, not another's");
    const QDir theirDir = QFileInfo(qs(topKey)).absoluteDir();
    const std::string ourTopFile = ourTop.FileName.getStrValue();
    // By its key, as our files are keyed (oursOf): a folder spelt through
    // a link -- macOS's /var for /private/var -- would put every one of
    // them outside it, and no relative path would pair
    const QDir ourDir = QFileInfo(qs(keyOf(ourTopFile))).absoluteDir();

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
        t.objects = objectUidsOf(xml);
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
        // U5 (sec 36, 35.18), where the Uid says nothing: their objects'
        // UIDs, most of them ours' -- two at least, one being no proof (a
        // pasted copy shares it) -- and no other file of ours as many.
        hits.clear();
        {
            const auto& mine = theirs[file].objects;
            std::size_t best = 0;
            std::size_t second = 0;
            std::string bestFile;
            for (const auto& [ourFile, doc] : ours) {
                std::size_t shared = 0;
                for (auto obj : doc->getObjects()) {
                    if (mine.count(obj->getObjectUid()))
                        ++shared;
                }
                if (shared > best) {
                    second = best;
                    best = shared;
                    bestFile = ourFile;
                }
                else if (shared > second)
                    second = shared;
            }
            if (best >= 2 && best * 2 >= mine.size()) {
                hits.push_back(bestFile);
                // As many in another: neither, the user says
                if (second == best)
                    hits.push_back(std::string());
            }
        }
        if (hits.size() > 1) {
            f.ambiguous = true;
            plan.files.push_back(std::move(f));
            continue;
        }
        if (pairWith(hits, SetPairing::Objects, ours)) {
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

/// What an expression container (an object's expressions, a sheet) reads of
/// another file of the set, and what it reads instead (sec 35.16 G7).
struct ExpressionRelink
{
    PropertyXLinkContainer* prop = nullptr;
    DocumentObject* owner = nullptr;
    std::vector<std::pair<DocumentObject*, DocumentObject*>> swaps;   // old, new
};

/** The links of `doc` -- each a link, or an entry of a list of links (sec
 * 35.15 G2: a SubShapeBinder's Support) -- to a file of the set that came
 * as something else here: in `changes` the object they link now, or with
 * `plainly`, in `plain` the file they name plainly. An expression's reads of
 * such a file (G7) in `exprs`, each to the object of that name there --
 * plainly, ours, which an object their file made is not yet: it is left. */
void gatherRelinks(Document* doc, const std::map<LinkKey, Target>& targets,
                   const std::map<std::string, Target>& byHeld, bool plainly,
                   std::vector<std::pair<PropertyXLink*, App::DocumentObject*>>& changes,
                   std::vector<std::pair<PropertyXLink*, std::string>>& plain,
                   std::vector<ExpressionRelink>* exprs = nullptr)
{
    auto& app = GetApplication();
    // The file a link to `linked` names here instead, if any
    auto targetOf = [&](const PropertyXLink* link, const Document* linked) -> const Target* {
        const Target* found = nullptr;
        auto held = !link || link->getHeldHash().empty() ? byHeld.end()
                                                         : byHeld.find(link->getHeldHash());
        if (held != byHeld.end())
            found = &held->second;
        else {
            auto it = targets.find(keyOfDocument(linked));
            if (it != targets.end())
                found = &it->second;
        }
        if (!found || nameOf(*found) == linked->FileName.getStrValue())
            return nullptr;
        return found;
    };
    for (auto obj : doc->getObjects()) {
        std::vector<App::Property*> props;
        obj->getPropertyList(props);
        // A link, and each entry of a list of links (sec 35.15 G2: a
        // SubShapeBinder's Support): an entry's change is its list's
        std::vector<PropertyXLink*> links;
        for (auto prop : props) {
            if (prop->getContainer() != obj)
                continue;
            if (auto link = Base::freecad_dynamic_cast<PropertyXLink>(prop))
                links.push_back(link);
            else if (auto list = Base::freecad_dynamic_cast<PropertyXLinkSubList>(prop)) {
                for (auto& entry : list->getSubListValues())
                    links.push_back(const_cast<PropertyXLinkSub*>(&entry));
            }
        }
        for (auto link : links) {
            if (!link->getValue())
                continue;
            const Document* linked = link->getValue()->getDocument();
            if (linked == doc)
                continue;
            const Target* found = targetOf(link, linked);
            if (!found)
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
        if (!exprs)
            continue;
        // G7: what its expressions read of another file of the set
        for (auto prop : props) {
            auto container = Base::freecad_dynamic_cast<PropertyXLinkContainer>(prop);
            if (!container || prop->getContainer() != obj)
                continue;
            std::vector<DocumentObject*> read;
            container->getLinks(read, true);
            ExpressionRelink relink;
            std::set<DocumentObject*> seen;
            for (auto old : read) {
                if (!old || !seen.insert(old).second || old->getDocument() == doc)
                    continue;
                const Target* found = targetOf(nullptr, old->getDocument());
                if (!found)
                    continue;
                Document* to = plainly ? app.getDocumentByPath(found->file.c_str())
                                       : app.openDocument(nameOf(*found).c_str(), false);
                if (plainly && !to)
                    to = app.openDocument(found->file.c_str(), false);
                auto now = to ? to->getObject(old->getNameInDocument()) : nullptr;
                if (!now) {
                    FC_WARN("set import: " << prop->getFullName() << " reads "
                            << old->getFullName() << ", which " << nameOf(*found)
                            << " has not: left as it is");
                    continue;
                }
                relink.swaps.emplace_back(old, now);
            }
            if (!relink.swaps.empty()) {
                relink.prop = container;
                relink.owner = obj;
                exprs->push_back(std::move(relink));
            }
        }
    }
}

/// G7: each container's reads swapped, one value set per container.
void applyExpressionRelinks(const std::vector<ExpressionRelink>& exprs)
{
    for (const auto& r : exprs) {
        std::unique_ptr<Property> now;
        for (const auto& [old, with] : r.swaps) {
            const auto from = now ? Base::freecad_dynamic_cast<PropertyLinkBase>(now.get())
                                  : static_cast<const PropertyLinkBase*>(r.prop);
            if (!from)
                break;
            if (auto copy = from->CopyOnLinkReplace(r.owner, old, with))
                now.reset(copy);
        }
        if (now)
            r.prop->Paste(*now);
    }
}

/** The links of branch `branch` of `ourFile` that still point at a file of
 * the set as it is here -- set before the copies parted, they are in no
 * row brought -- pointed at what that file came as: one row on the branch.
 * Returns how many (gatherRelinks(): a list's entries too).
 */
int relinkBranch(const std::string& ourFile, const std::string& branch,
                 const std::map<LinkKey, Target>& targets,
                 const std::map<std::string, Target>& byHeld = {},
                 const char* rowName = "Link their set", bool plainly = false)
{
    auto& app = GetApplication();
    // The branch's rows are committed as they are written: inside a command,
    // the command's transaction is held open to its end, and what the
    // branch's document took into it -- opened in the command, its first
    // change -- with it; the branch then has a transaction open, and a merge
    // of it refuses. The hold is let go here, as setEdit() lets it go, and
    // put back.
    struct Release
    {
        bool held = AutoTransaction::isEnabled();
        Release()
        {
            if (held)
                AutoTransaction::setEnable(false);
        }
        ~Release()
        {
            if (held)
                AutoTransaction::setEnable(true);
        }
    } release;
    Document* doc = app.openDocument((ourFile + "@" + branch + "@").c_str(), false);
    if (!doc)
        return 0;
    if (doc->hasPendingTransaction())
        doc->commitTransaction();
    std::vector<std::pair<PropertyXLink*, App::DocumentObject*>> changes;
    std::vector<std::pair<PropertyXLink*, std::string>> plain;
    std::vector<ExpressionRelink> exprs;
    gatherRelinks(doc, targets, byHeld, plainly, changes, plain, &exprs);
    if (changes.empty() && plain.empty() && exprs.empty())
        return 0;
    const int mode = doc->getUndoMode();
    if (!mode)
        doc->setUndoMode(1);
    doc->openTransaction(rowName);
    try {
        for (auto& [link, target] : changes)
            link->setValue(target, std::vector<std::string>(link->getSubValues()));
        for (auto& [link, file] : plain)
            link->setPlainFile(file);
        applyExpressionRelinks(exprs);
        doc->commitTransaction();
    }
    catch (...) {
        doc->abortTransaction();
        if (!mode)
            doc->setUndoMode(0);
        throw;
    }
    if (!mode)
        doc->setUndoMode(0);
    return static_cast<int>(changes.size() + plain.size() + exprs.size());
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
    // docs/TransactionLogJournal.md sec 35 L4.
    // The documents this opens -- branches' tips, to replay into and to
    // relink -- it fills in itself, as a load does: what the Gui still fills
    // in of them is their views, and their changes are the program's, not
    // the command's that runs the import.
    Document::UserEditSuspend suspend;
    auto& app = GetApplication();
    SetImportResult result;
    std::set<Document*> before;
    for (auto doc : app.getDocuments())
        before.insert(doc);
    std::map<LinkKey, Target> targets;
    PropertyXLink::ImportSetLinks links;

    // L5, Q2: their files alone held in our top's log, each written under
    // the top's transient directory at its path in their set, so that links
    // between them hold; links to them are to that file, held.
    std::map<std::string, std::pair<int64_t, std::string>> held;   // theirs -> row, staged
    // Q9: the set, named in every file's import record and mapped in the
    // top's.
    const std::string setId = Base::Uuid::createUuid().substr(0, 8);
    result.id = setId;
    result.theirTop = plan.files.empty() ? std::string() : plan.files.back().theirs;
    result.name = ss(QFileInfo(qs(result.theirTop)).fileName());
    ImportSetRecord setRecord(result.id, result.name);
    auto holdAlone = [&](const SetImportFile& f) {
        const std::string& theirTop = result.name;
        // Their path, with no way out of the folder it is written in.
        QStringList parts = QDir::cleanPath(qs(f.relative)).split(QLatin1Char('/'));
        for (auto& part : parts) {
            if (part == QStringLiteral(".."))
                part = QStringLiteral("_up");
        }
        const std::string heldPath = setId + "/" + ss(parts.join(QLatin1Char('/')));
        const std::string staged =
            plan.ourTop->TransientDir.getStrValue() + "/held/" + heldPath;
        try {
            QDir().mkpath(QFileInfo(qs(staged)).absolutePath());
            // G4 (sec 35.15): their file read with its links mapped as the
            // rows brought are -- to what a file of the set came as, a file
            // of theirs alone held before it -- and that copy held, so its
            // links resolve here, and where the copy is written again in
            // another session. Not when their file is open already: it is
            // not read again, and is held as it is.
            bool relinked = false;
            if (!app.getDocumentByPath(f.theirs.c_str())) {
                PropertyXLink::ImportPaths paths(f.theirs, f.theirs);
                PropertyXLink::ImportPaths::intoFile(f.theirs);
                if (Document* doc = app.openDocument(f.theirs.c_str(), false)) {
                    ExportSetPaths copy({});
                    ExportSetPaths::writing(doc);
                    relinked = doc->saveCopy(staged.c_str(), false);
                }
            }
            const int64_t seq = plan.ourTop->keepSentFileAt(
                relinked ? staged : f.theirs, ss(QFileInfo(qs(f.theirs)).fileName()), theirTop,
                "set");
            std::string hash;
            for (const auto& sent : plan.ourTop->sentFiles()) {
                if (sent.seq == seq)
                    hash = sent.hash;
            }
            if (!relinked)
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
    };

    const QDir ourDir = plan.ourTop
        ? QFileInfo(qs(plan.ourTop->FileName.getStrValue())).absoluteDir() : QDir();
    std::set<std::string> branched;   // their files that came as a branch
    for (const auto& f : plan.files) {
        SetImportedFile r;
        r.theirs = f.theirs;
        r.relative = f.relative;
        r.pairing = f.pairing;
        r.action = defaultSetMergeAction(f);
        if (!f.ours && !f.ambiguous) {
            // Under our folder: a file outside theirs -- a library beside
            // both -- goes under _external/, as the export lays it out,
            // never up to where their own file is (sec 39, L1).
            const QString rel = QDir::cleanPath(qs(f.relative));
            const bool outside = QDir::isAbsolutePath(rel) || rel == QStringLiteral("..")
                || rel.startsWith(QStringLiteral("../"));
            r.writeTo = ss(QDir::cleanPath(ourDir.absoluteFilePath(
                outside ? QStringLiteral("_external/") + QFileInfo(rel).fileName() : rel)));
        }
        if (!f.ours) {
            // Held at its turn: after the files it links (G4)
            if (!f.ambiguous && plan.ourTop && plan.ourTop->getTransactionLog())
                holdAlone(f);
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
            // G1 (sec 35.15): nothing new of its own, but a file it links
            // came as a branch -- else what links it sees, through it, ours.
            // Files come after what they link, so those are known.
            if (r.branch.empty() && res.base > 0 && !res.independent && !res.stoppedAt) {
                bool reaches = false;
                for (const auto& link : f.links)
                    reaches = reaches || branched.count(link);
                if (reaches) {
                    r.branch = f.ours->importLinksOnly(
                        res.base, ss(QFileInfo(qs(f.theirs)).completeBaseName()));
                    r.linksOnly = true;
                    r.action = SetMergeAction::KeepOurs;
                }
            }
        }
        catch (Base::Exception& e) {
            r.error = e.what();
        }
        catch (std::exception& e) {
            r.error = e.what();
        }
        if (!r.branch.empty())
            branched.insert(f.theirs);
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
    // Q9: the map, in our top's log -- a record, no undo step.
    if (plan.ourTop) {
        if (TransactionLog* log = plan.ourTop->getTransactionLog()) {
            auto list = nlohmann::json::array();
            for (std::size_t i = 0; i < result.files.size(); ++i) {
                const auto& r = result.files[i];
                list.push_back({{"theirs", r.theirs},
                                {"relative", r.relative},
                                {"our_file", r.ourFile},
                                {"pairing", setPairingName(r.pairing)},
                                {"branch", r.branch},
                                {"links_only", r.linksOnly},
                                {"held", r.held},
                                {"action", setMergeActionName(r.action)},
                                {"write_to", r.writeTo},
                                {"error", r.error}});
            }
            nlohmann::json j;
            j["set"] = {{"id", result.id},
                        {"name", result.name},
                        {"top", result.theirTop},
                        {"files", std::move(list)}};
            result.record = log->record("set", "Import set " + result.name, j.dump());
            log->flush();
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
void recordTaken(Document& doc, const std::string& branch, const char* how, int step)
{
    TransactionLog* log = doc.getTransactionLog();
    LogBranch b;
    if (!log || !log->store().findBranch(branch, b))
        return;
    nlohmann::json j;
    // The set's step: undone, the decision is taken back with it (sec 39.4)
    j["set"] = {{"branch", branch}, {"action", how}, {"step", step}};
    log->record("merge", std::string(how) + " " + branch, j.dump(), b.head);
    log->flush();
}

/// Whether step `tid` stands undone at `doc`'s head (sec 39, L4): a row of
/// it on the chain, taken back by an undo no redo took back again.
bool stepUndone(Document& doc, int tid)
{
    TransactionLog* log = doc.getTransactionLog();
    if (!log || tid <= 0)
        return false;
    log->flush();
    const auto rows = log->store().chain(log->head());
    std::set<int64_t> turns;
    for (const auto& t : rows) {
        if (t.id == tid && t.kind != "undo" && t.kind != "redo")
            turns.insert(t.seq);
    }
    int undone = 0;
    for (const auto& t : rows) {
        if ((t.kind == "undo" || t.kind == "redo") && turns.count(t.inverts)) {
            turns.insert(t.seq);
            undone += t.kind == "undo" ? 1 : -1;
        }
    }
    return undone > 0;
}

/// Whether the merge a set's "Merged set" record names stands undone (sec
/// 39, L4): its step undone in the top or in any file it changed.
bool setMergeUndone(Document& top, const nlohmann::json& set)
{
    const int step = set.value("step", 0);
    if (!step)
        return false;
    if (stepUndone(top, step))
        return true;
    for (const auto& file : set.value("changed", nlohmann::json::array())) {
        if (!file.is_string())
            continue;
        for (auto doc : GetApplication().getDocuments()) {
            if (!doc->testStatus(Document::VersionDoc)
                    && keyOf(doc->FileName.getStrValue()) == file.get<std::string>()
                    && stepUndone(*doc, step))
                return true;
        }
    }
    return false;
}

}  // namespace

namespace
{

/// The file a document is of: its key, a branch's or a version's as its own
std::string fileOf(const Document* doc)
{
    return std::get<0>(keyOfDocument(doc));
}

/** G5: the references of `doc` to a file of the set (keyed in `after`) that
 * do not resolve in what that file is after the merge. */
void checkReferences(Document* doc, const std::map<std::string, Document*>& after,
                     std::vector<std::string>& out)
{
    const std::string self = fileOf(doc);
    // `file` and `name` what is read: an object of a document, or what a
    // link that does not resolve names
    auto check = [&](DocumentObject* obj, Property* prop, const std::string& file,
                     const std::string& name, const std::vector<std::string>& subs,
                     const std::vector<std::string>& shown) {
        auto it = after.find(file);
        if (file.empty() || name.empty() || file == self || it == after.end() || !it->second)
            return;
        const std::string from = std::string(obj->getNameInDocument()) + "." + prop->getName()
            + " -> " + ss(QFileInfo(qs(file)).fileName()) + "#" + name;
        auto there = it->second->getObject(name.c_str());
        if (!there) {
            out.push_back(from);
            return;
        }
        for (std::size_t i = 0; i < subs.size(); ++i) {
            const char* element = Data::findElementName(subs[i].c_str());
            if (!element || !*element)
                continue;
            std::pair<std::string, std::string> name;
            GeoFeature* geo = nullptr;
            auto owner = GeoFeature::resolveElement(there, subs[i].c_str(), name, false,
                                                    GeoFeature::Normal, nullptr, nullptr, &geo);
            bool missing = !owner || GeoFeature::hasMissingElement(name.first.c_str())
                || GeoFeature::hasMissingElement(name.second.c_str());
            // An element named by its index: one past the shape's count
            if (!missing && geo && !name.second.empty()) {
                Data::IndexedName indexed(name.second.c_str());
                auto prop = geo->getPropertyOfGeometry();
                auto data = prop ? prop->getComplexData() : nullptr;
                if (indexed && data && indexed.getIndex() > 0
                        && static_cast<unsigned long>(indexed.getIndex())
                            > data->countSubElements(indexed.getType()))
                    missing = true;
            }
            if (missing)
                out.push_back(from + "." + (i < shown.size() ? shown[i] : subs[i]));
        }
    };
    for (auto obj : doc->getObjects()) {
        std::vector<Property*> props;
        obj->getPropertyList(props);
        for (auto prop : props) {
            if (prop->getContainer() != obj)
                continue;
            std::vector<PropertyXLink*> links;
            if (auto link = Base::freecad_dynamic_cast<PropertyXLink>(prop))
                links.push_back(link);
            else if (auto list = Base::freecad_dynamic_cast<PropertyXLinkSubList>(prop)) {
                for (auto& entry : list->getSubListValues())
                    links.push_back(const_cast<PropertyXLinkSub*>(&entry));
            }
            else if (auto container = Base::freecad_dynamic_cast<PropertyXLinkContainer>(prop)) {
                std::vector<DocumentObject*> read;
                container->getLinks(read, true);
                std::set<DocumentObject*> seen;
                for (auto target : read) {
                    if (target && target->isAttachedToDocument() && target->getDocument() != doc
                            && seen.insert(target).second)
                        check(obj, prop, fileOf(target->getDocument()),
                              target->getNameInDocument(), {}, {});
                }
                continue;
            }
            for (auto link : links) {
                auto target = link->getValue();
                if (target && target->isAttachedToDocument()) {
                    if (target->getDocument() != doc)
                        check(obj, prop, fileOf(target->getDocument()),
                              target->getNameInDocument(), link->getSubValues(true),
                              link->getSubValues(false));
                    continue;
                }
                // Not resolved: by the file and the object it names
                std::string path = link->getDocumentPath() ? link->getDocumentPath() : "";
                if (path.empty())
                    continue;
                if (QFileInfo(qs(path)).isRelative())
                    path = ss(QFileInfo(qs(doc->FileName.getStrValue()))
                                  .absoluteDir()
                                  .absoluteFilePath(qs(path)));
                FileHistory::NameParts parts;
                const std::string file =
                    keyOf(FileHistory::parseName(path, parts) ? parts.file : path);
                check(obj, prop, file, link->getObjectName(), link->getSubValues(true),
                      link->getSubValues(false));
            }
        }
    }
}

/** What each of our files of the set is after the merge as `files` say:
 * its merge's result, its branch, or itself; `made` the results, the
 * caller's to close. A file that cannot be told says so in `out`. */
std::map<std::string, Document*> afterMerge(const std::vector<SetMergeFile>& files,
                                            std::vector<Document*>& made,
                                            std::vector<std::vector<std::string>>& out)
{
    auto& app = GetApplication();
    std::map<std::string, Document*> after;
    for (std::size_t i = 0; i < files.size(); ++i) {
        const auto& f = files[i];
        if (f.ourFile.empty())
            continue;
        Document* doc = nullptr;
        try {
            const std::string tip = keyOf(f.ourFile) + "@" + f.branch + "@";
            if (!f.branch.empty() && f.action == SetMergeAction::Merge) {
                Document* ours = app.openDocument(f.ourFile.c_str(), false);
                auto outcome = ours ? ours->previewMergeResult(f.branch, f.picks, f.fallback)
                                    : Document::MergeOutcome();
                if (outcome.document) {
                    doc = outcome.document;
                    made.push_back(doc);
                }
                else if (outcome.theirs)
                    doc = app.openDocument(tip.c_str(), false);
            }
            else if (!f.branch.empty() && f.action == SetMergeAction::TakeTheirs)
                doc = app.openDocument(tip.c_str(), false);
            if (!doc)
                doc = app.openDocument(f.ourFile.c_str(), false);
        }
        catch (Base::Exception& e) {
            out[i].push_back(std::string("not checked: ") + e.what());
        }
        after[keyOf(f.ourFile)] = doc;
    }
    return after;
}

}  // namespace

std::vector<std::vector<std::string>>
App::previewSetReferences(Document& ourTop, const std::vector<SetMergeFile>& files)
{
    (void)ourTop;
    Document::UserEditSuspend suspend;
    auto& app = GetApplication();
    std::set<Document*> before;
    for (auto doc : app.getDocuments())
        before.insert(doc);
    std::vector<std::vector<std::string>> out(files.size());
    std::vector<Document*> made;
    auto after = afterMerge(files, made, out);
    for (std::size_t i = 0; i < files.size(); ++i) {
        const auto& f = files[i];
        if (f.ourFile.empty() || f.branch.empty()
                || (f.action != SetMergeAction::Merge && f.action != SetMergeAction::TakeTheirs))
            continue;
        if (auto doc = after[keyOf(f.ourFile)])
            checkReferences(doc, after, out[i]);
    }
    for (auto doc : made) {
        if (app.getDocument(doc->getName()) == doc)
            app.closeDocument(doc->getName());
    }
    closeOpened(before);
    return out;
}

SetMergeResult App::mergeSet(Document& ourTop, const std::vector<SetMergeFile>& files,
                             const SetMergeOptions& options)
{
    // docs/TransactionLogJournal.md sec 35 L6. The branches' documents it opens it
    // changes itself, as importSet() does.
    Document::UserEditSuspend suspend;
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

    // The set merged before and that merge undone: each file's Keep ours
    // of it is taken back, so that every file is asked again (user,
    // 2026-10-27, sec 39.4). One worked on since keeps it, and a Merge of
    // it is refused below: it would bring nothing.
    std::set<std::size_t> declined;
    if (!options.set.empty()) {
        int step = 0;
        if (TransactionLog* log = ourTop.getTransactionLog()) {
            log->flush();
            for (const auto& t : log->store().history(log->head())) {
                if (t.kind != "set")
                    continue;
                const auto j = nlohmann::json::parse(t.script, nullptr, false);
                if (j.is_object() && j.contains("set") && j["set"].is_object()
                        && j["set"].value("merged", false)
                        && j["set"].value("id", std::string()) == options.set)
                    step = setMergeUndone(ourTop, j["set"]) ? j["set"].value("step", 0) : 0;
            }
        }
        for (std::size_t i = 0; step && i < files.size(); ++i) {
            if (files[i].ourFile.empty() || files[i].branch.empty())
                continue;
            Document* ours = docOf(files[i].ourFile);
            if (ours && ours->rollBackSetDecision(step) < 0)
                declined.insert(i);
        }
    }

    // Nothing moves until all of it can: conflicts with no side, a file to
    // be written that is there -- and never one of their set, which a write
    // would take the history and the links of (sec 39, L1).
    std::set<std::string> theirFiles;
    for (const auto& f : files)
        theirFiles.insert(FileHistory::canonicalPath(f.theirs));
    for (std::size_t i = 0; i < files.size(); ++i) {
        const auto& f = files[i];
        auto& r = result.files[i];
        if (f.action == SetMergeAction::Write) {
            if (!f.held)
                r.error = "no file is held for it";
            else if (f.writeTo.empty())
                r.error = "no place to write it";
            else if (theirFiles.count(FileHistory::canonicalPath(f.writeTo)))
                r.error = "'" + f.writeTo + "' is a file of their set";
            else if (!options.overwrite && QFileInfo::exists(qs(f.writeTo)))
                r.error = "'" + f.writeTo + "' is there";
        }
        else if (f.action == SetMergeAction::Merge && declined.count(i)) {
            r.error = "Keep ours took its branch in, and the file was changed since: "
                      "Take theirs, or pick the branch's rows";
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
    std::vector<std::size_t> writes;
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
        Target t;
        t.file = f.writeTo;
        if (!hash.empty())
            byHeld[hash] = t;
        writes.push_back(i);
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
    // A file of theirs alone is held with its links the set's (G4, sec
    // 35.15): to the branches, to the others held. Written, its links are
    // made the same plain links to ours, and to the others written --
    // relative, as between the files of a set. Read from a copy written
    // aside; the held bytes stay as they are. Written as held when it
    // cannot be read.
    {
        std::map<const Document*, std::string> copies;
        std::vector<std::pair<std::size_t, Document*>> read;
        for (auto i : writes) {
            const auto& f = files[i];
            const std::string aside = ourTop.TransientDir.getStrValue() + "/held/merge/"
                + std::to_string(i) + "/" + ss(QFileInfo(qs(f.writeTo)).fileName());
            Document* doc = nullptr;
            try {
                QDir().mkpath(QFileInfo(qs(aside)).absolutePath());
                ourTop.writeSentFile(f.held, aside);
                doc = app.openDocument(aside.c_str(), false);
            }
            catch (Base::Exception& e) {
                FC_WARN("set merge: " << f.writeTo << " written as held: " << e.what());
            }
            if (doc)
                copies[doc] = f.writeTo;
            read.emplace_back(i, doc);
        }
        for (const auto& f : files) {
            if (f.ourFile.empty())
                continue;
            if (auto doc = app.getDocumentByPath(f.ourFile.c_str()))
                copies[doc] = keyOf(f.ourFile);
        }
        for (auto [i, doc] : read) {
            const auto& f = files[i];
            bool done = false;
            if (doc) {
                try {
                    std::vector<std::pair<PropertyXLink*, App::DocumentObject*>> none;
                    std::vector<std::pair<PropertyXLink*, std::string>> links;
                    std::vector<ExpressionRelink> exprs;
                    gatherRelinks(doc, plain, byHeld, true, none, links, &exprs);
                    for (auto& [link, file] : links)
                        link->setPlainFile(file);
                    applyExpressionRelinks(exprs);
                    ExportSetPaths scope(copies);
                    ExportSetPaths::writing(doc);
                    done = doc->saveCopy(f.writeTo.c_str(), false);
                }
                catch (Base::Exception& e) {
                    FC_WARN("set merge: " << f.writeTo << " written as held: " << e.what());
                }
            }
            if (!done)
                ourTop.writeSentFile(f.held, f.writeTo);
            result.files[i].written = f.writeTo;
        }
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
                        // Sec 36 U7: theirs under their own names, where
                        // ours that held them goes in this row.
                        ours->restoreVersion(num, ours->importedNames(f.branch));
                        r.seq = ours->getTransactionLog() ? ours->getTransactionLog()->lastSeq() : 0;
                        recordTaken(*ours, f.branch, "Take theirs", tid);
                        break;
                    }
                    case SetMergeAction::KeepOurs:
                        recordTaken(*ours, f.branch, "Keep ours", tid);
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
    // Q4 (b), G5: what does not resolve now, in the report
    {
        std::map<std::string, Document*> now;
        for (const auto& f : files) {
            if (!f.ourFile.empty())
                now[keyOf(f.ourFile)] = docOf(f.ourFile);
        }
        for (std::size_t i = 0; i < files.size(); ++i) {
            const auto& f = files[i];
            auto& r = result.files[i];
            if (f.ourFile.empty() || f.branch.empty() || !r.error.empty()
                    || (f.action != SetMergeAction::Merge
                        && f.action != SetMergeAction::TakeTheirs))
                continue;
            if (auto doc = now[keyOf(f.ourFile)])
                checkReferences(doc, now, r.references);
        }
    }
    // Q9: the set no longer pending -- while its step stands: the step and
    // the files it changed, so an undo of it makes it pending again (sec 39,
    // L4).
    if (!options.set.empty()) {
        if (TransactionLog* log = ourTop.getTransactionLog()) {
            nlohmann::json j;
            j["set"] = {{"id", options.set}, {"merged", true}};
            auto changed = nlohmann::json::array();
            for (std::size_t i = 0; i < files.size(); ++i) {
                if (result.files[i].undoId)
                    changed.push_back(keyOf(files[i].ourFile));
            }
            j["set"]["step"] = result.transaction;
            j["set"]["changed"] = changed;
            log->record("set", "Merged set", j.dump());
            log->flush();
        }
    }
    closeOpened(before);
    return result;
}

std::vector<PendingSet> App::pendingSets(Document& ourTop)
{
    std::vector<PendingSet> out;
    TransactionLog* log = ourTop.getTransactionLog();
    if (!log)
        return out;
    log->flush();
    std::set<std::string> merged;
    std::vector<std::pair<LogTransaction, nlohmann::json>> imports;
    for (const auto& t : log->store().history(log->head())) {
        if (t.kind != "set")
            continue;
        const auto j = nlohmann::json::parse(t.script, nullptr, false);
        if (!j.is_object() || !j.contains("set") || !j["set"].is_object())
            continue;
        const auto& set = j["set"];
        if (set.value("merged", false)) {
            // A merge whose step was undone leaves the set pending (L4).
            if (!setMergeUndone(ourTop, set))
                merged.insert(set.value("id", std::string()));
        }
        else
            imports.emplace_back(t, set);
    }
    for (const auto& [t, set] : imports) {
        PendingSet p;
        p.when = t.time;
        auto& r = p.imported;
        r.id = set.value("id", std::string());
        if (merged.count(r.id))
            continue;
        r.name = set.value("name", std::string());
        r.theirTop = set.value("top", std::string());
        r.record = t.seq;
        for (const auto& e : set.value("files", nlohmann::json::array())) {
            SetImportedFile f;
            f.theirs = e.value("theirs", std::string());
            f.relative = e.value("relative", std::string());
            const std::string pairing = e.value("pairing", std::string());
            for (auto how : {SetPairing::History, SetPairing::SaveId, SetPairing::Manifest,
                             SetPairing::Objects, SetPairing::Uid, SetPairing::Path,
                             SetPairing::User}) {
                if (pairing == setPairingName(how))
                    f.pairing = how;
            }
            f.ourFile = e.value("our_file", std::string());
            f.branch = e.value("branch", std::string());
            f.held = e.value("held", int64_t(0));
            f.writeTo = e.value("write_to", std::string());
            f.error = e.value("error", std::string());
            const std::string action = e.value("action", std::string());
            for (auto a : {SetMergeAction::Merge, SetMergeAction::TakeTheirs, SetMergeAction::KeepOurs,
                           SetMergeAction::Write, SetMergeAction::Drop}) {
                if (action == setMergeActionName(a))
                    f.action = a;
            }
            r.files.push_back(std::move(f));
        }
        out.push_back(std::move(p));
    }
    return out;
}
