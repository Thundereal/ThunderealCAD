/****************************************************************************
 *   Copyright (c) 2018 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
 *                                                                          *
 *   This file is part of the FreeCAD CAx development system.               *
 *                                                                          *
 *   This library is free software; you can redistribute it and/or          *
 *   modify it under the terms of the GNU Library General Public            *
 *   License as published by the Free Software Foundation; either           *
 *   version 2 of the License, or (at your option) any later version.       *
 *                                                                          *
 *   This library  is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of         *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the          *
 *   GNU Library General Public License for more details.                   *
 *                                                                          *
 *   You should have received a copy of the GNU Library General Public      *
 *   License along with this library; see the file COPYING.LIB. If not,     *
 *   write to the Free Software Foundation, Inc., 59 Temple Place,          *
 *   Suite 330, Boston, MA  02111-1307, USA                                 *
 *                                                                          *
 ****************************************************************************/


#include "PreCompiled.h"

#ifndef _PreComp_
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cassert>
#include <deque>
#include <limits>
#include <set>
#include <unordered_set>
#include <shared_mutex>
#include <sstream>
#include <vector>
#include <boost/io/ios_state.hpp>
#include <boost/iostreams/device/array.hpp>
#include <boost/iostreams/stream.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/bimap.hpp>
#include <boost/bimap/unordered_set_of.hpp>
#include <boost/bimap/unordered_multiset_of.hpp>
#include <boost/bimap/set_of.hpp>
#include <QHash>
#include <QCryptographicHash>
#include <Base/Console.h>
#include <Base/Exception.h>
#include <Base/Writer.h>
#include <Base/Reader.h>
#include <App/Application.h>
#include <App/StringHasher.h>
#include <App/StringHasherPy.h>
#include <App/StringIDPy.h>
#include <App/MappedElement.h>
#include <App/ElementNamingUtils.h>
#include <App/DocumentParams.h>
#include <App/TransactionValue.h>

FC_LOG_LEVEL_INIT("App",true,true)

namespace bio = boost::iostreams;
using namespace App;

///////////////////////////////////////////////////////////

struct StringIDHasher {
    std::size_t operator()(const StringID *sid) const {
        if (!sid)
            return 0;
#if QT_VERSION  >= 0x050000
        return qHash(sid->data(), qHash(sid->postfix()));
#else
        return qHash(sid->data()) ^ qHash(sid->postfix());
#endif
    }

    bool operator()(const StringID *a, const StringID *b) const {
        if (a == b)
            return true;
        if (!a || !b)
            return false;
        return a->data() == b->data() && a->postfix() == b->postfix();
    }
};

// The string side is a multiset (docs/TransactionLogJournal.md sec 27.41 Q2): a
// table shared by the documents of one file can come to hold one string
// under two ids -- dropped from the saved state, minted again after a
// reopen, then brought back under its old id by an old version. Both ids
// stay valid; a lookup by string finds either.
typedef boost::bimap<
            boost::bimaps::unordered_multiset_of<StringID*,
                                                 StringIDHasher,
                                                 StringIDHasher>,
            boost::bimaps::set_of<long> >
            HashMapBase;

class StringHasher::HashMap: public HashMapBase 
{
public:
    bool SaveAll = false;
    int Threshold = 0;
    bool IndexedNames = false;
    /// Restored without a mode: inferIndexedNames() decides once the table is in
    bool InferIndexed = false;
    /// The last id handed out or read (sec 27.40 item 2), which a save
    /// writes and a restore reads back: ids the saved state no longer
    /// uses are not handed out again.
    long LastID = 0;
    /// Where new ids come from in a worker (docs/WorkerProcess.md sec 15)
    std::shared_ptr<StringIDLease> Lease;
    /// The lowest id takeRows() put in below one held, not yet told
    long LowTaken = 0;
    /// New strings spelled by their digests (docs/StringTableContentIds.md)
    bool ContentIds = false;
    /// The strings by the number names spell them by, in a table that spells
    /// digests (one content may be under two ids: one spelling)
    std::unordered_multimap<std::int64_t, StringID*> Spelled;
    /// The contested spellings, each its owner's digest (sec 3.2):
    /// append-only, kept through clear() as the mode is
    std::map<std::int64_t, StringID::Digest> Registry;
    /// The other ids of a content held once, in a table that spells digests
    /// (docs/StringTableContentIds.md sec 15): each the entry it stands for,
    /// whose own id is lower than any of them
    std::map<long, StringID*> Aliases;
    /// The same, by the entry
    std::unordered_multimap<const StringID*, long> AliasIds;

    /// The entries, the spellings and the other ids; the mode and the
    /// registry stay
    void clear()
    {
        HashMapBase::clear();
        Spelled.clear();
        Aliases.clear();
        AliasIds.clear();
    }
    /// The entry `id` is, or stands for; null for none
    StringID* entry(long id) const
    {
        auto it = right.find(id);
        if (it != right.end()) {
            return it->second;
        }
        auto alias = Aliases.find(id);
        return alias == Aliases.end() ? nullptr : alias->second;
    }
    void addAlias(long id, StringID* sid)
    {
        Aliases[id] = sid;
        AliasIds.emplace(sid, id);
    }
    void dropAlias(long id)
    {
        auto it = Aliases.find(id);
        if (it == Aliases.end()) {
            return;
        }
        auto range = AliasIds.equal_range(it->second);
        for (auto at = range.first; at != range.second; ++at) {
            if (at->second == id) {
                AliasIds.erase(at);
                break;
            }
        }
        Aliases.erase(it);
    }
    /// The other ids of `sid` go with it, each into `gone` when given
    void dropAliasesOf(const StringID* sid, std::vector<long>* gone = nullptr)
    {
        if (AliasIds.empty()) {
            return;
        }
        auto range = AliasIds.equal_range(sid);
        for (auto at = range.first; at != range.second; ++at) {
            Aliases.erase(at->second);
            if (gone) {
                gone->push_back(at->second);
            }
        }
        AliasIds.erase(range.first, range.second);
    }
    void forget(const StringID* sid)
    {
        if (!ContentIds) {
            return;
        }
        auto range = Spelled.equal_range(sid->spelled());
        for (auto it = range.first; it != range.second; ++it) {
            if (it->second == sid) {
                Spelled.erase(it);
                return;
            }
        }
    }
    /// Counts the changes to the entries, which is what the cached hash of
    /// the whole table (contentHash()) is valid for.
    uint64_t Revision = 0;
    /// Many readers or one writer, of any thread (docs/TransactionLogJournal.md sec
    /// 31.30): taken through TableLock by every function that reads or
    /// changes the entries.
    mutable std::shared_mutex mutex;
};

namespace {

/// What this thread holds of one table
struct HeldTable
{
    const std::shared_mutex* mutex;
    int readers;
    int writers;
    bool exclusive;
};
thread_local std::vector<HeldTable> heldTables;

/** A table's lock for one function: shared to read, exclusive to change.
 * The functions of the table call each other, so it may be taken again by
 * the thread that has it: to read under either, to write under a write.
 * To write under a read is a fault of the function that took the read --
 * what it looked up may not be there once the lock was let go to take the
 * other -- and is asserted; every function that may come to add a string
 * takes the write from its start. The one that only may, the lookup that
 * adds what it does not find, takes them one after the other and looks
 * again.
 */
class TableLock
{
public:
    enum Mode { Read, Write };

    template<class Table>
    TableLock(const Table& table, Mode mode)
        : _mutex(table.mutex)
        , _mode(mode)
    {
        HeldTable* held = find();
        if (!held) {
            if (mode == Write)
                _mutex.lock();
            else
                _mutex.lock_shared();
            heldTables.push_back({&_mutex, mode == Read ? 1 : 0, mode == Write ? 1 : 0,
                                  mode == Write});
            return;
        }
        if (mode == Write && !held->exclusive) {
            assert(!"string table: a write taken under a read");
            _mutex.unlock_shared();
            _mutex.lock();
            held->exclusive = true;
            _raised = true;
        }
        ++(mode == Read ? held->readers : held->writers);
    }

    ~TableLock()
    {
        HeldTable* held = find();
        --(_mode == Read ? held->readers : held->writers);
        if (_raised) {
            _mutex.unlock();
            _mutex.lock_shared();
            held->exclusive = false;
            return;
        }
        if (held->readers == 0 && held->writers == 0) {
            if (held->exclusive)
                _mutex.unlock();
            else
                _mutex.unlock_shared();
            heldTables.erase(heldTables.begin() + (held - heldTables.data()));
        }
    }

    TableLock(const TableLock&) = delete;
    TableLock& operator=(const TableLock&) = delete;

    /// Whether this thread has the table for writing already
    template<class Table>
    static bool writing(const Table& table)
    {
        for (const auto& held : heldTables) {
            if (held.mutex == &table.mutex)
                return held.exclusive;
        }
        return false;
    }

private:
    HeldTable* find() const
    {
        for (auto& held : heldTables) {
            if (held.mutex == &_mutex)
                return &held;
        }
        return nullptr;
    }

    std::shared_mutex& _mutex;
    Mode _mode;
    bool _raised {false};
};

} // namespace

///////////////////////////////////////////////////////////

TYPESYSTEM_SOURCE_ABSTRACT(App::StringID, Base::BaseClass)

StringID::~StringID()
{
    if (_hasher) {
        _hasher->_hashes->forget(this);
        _hasher->_hashes->dropAliasesOf(this);
        _hasher->_hashes->right.erase(_id);
        ++_hasher->_hashes->Revision;
    }
}

PyObject *StringID::getPyObject() {
    return new StringIDPy(this);
}

PyObject *StringID::getPyObjectWithIndex(int index) {
    auto res = new StringIDPy(this);
    res->_index = index;
    return res;
}

namespace {

/// A reference in a string's canonical content: 0x01 and the digest of the
/// string referred to (docs/StringTableContentIds.md sec 3.1)
void appendReference(QByteArray& out, const StringIDRef& ref)
{
    out.append('\x01');
    const StringID::Digest& digest = ref.deref().digest();
    out.append(reinterpret_cast<const char*>(digest.data()), static_cast<int>(digest.size()));
}

int hexDigit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/// `text` with every id spelled in it (`#` and hex digits) that names a
/// string of `sids` replaced by that string's reference; a `#` naming none
/// is left as it is
QByteArray replaceSpelledIDs(const QByteArray& text, const QVector<StringIDRef>& sids)
{
    if (sids.isEmpty() || text.indexOf('#') < 0) {
        return text;
    }
    QByteArray out;
    out.reserve(text.size() + 32);
    const int n = text.size();
    int i = 0;
    while (i < n) {
        if (text[i] == '#') {
            int j = i + 1;
            long long value = 0;
            int d = 0;
            while (j < n && j - i <= 16 && (d = hexDigit(text[j])) >= 0) {
                value = value * 16 + d;
                ++j;
            }
            if (j > i + 1) {
                const StringIDRef* found = nullptr;
                for (const auto& sid : sids) {
                    if (sid && sid.spelled() == value) {
                        found = &sid;
                        break;
                    }
                }
                if (found) {
                    appendReference(out, *found);
                    i = j;
                    continue;
                }
            }
        }
        out.append(text[i]);
        ++i;
    }
    return out;
}

void appendSized(QCryptographicHash& hash, const QByteArray& bytes)
{
    const auto size = static_cast<std::uint32_t>(bytes.size());
    const char length[4] = {static_cast<char>(size & 0xff), static_cast<char>((size >> 8) & 0xff),
                            static_cast<char>((size >> 16) & 0xff),
                            static_cast<char>((size >> 24) & 0xff)};
    hash.addData(QByteArrayView(length, 4));
    hash.addData(bytes);
}

}  // namespace

