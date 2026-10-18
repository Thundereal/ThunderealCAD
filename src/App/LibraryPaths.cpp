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
# include <algorithm>
# include <cctype>
# include <map>
# include <set>
# include <string>
#endif

#include <QDir>
#include <QFileInfo>

#include <Base/Console.h>
#include <Base/Exception.h>
#include <Base/Parameter.h>

#include "LibraryPaths.h"
#include "Application.h"
#include "Document.h"
#include "DocumentObject.h"
#include "PropertyLinks.h"

FC_LOG_LEVEL_INIT("App", true, true)

using namespace App;

namespace
{

const char* const SettingPath = "User parameter:BaseApp/Preferences/Document/LibraryPaths";

ParameterGrp::handle setting()
{
    return GetApplication().GetParameterGroupByPath(SettingPath);
}

QString toQ(const std::string& s)
{
    return QString::fromUtf8(s.c_str());
}

std::string fromQ(const QString& s)
{
    return s.toUtf8().constData();
}

/// For comparing: the platform's file names are case-insensitive on Windows.
QString comparable(const QString& path)
{
#ifdef FC_OS_WIN32
    return path.toLower();
#else
    return path;
#endif
}

/// The part of `path` under `folder`, both cleaned; empty when it is not
/// under it, on a whole path component (3.5).
QString under(const QString& path, const QString& folder)
{
    if (folder.isEmpty() || path.size() <= folder.size())
        return {};
    const QString p = comparable(path);
    QString f = comparable(folder);
    if (!f.endsWith(QLatin1Char('/')))
        f += QLatin1Char('/');
    if (!p.startsWith(f))
        return {};
    return path.mid(f.size());
}

/// `path` as written and canonical, cleaned; the second empty when it is the
/// first or does not exist.
std::pair<QString, QString> forms(const std::string& path)
{
    const QString clean = QDir::cleanPath(toQ(path));
    QString canonical = QFileInfo(clean).canonicalFilePath();
    if (canonical == clean)
        canonical.clear();
    return {clean, canonical};
}

/// The part of `path` under `folder`, as written or canonical.
QString underAny(const std::pair<QString, QString>& path, const std::string& folder)
{
    const auto f = forms(folder);
    QString rel = under(path.first, f.first);
    if (rel.isEmpty() && !path.second.isEmpty())
        rel = under(path.second, f.second.isEmpty() ? f.first : f.second);
    if (rel.isEmpty() && !f.second.isEmpty())
        rel = under(path.first, f.second);
    return rel;
}

bool isUnderAny(const std::string& path, const std::string& folder)
{
    return !path.empty() && !underAny(forms(path), folder).isEmpty();
}

/// The next free `<name>_<n>`, free in both maps (Q4).
std::string freeName(const std::string& name,
                     const LibraryPaths::Map& docMap,
                     const LibraryPaths::Map& sysMap)
{
    for (int i = 2;; ++i) {
        std::string candidate = name + "_" + std::to_string(i);
        if (!docMap.count(candidate) && !sysMap.count(candidate))
            return candidate;
    }
}

bool fileExists(const std::string& folder, const std::string& file)
{
    return !folder.empty() && QFileInfo(QDir(toQ(folder)).filePath(toQ(file))).exists();
}

}  // namespace

LibraryPaths::Map LibraryPaths::getSystem()
{
    Map ret;
    for (auto& v : setting()->GetASCIIMap()) {
        if (isValidName(v.first) && !v.second.empty())
            ret.emplace(v.first, v.second);
    }
    return ret;
}

void LibraryPaths::setSystem(const std::string& name, const std::string& folder)
{
    if (!isValidName(name))
        FC_THROWM(Base::ValueError, "Invalid library name '" << name << "'");
    setting()->SetASCII(name.c_str(), cleanFolder(folder));
}

void LibraryPaths::removeSystem(const std::string& name)
{
    setting()->RemoveASCII(name.c_str());
}

bool LibraryPaths::isValidName(const std::string& name)
{
    if (name.empty() || !std::isalpha(static_cast<unsigned char>(name[0])))
        return false;
    for (char c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
            return false;
    }
    return true;
}

