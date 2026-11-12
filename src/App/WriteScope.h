// SPDX-License-Identifier: LGPL-2.1-or-later

#ifndef APP_WRITESCOPE_H
#define APP_WRITESCOPE_H

#include <FCGlobal.h>

namespace App
{

/** Who is writing a property, as bits: several hold at once (an origin
 * fitted while an Animation plays).
 *
 * A write is classified once, by Document::writeEffects(), from who writes
 * (these) and what is written (WriteTraits); everything that has to decide
 * something about a write -- the undo stack, the transaction log, the folder
 * save, the Gui's modified flag, an Animation's Record -- reads the
 * WriteEffect bits of the answer and never asks about the cause itself.
 * The table is writeEffectsOf(), and tests/src/App/WriteScope.cpp pins it.
 *
 * ViewFit and Animation are set by a WriteScope; the rest is the document's
 * own state, which Document::writeEffects() adds.
 */
namespace WriteCause
{
enum : unsigned
{
    /// A command, a script, the user: what a WriteScope of it goes back to
    Edit = 0,
    /// View state the Gui derives from the model -- an origin fitted to the
    /// model's extent (docs/TransactionLogJournal.md sec 24.10)
    ViewFit = 1U << 0,
    /// An Animation playing (docs/Animation.md sec 5.1)
    Animation = 1U << 1,
    /// The document's own bookkeeping: a compatibility name put on an
    /// object (TransactionLogJournal.md sec 36.7)
    Bookkeeping = 1U << 2,
    /// An undo, a redo, a rollback or a replay putting recorded values back
    Transacting = 1U << 3,
    /// The document is being restored
    Restoring = 1U << 4,
    /// A recompute writing what it derives
    Recompute = 1U << 5,
    /// A deferred load serving the file's value to one object
    /// (docs/DocumentLoad.md sec 14): the restore's value, not a change
    DeferredLoad = 1U << 6,
};
}  // namespace WriteCause

/// What a write does, as bits
namespace WriteEffect
{
enum : unsigned
{
    /// Opens an undo step when none is open
    Step = 1U << 0,
    /// Is recorded in the undo step that is open, and so in the log's row
    Undo = 1U << 1,
    /// Changes what the owner's file holds: a folder save writes it again
    Dirty = 1U << 2,
    /// Marks the Gui document modified
    Modified = 1U << 3,
    /// Is keyed by an Animation that is recording
    Key = 1U << 4,
};
}  // namespace WriteEffect

/// What is written
struct WriteTraits
{
    /// A touch of the owner, not a property write
    bool touch = false;
    /// A view provider's property
    bool view = false;
    /// The object's ViewObject, which its view provider touches to say one
    /// of its own changed
    bool viewMirror = false;
    /// Never saved: Transient or NoPersist, by type or by status
    bool unsaved = false;
    /// Property::NoModify
    bool noModify = false;
    /// Property::Legacy: a name over a value kept elsewhere
    bool legacy = false;
    /// The transaction log is on
    bool logOn = false;
    /// With the log off: a view provider's property opens a step
    /// (ViewObjectTransaction, or an AutoTransaction asking for it)
    bool viewUndo = false;
};

/// The table: the WriteEffect bits of a write of `traits` under `causes`
AppExport unsigned writeEffectsOf(unsigned causes, const WriteTraits& traits);

/** Says who writes while it lives. Scopes add up -- under Animation and
 * ViewFit a write is both, and loses what either takes away -- but one of
 * WriteCause::Edit starts over: a real edit made from inside the others.
 */
class AppExport WriteScope
{
public:
    explicit WriteScope(unsigned cause);
    ~WriteScope();
    WriteScope(const WriteScope&) = delete;
    WriteScope& operator=(const WriteScope&) = delete;

    /// The causes the live scopes set
    static unsigned causes();

private:
    unsigned saved;
};

}  // namespace App

#endif  // APP_WRITESCOPE_H