/** The digest of a string's canonical content, version 1
 * (docs/StringTableContentIds.md sec 3.1): its structural references by the
 * digests of the strings referred to, the ids spelled in its text likewise
 * -- the same for a string made here, read back from a stream (whose
 * encoded postfix holds the text, not the id) or made by another table under
 * other ids.
 */
static StringID::Digest digestOf(const QVector<StringIDRef>& sids, const QByteArray& dataBytes,
                                 const QByteArray& postfixBytes, bool binary, bool hashed,
                                 bool postfixEncoded, bool prefixRef, bool prefixIndex)
{
    const int offset = postfixEncoded ? 1 : 0;
    QByteArray data;
    if (prefixRef && sids.size() > offset && sids[offset]) {
        appendReference(data, sids[offset]);
        if (prefixIndex) {
            data.append(':');
        }
    }
    else if (binary || hashed) {
        data = dataBytes;
    }
    else {
        data = replaceSpelledIDs(dataBytes, sids);
    }
    QByteArray postfix;
    if (postfixEncoded && !sids.isEmpty() && sids[0]) {
        appendReference(postfix, sids[0]);
    }
    else {
        postfix = replaceSpelledIDs(postfixBytes, sids);
    }
    QCryptographicHash hash(QCryptographicHash::Blake2s_128);
    hash.addData(QByteArrayView("FCSID1\0", 7));
    const char kind = static_cast<char>((binary ? 1 : 0) | (hashed ? 2 : 0));
    hash.addData(QByteArrayView(&kind, 1));
    appendSized(hash, data);
    appendSized(hash, postfix);
    const QByteArray result = hash.result();
    StringID::Digest digest {};
    for (std::size_t k = 0; k < digest.size() && k < static_cast<std::size_t>(result.size()); ++k) {
        digest[k] = static_cast<std::uint8_t>(result[static_cast<int>(k)]);
    }
    return digest;
}

namespace {

/// The first spelling level's bits (docs/StringTableContentIds.md sec 3.2);
/// a test makes it small to have collisions
int spellingBits = 32;

/** The spelling of `digest` at `level`: the first `spellingBits` bits of it
 * at level 0; above, 52 bits of the digest of it and the level, marked by
 * bit 60 and the level so that no two levels share a number. Never 0.
 */
std::int64_t spellingAt(const StringID::Digest& digest, int level)
{
    if (level == 0) {
        std::uint64_t prefix = 0;
        for (int k = 0; k < 8; ++k) {
            prefix = (prefix << 8) | digest[static_cast<std::size_t>(k)];
        }
        prefix >>= 64 - spellingBits;
        return static_cast<std::int64_t>(prefix);
    }
    QCryptographicHash hash(QCryptographicHash::Blake2s_128);
    hash.addData(QByteArrayView(reinterpret_cast<const char*>(digest.data()),
                                static_cast<qsizetype>(digest.size())));
    const char byte = static_cast<char>(level);
    hash.addData(QByteArrayView(&byte, 1));
    const QByteArray more = hash.result();
    std::uint64_t bits = 0;
    for (int k = 0; k < 8; ++k) {
        bits = (bits << 8) | static_cast<std::uint8_t>(more[k]);
    }
    bits &= (std::uint64_t(1) << 52) - 1;
    return static_cast<std::int64_t>((std::uint64_t(1) << 60) | (std::uint64_t(level) << 52) | bits);
}

}  // namespace

namespace {

/// The registry as an attribute: `<hex spelling>:<hex digest>`, space apart
std::string registryText(const std::map<std::int64_t, StringID::Digest>& registry)
{
    static const char hex[] = "0123456789abcdef";
    std::ostringstream out;
    bool first = true;
    for (const auto& [spelled, digest] : registry) {
        if (!first) {
            out << ' ';
        }
        first = false;
        out << std::hex << spelled << ':';
        for (auto byte : digest) {
            out << hex[byte >> 4] << hex[byte & 0xf];
        }
    }
    return out.str();
}

void readRegistry(const std::string& text, std::map<std::int64_t, StringID::Digest>& registry)
{
    std::istringstream in(text);
    std::string entry;
    while (in >> entry) {
        const auto colon = entry.find(':');
        if (colon == std::string::npos || entry.size() != colon + 33) {
            continue;
        }
        StringID::Digest digest {};
        bool ok = true;
        for (std::size_t k = 0; k < digest.size() && ok; ++k) {
            const int hi = hexDigit(entry[colon + 1 + 2 * k]);
            const int lo = hexDigit(entry[colon + 2 + 2 * k]);
            ok = hi >= 0 && lo >= 0;
            digest[k] = static_cast<std::uint8_t>(hi * 16 + lo);
        }
        if (ok) {
            registry.emplace(std::stoll(entry.substr(0, colon), nullptr, 16), digest);
        }
    }
}

}  // namespace

std::string StringID::digestText() const
{
    static const char hex[] = "0123456789abcdef";
    std::string text;
    text.reserve(32);
    for (auto byte : _digest) {
        text += hex[byte >> 4];
        text += hex[byte & 0xf];
    }
    return text;
}

std::string StringID::toString(int index) const {
    std::ostringstream ss;
    ss << '#' << std::hex << spelled();
    if (index)
        ss << ':' << index;
    return ss.str();
}

StringID::IndexID StringID::fromString(const char *name, bool eof, int size) {
    IndexID res;
    res.id = 0;
    res.index = 0;
    if (!name) {
        res.id = -1;
        return res;
    }
    if (size < 0)
        size = std::strlen(name);
    bio::stream<bio::array_source> iss(name, size);
    char sep = 0;
    char sep2 = 0;
    iss >> sep >> std::hex >> res.id >> sep2 >> res.index;
    if((eof && !iss.eof()) || sep!='#' || (sep2!=0 && sep2!=':')) {
        res.id = -1;
        return res;
    }
    return res;
}

std::string StringID::dataToText(int index) const {
    if(isHashed() || isBinary())
        return _data.toBase64().constData();

    std::string res(_data.constData());
    if (index) 
        res += std::to_string(index);
    if (_postfix.size())
        res += _postfix.constData();
    return res;
}

void StringID::mark() const
{
    if (isMarked())
        return;
    _flags.setFlag(Flag::Marked);
    for (auto & sid : _sids)
        sid.deref().mark();
}

///////////////////////////////////////////////////////////

TYPESYSTEM_SOURCE(App::StringHasher, Base::Persistence)

StringHasher::StringHasher()
    :_hashes(new HashMap)
{
    static std::atomic<std::uint64_t> serials {0};
    _serial = ++serials;
    _hashes->IndexedNames = DocumentParams::getHashIndexedName();
    _hashes->ContentIds = GetApplication()
                              .GetParameterGroupByPath("User parameter:BaseApp/Preferences/Document")
                              ->GetBool("ContentStringIds", true);
}

StringHasher::~StringHasher() {
    clear();
}

void StringHasher::setSaveAll(bool enable) {
    TableLock lock(*_hashes, TableLock::Write);
    // No compaction here: what may go depends on what the file's history
    // still uses, which only the document knows (docs/TransactionLogJournal.md sec
    // 27.50 item 4). The next save compacts.
    _hashes->SaveAll = enable;
}

std::vector<long> StringHasher::compact(const std::function<bool(long)>& keep)
{
    TableLock lock(*_hashes, TableLock::Write);
    std::vector<long> dropped;
    if (_hashes->SaveAll)
        return dropped;

    // What goes is not handed out again (sec 27.40 item 2).
    _hashes->LastID = lastID();
    // Kept: persistent, or used by what memory does not hold (sec 27.50 item
    // 4). What a kept string is built from stays with it: the kept one holds
    // a reference to each, so none of them falls to one reference below.
    auto kept = [&keep, this](const StringID& sid) {
        if (sid.isPersistent() || (keep && keep(sid.value()))) {
            return true;
        }
        if (keep && !_hashes->AliasIds.empty()) {
            // Used under another of its ids
            auto range = _hashes->AliasIds.equal_range(&sid);
            for (auto it = range.first; it != range.second; ++it) {
                if (keep(it->second)) {
                    return true;
                }
            }
        }
        return false;
    };
    std::deque<StringIDRef> pendings;
    for (auto & v : _hashes->right) {
        if (!kept(*v.second) && v.second->getRefCount() == 1)
            pendings.emplace_back(v.second);
    }
    while(pendings.size()) {
        StringIDRef sid = pendings.front();
        pendings.pop_front();
        _hashes->forget(sid._sid);
        if (!_hashes->right.erase(sid.value()))
            continue;
        ++_hashes->Revision;
        dropped.push_back(sid.value());
        _hashes->dropAliasesOf(sid._sid, &dropped);
        sid._sid->_hasher = nullptr;
        sid._sid->unref();
        // What the dropped string was built from goes with it once nothing
        // else holds it: the table and this string alone. The string holds
        // one reference for each time its list names the other, and a list
        // of ten or less may name one twice (getID() sorts out the longer
        // ones only), which a count of two never met: it stayed for good.
        const auto & related = sid._sid->_sids;
        for (int i = 0; i < related.size(); ++i) {
            const StringID *child = related[i]._sid;
            if (child->_hasher != this || kept(*child))
                continue;
            int held = 0;
            for (int j = 0; j < related.size(); ++j) {
                if (related[j]._sid != child)
                    continue;
                if (j < i) {
                    // counted where the list named it first
                    held = 0;
                    break;
                }
                ++held;
            }
            if (held && child->getRefCount() == 1 + held)
                pendings.push_back(related[i]);
        }
    }
    // Another id of a string that stays is an id no one here holds: it
    // goes unless what memory does not hold uses it (sec 15)
    for (auto it = _hashes->Aliases.begin(); it != _hashes->Aliases.end();) {
        const long id = it->first;
        ++it;
        if (!keep || !keep(id)) {
            _hashes->dropAlias(id);
            dropped.push_back(id);
        }
    }
    std::sort(dropped.begin(), dropped.end());
    return dropped;
}

std::string StringHasher::saveTable() const
{
    TableLock lock(*_hashes, TableLock::Write);
    std::ostringstream out;
    out << "StringTableStart v1 " << size() << '\n';
    saveStream(out, true);
    if (_hashes->ContentIds) {
        // The mode and the registry after the rows, where an older build,
        // which reads its count of rows and no further, does not look: the
        // table stays one it reads (docs/StringTableContentIds.md sec 11)
        out << "StringTableContent " << _hashes->Registry.size() << '\n';
        for (const auto& [spelled, digest] : _hashes->Registry) {
            std::map<std::int64_t, StringID::Digest> one {{spelled, digest}};
            out << registryText(one) << '\n';
        }
    }
    std::string bytes = out.str();
    _contentHash = QCryptographicHash::hash(QByteArray::fromRawData(bytes.data(),
                                                                     static_cast<int>(bytes.size())),
                                            QCryptographicHash::Sha1)
                       .toHex()
                       .toStdString();
    _hashRevision = _hashes->Revision;
    return bytes;
}

const std::string& StringHasher::contentHash() const
{
    TableLock lock(*_hashes, TableLock::Write);
    if (_contentHash.empty() || _hashRevision != _hashes->Revision)
        saveTable();
    return _contentHash;
}