std::string LibraryPaths::cleanFolder(const std::string& folder)
{
    QString path = toQ(folder).trimmed();
    if (path == QLatin1String("~") || path.startsWith(QLatin1String("~/")))
        path = QDir::homePath() + path.mid(1);
    path = QDir::cleanPath(path);
    if (path.isEmpty() || !QFileInfo(path).isAbsolute())
        FC_THROWM(Base::ValueError, "A library folder must be an absolute path: '" << folder << "'");
    return fromQ(path);
}

std::string LibraryPaths::makeToken(const std::string& name, const std::string& file)
{
    return "%" + name + "%/" + file;
}

bool LibraryPaths::parseToken(const std::string& path, std::string& name, std::string& file)
{
    if (path.size() < 4 || path[0] != '%')
        return false;
    auto end = path.find('%', 1);
    if (end == std::string::npos || end + 1 >= path.size() || path[end + 1] != '/')
        return false;
    std::string n = path.substr(1, end - 1);
    if (!isValidName(n))
        return false;
    std::string f = fromQ(QDir::cleanPath(toQ(path.substr(end + 2))));
    // Never climbs out of its folder (3.4 item 5).
    if (f.empty() || f == "." || f == ".." || f.rfind("../", 0) == 0
            || QFileInfo(toQ(f)).isAbsolute())
        return false;
    name = std::move(n);
    file = std::move(f);
    return true;
}

std::string LibraryPaths::resolveToken(const std::string& path, const Document* doc)
{
    std::string name, file;
    if (!parseToken(path, name, file))
        return path;
    Resolved r;
    if (resolve(name, file, doc ? doc->LibraryPaths.getValues() : Map(), getSystem(), r))
        return r.path;
    return {};
}

bool LibraryPaths::isUnder(const std::string& path, const std::string& folder)
{
    return isUnderAny(path, folder);
}

std::string LibraryPaths::normalize(const std::string& path)
{
    return fromQ(QDir::cleanPath(toQ(path)));
}

bool LibraryPaths::samePath(const std::string& a, const std::string& b)
{
    if (a.empty() || b.empty())
        return a == b;
    const auto fa = forms(a);
    const auto fb = forms(b);
    const QString ca = comparable(fa.first);
    const QString cb = comparable(fb.first);
    if (ca == cb)
        return true;
    const QString xa = comparable(fa.second.isEmpty() ? fa.first : fa.second);
    const QString xb = comparable(fb.second.isEmpty() ? fb.first : fb.second);
    return xa == xb;
}

bool LibraryPaths::match(const std::string& fullPath,
                         const Map& docMap,
                         const Map& sysMap,
                         const std::string& ownerFile,
                         Match& out)
{
    if (fullPath.empty() || fullPath.rfind("https://", 0) == 0)
        return false;
    const auto path = forms(fullPath);

    struct Candidate
    {
        Match m;
        int rank;  // 0 the document's, 1 the system's
    };
    std::vector<Candidate> candidates;
    for (auto& v : docMap) {
        const QString rel = underAny(path, v.second);
        if (rel.isEmpty())
            continue;
        Match m;
        m.name = v.first;
        m.folder = normalize(v.second);
        m.file = fromQ(rel);
        candidates.push_back({std::move(m), 0});
    }
    for (auto& v : sysMap) {
        const QString rel = underAny(path, v.second);
        if (rel.isEmpty())
            continue;
        // One folder under two names: the document's (3.4 item 3).
        bool known = false;
        for (auto& c : candidates) {
            if (c.rank == 0 && samePath(c.m.folder, v.second)) {
                known = true;
                break;
            }
        }
        if (known)
            continue;
        Match m;
        m.folder = normalize(v.second);
        m.file = fromQ(rel);
        m.added = true;
        auto it = docMap.find(v.first);
        if (it == docMap.end())
            m.name = v.first;
        else {
            // A name the document maps elsewhere: a new name for the
            // system's folder (Q4).
            m.name = freeName(v.first, docMap, sysMap);
            m.renamed = true;
        }
        candidates.push_back({std::move(m), 1});
    }
    if (candidates.empty())
        return false;

    // A document inside a library that holds the target links relatively
    // (Q5): the library is self-contained.
    if (!ownerFile.empty()) {
        for (auto& c : candidates) {
            if (isUnderAny(ownerFile, c.m.folder))
                return false;
        }
    }

    // The deepest folder wins: the shortest path under it. On a tie the
    // document's, else the first by name (the maps are ordered).
    const Candidate* best = nullptr;
    for (auto& c : candidates) {
        if (!best || c.m.file.size() < best->m.file.size()
                || (c.m.file.size() == best->m.file.size() && c.rank < best->rank)) {
            best = &c;
        }
    }
    out = best->m;
    return true;
}

