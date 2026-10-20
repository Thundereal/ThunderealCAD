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
#endif

#include <QDir>
#include <QFileInfo>

#include <nlohmann/json.hpp>

#include <Base/Console.h>
#include <Base/Exception.h>
#include <Base/FileInfo.h>
#include <Base/Stream.h>

#include "Application.h"
#include "Document.h"
#include "ExportSet.h"
#include "FileBlobManager.h"
#include "FileHistory.h"
#include "ImportSet.h"
#include "LibraryPaths.h"
#include "PropertyLinks.h"

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
