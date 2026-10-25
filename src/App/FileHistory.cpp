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
# include <cstring>
# include <iterator>
# include <map>
# include <mutex>
# include <sstream>
#endif

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>

#ifdef _WIN32
# include <windows.h>
#endif

#include <zipios++/zipios-config.h>
#include <zipios++/zipfile.h>

#include <Base/Console.h>
#include <Base/FileInfo.h>
#include <Base/Reader.h>
#include <Base/Uuid.h>

#include "FileHistory.h"
#include "Application.h"
#include "Document.h"
#include "FileBlobManager.h"
#include "TransactionLog.h"
#include "TransactionStore.h"

FC_LOG_LEVEL_INIT("App", true, true)

using namespace App;

namespace {

/// The registry: canonical path -> the history of that file. Main thread,
/// but guarded anyway: a history can die on whatever thread drops it last.
std::mutex& registryMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::map<std::string, std::weak_ptr<FileHistory>>& registry()
{
    static std::map<std::string, std::weak_ptr<FileHistory>> map;
    return map;
}

} // namespace

FileHistory::FileHistory(Document& home)
    : _home(&home)
    , _dir(home.TransientDir.getStrValue())
{}

FileHistory::FileHistory(const std::string& dir)
    : _dir(dir)
{}

std::shared_ptr<FileHistory> FileHistory::create(Document& home)
{
    return std::shared_ptr<FileHistory>(new FileHistory(home));
}

namespace {

/// What openFile() needs of a file's Document.xml: the document's own
/// properties, before any object's.
struct FileFacts
{
    int schema {0};
    std::string label;
    std::string version;        ///< "<num> <save id>"
    std::string lastModified;
    std::string lastModifiedBy;
    std::string historyDb;
    /// A folder's history, a file beside Document.xml (sec 38.9).
    std::string historyFile;
};

std::string unescapeXml(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out += s[i];
            continue;
        }
        static const std::pair<const char*, char> named[] = {
            {"&quot;", '"'}, {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&apos;", '\''}};
        bool done = false;
        for (const auto& n : named) {
            const size_t len = std::strlen(n.first);
            if (s.compare(i, len, n.first) == 0) {
                out += n.second;
                i += len - 1;
                done = true;
                break;
            }
        }
        if (!done)
            out += s[i];
    }
    return out;
}

/// The value of attribute `attr` of the first element after `from` in
/// `xml`, before `limit`; empty when there is none.
std::string attributeAfter(const std::string& xml, size_t from, size_t limit, const char* attr)
{
    const std::string key = std::string(attr) + "=\"";
    const size_t a = xml.find(key, from);
    if (a == std::string::npos || a >= limit)
        return {};
    const size_t start = a + key.size();
    const size_t end = xml.find('"', start);
    if (end == std::string::npos)
        return {};
    return unescapeXml(xml.substr(start, end - start));
}

bool readFacts(const std::string& xml, FileFacts& facts)
{
    // The document's own properties come first; an object's Label must not
    // be taken for the document's.
    size_t limit = xml.find("<Objects");
    if (limit == std::string::npos)
        limit = xml.size();
    // The root is FCDocument in this fork, Document upstream.
    size_t doc = xml.find("<FCDocument ");
    if (doc == std::string::npos)
        doc = xml.find("<Document ");
    if (doc == std::string::npos || doc >= limit)
        return false;
    const std::string schema = attributeAfter(xml, doc, xml.find('>', doc), "SchemaVersion");
    facts.schema = schema.empty() ? 0 : std::atoi(schema.c_str());
    auto property = [&](const char* name) -> size_t {
        const std::string key = std::string("<Property name=\"") + name + "\"";
        const size_t at = xml.find(key);
        return at < limit ? at : std::string::npos;
    };
    auto value = [&](const char* name) -> std::string {
        const size_t at = property(name);
        if (at == std::string::npos)
            return {};
        const size_t end = xml.find("</Property>", at);
        return attributeAfter(xml, at + 1, end, "value");
    };
    facts.label = value("Label");
    facts.version = value("Version");
    facts.lastModified = value("LastModifiedDate");
    facts.lastModifiedBy = value("LastModifiedBy");
    const size_t history = property("History");
    if (history != std::string::npos) {
        const size_t end = xml.find("</Property>", history);
        const size_t element = xml.find("<History", history);
        if (element != std::string::npos && element < end) {
            const size_t close = xml.find('>', element);
            facts.historyDb = attributeAfter(xml, element, close, "db");
            if (facts.historyDb.empty())
                facts.historyFile = attributeAfter(xml, element, close, "file");
        }
    }
    return true;
}

} // namespace

