// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PreCompiled.h"

#include "WriteScope.h"

namespace App
{

namespace
{
unsigned scopeCauses = 0;
}

WriteScope::WriteScope(unsigned cause)
    : saved(scopeCauses)
{
    scopeCauses = cause == WriteCause::Edit ? 0U : (scopeCauses | cause);
}

WriteScope::~WriteScope()
{
    scopeCauses = saved;
}

unsigned WriteScope::causes()
{
    return scopeCauses;
}

unsigned writeEffectsOf(unsigned causes, const WriteTraits& t)
{
    // The file's value arriving late: nothing changed
    if (causes & WriteCause::DeferredLoad) {
        return 0;
    }
    // Played, not edited: in no step, no change to the file (a save writes
    // the rest values), not keyed
    const bool animated = (causes & WriteCause::Animation) != 0;
    if (t.touch) {
        return animated ? 0U : unsigned(WriteEffect::Modified);
    }
    // Fitted to the model: only a view provider's own state is, and the
    // mirror that says so
    const bool fitted = (causes & WriteCause::ViewFit) && (t.view || t.viewMirror);

    unsigned fx = 0;
    if (!animated && !t.unsaved) {
        fx |= WriteEffect::Dirty;
    }
    if (!animated && !(causes & WriteCause::Bookkeeping)) {
        fx |= WriteEffect::Undo;
        bool quiet = t.noModify || t.legacy;
        if (t.view) {
            // With the log on a saved view property is document data like
            // any other; with it off the user's setting decides
            quiet = quiet
                || (t.logOn ? (t.unsaved || (causes & WriteCause::ViewFit)) : !t.viewUndo);
        }
        if (!quiet) {
            fx |= WriteEffect::Step;
        }
    }
    if (!animated && !fitted && !t.noModify) {
        fx |= WriteEffect::Modified;
    }
    if (!animated
        && !(causes
             & (WriteCause::Transacting | WriteCause::Restoring | WriteCause::Recompute))) {
        fx |= WriteEffect::Key;
    }
    return fx;
}

}  // namespace App