void StringHasher::restoreTable(std::istream& stream)
{
    TableLock lock(*_hashes, TableLock::Write);
    // The last id stays what the element that named the table said.
    const long last = _hashes->LastID;
    clear();
    _hashes->LastID = last;
    std::string marker;
    stream >> marker;
    if (marker == "StringTableStart") {
        std::string ver;
        std::size_t count = 0;
        stream >> ver >> count;
        if (ver != "v1") {
            FC_WARN("Unknown string table format");
        }
        restoreStreamNew(stream, count);
        // A table that spells digests says so after its rows, with its
        // registry: read so, it spells what it holds by them, in the order
        // they were made -- as it spelled them then
        std::string trailer;
        if (stream >> trailer && trailer == "StringTableContent") {
            std::size_t entries = 0;
            stream >> entries;
            std::map<std::int64_t, StringID::Digest> registry;
            for (std::size_t k = 0; k < entries; ++k) {
                std::string entry;
                if (!(stream >> entry)) {
                    break;
                }
                readRegistry(entry, registry);
            }
            for (const auto& entry : registry) {
                _hashes->Registry.insert(entry);
            }
            if (!_hashes->ContentIds) {
                setContentIds(true);
            }
        }
        else if (_hashes->ContentIds) {
            // None: it counts its ids, whatever the hasher read into was made
            // to spell new strings by -- the transaction log's table of an
            // old file (FileHistory::readTable) is read into one made for it
            // (docs/StringTableContentIds.md sec 12)
            setContentIds(false);
        }
        return;
    }
    // A count alone: the older private tables (<shape>.Table), which count
    // their ids
    restoreStream(stream, std::strtoul(marker.c_str(), nullptr, 10));
    if (_hashes->ContentIds) {
        setContentIds(false);
    }
}

std::vector<StringHasher::Row> StringHasher::rows(long after,
                                                  const std::function<bool(long)>& want) const
{
    TableLock lock(*_hashes, TableLock::Read);
    std::vector<Row> out;
    // The other ids of a content as rows of their own (sec 15): what was
    // written under one is read back where the table is filled from rows
    auto alias = _hashes->Aliases.upper_bound(after);
    auto others = [&](long before) {
        for (; alias != _hashes->Aliases.end() && alias->first < before; ++alias) {
            if (!want || want(alias->first))
                out.push_back(rowOf(*alias->second, alias->first));
        }
    };
    for (auto it = _hashes->right.upper_bound(after); it != _hashes->right.end(); ++it) {
        const StringID& d = *it->second;
        others(d._id);
        if (want && !want(d._id))
            continue;
        out.push_back(rowOf(d, d._id));
    }
    others(std::numeric_limits<long>::max());
    return out;
}

StringHasher::Row StringHasher::rowOf(const StringID& d, long id)
{
    Row row;
    row.id = id;
    auto flags = d._flags;
    flags.setFlag(StringID::Flag::Marked, false);
    row.flags = static_cast<int>(flags.toUnderlyingType());
    row.sids.reserve(d._sids.size());
    for (const auto& sid : d._sids)
        row.sids.emplace_back(sid.value(), sid.getIndex());
    row.data = d._data;
    row.postfix = d._postfix;
    return row;
}

bool StringHasher::isRow(const StringID& d, const Row& row, const std::set<long>* coming) const
{
    // Whether it is marked or kept for good is not what a string is: one
    // content held once takes the mark of either id it came under
    constexpr auto kept = ~(static_cast<int>(StringID::Flag::Marked)
                            | static_cast<int>(StringID::Flag::Persistent));
    if (d._data != row.data || d._postfix != row.postfix
        || (static_cast<int>(d._flags.toUnderlyingType()) & kept) != (row.flags & kept)
        || d._sids.size() != static_cast<int>(row.sids.size())) {
        return false;
    }
    for (int i = 0; i < d._sids.size(); ++i) {
        // By the entry: an id the row names may be another of its ids --
        // one let go since (compact()), when it comes with the row again
        const StringID* named = _hashes->entry(row.sids[i].first);
        if (d._sids[i].getIndex() != row.sids[i].second
            || (named ? named != d._sids[i]._sid
                      : !(coming && coming->count(row.sids[i].first)))) {
            return false;
        }
    }
    return true;
}

StringID* StringHasher::insertOrAlias(const StringIDRef& sid)
{
    TableLock lock(*_hashes, TableLock::Write);
    if (!_hashes->ContentIds) {
        return insert(sid);
    }
    StringID& d = *sid._sid;
    const StringID::Digest digest = digestOf(
        d._sids, d._data, d._postfix, d.isBinary(), d.isHashed(), d.isPostfixEncoded(),
        d.isIndexed() || d.isPrefixID() || d.isPrefixIDIndex(), d.isPrefixIDIndex());
    StringID* held = nullptr;
    auto range = _hashes->left.equal_range(&d);
    for (auto it = range.first; it != range.second; ++it) {
        if (it->first->_digest == digest && (!held || it->first->_id < held->_id)) {
            held = it->first;
        }
    }
    if (!held) {
        return insert(sid);
    }
    // One content, held once (sec 15): the id coming in is another id of the
    // entry here. The entry keeps the lower of the two, which is the older
    // -- what names are ordered by (ElementNameOrder) -- and what a save
    // writes; the other stays an id that reads as the entry.
    if (d.isPersistent()) {
        held->_flags.setFlag(StringID::Flag::Persistent);
    }
    ++_hashes->Revision;
    if (d._id > held->_id) {
        _hashes->addAlias(d._id, held);
        return held;
    }
    const long old = held->_id;
    _hashes->right.erase(old);
    held->_id = d._id;
    _hashes->right.insert(HashMap::right_map::value_type(held->_id, held));
    _hashes->addAlias(old, held);
    return held;
}

std::map<long, long> StringHasher::aliases() const
{
    TableLock lock(*_hashes, TableLock::Read);
    std::map<long, long> out;
    for (const auto& v : _hashes->Aliases) {
        out.emplace(v.first, v.second->_id);
    }
    return out;
}

std::size_t StringHasher::insertRows(const std::vector<Row>& rows, std::size_t* conflicts)
{
    TableLock lock(*_hashes, TableLock::Write);
    constexpr auto kept = ~static_cast<int>(StringID::Flag::Marked);
    std::size_t taken = 0;
    std::size_t clashes = 0;
    auto take = [&](const Row& row) {
        StringIDRef sid(new StringID(row.id, row.data, static_cast<StringID::Flag>(row.flags & kept)));
        sid._sid->_postfix = row.postfix;
        sid._sid->_sids.reserve(static_cast<int>(row.sids.size()));
        for (const auto& ref : row.sids) {
            StringIDRef here = getID(ref.first, ref.second);
            if (!here) {
                return false;
            }
            sid._sid->_sids.push_back(here);
        }
        insertOrAlias(sid);
        ++taken;
        return true;
    };
    // A row may refer to one further on (a string given an id of a block
    // leased before the id of what it is built on was given): it waits
    // for that one
    std::vector<const Row*> waiting;
    for (const auto& row : rows) {
        if (const StringID* d = _hashes->entry(row.id)) {
            if (!isRow(*d, row))
                ++clashes;
            continue;
        }
        if (!take(row)) {
            waiting.push_back(&row);
        }
    }
    while (!waiting.empty()) {
        std::vector<const Row*> still;
        for (const Row* row : waiting) {
            if (!take(*row)) {
                still.push_back(row);
            }
        }
        if (still.size() == waiting.size()) {
            break;
        }
        waiting.swap(still);
    }
    clashes += waiting.size();
    _hashes->LastID = lastID();
    if (conflicts)
        *conflicts = clashes;
    return taken;
}

bool StringHasher::hasID(long id) const
{
    TableLock lock(*_hashes, TableLock::Read);
    return _hashes->entry(id) != nullptr;
}

StringHasher::StorageSizes StringHasher::getStorageSize() const
{
    TableLock lock(*_hashes, TableLock::Read);
    StorageSizes sizes;
    std::unordered_set<const char *> dataset;
    for (const auto & v : _hashes->right) {
        const StringID &sid = *v.second;
        size_t size = sid._data.size() + sid._postfix.size();
        size_t shared_size = 0;
        if (dataset.insert(sid._data.constData()).second)
            shared_size += sid._data.size();
        if (dataset.insert(sid._postfix.constData()).second)
            shared_size += sid._postfix.size();
        if (sid.isPersistent() || sid.getRefCount() > 1) {
            sizes.referenced_size += size;
            sizes.shared_size += shared_size;
        }
        sizes.total_size += size;
        sizes.total_shared_size += shared_size;
    }
    return sizes;
}

bool StringHasher::getSaveAll() const {
    TableLock lock(*_hashes, TableLock::Read);
    return _hashes->SaveAll;
}

void StringHasher::setThreshold(int threshold) {
    TableLock lock(*_hashes, TableLock::Write);
    _hashes->Threshold = threshold;
}

int StringHasher::getThreshold() const {
    TableLock lock(*_hashes, TableLock::Read);
    return _hashes->Threshold;
}

void StringHasher::setIndexedNames(bool enable) {
    _hashes->IndexedNames = enable;
    _hashes->InferIndexed = false;
}

bool StringHasher::getIndexedNames() const {
    return _hashes->IndexedNames;
}

void StringHasher::inferIndexedNames()
{
    _hashes->InferIndexed = false;
    // Evidence either way. A hasher indexing names stores an indexed one as
    // its text plus index (Indexed, PrefixIDIndex); one that does not stores
    // the whole name, so text that getID() would have split is the other
    // mode's mark. Element names are the postfixed entries.
    std::size_t indexed = 0;
    std::size_t whole = 0;
    for (const auto& v : _hashes->right) {
        const StringID& sid = *v.second;
        if (!sid.isPostfixed() || sid.isPrefixID()) {
            continue;
        }
        if (sid.isIndexed() || sid.isPrefixIDIndex()) {
            ++indexed;
        }
        else if (Data::IndexedName(sid._data)) {
            ++whole;
        }
        else {
            auto prefix = StringID::fromString(sid._data);
            if (prefix.id > 0 && prefix.index != 0) {
                ++whole;
            }
        }
    }
    if (!indexed && !whole) {
        // No name either mode would write differently: the new-document mode
        // reproduces every stored one
        return;
    }
    if (indexed && whole) {
        FC_WARN("String table holds names of both encodings (" << indexed << " indexed, "
                << whole << " whole); keeping the majority's");
    }
    _hashes->IndexedNames = indexed >= whole;
}

long StringHasher::lastID() const
{
    TableLock lock(*_hashes, TableLock::Read);
    // Another id of a string is an id given too
    long last = _hashes->LastID;
    if (!_hashes->Aliases.empty()) {
        last = std::max(last, _hashes->Aliases.rbegin()->first);
    }
    if (_hashes->right.empty()) {
        return last;
    }
    auto it = _hashes->right.end();
    --it;
    return std::max(last, it->first);
}

bool StringHasher::merge(const StringHasher& other, std::size_t* aliased)
{
    TableLock theirs(*other._hashes, TableLock::Read);
    TableLock lock(*_hashes, TableLock::Write);
    // docs/TransactionLogJournal.md sec 27.40 item 2: the table of one document of
    // a file, read on its own, joins the file's. First see that no id means
    // something else here; only then change anything.
    constexpr auto kept = ~static_cast<int>(StringID::Flag::Marked);
    for (const auto& v : other._hashes->right) {
        const StringID* held = _hashes->entry(v.first);
        if (held && !isRow(*held, rowOf(*v.second, v.first)))
            return false;
    }
    std::size_t aliases = 0;
    auto take = [&](long id, const StringID& from) {
        StringIDRef sid(new StringID(id, from._data, static_cast<StringID::Flag>(
            from._flags.toUnderlyingType() & kept)));
        sid._sid->_postfix = from._postfix;
        sid._sid->_sids.reserve(from._sids.size());
        for (const auto& ref : from._sids) {
            StringIDRef here = getID(ref.value(), ref.getIndex());
            if (!here)
                return false;
            sid._sid->_sids.push_back(here);
        }
        if (_hashes->left.find(sid._sid) != _hashes->left.end())
            ++aliases;
        insertOrAlias(sid);
        return true;
    };
    // In id order, so each one's references are mostly here before it; one
    // that refers to an id further on waits for it
    std::vector<std::pair<long, const StringID*>> waiting;
    for (const auto& v : other._hashes->right) {
        if (_hashes->entry(v.first))
            continue;
        if (!take(v.first, *v.second))
            waiting.emplace_back(v.first, v.second);
    }
    while (!waiting.empty()) {
        std::vector<std::pair<long, const StringID*>> still;
        for (const auto& w : waiting) {
            if (!take(w.first, *w.second))
                still.push_back(w);
        }
        if (still.size() == waiting.size())
            FC_THROWM(Base::RuntimeError, "Invalid string id reference");
        waiting.swap(still);
    }
    _hashes->LastID = std::max(_hashes->LastID, other.lastID());
    if (aliased)
        *aliased = aliases;
    return true;
}

