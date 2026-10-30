/***************************************************************************
 *   Copyright (c) 2011 Jürgen Riegel <juergen.riegel@web.de>              *
 *   Copyright (c) 2011 Werner Mayer <wmayer[at]users.sourceforge.net>     *
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


#ifndef APP_TRANSACTION_H
#define APP_TRANSACTION_H

#include <functional>
#include <memory>
#include <set>
#include <unordered_map>
#include <vector>
#include <Base/Factory.h>
#include <Base/Persistence.h>
#include <App/PropertyContainer.h>

namespace App
{

class Document;
class Property;
class Transaction;
class TransactionObject;
class TransactionalObject;
class TransactionMeasure;
class TransactionLog;
struct Actor;

/** While one of these lives, what this thread writes follows another value:
 * nobody set it, and its owner's recompute does not make it. A reference
 * looked up again because the shape it names changed
 * (PropertyLinkBase::updateElementReferences); a shape its object's
 * placement is laid on. A transaction records such a write as followed:
 * kept as a value set by hand is, and no change to a merge
 * (docs/TransactionLogJournal.md sec 31.25, 31.26).
 */
class AppExport TransactionFollowing
{
public:
    TransactionFollowing();
    ~TransactionFollowing();
    TransactionFollowing(const TransactionFollowing&) = delete;
    TransactionFollowing& operator=(const TransactionFollowing&) = delete;
    /// Whether this thread is inside one
    static bool active();
};

/** Represents a atomic transaction of the document
 */
class AppExport Transaction : public Base::Persistence
{
    TYPESYSTEM_HEADER_WITH_OVERRIDE();

public:
    /** Construction
     *
     * @param id: transaction id. If zero, then it will be generated
     * automatically as a monotonically increasing index across the entire
     * application. User can pass in a transaction id to group multiple
     * transactions from different document, so that they can be undo/redo
     * together.
     */
    explicit Transaction(int id = 0);
    /// Construction
    ~Transaction() override;

    /// apply the content to the document
    void apply(Document &Doc,bool forward);