bool FileHistory::savedAs(const std::string& path, Saved& saved, std::string* reason)
{
    auto fail = [reason](const std::string& why) {
        if (reason)
            *reason = why;
        return false;
    };
    Base::FileInfo fi(path);
    if (!fi.exists())
        return fail("no such file");
    std::string docXml;
    if (fi.isDir()) {
        // A document saved as a folder (sec 29.8): its Document.xml is a
        // file there.
        QFile in(QString::fromStdString(path + "/Document.xml"));
        if (!in.open(QIODevice::ReadOnly))
            return fail("no Document.xml");
        docXml = in.readAll().toStdString();
    }
    else {
        try {
            zipios::ZipFile zip(path);
            if (!zip.isValid())
                return fail("not a document archive");
            std::unique_ptr<std::istream> in(zip.getInputStream("Document.xml"));
            if (!in)
                return fail("no Document.xml");
            std::ostringstream buffer;
            buffer << in->rdbuf();
            docXml = buffer.str();
        }
        catch (const std::exception& e) {
            return fail(std::string("cannot read the archive: ") + e.what());
        }
    }
    FileFacts facts;
    if (!readFacts(docXml, facts))
        return fail("Document.xml cannot be read");
    saved = Saved();
    std::istringstream version(facts.version);
    version >> saved.version >> saved.saveId;
    saved.modifiedBy = facts.lastModifiedBy;
    saved.hash = FileBlobManager::hashBytes(docXml);
    saved.history = !facts.historyDb.empty() || !facts.historyFile.empty();
    return true;
}

