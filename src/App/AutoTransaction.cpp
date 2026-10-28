/****************************************************************************
 *   Copyright (c) 2019 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
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

#include <Base/Interpreter.h>
#include <optional>

#include "AutoTransaction.h"
#include "Application.h"
#include "Document.h"
#include "Transactions.h"


FC_LOG_LEVEL_INIT("App", true, true)

using namespace App;

static int _TransactionLock;
static int _TransactionClosed;
static int _TransactionViewObject;

AutoTransaction::AutoTransaction(const char *name, bool tmpName, bool recordViewObject) {
    auto &app = GetApplication();
    recordViewObj = recordViewObject;
    if (recordViewObject)
        ++_TransactionViewObject;
    if(name && name[0] && app._activeTransactionGuard>=0) {
        if(!app.getActiveTransaction()
                || (!tmpName && app._activeTmpName()))
        {
            FC_LOG("auto transaction '" << name << "', " << tmpName);
            tid = app.setActiveTransaction(name);
            app._activeTmpName() = tmpName;
        }
    }
    // We use negative transaction guard to disable auto transaction from here
    // and any stack below. This is to support user setting active transaction
    // before having any existing AutoTransaction on stack, or 'persist'
    // transaction that can out live AutoTransaction.
    if(app._activeTransactionGuard<0)
        --app._activeTransactionGuard;
    else if(tid || app._activeTransactionGuard>0)
        ++app._activeTransactionGuard;
    else if(app.getActiveTransaction()) {
        FC_TRACE("auto transaction disabled because of '" << app._activeTransactionName << "'");
        --app._activeTransactionGuard;
    } else
        ++app._activeTransactionGuard;
    FC_TRACE("construct auto Transaction " << app._activeTransactionGuard);
}

AutoTransaction::~AutoTransaction() {
    auto &app = GetApplication();
    FC_TRACE("before destruct auto Transaction " << app._activeTransactionGuard);
    if (recordViewObj)
        --_TransactionViewObject;
    if(app._activeTransactionGuard<0)
        ++app._activeTransactionGuard;
    else if(!app._activeTransactionGuard) {
#ifdef FC_DEBUG
        FC_ERR("Transaction guard error");
#endif
    } else if(--app._activeTransactionGuard == 0) {
        try {
            // We don't call close() here, because close() only closes
            // transaction that we opened during construction time. However,
            // when _activeTransactionGuard reaches zero here, we are supposed
            // to close any transaction opened.
            app.closeActiveTransaction();
        } catch(Base::Exception &e) {
            e.ReportException();
        } catch(...)
        {}
    }
    FC_TRACE("destruct auto Transaction " << app._activeTransactionGuard);
}

bool AutoTransaction::recordViewObjectChange()
{
    return _TransactionViewObject > 0;
}

static int _DerivedViewWrites;

DerivedViewWrites::DerivedViewWrites()
{
    ++_DerivedViewWrites;
}

DerivedViewWrites::~DerivedViewWrites()
{
    --_DerivedViewWrites;
}

bool DerivedViewWrites::active()
{
    return _DerivedViewWrites > 0;
}

void AutoTransaction::close(bool abort) {
    if(tid || abort) {
        GetApplication().closeActiveTransaction(abort,abort?0:tid);
        tid = 0;
    }
}

void AutoTransaction::setEnable(bool enable) {
    auto &app = GetApplication();
    if(!app._activeTransactionGuard)
        return;
    if((enable && app._activeTransactionGuard>0)
            || (!enable && app._activeTransactionGuard<0))
        return;
    app._activeTransactionGuard = -app._activeTransactionGuard;
    FC_TRACE("toggle auto Transaction " << app._activeTransactionGuard);
    if(!enable && app._activeTmpName()) {
        bool close = true;
        for(auto &v : app.DocMap) {
            if(v.second->hasPendingTransaction()) {
                close = false;
                break;
            }
        }
        if(close)
            app.closeActiveTransaction();
    }
}

bool AutoTransaction::isEnabled() {
    return GetApplication()._activeTransactionGuard > 0;
}

Application::InvocationScope::InvocationScope(const char* origin)
{
    GetApplication()._invocations.push_back(origin ? origin : "");
}

Application::InvocationScope::~InvocationScope()
{
    auto& app = GetApplication();
    if (app._invocations.empty())
        return;
    app._invocations.pop_back();
    if (app._invocations.empty())
        app.commitImplicitTransactions();
}

const char* Application::InvocationScope::current()
{
    auto& inv = GetApplication()._invocations;
    return inv.empty() ? "" : inv.back();
}

int Application::InvocationScope::depth()
{
    return static_cast<int>(GetApplication()._invocations.size());
}

void Application::commitImplicitTransactions()
{
    for (auto& v : DocMap)
        v.second->commitImplicitTransaction();
}

const Application::BoundTransaction *Application::_boundTransaction(const Document *doc) const
{
    if (_boundTransactions.empty())
        return nullptr;
    if (doc) {
        auto it = _boundTransactions.find(doc);
        if (it != _boundTransactions.end())
            return &it->second;
    }
    auto it = _boundTransactions.find(_pActiveDoc);
    return it == _boundTransactions.end() ? nullptr : &it->second;
}

Application::BoundTransaction *Application::_boundTransaction(const Document *doc)
{
    return const_cast<BoundTransaction*>(
        static_cast<const Application*>(this)->_boundTransaction(doc));
}

bool &Application::_activeTmpName()
{
    if (auto bound = _boundTransaction())
        return bound->tmpName;
    return _activeTransactionTmpName;
}

bool Application::_ownedByOtherBinding(const Document *doc, const BoundTransaction *bound) const
{
    for (auto &v : _boundTransactions) {
        if (&v.second == bound)
            continue;
        if (v.first == doc)
            return true;
        // A document changed in that edit's step, not bound itself
        if (v.second.id && doc->hasPendingTransaction()
                && doc->getTransactionID(true) == v.second.id)
            return true;
    }
    return false;
}

bool Application::isActiveTransactionBound(const Document *doc) const
{
    return doc && _boundTransactions.count(doc) != 0;
}

void Application::bindActiveTransaction(Document *doc)
{
    if (!doc || isActiveTransactionBound(doc))
        return;
    BoundTransaction bound;
    // The application's own, not whatever the active document's binding
    // is: the one the command that started the edit opened
    if (_activeTransactionID && Transaction::getLastID() == _activeTransactionID) {
        bound.name = _activeTransactionName;
        bound.id = _activeTransactionID;
        bound.tmpName = _activeTransactionTmpName;
        _activeTransactionID = 0;
        _activeTransactionTmpName = false;
    }
    FC_LOG("bind active transaction of " << doc->getName() << " '" << bound.name
           << "' " << bound.id);
    _boundTransactions[doc] = std::move(bound);
}

void Application::unbindActiveTransaction(Document *doc, bool abort)
{
    auto it = _boundTransactions.find(doc);
    if (it == _boundTransactions.end())
        return;
    const int id = it->second.id;
    FC_LOG("unbind active transaction of " << doc->getName() << " '" << it->second.name
           << "' " << id);
    if (id && ((_activeTransactionGuard > 0 && !abort) || _TransactionLock)) {
        // The close is put off: to the end of the command it is asked in
        // (AutoTransaction), or of the lock. Either closes the application's
        // active one then, so it is handed back to the application -- as it
        // was when there was only that
        _activeTransactionID = id;
        _activeTransactionName = it->second.name;
        _activeTransactionTmpName = it->second.tmpName;
        _boundTransactions.erase(it);
        closeActiveTransaction(abort, id);
        return;
    }
    // Closed while the binding still has it, so the application's own
    // active transaction -- another document's step -- is left alone
    if (id)
        closeActiveTransaction(abort, id);
    _boundTransactions.erase(doc);
}

int Application::setActiveTransaction(const char *name, bool persist) {
    return setActiveTransaction(name, persist, nullptr);
}

int Application::setActiveTransaction(const char *name, bool persist, const Document *doc) {
    if(!name || !name[0])
        name = "Command";

    // A document's own while it is bound and active (or named), else the
    // application's; looked up by its document again after the commits
    // below, which may run anything
    BoundTransaction *bound = _boundTransaction(doc);
    const Document *boundDoc = nullptr;
    if (bound) {
        for (auto &v : _boundTransactions) {
            if (&v.second == bound)
                boundDoc = v.first;
        }
    }

    int activeId = 0;
    getActiveTransaction(&activeId, doc);
    bool &tmpName = bound ? bound->tmpName : _activeTransactionTmpName;
    if(_activeTransactionGuard>0 && activeId) {
        if(tmpName) {
            FC_LOG("transaction rename to '" << name << "'");
            for(auto &v : DocMap)
                v.second->renameTransaction(name,activeId);
        } else {
            if(persist)
                AutoTransaction::setEnable(false);
            return 0;
        }
    } else if (_TransactionLock) {
        if (FC_LOG_INSTANCE.isEnabled(FC_LOGLEVEL_LOG))
            FC_WARN("Transaction locked, ignore new transaction '" << name << "'");
        return 0;
    } else {
        FC_LOG("set active transaction '" << name << "'"
               << (boundDoc ? " of " : "") << (boundDoc ? boundDoc->getName() : ""));
        if (bound)
            bound->id = 0;
        else
            _activeTransactionID = 0;
        // Every document's open transaction is committed, but one that
        // another document's edit is in the middle of (entry 232)
        for(auto &v : DocMap) {
            if (!_ownedByOtherBinding(v.second, bound))
                v.second->_commitTransaction();
        }
        bound = nullptr;
        if (boundDoc) {
            auto it = _boundTransactions.find(boundDoc);
            if (it != _boundTransactions.end())
                bound = &it->second;
        }
        activeId = Transaction::getNewID();
        if (bound)
            bound->id = activeId;
        else
            _activeTransactionID = activeId;
    }
    if (bound) {
        bound->tmpName = false;
        bound->name = name;
    }
    else {
        _activeTransactionTmpName = false;
        _activeTransactionName = name;
    }
    if(persist)
        AutoTransaction::setEnable(false);
    return activeId;
}

const char *Application::getActiveTransaction(int *id) const {
    return getActiveTransaction(id, nullptr);
}

const char *Application::getActiveTransaction(int *id, const Document *doc) const {
    if (auto bound = _boundTransaction(doc)) {
        if (id)
            *id = bound->id;
        return bound->id ? bound->name.c_str() : nullptr;
    }
    int tid = 0;
    if(Transaction::getLastID() == _activeTransactionID)
        tid = _activeTransactionID;
    if (id)
        *id = tid;
    return tid ? _activeTransactionName.c_str() : nullptr;
}

void Application::closeActiveTransaction(bool abort, int id) {
    OperationScope scope;   // sec 27.38
    auto stillOpen = [this](int tid) {
        for (auto &v : DocMap) {
            if (v.second->hasPendingTransaction() && v.second->getTransactionID(true) == tid)
                return true;
        }
        return false;
    };

    if (abort && id && !stillOpen(id)) {
        // An explicit id may name a transaction that has already been closed --
        // cancelling a transformation task does exactly that, and undoing it is how
        // the object gets back to where it started. Only a closed one may take this
        // path: undo() commits a still open transaction before undoing it, so it
        // would announce itself as a commit and come to rest on the redo stack
        // rather than being discarded, which is not what aborting means.
        TransactionSignaller signaller(abort,false);
        // No guard around a document with a log: its undo(id) holds its own
        // and lets go of it around a step that goes through the log, which
        // refuses under one -- a step another actor wrote past (sec 30.10,
        // 37.7 item 3). Without the log, one guard for them all as before.
        std::optional<TransactionGuard> guard;
        for(auto &v : DocMap) {
            if (v.second->getTransactionLog())
                guard.reset();
            else if (!guard)
                guard.emplace(TransactionGuard::Abort);
            v.second->undo(id);
        }
        for (auto &v : _boundTransactions) {
            if (v.second.id == id)
                v.second.id = 0;
        }
        return;
    }

    if (!id) {
        if (auto bound = _boundTransaction())
            id = bound->id;
        else
            id = _activeTransactionID;
    }
    if(!id)
        return;

    if(_activeTransactionGuard>0 && !abort) {
        FC_LOG("ignore close transaction");
        return;
    }

    if(_TransactionLock) {
        if(_TransactionClosed >= 0)
            _TransactionLock = abort?-1:1;
        FC_LOG("pending " << (abort?"abort":"close") << " transaction");
        return;
    }

    FC_LOG("close transaction " << id << " " << abort);
    // The binding that has it, else the application's: a step one edit
    // closes ends no other's (entry 232)
    bool wasBound = false;
    for (auto &v : _boundTransactions) {
        if (v.second.id == id) {
            v.second.id = 0;
            wasBound = true;
        }
    }
    if (!wasBound || id == _activeTransactionID)
        _activeTransactionID = 0;

    TransactionSignaller signaller(abort,false);
    if (abort) {
        std::vector<Document*> aborted;
        {
            TransactionGuard guard(TransactionGuard::Abort);
            for(auto &v : DocMap) {
                if(v.second->getTransactionID(true) != id)
                    continue;
                bool wasOpen = v.second->hasPendingTransaction();
                v.second->_abortTransaction();
                if(wasOpen && !v.second->hasPendingTransaction())
                    aborted.push_back(v.second);
            }
        }
        // The guard flushes the property changes the rollback deferred as it
        // unwinds. Announcing the abort before that would hand an observer the
        // leftovers of a transaction it has already been told is gone.
        for(auto doc : aborted)
            doc->signalAbortTransaction(*doc);
    } else {
        for(auto &v : DocMap) {
            if(v.second->getTransactionID(true) != id)
                continue;
            v.second->_commitTransaction();
        }
    }
}

////////////////////////////////////////////////////////////////////////

TransactionLocker::TransactionLocker(bool lock)
    :active(lock)
{
    if(lock)
        ++_TransactionLock;
}

TransactionLocker::~TransactionLocker()
{
    if(active) {
        try {
            activate(false);
            return;
        } catch (Base::Exception &e) {
            e.ReportException();
        } catch (Py::Exception &) {
            Base::PyException e;
            e.ReportException();
        } catch (std::exception &e) {
            FC_ERR(e.what());
        } catch (...) {
        }
        FC_ERR("Exception when unlocking transaction");
    }
}

void TransactionLocker::activate(bool enable)
{
    if(active == enable)
        return;

    active = enable;
    if(active) {
        ++_TransactionLock;
        return;
    }

    if(--_TransactionLock != 0)
        return;

    if(_TransactionClosed) {
        bool abort = (_TransactionClosed<0);
        _TransactionClosed = 0;
        GetApplication().closeActiveTransaction(abort);
    }
}

bool TransactionLocker::isLocked() {
    return _TransactionLock > 0;
}