    // the utf-8 name of the transaction
    std::string Name;
    /** Opened by the document itself because a write arrived with no
     * transaction active and the log is on (docs/TransactionLogJournal.md sec
     * 9.1). Closed when the invocation that opened it returns
     * (Application::InvocationScope), by the next explicit transaction,
     * or by a save or close. `Origin` names the invocation.
     */
    bool Implicit {false};
    std::string Origin;
    /// The row the transaction log wrote for this transaction (its seq),
    /// 0 when none was written (docs/TransactionLogJournal.md sec 24.2).
    int64_t LogSeq {0};
    /// For a transaction the document makes to undo a log row (sec 24.4):
    /// the kind the log records ("undo") and the row it inverts.
    std::string LogKind;
    int64_t Inverts {0};
    /// For a merge (sec 28.2 item 1): the head merged in, the row's second
    /// parent.
    int64_t MergeFrom {0};
    /// The row's script (sec 27.63): an undo's, a redo's or a restore's
    /// record of the touched state it changed.
    std::string LogScript;
    /// Who opened it (docs/TransactionLogJournal.md sec 30.3 S.b), null for the
    /// desktop user: the actor of the thread when it was made, kept so a
    /// row written later -- at the event loop, by whoever closes it -- is
    /// still its author's.
    std::shared_ptr<const Actor> Author;
    /** The view the input that made it went to (Application::getStepView),
     * 0 for none: a tree's, a property view's, a script's, which is every
     * view's. What a view's undo lists by (UndoViewScope); the session's
     * only, not written to the log.
     */
    int View {0};
    /** The state of the document's values this step was made in and the
     * one it left (docs/TransactionLogJournal.md sec 30.10): tokens of the
     * document, a new one for every change. A step can be applied from its
     * copies only while the document is in the state it left; an undo's
     * redo step has the two the other way round. 0 is no known state.
     */
    int64_t StateBefore {0};
    int64_t StateAfter {0};
    /** Log writes held until this transaction's own row is written
     * (docs/TransactionLogJournal.md sec 27.59): a recompute run while it is open
     * comes after its writes, and its record must follow its row. Dropped
     * with the transaction when it is aborted.
     */
    std::vector<std::function<void(TransactionLog&)>> AfterLogRow;
    /** Take each changed property's derived flag from `from`, the
     * transaction this one is the inverse of (sec 24.2). The flag is set
     * where the write happens -- the owner is recomputing -- and an undo's
     * writes are never made while recomputing, so without this undoing a
     * feature would log its own output as an input. The followed flag
     * (sec 31.25) is taken the same way and for the same reason: an undo
     * writes a reference outside the update that follows it.
     */
    void inheritDerived(const Transaction& from);
    /** The one property this transaction wrote by hand (docs/TransactionLogJournal.md
     * sec 37.4 P4): every other write was a recompute's or followed another
     * value, no object came or went, no freeze changed. False otherwise.
     */
    bool handWrite(const TransactionalObject*& obj, int64_t& propId) const;
    /// Whether absorb() would take this one in: no object came or went and
    /// no freeze changed.
    bool absorbable() const;
    /** Take `later`'s changes in, as though made in this one: what it
     * changed that this did not, with the value before `later`; where both
     * changed a property, this one's before stays. `later` keeps nothing it
     * gave away and is deleted by the caller. False, with nothing moved,
     * when `later` made or removed an object or changed a freeze (sec 37.4
     * P4, a coalesced commit).
     */
    bool absorb(Transaction& later);
    /// The same for a cold step, whose copies are gone: `derived` and
    /// `followed` say, per container and property, what the log row it
    /// reverted recorded.
    void inheritDerived(
        const std::function<bool(const TransactionalObject*, const Property*)>& derived,
        const std::function<bool(const TransactionalObject*, const Property*)>& followed);
    /** Past the hot window (docs/TransactionLogJournal.md sec 24.3): the step's
     * copies are gone and only its id, name and LogSeq are left. Applying
     * it reverts that log row from the log (Document::revertFromLog).
     */
    bool Cold {false};
    /** Whether deleting this transaction destroys a document object: one
     * it removed and that is still out of the document. The transaction
     * log's worker may still hold a copy of a link to such an object, so
     * the document drains the log's queue first (sec 24.3).
     */
    bool destroysObjects() const;
    /// A cold stub of `t`, which the caller deletes.
    static Transaction* coldCopy(const Transaction& t);

    unsigned int getMemSize () const override;
    void Save (Base::Writer &writer) const override;
    /// This method is used to restore properties from an XML document.
    void Restore(Base::XMLReader &reader) override;

    /// Return the transaction ID
    int getID() const;

    /// Generate a new unique transaction ID
    static int getNewID();
    static int getLastID();

    /// Returns true if the transaction list is empty; otherwise returns false.
    bool isEmpty() const;
    /// check if this object is used in a transaction
    bool hasObject(const TransactionalObject *Obj) const;
    void addOrRemoveProperty(TransactionalObject *Obj, const Property* pcProp, bool add);

    void addObjectNew(TransactionalObject *Obj);
    void addObjectDel(const TransactionalObject *Obj);
    void addObjectChange(const TransactionalObject *Obj, const Property *Prop);
    /// Record a document object's freeze, which is a status and no property
    void addObjectFreeze(const TransactionalObject *Obj, bool wasFrozen);

    /// Check if any transaction is being applied.
    static bool isApplying(Property *prop = nullptr);

    static void removePendingProperty(Property *prop);

private:
    int transID;
    using Info = std::pair<const TransactionalObject*, TransactionObject*>;
    bmi::multi_index_container<
        Info,
        bmi::indexed_by<
            bmi::sequenced<>,
            bmi::hashed_unique<
                bmi::member<Info, const TransactionalObject*, &Info::first>
            >
        >
    > _Objects;