std::shared_ptr<FileHistory> FileHistory::openFile(const std::string& path, std::string* reason)
{
    auto fail = [reason](const std::string& why) {
        if (reason)
            *reason = why;
        return std::shared_ptr<FileHistory>();
    };
    if (auto open = find(path))
        return open;
    Base::FileInfo fi(path);
    if (!fi.exists())
        return fail("no such file");

    // Document.xml, and the other XML entries a version of it holds (split
    // object files, GuiDocument.xml): the file as found, for its version.
    // A document saved as a folder holds them as files (sec 29.8).
    TransactionLog::Entries entries;
    FileFacts facts;
    try {
        std::unique_ptr<zipios::ZipFile> zip;
        std::vector<std::string> names;
        if (fi.isDir()) {
            const QDir root(QString::fromStdString(path));
            QDirIterator it(root.path(), QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot,
                            QDirIterator::Subdirectories);
            while (it.hasNext())
                names.push_back(root.relativeFilePath(it.next()).toStdString());
        }
        else {
            zip = std::make_unique<zipios::ZipFile>(path);
            if (!zip->isValid())
                return fail("not a document archive");
            for (const auto& entry : zip->entries()) {
                if (!entry->isDirectory())
                    names.push_back(entry->getName());
            }
        }
        auto read = [&zip, &path](const std::string& name, std::string& out) {
            if (!zip) {
                QFile in(QString::fromStdString(path + "/" + name));
                if (!in.open(QIODevice::ReadOnly))
                    return false;
                out = in.readAll().toStdString();
                return true;
            }
            std::unique_ptr<std::istream> in(zip->getInputStream(name));
            if (!in)
                return false;
            std::ostringstream buffer;
            buffer << in->rdbuf();
            out = buffer.str();
            return true;
        };
        std::string docXml;
        if (!read("Document.xml", docXml))
            return fail("no Document.xml");
        if (!readFacts(docXml, facts))
            return fail("Document.xml cannot be read");
        if (facts.historyDb.empty() && (facts.historyFile.empty() || !fi.isDir()))
            return fail("the file carries no history");
        entries.emplace_back("Document.xml", std::move(docXml));
        const std::string prefix = FileBlobManager::archivePrefix();
        for (const auto& name : names) {
            // The string table is the file's, not a version's (sec 27.50):
            // read into the history's hasher below.
            if (name == "Document.xml" || name == Document::stringTableName()
                    || (!facts.historyFile.empty()
                        && name.compare(0, facts.historyFile.size(), facts.historyFile) == 0)
                    || name.compare(0, prefix.size(), prefix) == 0
                    || name.compare(0, 11, "thumbnails/") == 0)
                continue;
            std::string bytes;
            if (read(name, bytes))
                entries.emplace_back(name, std::move(bytes));
        }
    }
    catch (const std::exception& e) {
        return fail(std::string("cannot read the archive: ") + e.what());
    }

    // A directory named as a document's transient directory is, so that a
    // crash with only this history open is found by recovery too (sec 25).
    QCryptographicHash hash(QCryptographicHash::Sha1);
    hash.addData(QByteArray::fromStdString(path));
    std::ostringstream dir;
    dir << Application::getUserCachePath() << Application::getExecutableName() << "_Doc_"
        << Base::Uuid::createUuid() << "_" << hash.result().toHex().left(6).constData() << "_"
        << QCoreApplication::applicationPid();
    Base::FileInfo(dir.str()).createDirectories();
    std::shared_ptr<FileHistory> history(new FileHistory(dir.str()));
    if (!history->setPath(path))
        return find(path);
    history->_fileLabel = facts.label.empty() ? fi.fileNamePure() : facts.label;

    auto& blobs = history->blobs();
    if (!blobs.splitArchive(path))
        return fail("the archive's blobs cannot be read");
    struct EndRestore
    {
        FileBlobManager& blobs;
        ~EndRestore() { blobs.endRestore(); }
    } ending {blobs};
    // A folder's history is a file of its own (sec 38.9), an archive's a
    // blob.
    std::string dbPath;
    if (!facts.historyDb.empty()) {
        FileBlobHandle db = blobs.find(facts.historyDb);
        if (!db)
            return fail("the history's database is not in the archive");
        dbPath = db->path();
    }
    else {
        dbPath = path + "/" + facts.historyFile;
        if (!Base::FileInfo(dbPath).exists())
            return fail("the history's database is not in the folder");
    }

    // The guard of 16.4: the save that wrote the copy is the save the file
    // is from. When it is not, the copy's branches are kept closed and the
    // file as found starts a new main (16.6).
    bool matches = false;
    try {
        std::string saveId;
        {
            std::istringstream in(facts.version);
            int64_t num = 0;
            in >> num >> saveId;
        }
        auto copy = TransactionStore::openSQLite(dbPath, true);
        const std::string id = copy->getMeta("save_id");
        matches = !id.empty() && id == saveId && copy->getMeta("save_date") == facts.lastModified;
    }
    catch (const Base::Exception& e) {
        return fail(std::string("the history's database cannot be read: ") + e.what());
    }
    // The file's strings, before anything is read from its history: a
    // version carries none (sec 27.50 item 2).
    history->readTable(path);
    auto& core = TransactionLogCore::of(*history);
    if (!core.adoptEmbedded(dbPath))
        return fail("the history cannot be adopted");
    if (!matches) {
        FC_WARN("the embedded history of " << path
                << " is not the file's (edited elsewhere?): kept as closed branches");
        core.closeAdopted();
    }
    history->_fileVersion = core.recordFile(
        path, entries, TransactionLog::versionBlobs(entries, blobs.restoredEntries()), facts.schema);
    core.flush();
    return history;
}

