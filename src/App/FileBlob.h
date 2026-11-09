/***************************************************************************
 *   Copyright (c) 2026 FreeCAD contributors                               *
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

#ifndef APP_FILEBLOB_H
#define APP_FILEBLOB_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <FCGlobal.h>

namespace App
{

class DocumentObject;
class FileBlobManager;

/** Who refers to a blob, for the purpose of naming the file it is saved to.
 *
 * A blob is content and its identity is its hash, but the file a save
 * writes it to has a name, and for a project saved as a directory that name
 * is what version control follows. Deriving it from the referring property --
 * `Box.Image.png` -- makes an edit a modification of one file rather than the
 * delete plus add of an opaque hash that content-addressed entry names forced.
 */
struct BlobReferrer
{
    /** Id of the object the referrer belongs to, 0 when it has none.
     *
     * The lowest id present wins the name. The id also travels in the content
     * index as a generation token, because an object's internal name is
     * reused after a delete and its id (App::DocumentObject::_Id) never is.
     */
    long id {0};
    /// `Object.Property`, the derived name without its extension. Empty when
    /// nothing can name the content, which leaves it named by its hash.
    std::string name;
    /// Extension the referrer stores its file under, leading dot included, so
    /// a saved blob still opens in whatever handles that type.
    std::string ext;
};

/** One immutable file living in a document's transient directory.
 *
 * A blob is shared by every referrer that holds a handle to it -- properties,
 * undo transaction snapshots, the clipboard -- and is deleted from disk only
 * when the last handle goes away. That is the whole point: before this,
 * ~PropertyFileIncluded deleted its file unconditionally, so two properties
 * could never name the same file and Copy() had to duplicate the bytes.
 *
 * A blob is pure content: it is identified by the hash of its bytes. Names
 * belong to the referring property, which persists its own file name and
 * original path, so any number of properties can share one blob while each
 * keeps the name the user gave it. The file in the transient directory is
 * named by a uuid rather than by the hash -- at insertFile() time there may
 * be no referrer at all, and the name there is a cache detail nothing may
 * depend on; only the extension is kept, so the path handed to a consumer
 * still says what the content is.
 *
 * Blobs are immutable. Changing a property's file always produces a new blob;
 * the file on disk is kept read-only to enforce that.
 *
 * In a document's store the content is normally not a file at all but a
 * member of the pack store (docs/FileBlobsManager.md sec 15.7-15.10): held
 * compressed in memory until the next batch is written, then a member of a
 * zip segment file. A file is written only for a consumer that asks for a
 * path.
 */
class AppExport FileBlob
{
public:
    ~FileBlob();

    FileBlob(const FileBlob&) = delete;
    FileBlob& operator=(const FileBlob&) = delete;

    /// Store this blob belongs to. Blobs do not migrate between documents;
    /// crossing documents means importing the content into the target store.
    FileBlobManager* owner() const { return _owner; }
    /// Content hash, the blob's identity and its key in the manager.
    const std::string& hash() const { return _hash; }
    /** Absolute path in the owning document's transient directory.
     *
     * Content in the pack store has no file of its own until something asks
     * for one: the first call here writes it. A caller that only needs the bytes should
     * use read() instead, and one that only needs to know what the content is
     * should use hasExtension() -- either of those asked of thousands of blobs
     * through this is thousands of file creates.
     */
    const std::string& path() const;
    uint64_t size() const { return _size; }

    /** Whether the content has no file of its own, only a pack store member.
     *
     * Such a blob has no path whose existence means anything: asking for one
     * creates it. Code that repairs stale paths skips these.
     */
    bool inArchive() const;

    /// Whether the content is a pack store member, in memory or in a segment,
    /// whether or not it also has a file.
    bool inPack() const;

    /** Whether the file is the document's folder's, read where it is
     * (docs/TransactionLogJournal.md sec 38.9): the store never deletes it or
     * changes its permissions, and copies it in before a save takes it
     * away or the document leaves the folder.
     */
    bool inPlace() const;

    /// Whether the content is stored under this extension. Never writes a file.
    bool hasExtension(const char* ext) const;
    /// The extension the content is stored under, without the dot, empty
    /// for none. Never writes a file.
    std::string extension() const;

    /// The whole content, from the file or from the pack store. False, and
    /// \a bytes empty, when it cannot be read.
    bool read(std::string& bytes) const;

    /** The other stored files this content reads, by hash.
     *
     * A shape file that borrows geometry names the files it borrows from
     * (docs/SharedShapeStorage.md sec 11.5); it cannot be read without them,
     * so whoever keeps it for later -- the transaction log
     * (docs/TransactionLogJournal.md sec 23.16) -- has to keep them too. The module
     * that owns a format says how to find them, see
     * FileBlobManager::registerSourceReader(); content with no reader for
     * its extension reads nothing else. Reads the content.
     */
    std::vector<std::string> sources() const;

private:
    friend class FileBlobManager;
    FileBlob() = default;

    FileBlobManager* _owner {nullptr};
    std::string _hash;
    /// Written once by the manager when a packed blob gets its file.
    mutable std::string _path;
    uint64_t _size {0};
    /// Extension of a packed blob, without the dot. A file's is its path's.
    std::string _ext;
    /// Whether the content is a pack store member. Set before the blob is
    /// published and never cleared.
    bool _packed {false};
    /// Read where it is (inPlace()); cleared, under the manager's lock and
    /// after _path, when the file is copied in.
    std::atomic<bool> _inPlace {false};
    /** The logical segment holding the member, 0 while it is only in memory.
     *
     * Logical: a merge sends the segments it empties to the one it wrote
     * through the manager's redirection, so no blob is touched (sec 15.8).
     * Read and written under the manager's lock.
     */
    int _segment {0};
    /// False while the content has no file. Set after _path, so a reader
    /// that sees it set may read _path without the lock.
    mutable std::atomic<bool> _materialized {true};
};

