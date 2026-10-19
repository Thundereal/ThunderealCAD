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
# include <cctype>
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
#include "FileHistory.h"
#include "PropertyLinks.h"

FC_LOG_LEVEL_INIT("App", true, true)

using namespace App;

namespace
{
thread_local ExportSetPaths* _ExportSetPaths = nullptr;

QString qs(const std::string& s)
{
    return QString::fromUtf8(s.c_str());
}

std::string ss(const QString& s)
{
    return s.toUtf8().constData();
}

/// The documents `top`'s links reach, transitively, `top` first.
std::vector<Document*> closureOf(Document& top)
{
    std::vector<Document*> out {&top};
    std::set<Document*> seen {&top};
    for (std::size_t i = 0; i < out.size(); ++i) {
        // The map held: a range over an element of a temporary dangles.
        auto outList = PropertyXLink::getDocumentOutList(out[i]);
        for (auto doc : outList[out[i]]) {
            if (doc && seen.insert(doc).second)
                out.push_back(doc);
        }
    }
    return out;
}

/// A document of the set read in part (sec 35 L8, or a plain file opened
/// for links) is read again whole: a copy is of the whole file. False when
/// none was.
bool readWhole(const std::vector<Document*>& docs)
{
    bool any = false;
    for (auto doc : docs) {
        if (!doc->testStatus(Document::PartialDoc))
            continue;
        any = true;
        const std::string name = doc->FileName.getStrValue();
        FileHistory::NameParts parts;
        if (doc->testStatus(Document::VersionDoc) && FileHistory::parseName(name, parts)) {
            std::string reason;
            auto history = FileHistory::openFile(parts.file, &reason);
            if (!history)
                THROWM(Base::FileSystemError, "no history in '" + parts.file + "': " + reason);
            Document::openFileBranch(history, parts.branch, parts.version, false);
        }
        else {
            GetApplication().openDocument(name.c_str(), false);
        }
    }
    return any;
}

/// `<dir>/<stem><tail><ext>` of `rel`.
std::string withTail(const std::string& rel, const std::string& tail)
{
    const QFileInfo info(qs(rel));
    const QString dir = info.path();
    QString name = info.completeBaseName() + qs(tail);
    if (!info.suffix().isEmpty())
        name += QStringLiteral(".") + info.suffix();
    return ss(dir == QStringLiteral(".") ? name : dir + QStringLiteral("/") + name);
}

/// A name in a branch's or a version's file name: what a file system takes.
std::string fileSafe(const std::string& name)
{
    std::string out;
    for (char c : name)
        out += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') ? c : '_';
    return out;
}

}  // namespace

ExportSetPaths::ExportSetPaths(std::map<const Document*, std::string> files)
    : _files(std::move(files))
    , _outer(_ExportSetPaths)
{
    _ExportSetPaths = this;
}

ExportSetPaths::~ExportSetPaths()
{
    _ExportSetPaths = _outer;
}

void ExportSetPaths::writing(const Document* doc)
{
    if (_ExportSetPaths)
        _ExportSetPaths->_writing = doc;
}

std::string ExportSetPaths::pathTo(const Document* owner, const Document* target)
{
    auto scope = _ExportSetPaths;
    if (!scope || !owner || !target || scope->_writing != owner)
        return {};
    auto own = scope->_files.find(owner);
    auto it = scope->_files.find(target);
    if (own == scope->_files.end() || it == scope->_files.end())
        return {};
    return ss(QFileInfo(qs(own->second)).absoluteDir().relativeFilePath(qs(it->second)));
}

const char* App::exportSetManifestName()
{
    return "FreeCAD-set.json";
}

