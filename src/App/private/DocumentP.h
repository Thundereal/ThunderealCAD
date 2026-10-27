/***************************************************************************
 *   Copyright (c) 2002 Jürgen Riegel <juergen.riegel@web.de>              *
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

#ifndef APP_DOCUMENTP_H
#define APP_DOCUMENTP_H

#ifdef _MSC_VER
#pragma warning( disable : 4834 )
#endif

#include <App/Document.h>
#include <App/DocumentObject.h>
#include <App/DocumentObserver.h>
#include <App/StringHasher.h>
#include <App/FileBlobManager.h>
#include <App/FileElsewhere.h>
#include <App/FileHistory.h>
#include <App/TransactionLog.h>
#include <Base/Reader.h>
#include <Base/Sequencer.h>
#include <CXX/Objects.hxx>
#include <boost/bimap.hpp>
#include <boost/graph/adjacency_list.hpp>
#include <boost/bimap.hpp>
#include <chrono>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>


// using VertexProperty = boost::property<boost::vertex_root_t, DocumentObject* >;
using DependencyList = boost::adjacency_list <
boost::vecS,           // class OutEdgeListS  : a Sequence or an AssociativeContainer
boost::vecS,           // class VertexListS   : a Sequence or a RandomAccessContainer
boost::directedS,      // class DirectedS     : This is a directed graph
boost::no_property,    // class VertexProperty:
boost::no_property,    // class EdgeProperty:
boost::no_property,    // class GraphProperty:
boost::listS           // class EdgeListS:
>;
using Traits = boost::graph_traits<DependencyList>;
using Vertex = Traits::vertex_descriptor;
using Edge =  Traits::edge_descriptor;
using Node =  std::vector <size_t>;
using Path =  std::vector <size_t>;

namespace App {
using HasherMap = boost::bimap<StringHasherRef, int>;
class Transaction;

using HasherMap = boost::bimap<StringHasherRef,int>;

// Pimpl class
struct DocumentP
{
    // Array to preserve the creation order of created objects
    std::vector<DocumentObject*> objectArray;
    std::unordered_set<App::DocumentObject*> touchedObjs;
    /// The ids of the objects the last restore read as touched -- the
    /// `Touched` attribute as saved, before the restore touched anything
    /// of its own (docs/TransactionLogJournal.md sec 27.60).
    std::set<long> savedTouched;
    /// A file in an older format was opened: its version is taken once the
    /// restore is over, at the current schema (sec 27.62).
    bool openVersionPending = false;
    std::unordered_map<std::string, DocumentObject*> objectMap;
    std::unordered_map<long, DocumentObject*> objectIdMap;
    /// The live objects by their UID (docs/TransactionLogJournal.md sec 36).
    std::unordered_map<std::string, DocumentObject*> objectUidMap;
    std::unordered_map<std::string, bool> partialLoadObjects;
    std::vector<DocumentObjectT> pendingRemove;
    std::vector<App::DocumentObject*> skippedObjs;
    long lastObjectId;
    /// The id readObjects restores the next object under, 0 for a new one.
    long restoringId = 0;
    /// The UID the object being restored was saved with (sec 36), and the
    /// file's `Uid` as read, which an older file's UIDs are derived from.
    std::string restoringUid;
    std::string fileUid;
    /// The objects whose UID was derived (sec 36 U3), by id: their file was
    /// written by a build that keeps none -- an older one, or upstream's --
    /// and an import knows them by their id and name as before.
    std::unordered_set<long> derivedUids;
    /// The objects of the read under way whose UID came from nothing -- no
    /// attribute, no history -- by id: one they carry as `_ObjectUUID`, read
    /// with their properties, may still be theirs (sec 36.7).
    std::unordered_set<long> unsourcedUids;
    /// The UID each object of the import under way was brought with, by id,
    /// whatever it was given here (sec 36.7).
    std::unordered_map<long, std::string> broughtUids;
    /// An import's objects by id: the name each had in the stream, which a
    /// paste's answers name it by (sec 37.9)
    std::unordered_map<long, std::string> broughtNames;
    /// The clashes of the import under way (sec 36.4), and whether what it
    /// brings was copied from this very document, which is never asked.
    std::vector<Document::ImportClash> importClashes;
    bool importFromHere = false;
    /// Whether the last addObject() gave a new id.
    bool freshId = false;
    /// The last geometry id of each object while the document has no file
    /// history; the history takes them when it comes (sec 27.40 item 4).
    std::unordered_map<long, long> lastGeoIds;
    mutable std::pair<long, long> treeRanks = std::make_pair(0,0);
    long treeRankRevision = 0;
    long revision = 0; // will increase on object add or remove
    DocumentObject* activeObject;
    Transaction *activeUndoTransaction;
    // pointer to the python class
    Py::Object DocumentPythonObject;
    int iTransactionMode;
    bool rollback;
    bool undoing; ///< document in the middle of undo or redo
    bool committing;
    bool opentransaction;
    std::bitset<32> StatusBits;
    int iUndoMode;
    unsigned int UndoMemSize;
    unsigned int UndoMaxStackSize;
    std::string programVersion;
    mutable HasherMap hashers;
    /// Lifetime of the files behind this document's PropertyFileIncluded.
    /// The file's history (docs/TransactionLogJournal.md sec 27.7): its blob store
    /// and directory, shared with the other documents of the file.
    mutable std::shared_ptr<App::FileHistory> history;
    /// The transaction log (docs/TransactionLogJournal.md), when the mode is not off.
    std::unique_ptr<App::TransactionLog> transactionLog;
    /// Whether the mode was looked at for this document yet.
    bool transactionLogChecked {false};
    /// The document's own bookkeeping being written -- a save's stamps, the
    /// file name and label Save As gives it, the log's History, Version and
    /// Branch: never a transaction, never an undo step (docs/TransactionLogJournal.md
    /// sec 16.4, 27.5).
    bool bookkeeping {false};
    /// This document joined a file history another document of the file
    /// made (docs/TransactionLogJournal.md sec 27.7): its restore does not start
    /// or adopt a history, the log has one.
    bool joinedHistory {false};
    /// A version document's name parts (docs/TransactionLogJournal.md sec 27.23):
    /// the file and the label it is named after, and the version its name
    /// ends in -- the one it was opened at, then the one it last saved.
    std::string versionFile;
    std::string versionLabel;
    int64_t versionTail {0};
    /// Document.xml as the reader saw it, tapped by restore(const char*)
    /// for the log's version 1 (docs/TransactionLogJournal.md sec 16.6); empty
    /// when no tap was installed.
    std::string restoreDocXml;
    bool restoreTapped {false};
    /// The XML entries the reader served during the restore in progress
    /// (GuiDocument.xml, split object files), through its entry sink, for
    /// the version manifest.
    std::vector<std::pair<std::string, std::string>> fileEntries;
    /// The same for a save or snapshot: every entry as the writer captured
    /// it (docs/TransactionLogJournal.md 23.3), Document.xml first.
    TransactionLog::Captures captures;
    /// The cadence of unnamed versions (sec 16.3): commits since the last
    /// version, and when it was taken (steady clock seconds).
    long commitsSinceVersion {0};
    double lastVersionTime {0};
    /// The last commit's row and when it was made (steady clock seconds):
    /// what a commit right after it on the same property folds into (sec
    /// 37.4 P4).
    int64_t lastCommitSeq {0};
    double lastCommitTime {0};
    bool snapshotting {false};
    /// The save in progress writes the string table as a member of its own
    /// (docs/TransactionLogJournal.md sec 27.50 item 1) -- or, snapshotting, names
    /// the log's table and writes none; `tableMember` is what the writer
    /// writes it from, `tableEntry` the name it got.
    bool tableAsMember {false};
    std::unique_ptr<Base::Persistence> tableMember;
    std::string tableEntry;
    /// A restoreVersion() in progress: the restore takes no version 1.
    bool checkingOut {false};
    /// Another copy's rows or state being written in (isWritingForeign)
    bool writingForeign {false};
    /// The scratch document a restore to a version reads the version into
    /// (docs/TransactionLogJournal.md sec 24.5): it keeps no log of its own.
    bool noLog {false};
    /// The document is being destroyed: what is still open is not logged
    /// (Document::~Document).
    bool closing {false};
    /// The document's objects are being taken down to be read in again or
    /// to leave it empty (Document::restore, Document::clearDocument): what
    /// the teardown writes joins no transaction, see transactionsWanted().
    bool clearing {false};
    /// A crash recovery rebuilding the document from its log (sec 25.2):
    /// what it writes is in the log already, and is no transaction.
    bool replaying {false};
    /// The log's last seq when the document was opened: undo reaches no
    /// further back on any branch (docs/TransactionLogJournal.md sec 26.4).
    int64_t undoFloor {0};
    /// The state the document's values are in, as its undo steps know it
    /// (docs/TransactionLogJournal.md sec 30.10): a new token for every change,
    /// the old one back when a step is applied from its copies.
    int64_t stateToken {1};
    int64_t stateCounter {1};
    /// Why the last undo or redo was refused, empty when it was not.
    std::string undoRefusal;
    /// A saveCopy() without history: the copy embeds nothing, and the
    /// History property the live document had is put back afterwards.
    bool savingWithoutHistory {false};
    /// The schema the file at FileName holds, as read or last written; 0
    /// while there is no such file. A plain save that would raise a file
    /// other builds read into one they do not writes a sibling instead.
    long fileSchema {0};
    /// The sent file importFork() is bringing in (sec 30.29), by its hash,
    /// for the import's record to name; empty for a file from a path.
    std::string importingRequest;
    std::function<void()> restoreHistory;
    /// The save id of a save to a folder whose history went into its stage
    /// (docs/TransactionLogJournal.md sec 38.9), told to the log once it is in.
    std::string folderSaveId;
    /// Two processes on one file (docs/TransactionLogJournal.md sec 29.8): the file
    /// as this document last read or wrote it -- its stamp, the save id its
    /// `Version` carried, and the row of the log whose state it is -- and
    /// the stamp taken as a restore starts, kept once the restore is done.
    DiskStamp diskStamp;
    std::string diskSaveId;
    int64_t diskSeq {0};
    DiskStamp readStamp;
    /// A stamp seen and found to be only a touch: not asked again.
    mutable DiskStamp touchedStamp;
    /// A stamp seen and found to be another's write: not read again.
    mutable DiskStamp movedStamp;
    /// The file this process marks itself as having open.
    std::string openMarker;
    Document::SavePull lastPull;
    /// The branch a pull came as and has not been merged: the next save
    /// merges it again, with nothing new to bring.
    std::string pullBranch;
    /// The file that branch was pulled from: a save to another, or with it
    /// gone, merges the branch without reading it again (sec 38.21 F7).
    std::string pullFile;
    /// importFork() of the file as another process left it: the name it
    /// comes under, and the row of a file without history to start from.
    std::string pullLabel;
    int64_t pullBase {0};
#ifdef USE_OLD_DAG
    DependencyList DepList;
    std::map<DocumentObject*, Vertex> VertexObjectList;
    std::map<Vertex, DocumentObject*> vertexMap;
#endif //USE_OLD_DAG
    std::multimap<const App::DocumentObject*,
        std::unique_ptr<App::DocumentObjectExecReturn> > _RecomputeLog;
    /// Entries ever added to _RecomputeLog: a recompute failed if it added any
    unsigned long recomputeLogAdded = 0;

    StringHasherRef Hasher;

    // restored files
    std::set<std::string> files;

    /// What was written to this document while it was in a
    /// Document::RestoreDrainGuard, and therefore not touched. Reported once
    /// by whoever opened the scope, then cleared.
    Document::RestoreDrainReport drainReport;

    /// Where the last restore() spent its time, reported as one line when it
    /// finishes. Split by stage so a load can be attributed without a
    /// profiler: the two XML passes, the archive bulk, and the fixup after.
    struct RestoreTiming {
        std::chrono::duration<double> create {0};
        /// Of 'create': what the addObject() calls took by themselves --
        /// the factory, the name bookkeeping, the notifications -- against
        /// the XML element reads that surround them in the same pass.
        std::chrono::duration<double> createAdd {0};
        /// Of 'create': the progress sequencer's next() calls, which pump
        /// the event loop -- and with it every paint a GUI view schedules
        /// while the objects arrive.
        std::chrono::duration<double> createSeq {0};
        std::chrono::duration<double> data {0};
        std::chrono::duration<double> files {0};
        std::size_t objectCount = 0;
        /// What the <ObjectData> pass spent inside property restores, of
        /// which 'value' is the share the properties themselves took once
        /// the element was read and the property found.
        App::PropertyContainer::RestoreStats props;

        void clear() { *this = RestoreTiming(); }
    };
    RestoreTiming restoreTiming;

    /** What a <Defaults> block says an object class holds.
     *
     * Restored into an object built for the purpose, and reduced to the
     * properties the record actually moved off what this build's constructor
     * produces. That list is usually empty -- the file was written by a build
     * that agrees with this one -- and then a document's objects cost nothing
     * to default. When it is not empty, those few properties are pasted onto
     * every object of the class before the file's own statement about that
     * object is read.
     */
    struct RestoreDefaults {
        std::unique_ptr<DocumentObject> proto;
        std::vector<std::string> names;
    };
    std::map<std::string, RestoreDefaults> restoreDefaults;

    /** Deferred archive-entry restores (docs/DocumentLoad.md §14).
     *
     * While DeferShapeLoad is on, the archive walk parks entries whose
     * consumer opted in (Property::DeferRestore) instead of serving
     * them. The reader stays behind -- it holds no open handle, only
     * the central-directory index -- and each consumer is served on
     * first real use through Document::restoreDeferredFile(), or in
     * bulk by flushDeferredFiles(). Keyed by object and property NAME,
     * not pointer: an entry whose object got deleted (or is parked in a
     * transaction) simply stops resolving, instead of dangling.
     */
    std::shared_ptr<Base::ZipFileReader> archiveReader;
    std::map<std::pair<std::string, std::string>, std::string> deferredFiles;
    /** Which object each split-XML entry of the save in progress is for.
     *
     * The entry used to be found by reading the object's name back out of the
     * file name, which made the name an identity rather than a name -- and a
     * name a file system will take is not always the name an object has, so
     * an object whose name is too long for a file name silently wrote an
     * empty entry. Written as the Objects section is, read as the entries
     * are, and stale only between the two.
     */
    std::map<std::string, App::DocumentObject*> splitXmlEntries;
    /** S2b (docs/TransactionLogJournal.md sec 38.12): what a save to a folder
     * wrote, so that the next save of it keeps the files of the objects
     * nothing changed since. `seq` is the log's row the save was of,
     * `version` the version it recorded, `options` what decides the bytes,
     * `refs` the blobs each object referred to, `files` the entries each
     * object's write made (its own file first), `stamp` the folder as the
     * commit left it. `views` and `heads` are the same for each object's
     * view-provider file, and what that file says outside its properties
     * (sec 38.14).
     */
    struct FolderState
    {
        std::string folder;
        int64_t seq {0};
        int64_t version {0};
        std::string options;
        DiskStamp stamp;
        std::map<long, std::vector<std::string>> refs;
        std::map<long, std::vector<std::string>> files;
        std::map<long, std::vector<std::string>> views;
        std::map<long, std::string> heads;
        /// `refs` are what an open was handed (sec 38.14): only some of
        /// what a save notes, compared property by property.
        bool refsRead {false};
        /// The ids of the element maps each object's file holds, sorted.
        std::map<long, std::vector<unsigned>> mapIds;
        /// What each object's Python proxies saved as (sec 38.21 F1), for
        /// the objects that have one.
        std::map<long, std::string> proxies;
    };
    FolderState folderLast;
    FolderState folderNext;
    /// The objects of this save the log says nothing changed since
    /// folderLast, and those whose view provider it says the same of.
    std::set<long> folderUnchanged;
    std::set<long> folderViewsUnchanged;
    /** Sec 38.14: every change to what an object's file or its view
     * provider's file holds, whether the log records it or not -- a write
     * the log leaves out (NoModify, a view fitted to the model), one made
     * while a restore settles, a property's status. By object id, with
     * the generation it came in; a save of the folder clears what came in
     * up to its own (folderSaveGen), so that a change made during it stays
     * for the next.
     */
    std::unordered_map<long, uint64_t> folderDirty;
    std::unordered_map<long, uint64_t> folderDirtyViews;
    uint64_t folderGen {1};
    uint64_t folderSaveGen {0};
    /// A restore is past reading the file: what changes now is no longer
    /// what the file holds.
    bool restoreWatch {false};
    /** What an open of the document's own folder read (sec 38.14), which
     * becomes folderLast once the read and the restore after it are both
     * done: the save options Document.xml states, each object's entry by
     * name, the entries each object and view provider read, the log's
     * version of the folder as found, and the folder's stamp before the read
     * when nothing saved it during the read.
     */
    struct FolderRead
    {
        std::string folder;
        std::string options;
        int64_t version {0};
        DiskStamp stamp;
        std::map<std::string, long> entries;
        std::map<long, std::vector<std::string>> files;
        std::map<long, std::vector<std::string>> views;
        std::map<long, std::string> heads;
        std::map<long, std::vector<std::string>> refs;
        std::map<long, std::vector<unsigned>> mapIds;
        bool readDone {false};
        bool afterDone {false};
    };
    FolderRead folderRead;
    bool folderKeeping {false};
    bool folderChecking {false};
    std::map<long, std::string> folderEntryOf;
    std::map<long, std::vector<std::string>> folderRefsNow;
    /// The ids this save numbers each object's element maps with (sec 38.14).
    std::map<long, std::vector<unsigned>> folderMapIdsNow;
    /// Entries kept this save, for the log's version; and, checking, the
    /// objects that would have been, with their entries.
    std::vector<std::string> folderKept;
    std::vector<std::pair<std::string, std::vector<std::string>>> folderWouldKeep;
    Document::FolderSaveInfo folderInfo;
    /// The serve phase's progress: alive across serve slices so the
    /// indicator shows shapes-served over the whole backlog, with the
    /// per-shape import indicators nested beneath it.
    std::unique_ptr<Base::SequencerLauncher> deferServeSeq;
    /// The save's progress, owned by Document::save() for the length of one
    /// save and borrowed by the loops it spans. Null outside a save -- an
    /// export writes objects through the same function and reports nothing.
    Base::SequencerLauncher* saveSeq {nullptr};
    /// Serve-time attribution for the slice log: entry opening vs the
    /// consumer's RestoreDocFile, against the slice wall clock.
    std::chrono::duration<double> deferOpenTime {0};
    std::chrono::duration<double> deferRestoreTime {0};

    DocumentP();

    long addObject(App::DocumentObject *pcObject) {
        long id = pcObject->getID();
        if (!id)
            id = restoringId;
        freshId = false;
        for (;;) {
            if (!id) {
                id = nextObjectId();
                freshId = true;
            }
            auto &entry = this->objectIdMap[id];
            if (entry) {
                id = 0;
                continue;
            }
            entry = pcObject;
            break;
        }
        noteObjectId(id);
        ++revision;
        this->objectArray.push_back(pcObject);
        return id;
    }

    /// A new object's id: from the file's counter when the document has a
    /// history (docs/TransactionLogJournal.md sec 27.40 item 1), else its own.
    /// `obj` is not live here any more: its UID is free again (sec 36).
    void forgetObjectUid(const DocumentObject* obj) {
        auto it = objectUidMap.find(obj->getObjectUid());
        if (it != objectUidMap.end() && it->second == obj)
            objectUidMap.erase(it);
        derivedUids.erase(obj->getID());
    }

    long nextObjectId() {
        if (history) {
            history->noteObjectId(lastObjectId);
            lastObjectId = history->nextObjectId();
        }
        else
            ++lastObjectId;
        return lastObjectId;
    }

    /// `id` has `name`, in the file's name table (sec 27.40 item 3).
    void noteObjectName(const std::string& name, long id) {
        if (history)
            history->noteObjectName(name, id);
    }

    /// An id in use: neither counter hands it out again.
    void noteObjectId(long id) {
        if (id > lastObjectId)
            lastObjectId = id;
        if (history)
            history->noteObjectId(id);
    }

    void addRecomputeLog(const char *why, App::DocumentObject *obj) {
        addRecomputeLog(new DocumentObjectExecReturn(why, obj));
    }

    void addRecomputeLog(const std::string &why, App::DocumentObject *obj) {
        addRecomputeLog(new DocumentObjectExecReturn(why, obj));
    }

    void addRecomputeLog(DocumentObjectExecReturn *returnCode) {
        if(!returnCode->Which) {
            delete returnCode;
            return;
        }
        _RecomputeLog.emplace(returnCode->Which, std::unique_ptr<DocumentObjectExecReturn>(returnCode));
        returnCode->Which->setStatus(ObjectStatus::Error, true);
        ++recomputeLogAdded;
    }

    void clearRecomputeLog(const App::DocumentObject *obj=nullptr) {
        if(!obj)
            _RecomputeLog.clear();
        else
            _RecomputeLog.erase(obj);
    }

    /** Keep the message of every object still in error; drop the rest, and
     * any of an object no longer in the document. A recompute clears an
     * object's own entry when it reruns it (_recomputeFeature); one it does
     * not rerun stays in error, and its message with it -- the whole log
     * cleared at a recompute's start left such an object with a bare
     * "Error" (docs/HandsOnQueue.md entry 205, a TechDraw section view).
     */
    void pruneRecomputeLog() {
        std::unordered_set<const App::DocumentObject*> live(objectArray.begin(),
                                                            objectArray.end());
        for (auto it = _RecomputeLog.begin(); it != _RecomputeLog.end();) {
            // asked of the document first: a removed object may be gone
            if (!live.count(it->first) || !it->first->isError())
                it = _RecomputeLog.erase(it);
            else
                ++it;
        }
    }

    void clearDocument() {
        activeObject = nullptr;
        objectArray.clear();
        decltype(objectMap) map = std::move(objectMap);
        objectMap.clear();
        objectIdMap.clear();
        objectUidMap.clear();
        for (auto &v : map) {
            v.second->setStatus(ObjectStatus::Destroy, true);
            delete(v.second);
        }
    }

    const char *findRecomputeLog(const App::DocumentObject *obj) {
        auto range = _RecomputeLog.equal_range(obj);
        if(range.first == range.second)
            return nullptr;
        return (--range.second)->second->Why.c_str();
    }

    static
    void findAllPathsAt(const std::vector <Node> &all_nodes, size_t id,
                        std::vector <Path> &all_paths, Path tmp);
    std::vector<App::DocumentObject*>
    topologicalSort(const std::vector<App::DocumentObject*>& objects) const;
    std::vector<App::DocumentObject*>
    static partialTopologicalSort(const std::vector<App::DocumentObject*>& objects);
};

} // namespace App

#endif // APP_DOCUMENTP_H