    friend class TransactionMeasure;
    friend class TransactionLog;
};

/** Represents an entry for an object in a Transaction
 */
class AppExport TransactionObject : public Base::Persistence
{
    TYPESYSTEM_HEADER_WITH_OVERRIDE();

public:
    /// Construction
    TransactionObject();
    /// Destruction
    ~TransactionObject() override;

    virtual void applyNew(Document &Doc, TransactionalObject *pcObj);
    virtual void applyDel(Document &Doc, TransactionalObject *pcObj);
    virtual void applyChn(Document &Doc, TransactionalObject *pcObj, bool Forward);

    void setProperty(const Property* pcProp);
    void addOrRemoveProperty(const Property* pcProp, bool add);
    /// The freeze the object had before the transaction; the first one wins
    void setFrozen(bool wasFrozen);

    unsigned int getMemSize () const override;
    void Save (Base::Writer &writer) const override;
    /// This method is used to restore properties from an XML document.
    void Restore(Base::XMLReader &reader) override;

    friend class Transaction;

protected:
    enum Status {New,Del,Chn} status{New};

    struct PropData : DynamicProperty::PropData {
        Base::Type propertyType;
        const Property *propertyOrig = nullptr;
        /// The first write of this transaction happened while the owning
        /// object was recomputing, i.e. the value is the object's own
        /// output (docs/TransactionLogJournal.md sec 10). Recorded here because
        /// only the write site can tell; a commit-time reader cannot.
        bool derived = false;
        /// The first write of this transaction was made inside a
        /// TransactionFollowing: the value follows another -- the shape of
        /// the object a reference names, the placement of a shape's own
        /// object -- and is no change to a merge (docs/TransactionLogJournal.md
        /// sec 31.25, 31.26). Never with `derived`; a write by hand later
        /// in the transaction takes it off again.
        bool followed = false;
        /// Set by the transaction log when it hands `property` to its
        /// writer thread (docs/TransactionLogJournal.md sec 20.2, decision 4):
        /// the copy is then co-owned, and outlives this record until it
        /// is serialised. While set, `property` is not deleted here.
        std::shared_ptr<Property> shared;
        /// The log's hash of `property` when it was adopted from
        /// TransactionCopyCache: filled by the log's worker when it wrote
        /// the copy as an after value, so the before value it now is need
        /// not be serialised again (docs/TransactionLogJournal.md sec 25.4).
        std::shared_ptr<std::string> logHash;
        /// The touched state before the first write of this transaction,
        /// for the log (sec 27.58): the property's Touched bit and its
        /// object's, as TransactionLog::touchedBits() packs them; -1 for a
        /// container that is not a document object.
        int touchedBefore = -1;
    };
    std::unordered_map<int64_t, PropData> _PropChangeMap;
    /// The object's own touched bits before this transaction first wrote
    /// any of its properties; -1 until then.
    int _objectBitsBefore = -1;

    std::string _NameInDocument;
    /// -1 when the transaction did not change the object's freeze
    signed char _frozen{-1};

    friend class TransactionMeasure;
    friend class TransactionLog;
};

/** Represents an entry for a document object in a transaction
 */
class AppExport TransactionDocumentObject : public TransactionObject
{
    TYPESYSTEM_HEADER_WITH_OVERRIDE();

public:
    /// Construction
    TransactionDocumentObject();
    /// Destruction
    ~TransactionDocumentObject() override;

    void applyNew(Document &Doc, TransactionalObject *pcObj) override;
    void applyDel(Document &Doc, TransactionalObject *pcObj) override;
};

class AppExport TransactionFactory
{
public:
    static TransactionFactory& instance();
    static void destruct ();

    TransactionObject* createTransaction (const Base::Type& type) const;
    void addProducer (const Base::Type& type, Base::AbstractProducer *producer);

private:
    static TransactionFactory* self;
    std::map<Base::Type, Base::AbstractProducer*> producers;