std::vector<StringHasher::Row> StringHasher::rowsOf(const std::vector<long>& ids) const
{
    TableLock lock(*_hashes, TableLock::Read);
    std::set<long> wanted;
    std::vector<long> left(ids);
    while (!left.empty()) {
        const long id = left.back();
        left.pop_back();
        if (wanted.count(id))
            continue;
        // Under the id asked for, which may be another id of its string
        const StringID* d = _hashes->entry(id);
        if (!d)
            continue;
        wanted.insert(id);
        for (const auto& sid : d->_sids) {
            if (sid._sid && sid._sid->_hasher == this)
                left.push_back(sid.value());
        }
    }
    std::vector<Row> out;
    out.reserve(wanted.size());
    for (long id : wanted) {
        out.push_back(rowOf(*_hashes->entry(id), id));
    }
    return out;
}

namespace {
template<typename T>
void putValue(std::ostream& out, T value)
{
    out.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

template<typename T>
bool getValue(std::istream& in, T& value)
{
    in.read(reinterpret_cast<char*>(&value), sizeof(value));
    return in.good();
}

void putArray(std::ostream& out, const QByteArray& bytes)
{
    putValue(out, static_cast<std::uint32_t>(bytes.size()));
    out.write(bytes.constData(), bytes.size());
}

bool getArray(std::istream& in, QByteArray& bytes, std::uint64_t most)
{
    std::uint32_t n = 0;
    if (!getValue(in, n) || n > most)
        return false;
    bytes.resize(static_cast<int>(n));
    in.read(bytes.data(), n);
    return in.good() || (in.eof() && static_cast<std::uint64_t>(in.gcount()) == n);
}
}  // namespace

void StringHasher::writeRows(std::ostream& out, const std::vector<Row>& rows)
{
    putValue(out, static_cast<std::uint64_t>(rows.size()));
    for (const auto& row : rows) {
        putValue(out, static_cast<std::int64_t>(row.id));
        putValue(out, static_cast<std::int32_t>(row.flags));
        putValue(out, static_cast<std::uint32_t>(row.sids.size()));
        for (const auto& sid : row.sids) {
            putValue(out, static_cast<std::int64_t>(sid.first));
            putValue(out, static_cast<std::int32_t>(sid.second));
        }
        putArray(out, row.data);
        putArray(out, row.postfix);
    }
}

bool StringHasher::readRows(std::istream& in, std::vector<Row>& rows, std::uint64_t most)
{
    rows.clear();
    std::uint64_t count = 0;
    if (!getValue(in, count) || count > most)
        return false;
    rows.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) {
        Row row;
        std::int64_t id = 0;
        std::int32_t flags = 0;
        std::uint32_t sids = 0;
        if (!getValue(in, id) || !getValue(in, flags) || !getValue(in, sids) || sids > most)
            return false;
        row.id = static_cast<long>(id);
        row.flags = flags;
        row.sids.reserve(sids);
        for (std::uint32_t k = 0; k < sids; ++k) {
            std::int64_t sid = 0;
            std::int32_t index = 0;
            if (!getValue(in, sid) || !getValue(in, index))
                return false;
            row.sids.emplace_back(static_cast<long>(sid), index);
        }
        if (!getArray(in, row.data, most) || !getArray(in, row.postfix, most))
            return false;
        rows.push_back(std::move(row));
    }
    return true;
}

bool StringHasher::takeRows(const std::vector<Row>& rows, std::string* why, std::size_t* taken)
{
    TableLock lock(*_hashes, TableLock::Write);
    if (taken)
        *taken = 0;
    auto refuse = [why](const std::string& text) {
        if (why)
            *why = text;
        return false;
    };
    // First see that every row can come in as it is (docs/WorkerProcess.md
    // sec 15); only then change anything
    std::set<long> coming;
    for (const auto& row : rows) {
        coming.insert(row.id);
    }
    std::set<std::pair<QByteArray, QByteArray>> strings;
    std::vector<Row> fresh;
    long previous = 0;
    for (const auto& row : rows) {
        const std::string which = "string " + std::to_string(row.id);
        if (row.id <= previous)
            return refuse("the rows are not in id order");
        previous = row.id;
        if (const StringID* d = _hashes->entry(row.id)) {
            if (!isRow(*d, row, &coming))
                return refuse(which + " is another string here");
            continue;
        }
        for (const auto& ref : row.sids) {
            if (!coming.count(ref.first) && !_hashes->entry(ref.first))
                return refuse(which + " refers to " + std::to_string(ref.first)
                              + ", which is neither here nor sent");
        }
        StringID probe;
        probe._data = row.data;
        probe._postfix = row.postfix;
        auto held = _hashes->left.find(&probe);
        // Held under another id: in a table that spells digests the two
        // spell alike, and every name reads the same -- the id coming in is
        // taken as another id of the string here, which stays held once
        // (docs/StringTableContentIds.md sec 10, 15)
        if (held != _hashes->left.end() && !_hashes->ContentIds)
            return refuse(which + " is held here as " + std::to_string(held->first->value()));
        if (!strings.emplace(row.data, row.postfix).second)
            return refuse(which + " is sent twice");
        fresh.push_back(row);
    }
    if (fresh.empty())
        return true;
    const long top = _hashes->right.empty() ? 0 : _hashes->right.rbegin()->first;
    std::size_t clashes = 0;
    const std::size_t in = insertRows(fresh, &clashes);
    if (taken)
        *taken = in;
    if (fresh.front().id < top
        && (!_hashes->LowTaken || fresh.front().id < _hashes->LowTaken))
        _hashes->LowTaken = fresh.front().id;
    return true;
}

std::vector<std::vector<long>> StringHasher::duplicates() const
{
    TableLock lock(*_hashes, TableLock::Read);
    std::map<StringID::Digest, std::vector<long>> byDigest;
    for (const auto& v : _hashes->right) {
        byDigest[v.second->digest()].push_back(v.first);
    }
    std::vector<std::vector<long>> groups;
    for (auto& [digest, ids] : byDigest) {
        if (ids.size() > 1) {
            groups.push_back(std::move(ids));
        }
    }
    std::sort(groups.begin(), groups.end());
    return groups;
}

long StringHasher::takeLowTaken()
{
    TableLock lock(*_hashes, TableLock::Write);
    const long low = _hashes->LowTaken;
    _hashes->LowTaken = 0;
    return low;
}

long StringHasher::reserveBlock(long count)
{
    TableLock lock(*_hashes, TableLock::Write);
    const long last = lastID();
    if (count <= 0 || last > std::numeric_limits<long>::max() - count)
        return 0;
    // What is held stays as it is: only the next id moves, which a save
    // writes as the last one
    _hashes->LastID = last + count;
    return last + 1;
}

void StringHasher::setLease(std::shared_ptr<StringIDLease> lease)
{
    TableLock lock(*_hashes, TableLock::Write);
    _hashes->Lease = std::move(lease);
}

std::shared_ptr<StringIDLease> StringHasher::lease() const
{
    TableLock lock(*_hashes, TableLock::Read);
    return _hashes->Lease;
}

long StringHasher::nextID()
{
    TableLock lock(*_hashes, TableLock::Write);
    if (!_hashes->Lease)
        return lastID() + 1;
    // Above every id held: a string is built on strings of lower ids, which
    // is the order a table is saved and read in. What is left of a range
    // leased before this table took in an id above it (the pool's own
    // strings, read at a move; another worker's) is let go
    // (docs/WorkerProcess.md sec 32)
    long top = _hashes->right.empty() ? 0 : _hashes->right.rbegin()->first;
    if (!_hashes->Aliases.empty()) {
        top = std::max(top, _hashes->Aliases.rbegin()->first);
    }
    if (_hashes->Lease->next <= top && _hashes->Lease->next < _hashes->Lease->end) {
        _hashes->Lease->drop();
    }
    for (;;) {
        // Never one held: a lease is ids nothing here was given
        const long id = _hashes->Lease->take();
        if (!_hashes->entry(id))
            return id;
    }
}

void StringIDLease::add(long first, long count)
{
    next = first;
    end = first + count;
    stale = false;
    ++ranges;
}

void StringIDLease::drop()
{
    next = end;
    stale = true;
    ++dropped;
}

long StringIDLease::take()
{
    if (next <= 0 || next >= end) {
        long first = 0;
        long count = 0;
        if (!more || !more(first, count) || first <= 0 || count <= 0)
            THROWM(Base::RuntimeError, "no string ids are left to give");
        add(first, count);
    }
    return next++;
}

StringIDRef StringHasher::getID(const char* text, int len, bool hashable)
{
    if (len < 0) {
        len = static_cast<int>(strlen(text));
    }
    return getID(QByteArray::fromRawData(text, len), hashable ? Option::Hashable : Option::None);
}

StringIDRef StringHasher::getID(const QByteArray& data, Options options)
{
    bool binary = options.testFlag(Option::Binary);
    bool hashable = options.testFlag(Option::Hashable);
    bool nocopy = options.testFlag(Option::NoCopy);

    // Looked up under the shared lock, which is all a string that is there
    // needs; one that is not is put in under the exclusive one, and looked
    // up again first where the lock was let go in between: another thread
    // may have put it in (docs/TransactionLogJournal.md sec 31.30).
    const bool writing = TableLock::writing(*_hashes);
    bool hashed = false;
    StringID dataID;
    {
        TableLock lock(*_hashes, TableLock::Read);
        hashed = hashable && _hashes->Threshold > 0 && (int)data.size() > _hashes->Threshold;
        if (hashed) {
            QCryptographicHash hasher(QCryptographicHash::Sha1);
            hasher.addData(data);
            dataID._data = hasher.result();
        }
        else {
            dataID._data = data;
        }

        auto it = _hashes->left.find(&dataID);
        if (it != _hashes->left.end()) {
            return {it->first};
        }
    }
    TableLock lock(*_hashes, TableLock::Write);
    if (!writing) {
        auto it = _hashes->left.find(&dataID);
        if (it != _hashes->left.end()) {
            return {it->first};
        }
    }

    if (!hashed && !nocopy) {
        // if not hashed, make a deep copy of the data
        dataID._data = QByteArray(data.constData(), data.size());
    }

    StringID::Flags flags(StringID::Flag::None);
    if (binary) {
        flags.setFlag(StringID::Flag::Binary);
    }
    if (hashed) {
        flags.setFlag(StringID::Flag::Hashed);
    }
    StringIDRef sid(new StringID(nextID(), dataID._data, flags));
    return {insert(sid)};
}

