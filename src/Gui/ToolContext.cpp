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
#include <list>
#include <map>
#include <memory>
#include <set>

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentObject.h>

#include "ToolContext.h"
#include "Application.h"
#include "Command.h"
#include "MDIView.h"
#include "MenuManager.h"
#include "ToolBarManager.h"
#include "ViewProviderDocumentObject.h"
#include "Workbench.h"
#include "WorkbenchManager.h"
#include "WorkbenchManipulator.h"

using namespace Gui;

namespace
{

/// Adds each context's tool bars to its workbenches, unavailable, and its
/// toggle where the workbench hosts toggles
class Manipulator: public WorkbenchManipulator
{
protected:
    void modifyToolBars(ToolBarItem* root) override;
    void modifyMenuBar(MenuItem* root) override;
};

/// Where a workbench shows the toggles (ToolContext::hostToggles())
struct Host
{
    std::string toolbar;
    std::string menu;
    std::string submenu;
};

struct Registry
{
    std::vector<ToolContext::Spec> specs;
    std::map<std::string, Host> hosts;
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
    auto& reg = registry();
    auto host = reg.hosts.find(wb);
    ToolBarItem* toggles = host != reg.hosts.end() ? root->findItem(host->second.toolbar) : nullptr;
    bool first = true;
    for (const auto& spec : reg.specs) {
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
        if (toggles) {
            if (first && !toggles->getItems().isEmpty()) {
                *toggles << "Separator";
            }
            first = false;
            *toggles << ToolContext::commandName(spec.name);
        }
    }
}

void Manipulator::modifyMenuBar(MenuItem* root)
{
    const std::string wb = activeWorkbench();
    auto& reg = registry();
    auto host = reg.hosts.find(wb);
    MenuItem* menu = host != reg.hosts.end() ? root->findItem(host->second.menu) : nullptr;
    if (!menu) {
        return;
    }
    MenuItem* submenu = nullptr;
    for (const auto& spec : reg.specs) {
        if (!inWorkbench(spec, wb)) {
            continue;
        }
        if (!submenu) {
            *menu << "Separator";
            submenu = new MenuItem(menu);  // NOLINT
            submenu->setCommand(host->second.submenu);
        }
        *submenu << ToolContext::commandName(spec.name);
    }
}

/// The preference group holding what is pinned in a workbench
ParameterGrp::handle pinGroup(const std::string& workbench)
{
    return App::GetApplication()
        .GetUserParameter()
        .GetGroup("BaseApp/Preferences/ToolContext/Pinned")
        ->GetGroup(workbench.c_str());
}

/// Whether something asks for the context's tool bars: its key in the active
/// view, an object of it in edit, or the user's pin
bool asked(const ToolContext::Spec& spec)
{
    if (ToolContext::isPinned(spec.name)) {
        return true;
    }
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

/// A context's toggle: checked while its tool bars are offered, for whatever
/// reason; a click pins them on, or takes the pin away (ToolContext::isPinned())
class ToggleCommand: public CheckableCommand
{
public:
    explicit ToggleCommand(const ToolContext::Spec& spec)
        : CheckableCommand(storeName(spec.name))
        , context(spec.name)
        , menuText(spec.menuText.empty() ? spec.name : spec.menuText)
        , toolTip(spec.toolTip.empty()
                      ? "Show or hide the " + menuText + " tool bars. Shown, they stay until "
                            "taken away here, across sessions; a click makes nothing"
                      : spec.toolTip)
        , icon(spec.icon)
    {
        sGroup = "View";
        sMenuText = menuText.c_str();
        sToolTipText = toolTip.c_str();
        sStatusTip = sToolTipText;
        sWhatsThis = getName();
        sPixmap = icon.empty() ? nullptr : icon.c_str();
        eType = NoTransaction | NoHistory;
    }

    const char* className() const override
    {
        return "Gui::ToolContextToggle";
    }

protected:
    bool getOption() const override
    {
        return ToolContext::isOffered(context);
    }
    void setOption(bool checked) override
    {
        ToolContext::setPinned(context, checked);
    }
    bool isActive() override
    {
        CheckableCommand::isActive();
        auto spec = ToolContext::find(context);
        return spec && inWorkbench(*spec, activeWorkbench());
    }

private:
    /// The command's name must outlive the base's constructor
    static const char* storeName(const std::string& context)
    {
        static std::list<std::string> names;
        names.push_back(ToolContext::commandName(context));
        return names.back().c_str();
    }

    std::string context;
    std::string menuText;
    std::string toolTip;
    std::string icon;
};

}  // namespace

bool ToolContext::isPinned(const std::string& name)
{
    const std::string wb = activeWorkbench();
    return !wb.empty() && pinGroup(wb)->GetBool(name.c_str(), false);
}

void ToolContext::setPinned(const std::string& name, bool pinned)
{
    const std::string wb = activeWorkbench();
    if (wb.empty()) {
        return;
    }
    if (pinned) {
        pinGroup(wb)->SetBool(name.c_str(), true);
    }
    else {
        pinGroup(wb)->RemoveBool(name.c_str());
    }
    refresh();
}

std::string ToolContext::commandName(const std::string& name)
{
    return "Std_ToolContext_" + name;
}

void ToolContext::hostToggles(const std::string& workbench, const std::string& toolbar,
                              const std::string& menu, const std::string& submenu)
{
    connect();
    registry().hosts[workbench] = Host {toolbar, menu, submenu};
}

void ToolContext::add(Spec spec)
{
    connect();
    if (Application::Instance) {
        auto& manager = Application::Instance->commandManager();
        if (!manager.getCommandByName(commandName(spec.name).c_str())) {
            manager.addCommand(new ToggleCommand(spec));  // NOLINT: the manager owns it
        }
    }
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