bool FileHistory::readTable(const std::string& path)
{
    std::string bytes;
    if (Base::FileInfo(path).isDir()) {
        // A document saved as a folder (sec 29.8).
        QFile in(QString::fromStdString(path + "/" + Document::stringTableName()));
        if (!in.open(QIODevice::ReadOnly))
            return false;
        bytes = in.readAll().toStdString();
    }
    else {
        try {
            Base::ZipFileReader zip(path);
            if (!zip.hasEntry(Document::stringTableName()))
                return false;
            auto in = zip.openEntry(Document::stringTableName());
            if (!in)
                return false;
            bytes.assign(std::istreambuf_iterator<char>(*in), std::istreambuf_iterator<char>());
        }
        catch (...) {
            return false;
        }
    }
    StringHasherRef table(new StringHasher);
    try {
        std::istringstream in(bytes);
        table->restoreTable(in);
    }
    catch (const Base::Exception& e) {
        FC_ERR("the string table of " << path << " cannot be read: " << e.what());
        return false;
    }
    if (!_hasher)
        _hasher = table;
    else if (!_hasher->merge(*table)) {
        FC_WARN("the string table of " << path << " disagrees with the file's hasher");
        return false;
    }
    _tables.insert(FileBlobManager::hashBytes(bytes));
    return true;
}

FileHistory::~FileHistory()
{
    setPath(std::string());
    // What ~Document did for its own store: nothing deletes a file held
    // open on Windows, and a segment of the blob store may be; nor may the
    // store's worker write one meanwhile.
    try {
        // The log first: its worker writes into the store and the blob
        // segments until its queue is empty.
        _logCore.reset();
        if (_blobs)
            _blobs->shutdown();
        if (!_dir.empty())
            Base::FileInfo(_dir).deleteDirectoryRecursive();
    }
    catch (const Base::Exception& e) {
        FC_ERR("removing the history directory " << _dir << " failed: " << e.what());
    }
}

void FileHistory::releaseHome(const Document& doc)
{
    if (_home == &doc)
        _home = nullptr;
}

FileBlobManager& FileHistory::blobs()
{
    if (!_blobs)
        _blobs = std::make_unique<FileBlobManager>(this);
    return *_blobs;
}

#ifdef _WIN32
namespace
{
/// The long form of a path: QFileInfo::canonicalFilePath keeps an 8.3 short
/// name (C:/Users/LEI~1.ZHE/...), so the same file reached by its long name
/// had another key. A file that is not there yet: its folder expanded, its
/// name kept.
QString longName(const QString& path)
{
    const std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    DWORD n = GetLongPathNameW(native.c_str(), nullptr, 0);
    if (n) {
        std::wstring out(n, L'\0');
        n = GetLongPathNameW(native.c_str(), out.data(), n);
        if (n) {
            out.resize(n);
            return QDir::fromNativeSeparators(QString::fromStdWString(out));
        }
    }
    const QFileInfo info(path);
    const QString parent = info.absolutePath();
    if (parent == path || parent.isEmpty())
        return path;
    QString head = longName(parent);
    if (!head.endsWith(QLatin1Char('/')))
        head += QLatin1Char('/');
    return head + info.fileName();
}
}   // namespace
#endif

std::string FileHistory::canonicalPath(const std::string& path)
{
    if (path.empty())
        return path;
    QFileInfo info(QString::fromUtf8(path.c_str()));
    QString canonical = info.canonicalFilePath();
    if (canonical.isEmpty())
        canonical = QDir::cleanPath(info.absoluteFilePath());
#ifdef _WIN32
    canonical = longName(canonical);
#endif
    return canonical.toUtf8().constData();
}

