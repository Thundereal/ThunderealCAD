// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PreCompiled.h"

#include <cstring>
#include <vector>

#include "DocumentObject.h"
#include "Expression.h"
#include "GeoFeature.h"
#include "PropertyExpressionEngine.h"
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
    // The touch state is saved, so a touch is an edit -- but in the
    // document's own file, among what it says of each object, which every
    // save writes: the object's file holds its properties and is as it was
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

std::set<DocumentObject*> writeReaches(const DocumentObject& obj, const Property& prop)
{
    auto self = const_cast<DocumentObject*>(&obj);
    const char* name = prop.getName();
    const bool relay = name && prop.getContainer() == &obj
        && (std::strcmp(name, "_GroupTouched") == 0 || std::strcmp(name, "HiddenChildren") == 0);
    if (&prop != &obj.Visibility && &prop != &obj.TreeRank && &prop != &obj.ViewObject
        && !relay) {
        std::set<DocumentObject*> reached = obj.getInListEx(true);
        reached.insert(self);
        return reached;
    }
    std::set<DocumentObject*> readers;
    if (relay) {
        // The shape gather (Part::Feature's) walks a group's children and
        // skips the hidden ones, unless the group keeps a shape of its own
        auto geo = freecad_cast<const GeoFeature*>(&obj);
        if (!geo || !geo->getPropertyOfGeometry()) {
            readers.insert(self);
        }
    }
    std::vector<DocumentObject*> candidates = obj.getInList();
    candidates.push_back(self);
    std::vector<Property*> props;
    for (auto o : candidates) {
        if (!o || readers.count(o)) {
            continue;
        }
        if (&prop == &obj.Visibility && o->getPropertyByName("Proxy")) {
            readers.insert(o);
            continue;
        }
        props.clear();
        o->getPropertyList(props);
        bool reads = false;
        for (auto p : props) {
            auto exprs = dynamic_cast<PropertyExpressionContainer*>(p);
            if (!exprs || !name) {
                continue;
            }
            for (auto& [path, expr] : exprs->getExpressions()) {
                if (!expr) {
                    continue;
                }
                auto deps = expr->getDeps(Expression::DepAll);
                auto it = deps.find(self);
                if (it != deps.end() && it->second.count(name)) {
                    reads = true;
                    break;
                }
            }
            if (reads) {
                break;
            }
        }
        if (reads) {
            readers.insert(o);
        }
    }
    std::set<DocumentObject*> reached = readers;
    for (auto o : readers) {
        o->getInListEx(reached, true);
    }
    return reached;
}

}  // namespace App