ExportSetResult App::exportLinkedSet(Document& top, const std::string& folder,
                                     const ExportSetOptions& options)
{
    // docs/TransactionLog.md sec 35 L7.
    const std::string topFile = top.FileName.getStrValue();
    FileHistory::NameParts topParts;
    if (topFile.empty())
        THROWM(Base::RuntimeError, "'" + std::string(top.Label.getValue())
                                       + "' has no file: save it before its set is exported");
    const std::string topPath = FileHistory::parseName(topFile, topParts) ? topParts.file : topFile;

    std::vector<Document*> docs = closureOf(top);
    for (int i = 0; i < 4 && readWhole(docs); ++i)
        docs = closureOf(top);
    for (auto doc : docs) {
        if (doc->testStatus(Document::PartialDoc))
            THROWM(Base::RuntimeError, "'" + std::string(doc->Label.getValue())
                                           + "' cannot be read whole");
        if (doc->FileName.getStrValue().empty())
            THROWM(Base::RuntimeError, "'" + std::string(doc->Label.getValue())
                                           + "' has no file: save it before the set is exported");
    }

    QDir out(qs(folder));
    if (out.exists() && !options.overwrite
            && !out.entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden).isEmpty())
        THROWM(Base::FileException, "the folder is not empty", folder.c_str());
    if (!out.mkpath(QStringLiteral(".")))
        THROWM(Base::FileException, "cannot make the folder", folder.c_str());

    // Q6, Q5 (sec 35.6): the layout under the top's folder kept; what lies
    // outside under _external/; a version or a branch a file of its own.
    const QDir topDir = QFileInfo(qs(topPath)).absoluteDir();
    std::map<const Document*, std::string> files;
    std::set<std::string> taken;
    ExportSetResult result;
    for (auto doc : docs) {
        ExportSetFile f;
        const std::string name = doc->FileName.getStrValue();
        FileHistory::NameParts parts;
        const bool named = doc->testStatus(Document::VersionDoc)
            && FileHistory::parseName(name, parts);
        f.file = named ? parts.file : name;
        const QString abs = QFileInfo(qs(f.file)).absoluteFilePath();
        QString rel = topDir.relativeFilePath(abs);
        if (rel.startsWith(QStringLiteral("..")) || QFileInfo(rel).isAbsolute())
            rel = QStringLiteral("_external/") + QFileInfo(abs).fileName();
        std::string path = ss(rel);
        if (named) {
            f.branch = parts.branch;
            f.version = parts.version;
            std::string tail;
            if (!parts.branch.empty())
                tail += "." + fileSafe(parts.branch);
            if (parts.version > 0)
                tail += ".v" + std::to_string(parts.version);
            path = withTail(path, tail);
        }
        const std::string wanted = path;
        for (int n = 2; taken.count(path); ++n)
            path = withTail(wanted, "~" + std::to_string(n));
        taken.insert(path);
        f.path = path;
        f.uid = doc->Uid.getValueStr();
        files[doc] = ss(out.absoluteFilePath(qs(path)));
        result.files.push_back(std::move(f));
    }

    {
        ExportSetPaths scope(files);
        for (std::size_t i = 0; i < docs.size(); ++i) {
            const std::string& dest = files[docs[i]];
            QDir().mkpath(QFileInfo(qs(dest)).absolutePath());
            ExportSetPaths::writing(docs[i]);
            if (!docs[i]->saveCopy(dest.c_str(), options.withHistory))
                THROWM(Base::FileException, "cannot write the copy", dest.c_str());
            FileHistory::Saved saved;
            if (FileHistory::savedAs(dest, saved))
                result.files[i].saveId = saved.saveId;
        }
    }

    // What pairs a file brought back with the one here (L3 item 3); not
    // needed by anyone who only opens the set.
    nlohmann::json j;
    j["format"] = 1;
    j["top"] = result.files.front().path;
    j["history"] = options.withHistory;
    auto list = nlohmann::json::array();
    for (const auto& f : result.files) {
        nlohmann::json e;
        e["path"] = f.path;
        e["file"] = f.file;
        if (!f.branch.empty())
            e["branch"] = f.branch;
        if (f.version > 0)
            e["version"] = f.version;
        e["save_id"] = f.saveId;
        e["uid"] = f.uid;
        list.push_back(std::move(e));
    }
    j["files"] = std::move(list);
    result.manifest = ss(out.absoluteFilePath(QString::fromLatin1(exportSetManifestName())));
    Base::ofstream manifest(Base::FileInfo(result.manifest), std::ios::out | std::ios::trunc);
    manifest << j.dump(2) << '\n';
    if (!manifest)
        THROWM(Base::FileException, "cannot write the manifest", result.manifest.c_str());
    FC_LOG("set of " << topFile << ": " << result.files.size() << " files to " << folder);
    return result;
}
