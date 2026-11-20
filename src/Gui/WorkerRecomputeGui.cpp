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

#include <map>
#include <string>

#include <QTimer>

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentObject.h>
#include <App/WorkerPool.h>
#include <App/WorkerRecompute.h>
#include <Base/Console.h>
#include <Base/Parameter.h>

#include "MainWindow.h"
#include "WorkerRecomputeGui.h"

FC_LOG_LEVEL_INIT("Workers", true, true)

using namespace Gui;

namespace
{

/// What a document's recompute had come to when it was handed over: a
/// change of these says how it ended
struct Watch
{
    long applied = 0;
    long borrowed = 0;
    long failed = 0;
    long unfit = 0;
    long cancelled = 0;
};

std::map<std::string, Watch>& watched()
{
    static std::map<std::string, Watch> map;
    return map;
}

Watch watchOf(const App::WorkerRecompute::Info& i)
{
    Watch w;
    w.applied = i.applied;
    w.borrowed = i.borrowed;
    w.failed = i.failed;
    w.unfit = i.unfit;
    w.cancelled = i.cancelled;
    return w;
}

void say(const QString& text, int ms = 0)
{
    if (auto mw = getMainWindow()) {
        mw->showMessage(text, ms);
    }
}

void onChanged(const App::Document& doc)
{
    auto it = watched().find(doc.getName());
    if (it == watched().end()) {
        return;
    }
    App::WorkerRecompute::Info info = App::WorkerRecompute::info(doc);
    if (info.running) {
        return;
    }
    Watch was = it->second;
    watched().erase(it);
    const QString label = QString::fromUtf8(doc.Label.getValue());
    if (info.applied > was.applied || info.borrowed > was.borrowed) {
        say(QObject::tr("%1 recomputed by a worker: %2 s there, %3 s putting it in")
                .arg(label)
                .arg(info.workerSeconds, 0, 'f', 1)
                .arg(info.applySeconds, 0, 'f', 1),
            8000);
        return;
    }
    if (info.cancelled > was.cancelled) {
        // The design changed under it: what is touched stays touched, as an
        // edit leaves it
        say(QObject::tr("%1: the recompute by a worker was dropped: %2")
                .arg(label, QString::fromUtf8(info.reason.c_str())),
            8000);
        return;
    }
    // Failed or unfit: the recompute asked for is run here
    FC_LOG(doc.getName() << ": recomputed here: " << info.reason);
    say(QObject::tr("%1: recomputing here: %2")
            .arg(label, QString::fromUtf8(info.reason.c_str())),
        8000);
    const std::string name = doc.getName();
    QTimer::singleShot(0, [name]() {
        auto d = App::GetApplication().getDocument(name.c_str());
        if (!d || App::WorkerRecompute::running(*d)) {
            return;
        }
        try {
            d->recompute();
        }
        catch (Base::Exception& e) {
            e.ReportException();
        }
    });
}

}  // namespace

namespace
{

ParameterGrp::handle workerParams()
{
    return App::GetApplication().GetParameterGroupByPath(
        "User parameter:BaseApp/Preferences/Workers");
}

bool handOver(App::Document& doc)
{
    static fastsignals::scoped_connection conn =
        App::WorkerRecompute::signalChanged.connect(&onChanged);
    if (App::WorkerRecompute::running(doc)) {
        // Asked again: a recompute here waits for it and takes its result
        return false;
    }
    std::string why;
    if (!App::WorkerRecompute::start(doc, {}, &why)) {
        FC_LOG(doc.getName() << ": not recomputed by a worker: " << why);
        return false;
    }
    watched()[doc.getName()] = watchOf(App::WorkerRecompute::info(doc));
    say(QObject::tr("%1: recomputing in a worker process")
            .arg(QString::fromUtf8(doc.Label.getValue())));
    return true;
}

}  // namespace

bool Gui::recomputeByWorkers(App::Document& doc)
{
    App::WorkerRecompute::trackTimes();
    if (!workerParams()->GetBool("RecomputeCommand", true)) {
        return false;
    }
    return handOver(doc);
}

bool Gui::editRecomputeIsLong(const App::Document& doc)
{
    App::WorkerRecompute::trackTimes();
    auto group = workerParams();
    if (!group->GetBool("RecomputeEdits", true) || App::WorkerRecompute::running(doc)
        || !App::WorkerPool::supported()) {
        return false;
    }
    // One that crashed a worker before goes to a worker again, however
    // short: it may crash again (docs/WorkerProcess.md sec 34)
    if (App::WorkerRecompute::risky(doc)) {
        FC_LOG(doc.getName() << ": the edit's recompute reaches one that crashed a worker");
        return true;
    }
    const double expected = App::WorkerRecompute::expectedSeconds(doc);
    const double most = group->GetFloat("RecomputeEditSeconds", 0.5);
    FC_LOG(doc.getName() << ": the edit's recompute is expected to take " << expected << " s");
    return expected >= most;
}

bool Gui::recomputeEditByWorkers(App::Document& doc)
{
    return workerParams()->GetBool("RecomputeEdits", true) && handOver(doc);
}

bool Gui::makeEditedFirst(App::DocumentObject& edited)
{
    App::WorkerRecompute::trackTimes();
    App::Document* doc = edited.getDocument();
    auto group = workerParams();
    if (!doc || !group->GetBool("RecomputeEdits", true) || App::WorkerRecompute::running(*doc)
        || !App::WorkerPool::supported() || doc->testStatus(App::Document::Recomputing)) {
        return false;
    }
    // What it reaches crashed a worker before: all of it to a worker, the
    // edited one too, however short (docs/WorkerProcess.md sec 34)
    if (App::WorkerRecompute::risky(*doc, {&edited})) {
        FC_LOG(edited.getFullName() << ": its recompute reaches one that crashed a worker");
        return true;
    }
    const std::vector<App::DocumentObject*> users = edited.getInList();
    if (users.empty()) {
        return false;
    }
    const double rest = App::WorkerRecompute::expectedSeconds(*doc, users);
    FC_LOG(edited.getFullName() << ": what uses it is expected to take " << rest << " s");
    if (rest < group->GetFloat("RecomputeEditSeconds", 0.5)) {
        return false;
    }
    if (edited.isTouched() || edited.mustExecute()) {
        // Up to date once made, as a recompute of the document leaves it;
        // one in error stays touched, for the caller to tell
        if (doc->recomputeFeature(&edited) && edited.isValid()) {
            edited.purgeTouched();
        }
    }
    // Made: what uses it is what is left, and is made from it
    for (auto obj : edited.getInList()) {
        obj->enforceRecompute();
    }
    return true;
}