StringIDRef StringHasher::getID(const Data::MappedName& name, const QVector<StringIDRef>& sids)
{
    TableLock lock(*_hashes, TableLock::Write);
    StringID tempID;
    tempID._postfix = name.postfixBytes();

    Data::IndexedName indexed;
    if (_hashes->IndexedNames && tempID._postfix.size()) {
        // Only check for IndexedName if there is postfix, because of the way
        // we restore the StringID. See StringHasher::saveStream/restoreStreamNew()
        indexed = Data::IndexedName(name.dataBytes());
    }
    if (indexed) {
        // If this is an IndexedName, then _data only stores the base part of the name, without the
        // integer index
        tempID._data =
            QByteArray::fromRawData(indexed.getType(), static_cast<int>(strlen(indexed.getType())));
    }
    else {
        // Store the entire name in _data, but temporarily re-use the existing memory
        tempID._data = name.dataBytes();
    }

    // Check to see if there is already an entry in the hash table for this StringID
    auto it = _hashes->left.find(&tempID);
    if (it != _hashes->left.end()) {
        auto res = StringIDRef(it->first);
        if (indexed) {
            res._index = indexed.getIndex();
        }
        return res;
    }

    if (!indexed && name.isRaw()) {
        // Make a copy of the memory if we didn't do so earlier
        tempID._data = QByteArray(name.dataBytes().constData(), name.dataBytes().size());
    }

    // If the postfix is not already encoded, use getID to encode it:
    StringIDRef postfixRef;
    if (tempID._postfix.size() && tempID._postfix.indexOf("#") < 0) {
        postfixRef = getID(tempID._postfix);
        postfixRef.toBytes(tempID._postfix);
    }

    // If _data is an IndexedName, use getID to encode it:
    StringIDRef indexRef;
    if (indexed) {
        indexRef = getID(tempID._data);
    }

    // The real StringID object that we are going to insert
    StringIDRef newStringIDRef(new StringID(nextID(), tempID._data));
    StringID& newStringID = *newStringIDRef._sid;
    if (tempID._postfix.size() != 0) {
        newStringID._flags.setFlag(StringID::Flag::Postfixed);
        newStringID._postfix = tempID._postfix;
    }

    // Count the related SIDs that use this hasher
    int numSIDs = 0;
    for (const auto& relatedID : sids) {
        if (relatedID && relatedID._sid->_hasher == this) {
            ++numSIDs;
        }
    }

    int numAddedSIDs = (postfixRef ? 1 : 0) + (indexRef ? 1 : 0);
    if (numSIDs == sids.size() && !postfixRef && !indexRef) {
        // The simplest case: just copy the whole list
        newStringID._sids = sids;
    }
    else {
        // Put the added SIDs at the front of the SID list
        newStringID._sids.reserve(numSIDs + numAddedSIDs);
        if (postfixRef) {
            newStringID._flags.setFlag(StringID::Flag::PostfixEncoded);
            newStringID._sids.push_back(postfixRef);
        }
        if (indexRef) {
            newStringID._flags.setFlag(StringID::Flag::Indexed);
            newStringID._sids.push_back(indexRef);
        }
        // Append the sids from the input list whose hasher is this one
        for (const auto& relatedID : sids) {
            if (relatedID && relatedID._sid->_hasher == this) {
                newStringID._sids.push_back(relatedID);
            }
        }
    }

    // If the number of related IDs is larger than some threshold (hardcoded to 10 right now), then
    // remove any duplicates (ignoring the new SIDs we may have just added)
    const int relatedIDSizeThreshold {10};
    if (newStringID._sids.size() > relatedIDSizeThreshold) {
        std::sort(newStringID._sids.begin() + numAddedSIDs, newStringID._sids.end());
        newStringID._sids.erase(
            std::unique(newStringID._sids.begin() + numAddedSIDs, newStringID._sids.end()),
            newStringID._sids.end());
    }

    // If the new StringID has a postfix, but is not indexed, see if the data string itself
    // contains an index.
    if (newStringID._postfix.size() && !indexed) {
        // Only check for existing StringID in _data if there is postfix,
        // because of the way we restore the StringID. See
        // StringHasher::saveStream/restoreStreamNew()
        StringID::IndexID res = StringID::fromString(newStringID._data);
        if (res.id > 0) {
            if (_hashes->IndexedNames || res.index == 0) {
                if (res.index != 0) {
                    indexed.setIndex(res.index);
                    newStringID._data.resize(newStringID._data.lastIndexOf(':')+1);
                }
                int offset = newStringID.isPostfixEncoded() ? 1 : 0;
                // Search for the SID with that index
                for (int i = offset; i < newStringID._sids.size(); ++i) {
                    if (newStringID._sids[i].spelled() == res.id) {
                        if (i != offset) {
                            // If this SID is not already the first element in sids, move it there by
                            // swapping it with whatever WAS there
                            std::swap(newStringID._sids[offset], newStringID._sids[i]);
                        }
                        if (res.index != 0) {
                            newStringID._flags.setFlag(StringID::Flag::PrefixIDIndex);
                        }
                        else {
                            newStringID._flags.setFlag(StringID::Flag::PrefixID);
                        }
                        break;
                    }
                }
            }
        }
    }

    // The string as it is kept -- its postfix said by id, its index taken
    // off -- is not the one looked up above, and the table takes one string
    // under two ids (sec 27.41 Q2): it is looked up as kept before it is
    // put in, or every asking would make it again under a new id.
    StringID* kept = nullptr;
    auto range = _hashes->left.equal_range(&newStringID);
    for (auto i = range.first; i != range.second; ++i) {
        if (!kept || i->first->_id < kept->_id)
            kept = i->first;
    }
    if (kept)
        return {kept, indexed.getIndex()};

    return {insert(newStringIDRef), indexed.getIndex()};
}

StringIDRef StringHasher::importID(const StringIDRef& foreign, ImportMemo* memo)
{
    ImportMemo local;
    return importOne(foreign, memo ? *memo : local, true);
}

StringIDRef StringHasher::lookupID(const StringIDRef& foreign, ImportMemo* memo) const
{
    ImportMemo local;
    // Nothing is taken in with `take` false: the table is not changed.
    return const_cast<StringHasher*>(this)->importOne(foreign, memo ? *memo : local, false);
}

bool StringHasher::importText(const QByteArray& text, const StringHasher& from, QByteArray& out,
                              QVector<StringIDRef>* sids, ImportMemo& memo)
{
    return rewriteIds(text, from, out, sids, memo, true);
}

bool StringHasher::lookupText(const QByteArray& text, const StringHasher& from, QByteArray& out,
                              ImportMemo& memo) const
{
    // As lookupID(): with `take` false the table is not changed.
    return const_cast<StringHasher*>(this)->rewriteIds(text, from, out, nullptr, memo, false);
}

bool StringHasher::importName(const Data::MappedName& name, const StringHasher& from,
                              Data::MappedName& out, QVector<StringIDRef>& sids,
                              ImportMemo& memo)
{
    TableLock lock(*_hashes, TableLock::Write);
    QVector<StringIDRef> named;
    QByteArray data;
    QByteArray postfix;
    if (!rewriteIds(name.dataBytes(), from, data, &named, memo, true)
            || !rewriteIds(name.postfixBytes(), from, postfix, &named, memo, true)) {
        return false;
    }
    QVector<StringIDRef> held;
    held.reserve(sids.size() + named.size());
    for (const auto& sid : sids) {
        // A held id its table no longer has is not needed: what the name
        // says is in its text, and the text's are all here.
        if (auto here = importOne(sid, memo, true)) {
            if (held.indexOf(here) < 0)
                held.push_back(here);
        }
    }
    for (const auto& sid : named) {
        if (held.indexOf(sid) < 0)
            held.push_back(sid);
    }
    sids = std::move(held);
    if (data == name.dataBytes() && postfix == name.postfixBytes()) {
        out = name;
        return true;
    }
    Data::MappedName res(data.constData(), data.size());
    res += postfix;
    out = std::move(res);
    return true;
}

namespace {
thread_local const std::map<long, long>* importTags = nullptr;

/// The id a tag says, `;:H<hex>` at `pos` of `text`. False where none is.
bool tagAt(const QByteArray& text, int pos, long& tag)
{
    static const QByteArray marker(Data::tagPostfix().c_str());
    if (pos < 0 || text.mid(pos, marker.size()) != marker)
        return false;
    int digits = pos + marker.size();
    if (digits < text.size() && text[digits] == '-')
        ++digits;
    int end = digits;
    while (end < text.size() && std::isxdigit(static_cast<unsigned char>(text[end])))
        ++end;
    bool ok = false;
    tag = text.mid(digits, end - digits).toLong(&ok, 16);
    return ok;
}

/// The id here of the object another copy's tag names (ImportTags).
long tagHere(long tag)
{
    if (tag < 0)
        tag = -tag;
    if (importTags) {
        auto it = importTags->find(tag);
        if (it != importTags->end())
            return it->second;
    }
    return tag;
}

/** `text` where it starts with an element's own name and the tag of the
 * object that made it -- `g5;SKT;:H24,E`, a name no string stands for --
 * with the name as this file numbers it (RestoreMinted,
 * docs/TransactionLogJournal.md sec 31.14). The tag is the copy's still.
 */
QByteArray rewriteMinted(const QByteArray& text)
{
    auto minted = App::RestoreMinted::current();
    if (!minted)
        return text;
    static const QByteArray marker(Data::tagPostfix().c_str());
    const int pos = text.indexOf(marker);
    long tag = 0;
    if (pos <= 0 || text.left(pos).contains('#') || !tagAt(text, pos, tag))
        return text;
    std::string name(text.constData(), static_cast<std::size_t>(pos));
    if (!minted->byId(tagHere(tag), name))
        return text;
    return QByteArray(name.c_str(), static_cast<int>(name.size())) + text.mid(pos);
}

/// `text` with the tags of ImportTags rewritten: each `;:H<hex>` whose id
/// the map names.
QByteArray rewriteTags(const QByteArray& text)
{
    if (!importTags || importTags->empty())
        return text;
    static const QByteArray marker(Data::tagPostfix().c_str());
    int pos = text.indexOf(marker);
    if (pos < 0)
        return text;
    QByteArray res;
    int last = 0;
    while (pos >= 0) {
        int digits = pos + marker.size();
        if (digits < text.size() && text[digits] == '-')
            ++digits;
        int end = digits;
        while (end < text.size() && std::isxdigit(static_cast<unsigned char>(text[end])))
            ++end;
        if (end > digits) {
            bool ok = false;
            const long tag = text.mid(digits, end - digits).toLong(&ok, 16);
            auto it = ok ? importTags->find(tag) : importTags->end();
            if (it != importTags->end()) {
                res.append(text.constData() + last, digits - last);
                res.append(QByteArray::number(static_cast<qlonglong>(it->second), 16));
                last = end;
            }
        }
        pos = text.indexOf(marker, end);
    }
    if (!last)
        return text;
    res.append(text.constData() + last, text.size() - last);
    return res;
}
}  // namespace