using FileBlobHandle = std::shared_ptr<FileBlob>;

/** A property that stores its value as blob content.
 *
 * Both directions of that are here, and both are keyed on this interface
 * rather than on `PropertyFileIncluded` because a shape property is an
 * ordinary referrer too (`docs/SharedShapeStorage.md` sec 12.3), a material
 * card is another (`docs/MaterialStorage.md` sec 8), and they have nothing
 * else in common -- one stores a file the user gave it, the next serializes
 * geometry, the third a material.
 */
class AppExport BlobReferrerProperty
{
public:
    virtual ~BlobReferrerProperty() = default;

    /** Take the content the manager restored on this property's behalf.
     *
     * Called once the archive entry holding the content has been read, or
     * straight away when it had been read already. Not a value change: it
     * completes the restore of a value the document already had, so it must
     * not touch the document.
     */
    virtual void assignRestoredBlob(const FileBlobHandle& blob) = 0;

    /** Note the blobs this property holds, for the save in progress.
     *
     * The collect pass runs before any property is written, and content it
     * does not see gets no archive entry, so this is where a property says
     * what it holds. It is also where the naming is decided: an owner that
     * wants a particular extension or referrer passes it to
     * FileBlobManager::noteReferenced itself rather than returning a handle.
     *
     * \a object supplies the id for a property whose own container has none,
     * i.e. a view provider's.
     *
     * Implement it as nothing only if the property notes its own content at
     * write time instead, and say so there -- silence here is content
     * missing from the archive.
     */
    virtual void collectBlobs(FileBlobManager& manager, const DocumentObject* object) const = 0;

    /** Whether the content this property refers to is LOST when the save
     * writes no store.
     *
     * The store exists at schema 5 and above only, and most referrers have a
     * schema 4 spelling that keeps the data: PropertyFileIncluded writes its
     * own copy of the file, a shape property writes the shape the old way and
     * forfeits sharing rather than content. A referrer with nothing to fall
     * back on answers true, and the save path offers the schema its content
     * needs rather than dropping it silently.
     */
    virtual bool blobContentNeedsStore() const { return false; }

    /** Tell a referrer the content it is waiting for is not coming.
     *
     * Called once the archive is drained and the blob this property asked for
     * was not in it, which is the point at which "not read yet" becomes "not
     * there". A property that can stand in for the missing content -- from a
     * library, a default, anything it can name without inventing it -- does so
     * here and answers true, so the document degrades instead of losing the
     * value outright.
     *
     * Whatever it puts in place is a stand-in, not the content: the property
     * still owes the document the reference it was restored from, so saving
     * must write back what the file said rather than what the stand-in is.
     */
    virtual bool blobUnavailable() { return false; }

    /** Extension the content should be stored under, without the dot.
     *
     * Only names things -- the archive entry, and the file inside an unpacked
     * project -- so that what version control sees says what it holds. Empty
     * leaves the derived name without one.
     */
    virtual std::string blobExtension() const { return {}; }

    /** The blob holding this property's whole content as it stands, or null.
     *
     * A property that keeps the file its value was last written to (a
     * shape after a save, until it changes) answers it here so that a
     * reader wanting a content hash -- the transaction log
     * (docs/TransactionLogJournal.md sec 20.2, decision 6b) -- can take the hash
     * and hold the blob instead of serialising the value again. Null
     * means "not known": the caller serialises. It must never name a blob
     * whose content differs from the value.
     */
    virtual FileBlobHandle contentBlob() const { return {}; }
};

/** What noteReferenced() is told on this thread while it lives, instead
 * of the save set.
 *
 * Every referrer's Save notes the blobs it writes the hashes of (the
 * "noted again here" convention), so a capture of a value under one learns
 * exactly which files the value names -- and the manager's save set, which
 * belongs to whatever save the document runs next, is left alone. The
 * transaction log captures values on its worker thread this way
 * (docs/TransactionLogJournal.md sec 23.16). Nests; the innermost records.
 */
class AppExport BlobRecorder
{
public:
    /// `store`: the manager of the document the captured value belongs to.
    /// A value captured from a detached copy has no container to name it,
    /// and its blobs go there rather than to the default manager.
    explicit BlobRecorder(FileBlobManager* store = nullptr);
    ~BlobRecorder();

    BlobRecorder(const BlobRecorder&) = delete;
    BlobRecorder& operator=(const BlobRecorder&) = delete;

    /// What was noted, each once, in the order first noted.
    const std::vector<FileBlobHandle>& blobs() const { return _blobs; }

    /// The recorder in effect on this thread, or null.
    static BlobRecorder* current();

    /// The manager the value belongs to, or null.
    FileBlobManager* store() const { return _store; }

private:
    friend class FileBlobManager;
    void add(const FileBlobHandle& blob);

    std::vector<FileBlobHandle> _blobs;
    BlobRecorder* _previous {nullptr};
    FileBlobManager* _store {nullptr};
};

}  // namespace App

#endif  // APP_FILEBLOB_H