bool FileHistory::setPath(const std::string& path)
{
    const std::string key = canonicalPath(path);
    std::lock_guard<std::mutex> lock(registryMutex());
    auto& map = registry();
    if (key == _path)
        return true;
    if (!key.empty()) {
        auto it = map.find(key);
        if (it != map.end()) {
            auto other = it->second.lock();
            if (other && other.get() != this) {
                FC_WARN("the file " << key << " has a history already; not registering another");
                return false;
            }
        }
    }
    if (!_path.empty()) {
        auto it = map.find(_path);
        if (it != map.end()) {
            auto other = it->second.lock();
            if (!other || other.get() == this)
                map.erase(it);
        }
    }
    _path = key;
    if (!key.empty()) {
        map[key] = weak_from_this();
    }
    return true;
}

namespace
{

bool allDigits(const std::string& s, size_t from)
{
    // Up to 18 digits: what an int64_t takes without a range check.
    if (from >= s.size() || s.size() - from > 18)
        return false;
    for (size_t i = from; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9')
            return false;
    }
    return true;
}

/// What follows the file: `@v<num>`, `@<branch>@v<num>` or `@<branch>@`.
bool parseTail(const std::string& tail, FileHistory::NameParts& parts)
{
    if (tail.size() < 2 || tail[0] != '@')
        return false;
    if (tail[1] == 'v' && allDigits(tail, 2)) {
        parts.branch.clear();
        parts.version = std::stoll(tail.substr(2));
        return parts.version > 0;
    }
    if (tail.back() == '@') {
        parts.branch = tail.substr(1, tail.size() - 2);
        parts.version = 0;
        return !parts.branch.empty();
    }
    const size_t at = tail.rfind("@v");
    if (at == 0 || at == std::string::npos || !allDigits(tail, at + 2))
        return false;
    parts.branch = tail.substr(1, at - 1);
    parts.version = std::stoll(tail.substr(at + 2));
    return !parts.branch.empty() && parts.version > 0;
}

} // namespace

bool FileHistory::parseName(const std::string& name, NameParts& parts)
{
    if (name.empty() || Base::FileInfo(name).exists())
        return false;
    // The longest prefix that is the file: a branch name may hold `@`.
    for (size_t at = name.rfind('@'); at != std::string::npos && at > 0;
         at = name.rfind('@', at - 1)) {
        const std::string file = name.substr(0, at);
        if (!Base::FileInfo(file).exists() && !find(file))
            continue;
        NameParts found;
        if (!parseTail(name.substr(at), found))
            return false;
        found.file = file;
        parts = std::move(found);
        return true;
    }
    // A file that is gone and has no history open: the frozen form only, as
    // before names carried a branch.
    const size_t at = name.rfind("@v");
    if (at == 0 || at == std::string::npos || !allDigits(name, at + 2))
        return false;
    NameParts found;
    found.file = name.substr(0, at);
    found.version = std::stoll(name.substr(at + 2));
    if (found.version <= 0)
        return false;
    parts = std::move(found);
    return true;
}

int64_t FileHistory::splitVersion(std::string& path)
{
    NameParts parts;
    if (!parseName(path, parts))
        return 0;
    path = parts.file;
    return parts.branch.empty() ? parts.version : -1;
}

bool FileHistory::findVersion(const std::string& path, int64_t num, const std::string& uuid,
                              std::shared_ptr<FileHistory>& history, std::string& reason)
{
    history = openFile(path, &reason);
    if (!history)
        return false;
    LogVersion version;
    if (!TransactionLogCore::of(*history).store().getVersion(num, version)) {
        reason = "it has no version " + std::to_string(num);
        return false;
    }
    if (!uuid.empty() && version.uuid != uuid) {
        reason = "its version " + std::to_string(num) + " is another history's";
        return false;
    }
    return true;
}

std::shared_ptr<FileHistory> FileHistory::find(const std::string& path)
{
    const std::string key = canonicalPath(path);
    if (key.empty())
        return {};
    std::lock_guard<std::mutex> lock(registryMutex());
    auto& map = registry();
    auto it = map.find(key);
    if (it == map.end())
        return {};
    auto history = it->second.lock();
    if (!history)
        map.erase(it);
    return history;
}