namespace {
/** `#b;:H24,E`, `#b;:G;XTR;:H24:7,F` are the name `#b` -- `g5;SKT` -- as
 * an element of the object the first tag after it says: the shape it came
 * from (docs/TransactionLogJournal.md sec 31.14). Where that object numbers the
 * thing otherwise here (RestoreMinted), `said` is what the string `prefix`
 * says here in place of its own text. `after` is what follows the
 * reference to it. False where it comes as it is.
 */
bool mintedText(const App::StringIDRef& prefix, const QByteArray& after, QByteArray& said)
{
    const auto minted = App::RestoreMinted::current();
    if (!minted || !prefix)
        return false;
    static const QByteArray marker(Data::tagPostfix().c_str());
    long tag = 0;
    if (!tagAt(after, after.indexOf(marker), tag))
        return false;
    const App::StringID& name = prefix.deref();
    const QByteArray tail = name.postfix();
    // A name of its own, not one built on another.
    if (name.isBinary() || name.isHashed() || name.data().contains('#') || tail.contains('#'))
        return false;
    std::string text = name.dataToText(0);
    if (!minted->byId(tagHere(tag), text) || text.size() < static_cast<std::size_t>(tail.size())
            || text.compare(text.size() - tail.size(), std::string::npos, tail.constData(),
                            tail.size()) != 0)
        return false;
    said = QByteArray(text.c_str(), static_cast<int>(text.size()) - tail.size());
    return true;
}

/** The same for a name whose number is in the reference and not in the
 * string (a table that indexes names, docs/TransactionLogJournal.md sec 33.7):
 * `#c:5;:G;XTR;:H24:7,F` is `#c` -- `g;SKT` -- with 5, the name `g5;SKT`.
 * `own` is what of the string's own postfix belongs to the name, `after`
 * what holds the tag. Where the object numbers the thing otherwise here,
 * `index` is its number here: the string is the same one, only the
 * reference changes. False where it comes as it is, which a name whose
 * text changes otherwise does too.
 */
bool mintedIndex(const App::StringID& name, const QByteArray& own, const QByteArray& after,
                 int& index)
{
    const auto minted = App::RestoreMinted::current();
    if (!minted || index <= 0)
        return false;
    static const QByteArray marker(Data::tagPostfix().c_str());
    long tag = 0;
    if (!tagAt(after, after.indexOf(marker), tag))
        return false;
    // A name of its own, not one built on another.
    if (name.isBinary() || name.isHashed() || name.data().contains('#') || own.contains('#'))
        return false;
    std::string text(name.data().constData(), static_cast<std::size_t>(name.data().size()));
    text += std::to_string(index);
    text.append(own.constData(), static_cast<std::size_t>(own.size()));
    if (!minted->byId(tagHere(tag), text) || text.size() < static_cast<std::size_t>(own.size())
            || text.compare(text.size() - own.size(), std::string::npos, own.constData(),
                            own.size()) != 0)
        return false;
    const Data::IndexedName here(
        QByteArray(text.c_str(), static_cast<int>(text.size()) - own.size()));
    if (!here || here.getIndex() <= 0 || name.data() != here.getType())
        return false;
    index = here.getIndex();
    return true;
}

/// mintedIndex() for the reference `index` of `theirs`, a string of `from`:
/// a name with its tag in its own postfix (`g` + `;SKT;:H24,E`), or one
/// built on a name, the tag in what it adds (`#c:` + `;:H24,E`).
bool mintedReference(const App::StringID& theirs, const App::StringHasher& from, int& index)
{
    if (index <= 0 || !App::RestoreMinted::current() || theirs.isBinary() || theirs.isHashed())
        return false;
    static const QByteArray marker(Data::tagPostfix().c_str());
    const QByteArray tail = theirs.postfix();
    if (theirs.data().startsWith('#')) {
        const auto head = App::StringID::fromString(theirs.data(), false);
        App::StringIDRef prefix = head.id > 0 && head.index == 0 ? from.getIDBySpelling(head.id)
                                                                 : App::StringIDRef();
        return prefix && mintedIndex(prefix.deref(), prefix.deref().postfix(), tail, index);
    }
    const int pos = tail.indexOf(marker);
    return pos >= 0 && mintedIndex(theirs, tail.left(pos), tail.mid(pos), index);
}
}  // namespace

StringHasher::ImportTags::ImportTags(const std::map<long, long>& tags)
    : _outer(importTags)
{
    importTags = &tags;
}

StringHasher::ImportTags::~ImportTags()
{
    importTags = _outer;
}

long StringHasher::ImportTags::map(long tag)
{
    if (!importTags)
        return tag;
    auto it = importTags->find(tag < 0 ? -tag : tag);
    if (it == importTags->end())
        return tag;
    return tag < 0 ? -it->second : it->second;
}

bool StringHasher::rewriteIds(const QByteArray& text, const StringHasher& from, QByteArray& out,
                              QVector<StringIDRef>* sids, ImportMemo& memo, bool take)
{
    TableLock lock(*_hashes, take ? TableLock::Write : TableLock::Read);
    int pos = text.indexOf('#');
    if (pos < 0) {
        out = rewriteTags(rewriteMinted(text));
        return true;
    }
    QByteArray res;
    res.reserve(text.size() + 8);
    int last = 0;
    while (pos >= 0) {
        int end = pos + 1;
        while (end < text.size() && std::isxdigit(static_cast<unsigned char>(text[end])))
            ++end;
        if (end == pos + 1) {
            pos = text.indexOf('#', end);
            continue;
        }
        bool ok = false;
        const std::int64_t id = text.mid(pos + 1, end - pos - 1).toLongLong(&ok, 16);
        StringIDRef theirs = ok ? from.getIDBySpelling(id) : StringIDRef();
        if (!theirs)
            return false;
        // The number a table that indexes names keeps in the reference,
        // `#c:5`: where this file numbers the thing otherwise the string is
        // the same and the number is the one here (sec 33.7).
        if (pos == 0 && end < text.size() && text[end] == ':') {
            int stop = end + 1;
            while (stop < text.size() && std::isxdigit(static_cast<unsigned char>(text[stop])))
                ++stop;
            int index = stop > end + 1 ? text.mid(end + 1, stop - end - 1).toInt(&ok, 16) : 0;
            if (index > 0 && ok
                    && mintedIndex(theirs.deref(), theirs.deref().postfix(), text.mid(stop), index)) {
                StringIDRef here = importOne(theirs, memo, take);
                if (!here)
                    return false;
                if (sids && sids->indexOf(here) < 0)
                    sids->push_back(here);
                res.append(text.constData() + last, pos + 1 - last);
                res.append(QByteArray::number(static_cast<qlonglong>(here.spelled()), 16));
                res.append(':');
                res.append(QByteArray::number(index, 16));
                last = stop;
                pos = text.indexOf('#', stop);
                continue;
            }
        }
        // The name the text is built on, as this file numbers the thing
        // (sec 31.14): not the string it comes as anywhere else, so not
        // through the memo.
        QByteArray said;
        StringIDRef here = pos == 0 && mintedText(theirs, text.mid(end), said)
            ? StringIDRef(importNew(*theirs._sid, from, memo, take, &said), theirs._index)
            : importOne(theirs, memo, take);
        if (!here)
            return false;
        if (sids && sids->indexOf(here) < 0)
            sids->push_back(here);
        res.append(text.constData() + last, pos + 1 - last);
        res.append(QByteArray::number(static_cast<qlonglong>(here.spelled()), 16));
        last = end;
        pos = text.indexOf('#', end);
    }
    res.append(text.constData() + last, text.size() - last);
    out = rewriteTags(res);
    return true;
}

StringIDRef StringHasher::importOne(const StringIDRef& foreign, ImportMemo& memo, bool take)
{
    TableLock lock(*_hashes, take ? TableLock::Write : TableLock::Read);
    if (!foreign)
        return {};
    const StringID* theirs = foreign._sid;
    if (theirs->_hasher == this)
        return foreign;
    const StringHasher* from = theirs->_hasher;
    if (!from)
        return {};
    auto it = memo.find(theirs);
    if (it == memo.end()) {
        // Nothing refers to itself or above itself (a string is made after
        // what it is built from), so the recursion ends.
        StringIDRef res = importNew(*theirs, *from, memo, take);
        it = memo.emplace(theirs, res).first;
    }
    if (!it->second)
        return {};
    // A name numbered in the reference, as this file numbers the thing
    // (sec 31.14, 33.7): the string is the one imported, the number the one
    // here.
    int index = foreign._index;
    mintedReference(*theirs, *from, index);
    return {it->second, index};
}

StringIDRef StringHasher::importNew(const StringID& theirs, const StringHasher& from,
                                    ImportMemo& memo, bool take, const QByteArray* said)
{
    TableLock lock(*_hashes, TableLock::Write);
    // The name this one is built on, as this file numbers the thing (sec
    // 31.14, mintedText): another string than the one it comes as anywhere
    // else, so not through the memo.
    const StringID* builtOn = nullptr;
    StringIDRef builtOnHere;
    if (!said && !theirs.isBinary() && !theirs.isHashed() && theirs._data.startsWith('#')
            && App::RestoreMinted::current()) {
        const StringID::IndexID head = StringID::fromString(theirs._data, false);
        StringIDRef prefix = head.id > 0 ? from.getIDBySpelling(head.id) : StringIDRef();
        QByteArray data;
        if (mintedText(prefix, theirs._postfix, data)) {
            builtOnHere = importNew(*prefix._sid, from, memo, take, &data);
            if (!builtOnHere)
                return {};
            builtOn = prefix._sid;
        }
    }
    QVector<StringIDRef> related;
    related.reserve(theirs._sids.size());
    for (const auto& sid : theirs._sids) {
        StringIDRef here = builtOn && sid._sid == builtOn ? StringIDRef(builtOnHere, sid._index)
                                                          : importOne(sid, memo, take);
        if (!here)
            return {};
        related.push_back(here);
    }
    QByteArray data = said ? *said : theirs._data;
    QByteArray postfix = theirs._postfix;
    QVector<StringIDRef> named;
    if (!theirs.isBinary() && !theirs.isHashed()) {
        // A prefix reference is its text too ("#7", "#7:"), and comes out
        // as the related id it names does; a postfix string of its own has
        // no '#' (getID(MappedName) makes one only then).
        if (builtOn) {
            int end = 1;
            while (end < theirs._data.size()
                   && std::isxdigit(static_cast<unsigned char>(theirs._data[end])))
                ++end;
            data = "#" + QByteArray::number(static_cast<qlonglong>(builtOnHere.spelled()), 16)
                 + theirs._data.mid(end);
            named.push_back(builtOnHere);
        }
        else if (!said && !rewriteIds(theirs._data, from, data, &named, memo, take)) {
            return {};
        }
        if (!rewriteIds(theirs._postfix, from, postfix, &named, memo, take))
            return {};
    }
    StringID key;
    key._data = data;
    key._postfix = postfix;
    StringID* found = nullptr;
    auto range = _hashes->left.equal_range(&key);
    for (auto i = range.first; i != range.second; ++i) {
        if (!found || i->first->_id < found->_id)
            found = i->first;
    }
    if (found)
        return {found};
    if (!take)
        return {};
    auto flags = theirs._flags;
    flags.setFlag(StringID::Flag::Marked, false);
    flags.setFlag(StringID::Flag::Persistent, false);
    StringIDRef sid(new StringID(nextID(), data, flags));
    sid._sid->_postfix = postfix;
    // The strings the text names are held after the related ones, whose
    // places the flags index: a combo string has no related ids, and its
    // names are held by nothing else once imported.
    for (const auto& n : named) {
        if (related.indexOf(n) < 0)
            related.push_back(n);
    }
    sid._sid->_sids = std::move(related);
    return {insert(sid)};
}

StringIDRef StringHasher::getID(long id, int index) const
{
    TableLock lock(*_hashes, TableLock::Read);
    if (id <= 0) {
        return {};
    }
    StringID* sid = _hashes->entry(id);
    if (!sid) {
        return {};
    }
    StringIDRef res(sid);
    res._index = index;
    return res;
}

void StringHasher::setPersistenceFileName(const char* filename) const
{
    if (!filename) {
        filename = "";
    }
    _filename = filename;
}

const std::string& StringHasher::getPersistenceFileName() const
{
    return _filename;
}

void StringHasher::Save(Base::Writer& writer) const
{
    TableLock lock(*_hashes, TableLock::Write);

    size_t count = 0;
    if (_hashes->SaveAll) {
        count = _hashes->size();
    }
    else {
        count = 0;
        for (auto & v : _hashes->right) {
            if (v.second->isMarked() || v.second->isPersistent()) {
                ++count;
            }
        }
    }

    writer.Stream() << writer.ind()
        << "<StringHasher saveall=\"" << _hashes->SaveAll
        << "\" threshold=\"" << _hashes->Threshold
        << "\" indexed=\"" << _hashes->IndexedNames
        << "\" lastid=\"" << lastID() << "\"";
    if (_hashes->ContentIds) {
        writer.Stream() << " contentids=\"1\" registry=\"" << registryText(_hashes->Registry)
                        << "\"";
    }

    if(!count) {
        writer.Stream() << " count=\"0\"></StringHasher>\n";
        return;
    }

    writer.Stream() << " count=\"0\" new=\"1\"/>\n";

    writer.Stream() << writer.ind() << "<StringHasher2 ";
    if (!_filename.empty()) {
        writer.Stream() << " file=\"" << writer.addFile((_filename + ".txt").c_str(), this)
                        << "\"/>\n";
        return;
    }

    writer.Stream() << " count=\"" << count << "\">\n";
    saveStream(writer.beginCharStream() << '\n');
    writer.endCharStream() << '\n';
    writer.Stream() << writer.ind() << "</StringHasher2>\n";
}