bool LibraryPaths::resolve(const std::string& name,
                           const std::string& file,
                           const Map& docMap,
                           const Map& sysMap,
                           Resolved& out)
{
    if (name.empty() || file.empty())
        return false;
    auto itDoc = docMap.find(name);
    auto itSys = sysMap.find(name);
    const std::string docFolder = itDoc != docMap.end() ? normalize(itDoc->second) : std::string();
    const std::string sysFolder = itSys != sysMap.end() ? normalize(itSys->second) : std::string();
    out = Resolved();
    if (fileExists(docFolder, file)) {
        out.folder = docFolder;
        out.fromDocument = true;
        if (!sysFolder.empty() && !samePath(docFolder, sysFolder) && fileExists(sysFolder, file)) {
            out.conflict = true;
            out.other = sysFolder;
        }
    }
    else if (fileExists(sysFolder, file))
        out.folder = sysFolder;
    else
        return false;
    out.path = fromQ(QDir::cleanPath(QDir(toQ(out.folder)).filePath(toQ(file))));
    return true;
}

std::vector<PropertyXLink*> LibraryPaths::getLinks(const Document& doc)
{
    return PropertyXLink::getLibraryLinks(&doc);
}

LibraryPaths::Map LibraryPaths::getUsed(const Document& doc)
{
    Map ret;
    for (auto link : getLinks(doc)) {
        auto& folder = ret[link->getLibraryName()];
        if (folder.empty())
            folder = link->getLibraryFolder();
    }
    return ret;
}

void LibraryPaths::settle(Document& doc, bool prune, PropertyXLink* extra)
{
    auto links = getLinks(doc);
    if (extra && !extra->getLibraryName().empty()
            && std::find(links.begin(), links.end(), extra) == links.end())
        links.push_back(extra);

    const Map& map = doc.LibraryPaths.getValues();
    Map result = prune ? Map() : map;
    const Map sys = getSystem();

    // name -> the folders its links resolved through, in order met
    std::map<std::string, std::vector<std::string>> used;
    for (auto link : links) {
        auto& folders = used[link->getLibraryName()];
        const std::string& folder = link->getLibraryFolder();
        if (folder.empty())
            continue;
        bool known = false;
        for (auto& f : folders) {
            if (samePath(f, folder)) {
                known = true;
                break;
            }
        }
        if (!known)
            folders.push_back(folder);
    }

    for (auto& v : used) {
        const std::string& name = v.first;
        auto& folders = v.second;
        auto it = map.find(name);
        if (folders.empty()) {
            // Resolved through none here: what the document had stays.
            if (it != map.end())
                result[name] = it->second;
            continue;
        }
        // The folder the document names, if a link resolves through it.
        size_t primary = 0;
        if (it != map.end()) {
            for (size_t i = 0; i < folders.size(); ++i) {
                if (samePath(folders[i], it->second)) {
                    primary = i;
                    break;
                }
            }
        }
        result[name] = folders[primary];
        for (size_t i = 0; i < folders.size(); ++i) {
            if (i == primary)
                continue;
            // A name the document already has for this folder, else a new one.
            std::string other;
            for (auto& m : result) {
                if (m.first != name && samePath(m.second, folders[i])) {
                    other = m.first;
                    break;
                }
            }
            if (other.empty()) {
                Map taken = map;
                taken.insert(result.begin(), result.end());
                other = freeName(name, taken, sys);
                result[other] = folders[i];
            }
            FC_WARN(doc.Label.getValue() << ": library '" << name << "' resolved through "
                    << folders[primary] << " and " << folders[i] << "; the links through the "
                    "second are saved as '" << other << "'");
            for (auto link : links) {
                if (link->getLibraryName() == name && samePath(link->getLibraryFolder(), folders[i]))
                    link->renameLibrary(other);
            }
        }
    }
    if (result != map)
        doc.setLibraryPathsQuietly(std::move(result));
}