    TransactionFactory() = default;
    ~TransactionFactory() = default;
};

template <class CLASS>
class TransactionProducer : public Base::AbstractProducer
{
public:
    explicit TransactionProducer (const Base::Type& type)
    {
        TransactionFactory::instance().addProducer(type, this);
    }

    ~TransactionProducer () override = default;

    /**
     * Creates an instance of the specified transaction object.
     */
    void* Produce () const override
    {
        return (new CLASS);
    }
};

/** The after values a transaction log copied at commit, kept for the next
 * write (docs/TransactionLogJournal.md sec 25.4).
 *
 * The log copies every property a commit set so its worker can write the
 * value at once. The next transaction to write that property would copy it
 * again for its undo before value -- the same value, because any change in
 * between passes through Property::aboutToSetValue. So the copy is kept
 * here by property id: the transaction recording the first write after the
 * commit takes it (TransactionObject::setProperty), and aboutToSetValue
 * drops whatever is left once the write's recording is done. Main thread
 * only.
 */
class AppExport TransactionCopyCache
{
public:
    struct Entry
    {
        std::shared_ptr<Property> copy;
        std::shared_ptr<std::string> hash;
        const void* owner = nullptr;
        /// The live property's status when copied, `Touched` cleared: never
        /// asked of the copy, whose getStatus() may read the objects a link
        /// names, which a detached copy does not keep alive.
        unsigned long status = 0;
    };
    static unsigned long statusOf(const Property& live);
    static void put(int64_t id, Entry entry);
    /// The entry for property `id`, removed; empty if none.
    static Entry take(int64_t id);
    static void drop(int64_t id)
    {
        if (!empty())
            dropSlow(id);
    }
    /// Every entry `owner` put, when it goes.
    static void dropOwner(const void* owner);
    static bool empty();
    static std::size_t size();

private:
    static void dropSlow(int64_t id);
};

class AppExport TransactionGuard
{
private:
    /// Private new operator to prevent heap allocation
    void* operator new(size_t size);

public:
    enum TransactionType {
        Redo,
        Undo,
        Abort,
    };
    TransactionGuard(TransactionType type);
    ~TransactionGuard();

    static bool addPendingRemove(TransactionalObject *);
    /** Run `fn` once the outermost guard has touched what the transactions
     * applied under it wrote, before it signals the undo or redo; at once
     * when no guard is active (docs/TransactionLogJournal.md sec 27.63: the
     * touched state an undo leaves is set after those touches).
     */
    static void afterTouches(std::function<void()> fn);

private:
    TransactionType transactionType;
};

/** What a view's undo lists and takes (docs/MultiViewEdit.md sec 12.9):
 * set by the Gui around a view's Undo, Redo and their lists, read by the
 * document's. The log on only: a step that is not the newest is taken back
 * from the log, so with it off every step is listed, as before.
 */
struct AppExport UndoView
{
    /// The steps made in this view and those made in none; 0 for all
    int view = 0;
    /// Only the steps in \a only (an edit's), when set
    bool limited = false;
    std::set<int> only;
    /// The steps left out (other views' open edits)
    std::set<int> hide;

    /// Whether \a step, a user's step, is listed
    bool sees(const Transaction& step) const;
};

/// The UndoView of what this thread does while the scope lives. Scopes
/// nest; the innermost wins.
class AppExport UndoViewScope
{
public:
    explicit UndoViewScope(UndoView view);
    ~UndoViewScope();

    UndoViewScope(const UndoViewScope&) = delete;
    UndoViewScope& operator=(const UndoViewScope&) = delete;

    /// The innermost scope's, null for none: every step is listed
    static const UndoView* current();

private:
    UndoView mine;
    const UndoView* previous;
};

} //namespace App

#endif // APP_TRANSACTION_H