void StringHasher::saveReference(Base::Writer& writer, const std::string& file,
                                 const std::string& hash, std::size_t count,
                                 const std::vector<long>& used) const
{
    TableLock lock(*_hashes, TableLock::Read);
    writer.Stream() << writer.ind()
        << "<StringHasher saveall=\"" << _hashes->SaveAll
        << "\" threshold=\"" << _hashes->Threshold << "\" lastid=\"" << lastID() << "\"";
    if (_hashes->ContentIds) {
        writer.Stream() << " contentids=\"1\" registry=\"" << registryText(_hashes->Registry)
                        << "\"";
    }
    writer.Stream() << " count=\"0\" new=\"1\"/>\n";
    writer.Stream() << writer.ind() << "<StringHasher2 table=\"" << encodeAttribute(file) << '"';
    if (!hash.empty())
        writer.Stream() << " hash=\"" << hash << '"';
    writer.Stream() << " count=\"" << count << "\" used=\"";
    for (std::size_t i = 0; i < used.size();) {
        std::size_t j = i;
        while (j + 1 < used.size() && used[j + 1] == used[j] + 1)
            ++j;
        if (i)
            writer.Stream() << ',';
        writer.Stream() << used[i];
        if (j > i)
            writer.Stream() << '-' << used[j];
        i = j + 1;
    }
    writer.Stream() << "\"/>\n";
}

bool StringHasher::parseUsed(const std::string& xml, std::vector<std::pair<long, long>>& ranges)
{
    ranges.clear();
    const std::size_t element = xml.find("<StringHasher2 table=");
    if (element == std::string::npos)
        return false;
    const std::size_t end = xml.find('>', element);
    const std::size_t at = xml.find(" used=\"", element);
    if (at == std::string::npos || at > end)
        return false;
    const char* p = xml.c_str() + at + 7;
    while (*p && *p != '"') {
        char* next = nullptr;
        const long lo = std::strtol(p, &next, 10);
        if (next == p)
            return false;
        long hi = lo;
        p = next;
        if (*p == '-') {
            hi = std::strtol(p + 1, &next, 10);
            p = next;
        }
        ranges.emplace_back(lo, hi);
        if (*p == ',')
            ++p;
    }
    return true;
}

std::vector<long> StringHasher::markedIDs() const
{
    TableLock lock(*_hashes, TableLock::Read);
    std::vector<long> out;
    for (auto & v : _hashes->right) {
        if (v.second->isMarked() || v.second->isPersistent())
            out.push_back(v.first);
    }
    return out;
}

void StringHasher::SaveDocFile (Base::Writer &writer) const {
    TableLock lock(*_hashes, TableLock::Write);
    // What saveStream() writes, under the marker RestoreDocFile() reads it
    // by: a bare count there means the older format.
    std::size_t count = 0;
    for (auto & v : _hashes->right) {
        if (_hashes->SaveAll || v.second->isMarked() || v.second->isPersistent())
            ++count;
    }
    writer.Stream() << "StringTableStart v1 " << count << '\n';
    saveStream(writer.Stream());
}

void StringHasher::saveStream(std::ostream &stream, bool all) const {
    TableLock lock(*_hashes, TableLock::Write);
    Base::OutputStream str(stream,false);
    boost::io::ios_flags_saver ifs(stream);
    stream << std::hex;

    bool allowRealtive = DocumentParams::getRelativeStringID();
    long anchor = 0;
    const StringID *last = nullptr;
    long lastid = 0;
    bool relative = false;

    // A string is read back after what it refers to. The rows go in id
    // order, which is that order but for a string whose id is below one it
    // is built on (an id out of a range leased before the other was given,
    // docs/WorkerProcess.md sec 32): what it refers to is written ahead of
    // it, and left out at its own turn -- a table any build reads
    std::unordered_set<const StringID*> ahead;
    std::function<void(const StringID&)> write = [&](const StringID& d) {
        long id = d._id;
        for (const auto& ref : d._sids) {
            const StringID* other = ref._sid;
            if (other && other->_hasher == this && other->_id > id && ahead.insert(other).second) {
                write(*other);
            }
        }

        if (!allowRealtive) {
            stream << id;
        }
        else {
            // We use relative coding to save space. But in order to have some
            // minimum protection against corruption, write an absolute value every
            // once a while. A row out of its order, the one after it, and one
            // that refers to an id above its own are absolute as well: a
            // relative row counts up from the row before, and its
            // references down from itself.
            relative = id > lastid && (id - anchor) < 1000;
            if (relative) {
                int from = last ? std::min(last->_sids.size(), d._sids.size()) : 0;
                for (int i = from; relative && i < d._sids.size(); ++i) {
                    relative = d._sids[i].value() <= id;
                }
            }
            if (relative)
                stream << '-' << id - lastid;
            else  {
                anchor = id;
                stream << id;
            }
            lastid = id;
        }

        int offset = d.isPostfixEncoded() ? 1 : 0;

        StringID::IndexID prefixID {};
        prefixID.id = 0;
        prefixID.index = 0;
        if (d.isPrefixID()) {
            assert(d._sids.size() > offset);
            prefixID.id = d._sids[offset].value();
        }
        else if (d.isPrefixIDIndex()) {
            prefixID = StringID::fromString(d._data);
            assert(d._sids.size() > offset && d._sids[offset].spelled() == prefixID.id);
        }

        auto flags = d._flags;
        flags.setFlag(StringID::Flag::Marked, false);
        stream << '.' << flags.toUnderlyingType();

        int position = 0;
        if (!relative) {
            for (; position < d._sids.size(); ++position) {
                stream << '.' << d._sids[position].value();
            }
        }
        else {
            if (last) {
                for (; position < d._sids.size() && position < last->_sids.size(); ++position) {
                    long m = last->_sids[position].value();
                    long n = d._sids[position].value();
                    if (n < m) {
                        stream << ".-" << m - n;
                    }
                    else {
                        stream << '.' << n - m;
                    }
                }
            }
            for (; position < d._sids.size(); ++position) {
                stream << '.' << id - d._sids[position].value();
            }
        }

        last = &d;

        // Having postfix means it is a geometry element name, which
        // guarantees to be a single line without space. So it is safe to
        // store in raw stream.
        if (d.isPostfixed()) {
            if (!d.isPrefixIDIndex() && !d.isIndexed() && !d.isPrefixID()) {
                stream << ' ' << d._data.constData();
            }

            if (!d.isPostfixEncoded()) {
                stream << ' ' << d._postfix.constData();
            }
            stream << '\n';
        }
        else {
            // Reaching here means the string may contain space and newlines
            // We rely on OutputStream (i.e. str) to save the string.
            stream << ' ';
            str << d._data.constData();
        }
    };
    for (auto& v : _hashes->right) {
        const StringID& d = *v.second;
        if (!ahead.empty() && ahead.count(&d)) {
            continue;
        }
        if (!all && !_hashes->SaveAll && !d.isMarked() && !d.isPersistent()) {
            continue;
        }
        write(d);
    }
}

void StringHasher::RestoreDocFile(Base::Reader& reader)
{
    TableLock lock(*_hashes, TableLock::Write);
    restoreTable(reader);
}

void StringHasher::restoreStreamNew(std::istream &stream, std::size_t count)
{
    TableLock lock(*_hashes, TableLock::Write);
    Base::InputStream str(stream,false);
    _hashes->clear();
    std::string content;
    boost::io::ios_flags_saver ifs(stream);
    stream >> std::hex;
    std::vector<std::string> tokens;
    long lastid = 0;
    // The ids the row before refers to, which a relative row counts from
    std::vector<long> lastRefs;
    bool hasLast = false;
    // A row that refers to one further on: taken in once that one is. A
    // worker's string, its id out of a block leased before this table gave
    // the id of what it is built on, was saved so (docs/WorkerProcess.md
    // sec 32); the rows are in id order, the references mostly back
    struct Waiting
    {
        StringIDRef sid;
        std::vector<long> refs;
    };
    std::vector<Waiting> waiting;
    // What a row says by its references, once it has them
    auto finish = [](StringID& d) {
        if (!d.isPostfixed()) {
            return;
        }
        int offset = 0;
        if (d.isPostfixEncoded()) {
            offset = 1;
            if (d._sids.empty()) {
                FC_THROWM(Base::RuntimeError, "Missing string postfix");
            }
            d._postfix = d._sids[0]._sid->_data;
        }
        if (d.isIndexed()) {
            if (d._sids.size() <= offset) {
                FC_THROWM(Base::RuntimeError, "Missing string prefix");
            }
            d._data = d._sids[offset]._sid->_data;
        }
        else if (d.isPrefixID() || d.isPrefixIDIndex()) {
            if (d._sids.size() <= offset) {
                FC_THROWM(Base::RuntimeError, "Missing string prefix id");
            }
            d._data = d._sids[offset]._sid->toString(0).c_str();
            if (d.isPrefixIDIndex())
                d._data += ":";
        }
    };
    auto resolve = [this](StringID& d, const std::vector<long>& refs) {
        d._sids.clear();
        for (long ref : refs) {
            StringIDRef sid = getID(ref);
            if (!sid) {
                d._sids.clear();
                return false;
            }
            d._sids.push_back(sid);
        }
        return true;
    };

    std::string tmp;

    for (uint32_t i = 0; i < count; ++i) {
        if (!(stream >> tmp)) {
            FC_THROWM(Base::RuntimeError, "Invalid string table");
        }

        tokens.clear();
        boost::split(tokens, tmp, boost::is_any_of("."));
        if (tokens.size() < 2) {
            FC_THROWM(Base::RuntimeError, "Invalid string table");
        }

        long id = 0;
        bool relative = false;
        if (tokens[0][0] == '-') {
            relative = true;
            id = lastid + strtol(tokens[0].c_str() + 1, nullptr, 16);
        }
        else {
            id = strtol(tokens[0].c_str(), nullptr, 16);
        }

        lastid = id;

        unsigned long flag = strtol(tokens[1].c_str(), nullptr, 16);
        StringIDRef sid(new StringID(id, QByteArray(), static_cast<StringID::Flag>(flag)));

        StringID& d = *sid._sid;
        std::vector<long> refs;
        refs.reserve(tokens.size() - 2);

        int j = 2;
        if (relative && hasLast) {
            for (; j < (int)tokens.size() && j - 2 < (int)lastRefs.size(); ++j) {
                long m = lastRefs[j - 2];
                long n;
                if (tokens[j][0] == '-') {
                    n = -strtol(&tokens[j][1], nullptr, 16);
                }
                else {
                    n = strtol(&tokens[j][0], nullptr, 16);
                }
                refs.push_back(m + n);
            }
        }
        for (; j < (int)tokens.size(); ++j) {
            long n = strtol(tokens[j].data(), nullptr, 16);
            refs.push_back(relative ? id - n : n);
        }
        d._sids.reserve(static_cast<int>(refs.size()));

        // What the stream holds of it, which its flags alone say
        if (!d.isPostfixed()) {
            str >> content;
            if(d.isHashed() || d.isBinary()) {
                d._data = QByteArray::fromBase64(content.c_str());
            }
            else {
                d._data = content.c_str();
            }
        }
        else {
            if (!d.isIndexed() && !d.isPrefixID() && !d.isPrefixIDIndex()) {
                stream >> content;
                d._data = content.c_str();
            }
            if (!d.isPostfixEncoded()) {
                stream >> content;
                d._postfix = content.c_str();
            }
        }

        if (resolve(d, refs)) {
            finish(d);
            insert(sid);
        }
        else {
            waiting.push_back({sid, refs});
        }
        lastRefs = std::move(refs);
        hasLast = true;
    }
    while (!waiting.empty()) {
        std::vector<Waiting> still;
        for (auto& w : waiting) {
            if (resolve(*w.sid._sid, w.refs)) {
                finish(*w.sid._sid);
                insert(w.sid);
            }
            else {
                still.push_back(std::move(w));
            }
        }
        if (still.size() == waiting.size()) {
            FC_THROWM(Base::RuntimeError, "Invalid string id reference");
        }
        waiting = std::move(still);
    }
    if (_hashes->InferIndexed) {
        inferIndexedNames();
    }
}