namespace
{

/// The links of `doc` the maps resolve to another file than they have, each
/// with where to.
std::vector<std::pair<PropertyXLink*, LibraryPaths::Resolved>>
changedLinks(const Document& doc, const LibraryPaths::Map& docMap, const LibraryPaths::Map& sysMap)
{
    std::vector<std::pair<PropertyXLink*, LibraryPaths::Resolved>> ret;
    for (auto link : LibraryPaths::getLinks(doc)) {
        LibraryPaths::Resolved r;
        if (!LibraryPaths::resolve(link->getLibraryName(), link->getLibraryFile(), docMap, sysMap, r))
            continue;
        if (!LibraryPaths::samePath(r.path, link->getFullPath()))
            ret.emplace_back(link, std::move(r));
    }
    return ret;
}

}  // namespace

std::vector<LibraryPaths::Affected> LibraryPaths::affected()
{
    std::vector<Affected> ret;
    const Map sys = getSystem();
    for (auto doc : GetApplication().getDocuments()) {
        const int n = static_cast<int>(changedLinks(*doc, doc->LibraryPaths.getValues(), sys).size());
        if (n)
            ret.push_back({doc, n});
    }
    return ret;
}

std::vector<LibraryPaths::Affected> LibraryPaths::changed(const Map& before)
{
    // What it affects as committed: the Gui's question may apply it.
    auto ret = affected();
    const Map after = getSystem();
    if (before != after)
        signalChanged()(before, after);
    return ret;
}

fastsignals::signal<void(const LibraryPaths::Map&, const LibraryPaths::Map&)>&
LibraryPaths::signalChanged()
{
    static fastsignals::signal<void(const Map&, const Map&)> signal;
    return signal;
}

int LibraryPaths::remap(Document& doc)
{
    auto links = changedLinks(doc, doc.LibraryPaths.getValues(), getSystem());
    for (auto& v : links)
        v.first->relinkFile(v.second.path, v.second.folder);
    return static_cast<int>(links.size());
}

namespace
{

/// Set the document's folders for `names` to the system's, as one undo step:
/// the remap follows from the change of the map.
int takeSystemFolders(Document& doc, const std::set<std::string>& names)
{
    const LibraryPaths::Map sys = LibraryPaths::getSystem();
    LibraryPaths::Map map = doc.LibraryPaths.getValues();
    bool changed = false;
    for (auto& name : names) {
        auto it = sys.find(name);
        if (it == sys.end())
            continue;
        auto& folder = map[name];
        if (folder != it->second) {
            folder = it->second;
            changed = true;
        }
    }
    if (!changed)
        return 0;
    const bool own = !doc.hasPendingTransaction();
    if (own)
        doc.openTransaction("Library paths");
    int count = 0;
    try {
        auto links = changedLinks(doc, map, sys);
        count = static_cast<int>(links.size());
        doc.LibraryPaths.setValues(std::move(map));
    }
    catch (...) {
        if (own)
            doc.abortTransaction();
        throw;
    }
    if (own)
        doc.commitTransaction();
    return count;
}

}  // namespace

std::vector<LibraryPaths::Problem> LibraryPaths::problems(const Document& doc)
{
    const Map& map = doc.LibraryPaths.getValues();
    const Map sys = getSystem();
    std::map<std::string, Problem> found;
    for (auto link : getLinks(doc)) {
        const std::string& name = link->getLibraryName();
        bool unresolved = link->getLibraryFolder().empty();
        if (!unresolved) {
            Resolved r;
            if (!resolve(name, link->getLibraryFile(), map, sys, r) || !r.conflict)
                continue;
        }
        auto& p = found[name];
        if (p.name.empty()) {
            p.name = name;
            auto it = map.find(name);
            if (it != map.end())
                p.documentFolder = it->second;
            auto itSys = sys.find(name);
            if (itSys != sys.end())
                p.systemFolder = itSys->second;
        }
        // A name with a link found nowhere is reported as that.
        p.unresolved = p.unresolved || unresolved;
        ++p.links;
    }
    std::vector<Problem> ret;
    for (auto& v : found)
        ret.push_back(std::move(v.second));
    return ret;
}

int LibraryPaths::apply(Document& doc)
{
    std::set<std::string> names;
    for (auto& v : changedLinks(doc, doc.LibraryPaths.getValues(), getSystem()))
        names.insert(v.first->getLibraryName());
    if (names.empty())
        return 0;
    return takeSystemFolders(doc, names);
}

int LibraryPaths::useSystemFolder(Document& doc, const std::string& name)
{
    return takeSystemFolders(doc, {name});
}
