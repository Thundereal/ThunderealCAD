// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
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
#include <QApplication>
#include <QTimer>
#endif

#include <algorithm>
#include <memory>
#include <set>

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentObject.h>

#include "ToolContext.h"
#include "Application.h"
#include "MDIView.h"
#include "ToolBarManager.h"
#include "ViewProviderDocumentObject.h"
#include "Workbench.h"
#include "WorkbenchManager.h"
#include "WorkbenchManipulator.h"

using namespace Gui;

namespace
{

/// Adds each context's tool bars to its workbenches, unavailable
class Manipulator: public WorkbenchManipulator
{
protected:
    void modifyToolBars(ToolBarItem* root) override;
};

struct Registry
{
    std::vector<ToolContext::Spec> specs;
    /// The contexts whose tool bars are offered in the active workbench
    std::set<std::string> offered;
    /// The objects in edit, by document and object name
    std::set<std::pair<std::string, std::string>> editing;
    bool scheduled = false;
    std::shared_ptr<Manipulator> manipulator;
    std::vector<fastsignals::scoped_connection> connections;
};

Registry& registry()
{
    static Registry reg;
    return reg;
}

std::string activeWorkbench()
{
    auto wb = WorkbenchManager::instance()->active();
    return wb ? wb->name() : std::string();
}

bool inWorkbench(const ToolContext::Spec& spec, const std::string& wb)
{
    return std::find(spec.workbenches.begin(), spec.workbenches.end(), wb)
        != spec.workbenches.end();
}

void Manipulator::modifyToolBars(ToolBarItem* root)
{
    const std::string wb = activeWorkbench();
    for (const auto& spec : registry().specs) {
        if (!inWorkbench(spec, wb)) {
            continue;
        }
        for (const auto& [name, fill] : spec.toolbars) {
            if (root->findItem(name)) {
                continue;  // the workbench's own, shown as it shows it
            }
            auto item = new ToolBarItem(root, ToolBarItem::DefaultVisibility::Unavailable);  // NOLINT
            item->setCommand(name);
            if (fill) {
                fill(*item);
            }
        }
    }
}

/// Whether something asks for the context's tool bars: its key in the active
/// view, or an object of it in edit
bool asked(const ToolContext::Spec& spec)
{
    if (!spec.activeKey.empty()) {
        if (auto view = Application::Instance->activeView()) {
            if (view->getActiveObject<App::DocumentObject*>(spec.activeKey.c_str())) {
                return true;
            }
        }
    }
    if (spec.applies) {
        for (const auto& [docName, objName] : registry().editing) {
            auto doc = App::GetApplication().getDocument(docName.c_str());
            auto obj = doc ? doc->getObject(objName.c_str()) : nullptr;
            if (obj && spec.applies(obj)) {
                return true;
            }
        }
    }
    return false;
}

void connect()
{
    auto& reg = registry();
    if (reg.manipulator) {
        return;
    }
    reg.manipulator = std::make_shared<Manipulator>();
    WorkbenchManipulator::installManipulator(reg.manipulator);

    auto& app = *Application::Instance;
    // A workbench sets its tool bars up afresh, the unavailable ones hidden
    reg.connections.push_back(app.signalActivateWorkbench.connect([](const char*) {
        registry().offered.clear();
        ToolContext::refresh();
    }));
    reg.connections.push_back(app.signalActivateView.connect([](const MDIView*) {
        ToolContext::refreshLater();
    }));
    reg.connections.push_back(app.signalInEdit.connect([](const ViewProviderDocumentObject& vp) {
        if (auto obj = vp.getObject(); obj && obj->isAttachedToDocument()) {
            registry().editing.emplace(obj->getDocument()->getName(), obj->getNameInDocument());
        }
        ToolContext::refreshLater();
    }));
    reg.connections.push_back(app.signalResetEdit.connect([](const ViewProviderDocumentObject& vp) {
        if (auto obj = vp.getObject(); obj && obj->isAttachedToDocument()) {
            registry().editing.erase({obj->getDocument()->getName(), obj->getNameInDocument()});
        }
        ToolContext::refreshLater();
    }));
    reg.connections.push_back(
        App::GetApplication().signalDeletedObject.connect([](const App::DocumentObject&) {
            ToolContext::refreshLater();
        }));
    reg.connections.push_back(
        App::GetApplication().signalDeleteDocument.connect([](const App::Document& doc) {
            auto& editing = registry().editing;
            for (auto it = editing.begin(); it != editing.end();) {
                it = it->first == doc.getName() ? editing.erase(it) : std::next(it);
            }
            ToolContext::refreshLater();
        }));
}

}  // namespace

void ToolContext::add(Spec spec)
{
    connect();
    auto& specs = registry().specs;
    auto it = std::find_if(specs.begin(), specs.end(), [&spec](const Spec& s) {
        return s.name == spec.name;
    });
    if (it != specs.end()) {
        *it = std::move(spec);
    }
    else {
        specs.push_back(std::move(spec));
    }
}

const std::vector<ToolContext::Spec>& ToolContext::contexts()
{
    return registry().specs;
}

const ToolContext::Spec* ToolContext::find(const std::string& name)
{
    for (const auto& spec : registry().specs) {
        if (spec.name == name) {
            return &spec;
        }
    }
    return nullptr;
}

const ToolContext::Spec* ToolContext::forKind(const std::string& kind)
{
    if (kind.empty()) {
        return nullptr;
    }
    for (const auto& spec : registry().specs) {
        if (spec.kind == kind) {
            return &spec;
        }
    }
    return nullptr;
}

bool ToolContext::isOffered(const std::string& name)
{
    return registry().offered.count(name) > 0;
}

void ToolContext::refresh()
{
    auto& reg = registry();
    const std::string wb = activeWorkbench();
    for (const auto& spec : reg.specs) {
        const bool want = inWorkbench(spec, wb) && asked(spec);
        if (want == isOffered(spec.name)) {
            continue;
        }
        if (want) {
            reg.offered.insert(spec.name);
        }
        else {
            reg.offered.erase(spec.name);
        }
        if (!inWorkbench(spec, wb)) {
            continue;  // the workbench has none of its tool bars
        }
        QList<QString> names;
        for (const auto& toolbar : spec.toolbars) {
            names << QString::fromStdString(toolbar.first);
        }
        ToolBarManager::getInstance()->setState(names,
                                                want ? ToolBarManager::State::ForceAvailable
                                                     : ToolBarManager::State::RestoreDefault);
    }
}

void ToolContext::refreshLater()
{
    auto& reg = registry();
    if (reg.scheduled || reg.specs.empty()) {
        return;
    }
    reg.scheduled = true;
    QTimer::singleShot(0, qApp, []() {
        registry().scheduled = false;
        ToolContext::refresh();
    });
}