StringID* StringHasher::insert(const StringIDRef& sid)
{
    TableLock lock(*_hashes, TableLock::Write);
    assert(sid && sid._sid->_hasher == nullptr);
    auto & d = *sid._sid;
    // Its identity, from its content as it comes in: every string it refers
    // to is in a table already, its digest made
    d._digest = digestOf(d._sids, d._data, d._postfix, d.isBinary(), d.isHashed(),
                         d.isPostfixEncoded(),
                         d.isIndexed() || d.isPrefixID() || d.isPrefixIDIndex(),
                         d.isPrefixIDIndex());
    d._spelled = d._id;
    d._hasher = this;
    d.ref();
    ++_hashes->Revision;
    auto res = _hashes->right.insert(_hashes->right.end(),
            HashMap::right_map::value_type(sid.value(),&d));
    if (res->second != &d) {
        d._hasher = nullptr;
        d.unref();
    }
    else if (_hashes->ContentIds) {
        // Spelled once in: a string the table already had under its id is
        // dropped, and contests nothing
        d._spelled = spellingOf(d._digest);
        _hashes->Spelled.emplace(d._spelled, &d);
    }
    return res->second;
}

std::int64_t StringHasher::spellingOf(const StringID::Digest& digest)
{
    // The first level whose spelling no other content holds, and that the
    // registry does not give another: a clash found is registered, its
    // owner the content that had it (docs/StringTableContentIds.md sec 3.2)
    for (int level = 0; level < 16; ++level) {
        const std::int64_t spelled = spellingAt(digest, level);
        if (spelled <= 0) {
            continue;
        }
        auto owner = _hashes->Registry.find(spelled);
        if (owner != _hashes->Registry.end()) {
            if (owner->second == digest) {
                return spelled;
            }
            continue;
        }
        bool clash = false;
        auto range = _hashes->Spelled.equal_range(spelled);
        for (auto it = range.first; it != range.second; ++it) {
            if (it->second->digest() != digest) {
                _hashes->Registry.emplace(spelled, it->second->digest());
                ++_hashes->Revision;
                clash = true;
                break;
            }
        }
        if (!clash) {
            return spelled;
        }
    }
    FC_ERR("a string's digest contested at every spelling level");
    return spellingAt(digest, 15);
}

StringIDRef StringHasher::getIDBySpelling(std::int64_t spelled, int index) const
{
    TableLock lock(*_hashes, TableLock::Read);
    if (!_hashes->ContentIds) {
        return getID(static_cast<long>(spelled), index);
    }
    StringID* found = nullptr;
    auto range = _hashes->Spelled.equal_range(spelled);
    for (auto it = range.first; it != range.second; ++it) {
        if (!found || it->second->_id < found->_id) {
            found = it->second;
        }
    }
    return found ? StringIDRef(found, index) : StringIDRef();
}

void StringHasher::setContentIds(bool enable)
{
    TableLock lock(*_hashes, TableLock::Write);
    if (_hashes->ContentIds == enable) {
        return;
    }
    // The spellings of what the table holds follow the mode
    _hashes->ContentIds = enable;
    _hashes->Spelled.clear();
    for (auto& v : _hashes->right) {
        StringID& d = *v.second;
        d._spelled = enable ? spellingOf(d._digest) : d._id;
        if (enable) {
            _hashes->Spelled.emplace(d._spelled, &d);
        }
    }
    ++_hashes->Revision;
}

bool StringHasher::getContentIds() const
{
    TableLock lock(*_hashes, TableLock::Read);
    return _hashes->ContentIds;
}

std::map<std::int64_t, StringID::Digest> StringHasher::spellingRegistry() const
{
    TableLock lock(*_hashes, TableLock::Read);
    return _hashes->Registry;
}

void StringHasher::takeRegistry(const std::map<std::int64_t, StringID::Digest>& registry)
{
    TableLock lock(*_hashes, TableLock::Write);
    for (const auto& entry : registry) {
        _hashes->Registry.insert(entry);
    }
}

std::string StringHasher::registryToText(const std::map<std::int64_t, StringID::Digest>& registry)
{
    return registryText(registry);
}

std::map<std::int64_t, StringID::Digest> StringHasher::registryFromText(const std::string& text)
{
    std::map<std::int64_t, StringID::Digest> registry;
    readRegistry(text, registry);
    return registry;
}

void StringHasher::setSpellingBitsForTesting(int bits)
{
    spellingBits = bits > 0 && bits <= 32 ? bits : 32;
}

void StringHasher::restoreStream(std::istream &stream, std::size_t count) {
    TableLock lock(*_hashes, TableLock::Write);
    Base::InputStream str(stream,false);
    _hashes->clear();
    std::string content;
    for (uint32_t i = 0; i < count; ++i) {
        int32_t id = 0;
        uint8_t type = 0;
        str >> id >> type >> content;
        StringIDRef sid = new StringID(id, QByteArray(), static_cast<StringID::Flag>(type));
        if (sid.isHashed() || sid.isBinary()) {
            sid._sid->_data = QByteArray::fromBase64(content.c_str());
        }
        else {
            sid._sid->_data = QByteArray(content.c_str());
        }
        insert(sid);
    }
    if (_hashes->InferIndexed) {
        inferIndexedNames();
    }
}

void StringHasher::clear() {
    TableLock lock(*_hashes, TableLock::Write);
    for (auto & v : _hashes->right) {
        v.second->_hasher = nullptr;
        v.second->unref();
    }
    _hashes->clear();
    _hashes->LastID = 0;
    ++_hashes->Revision;
}

size_t StringHasher::size() const
{
    TableLock lock(*_hashes, TableLock::Read);
    return _hashes->size();
}

size_t StringHasher::count() const
{
    TableLock lock(*_hashes, TableLock::Read);
    size_t count = 0;
    // What saveStream() writes when not saving all (upstream a1ce983035)
    for(auto &v : _hashes->right)  {
        if(v.second->isMarked() || v.second->isPersistent()) {
            ++count;
        }
    }
    return count;
}

void StringHasher::Restore(Base::XMLReader& reader)
{
    TableLock lock(*_hashes, TableLock::Write);
    clear();
    reader.readElement("StringHasher");
    _hashes->SaveAll = reader.getAttributeAsInteger("saveall")?true:false;
    _hashes->Threshold = reader.getAttributeAsInteger("threshold");
    if (reader.hasAttribute("indexed")) {
        _hashes->IndexedNames = reader.getAttributeAsInteger("indexed") != 0;
        _hashes->InferIndexed = false;
    }
    else {
        // Saved before the mode was: the table, restored below or from its
        // own file, shows which one it was written in
        _hashes->IndexedNames = DocumentParams::getHashIndexedName();
        _hashes->InferIndexed = true;
    }
    _hashes->LastID = std::max<long>(_hashes->LastID, reader.getAttributeAsInteger("lastid", "0"));
    // Spelled by digests where saved so. A table saved before counts ids,
    // and keeps counting them until the user accepts a migration recompute
    // of its document, which switches it (Document::spellNamesByContent(),
    // docs/StringTableContentIds.md sec 12)
    _hashes->ContentIds = reader.hasAttribute("contentids")
        && reader.getAttributeAsInteger("contentids") != 0;
    _hashes->Registry.clear();
    if (reader.hasAttribute("registry")) {
        readRegistry(reader.getAttribute("registry"), _hashes->Registry);
    }

    bool newTag = false;
    if (reader.getAttributeAsInteger("new","0") > 0) {
        reader.readElement("StringHasher2");
        newTag = true;
    }

    // The file's table as a member of its own (docs/TransactionLogJournal.md sec
    // 27.50 item 1): read before the objects by whoever restores the
    // document, which knows the archive -- or not at all, when the table is
    // in memory already (item 3).
    _tableFile.clear();
    _tableHash.clear();
    if (newTag && reader.hasAttribute("table")) {
        _tableFile = reader.getAttribute("table");
        if (reader.hasAttribute("hash"))
            _tableHash = reader.getAttribute("hash");
        return;
    }

    // The table in a member the file names, which is how upstream writes
    // one that is not empty, and this fork did. Named for whoever restores
    // the document to read, as the member above is, and not handed to the
    // reader to be served after the objects: a document that shares its
    // file's hasher reads its table into a hasher of its own and merges it
    // (Document::Restore), and that one was gone before the reader came to
    // it -- the table was lost, and the reader called into freed memory.
    if(reader.hasAttribute("file")) {
        const char *file = reader.getAttribute("file");
        if(*file) {
            _tableFile = file;
        }
        return;
    }

    std::size_t count = reader.getAttributeAsUnsigned("count");
    if (newTag) {
        try {
            restoreStreamNew(reader.beginCharStream(),count);
        } catch (const Base::Exception &e) {
            e.ReportException();
            FC_ERR("Failed to restore string table. It is strongly recommanded to recompute the whole document.");
            reader.setPartialRestore(true);
        }
        reader.readEndElement("StringHasher2");
        return;
    }
    else if (count && reader.FileVersion > 1) {
        restoreStream(reader.beginCharStream(),count);
    }
    else {
        for (std::size_t i = 0; i < count; ++i) {
            reader.readElement("Item");
            StringIDRef sid;
            long id = reader.getAttributeAsInteger("id");
            bool hashed = reader.hasAttribute("hash");
            if (hashed || reader.hasAttribute("data")) {
                const char* value =
                    hashed ? reader.getAttribute("hash") : reader.getAttribute("data");
                sid = new StringID(id, QByteArray::fromBase64(value), StringID::Flag::Hashed);
            }
            else {
                sid = new StringID(id, QByteArray(reader.getAttribute("text")));
            }
            insert(sid);
        }
    }
    reader.readEndElement("StringHasher");
}

unsigned int StringHasher::getMemSize() const
{
    TableLock lock(*_hashes, TableLock::Read);
    return (_hashes->SaveAll ? size() : count()) * 10;
}

PyObject* StringHasher::getPyObject()
{
    return new StringHasherPy(this);
}

std::map<long,StringIDRef> StringHasher::getIDMap() const
{
    TableLock lock(*_hashes, TableLock::Read);
    std::map<long,StringIDRef> ret;
    for(auto &v : _hashes->right) {
        ret.emplace_hint(ret.end(), v.first, StringIDRef(v.second));
    }
    return ret;
}

void StringHasher::clearMarks() const
{
    TableLock lock(*_hashes, TableLock::Write);
    for (auto & v : _hashes->right) {
        v.second->_flags.setFlag(StringID::Flag::Marked, false);
    }
}

///////////////////////////////////////////////////////////

namespace {
thread_local App::StringIDCollector* idCollector = nullptr;
}

StringIDCollector::StringIDCollector(const StringHasher* hasher)
    : hasher(hasher)
    , _outer(idCollector)
{
    idCollector = this;
}

StringIDCollector::~StringIDCollector()
{
    idCollector = _outer;
}

bool StringIDCollector::take(const StringIDRef& sid)
{
    StringIDCollector* collector = idCollector;
    if (!collector)
        return sid.isMarked();
    if (!sid.isFromHasher(collector->hasher))
        return false;
    collector->ids.push_back(sid.value());
    return true;
}

std::vector<long> StringIDCollector::sortedIds() const
{
    std::vector<long> out = ids;
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}
