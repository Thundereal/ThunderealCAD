// SPDX - License - Identifier: LGPL - 2.1 - or -later
/****************************************************************************
 *                                                                          *
 *   Copyright (c) 2025 Pierre-Louis Boyer                                  *
 *                                                                          *
 *   This file is part of FreeCAD.                                          *
 *                                                                          *
 *   FreeCAD is free software: you can redistribute it and/or modify it     *
 *   under the terms of the GNU Lesser General Public License as            *
 *   published by the Free Software Foundation, either version 2.1 of the   *
 *   License, or (at your option) any later version.                        *
 *                                                                          *
 *   FreeCAD is distributed in the hope that it will be useful, but         *
 *   WITHOUT ANY WARRANTY; without even the implied warranty of             *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU       *
 *   Lesser General Public License for more details.                        *
 *                                                                          *
 *   You should have received a copy of the GNU Lesser General Public       *
 *   License along with FreeCAD. If not, see                                *
 *   <https://www.gnu.org/licenses/>.                                       *
 *                                                                          *
 ***************************************************************************/

#include <cstring>
#include <vector>

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentObject.h>
#include <Gui/Action.h>
#include <Gui/Application.h>
#include <Gui/CommandT.h>
#include <Gui/Document.h>
#include <Gui/MainWindow.h>
#include <Gui/Selection/Selection.h>
#include <Gui/Tree.h>

#include <Mod/Assembly/App/AssemblyLink.h>
#include <Mod/Assembly/App/AssemblyObject.h>
#include <Mod/Assembly/App/AssemblyParams.h>
#include <Mod/Assembly/App/AssemblyUtils.h>

#include "Commands.h"
#include "ViewProviderAssembly.h"


using namespace Assembly;
using namespace AssemblyGui;

// Helper function to get the active AssemblyObject in edit mode
static AssemblyObject* getActiveAssembly()
{
    Gui::Document* doc = Gui::Application::Instance->activeDocument();
    if (!doc) {
        return nullptr;
    }

    auto* vp = doc->getInEdit();
    if (auto* assemblyVP = freecad_cast<ViewProviderAssembly*>(vp)) {
        return assemblyVP->getObject<AssemblyObject>();
    }

    return nullptr;
}

void selectObjects(const std::vector<App::DocumentObject*>& objectsToSelect)
{
    if (objectsToSelect.empty()) {
        return;
    }

    Gui::Selection().clearSelection();
    for (App::DocumentObject* obj : objectsToSelect) {
        Gui::Selection().addSelection(obj->getDocument()->getName(), obj->getNameInDocument());
    }
}

void selectObjectsByName(AssemblyObject* assembly, const std::vector<std::string>& names)
{
    if (!assembly || names.empty()) {
        return;
    }

    std::vector<App::DocumentObject*> objectsToSelect;
    App::Document* doc = assembly->getDocument();

    for (const auto& name : names) {
        if (auto* obj = doc->getObject(name.c_str())) {
            objectsToSelect.push_back(obj);
        }
    }

    selectObjects(objectsToSelect);
}

// ================================================================================
// Go to Linked Assembly
// ================================================================================

DEF_STD_CMD_A(CmdAssemblyLinkSelectLinked)

CmdAssemblyLinkSelectLinked::CmdAssemblyLinkSelectLinked()
    : Command("Assembly_LinkSelectLinked")
{
    sGroup = QT_TR_NOOP("Assembly");
    sMenuText = QT_TR_NOOP("Go to Linked Assembly");
    sToolTipText = QT_TR_NOOP("Selects the linked assembly and switches to its original document");
    sWhatsThis = "Assembly_LinkSelectLinked";
    sStatusTip = sToolTipText;
    eType = AlterSelection;
    sPixmap = "LinkSelect";
    sAccel = "S, G";
}

void CmdAssemblyLinkSelectLinked::activated(int iMsg)
{
    Q_UNUSED(iMsg);

    std::vector<Gui::SelectionObject> selection = Gui::Selection().getSelectionEx();
    if (selection.size() != 1) {
        return;
    }

    auto* asmLink = dynamic_cast<Assembly::AssemblyLink*>(selection[0].getObject());

    if (!asmLink) {
        return;
    }

    // Get the linked object (usually an AssemblyObject in another doc)
    App::DocumentObject* linkedObj = asmLink->getLinkedAssembly();
    if (!linkedObj) {
        return;
    }

    Gui::Selection().clearSelection();
    Gui::Selection().addSelection(linkedObj->getDocument()->getName(), linkedObj->getNameInDocument());

    // Switch view/tab
    Gui::Document* guiDoc = Gui::Application::Instance->getDocument(linkedObj->getDocument());
    if (guiDoc) {
        // Try to activate the view containing the object
        Gui::ViewProvider* vp = guiDoc->getViewProvider(linkedObj);
        if (auto vpDoc = dynamic_cast<Gui::ViewProviderDocumentObject*>(vp)) {
            guiDoc->setActiveView(vpDoc);
        }
    }
}

bool CmdAssemblyLinkSelectLinked::isActive()
{
    std::vector<Gui::SelectionObject> selection = Gui::Selection().getSelectionEx();
    return (
        selection.size() == 1 && selection[0].getObject()
        && selection[0].getObject()->isDerivedFrom(Assembly::AssemblyLink::getClassTypeId())
    );
}


// ================================================================================
// Select Conflicting Constraints
// ================================================================================

DEF_STD_CMD_A(CmdAssemblySelectConflictingConstraints)

CmdAssemblySelectConflictingConstraints::CmdAssemblySelectConflictingConstraints()
    : Command("Assembly_SelectConflictingConstraints")
{
    sGroup = QT_TR_NOOP("Assembly");
    sMenuText = QT_TR_NOOP("Select Conflicting Constraints");
    sToolTipText = QT_TR_NOOP("Selects conflicting joints in the active assembly");
    sWhatsThis = "Assembly_SelectConflictingConstraints";
    sStatusTip = sToolTipText;
    eType = ForEdit;
}

void CmdAssemblySelectConflictingConstraints::activated(int iMsg)
{
    Q_UNUSED(iMsg);
    AssemblyObject* assembly = getActiveAssembly();
    if (!assembly) {
        return;
    }

    selectObjectsByName(assembly, assembly->getLastConflicting());
}

bool CmdAssemblySelectConflictingConstraints::isActive()
{
    return getActiveAssembly() != nullptr;
}

// ================================================================================
// Select Redundant Constraints
// ================================================================================

DEF_STD_CMD_A(CmdAssemblySelectRedundantConstraints)

CmdAssemblySelectRedundantConstraints::CmdAssemblySelectRedundantConstraints()
    : Command("Assembly_SelectRedundantConstraints")
{
    sGroup = QT_TR_NOOP("Assembly");
    sMenuText = QT_TR_NOOP("Select Redundant Constraints");
    sToolTipText = QT_TR_NOOP("Selects redundant joints in the active assembly");
    sWhatsThis = "Assembly_SelectRedundantConstraints";
    sStatusTip = sToolTipText;
    eType = ForEdit;
}

void CmdAssemblySelectRedundantConstraints::activated(int iMsg)
{
    Q_UNUSED(iMsg);
    AssemblyObject* assembly = getActiveAssembly();
    if (!assembly) {
        return;
    }

    selectObjectsByName(assembly, assembly->getLastRedundant());
}

bool CmdAssemblySelectRedundantConstraints::isActive()
{
    return getActiveAssembly() != nullptr;
}

// ================================================================================
// Select Malformed Constraints
// ================================================================================

DEF_STD_CMD_A(CmdAssemblySelectMalformedConstraints)

CmdAssemblySelectMalformedConstraints::CmdAssemblySelectMalformedConstraints()
    : Command("Assembly_SelectMalformedConstraints")
{
    sGroup = QT_TR_NOOP("Assembly");
    sMenuText = QT_TR_NOOP("Select Malformed Constraints");
    sToolTipText = QT_TR_NOOP("Selects malformed joints in the active assembly");
    sWhatsThis = "Assembly_SelectMalformedConstraints";
    sStatusTip = sToolTipText;
    eType = ForEdit;
}

void CmdAssemblySelectMalformedConstraints::activated(int iMsg)
{
    Q_UNUSED(iMsg);
    AssemblyObject* assembly = getActiveAssembly();
    if (!assembly) {
        return;
    }

    selectObjectsByName(assembly, assembly->getLastMalformed());
}

bool CmdAssemblySelectMalformedConstraints::isActive()
{
    return getActiveAssembly() != nullptr;
}


// ================================================================================
// Select Components with Degrees of Freedom
// ================================================================================

DEF_STD_CMD_A(CmdAssemblySelectComponentsWithDoFs)

CmdAssemblySelectComponentsWithDoFs::CmdAssemblySelectComponentsWithDoFs()
    : Command("Assembly_SelectComponentsWithDoFs")
{
    sGroup = QT_TR_NOOP("Assembly");
    sMenuText = QT_TR_NOOP("Select Components With DoFs");
    sToolTipText = QT_TR_NOOP("Selects unconstrained components in the active assembly");
    sWhatsThis = "Assembly_SelectComponentsWithDoFs";
    sStatusTip = sToolTipText;
    eType = ForEdit;
}

void CmdAssemblySelectComponentsWithDoFs::activated(int iMsg)
{
    Q_UNUSED(iMsg);
    AssemblyObject* assembly = getActiveAssembly();
    if (!assembly) {
        return;
    }

    std::vector<App::DocumentObject*> objectsToSelect;
    std::vector<App::DocumentObject*> allParts = getAssemblyComponents(assembly);

    // Iterate through all collected parts and check their connectivity
    for (App::DocumentObject* part : allParts) {
        if (!assembly->isPartConnected(part)) {
            objectsToSelect.push_back(part);
        }
    }

    selectObjects(objectsToSelect);
}

bool CmdAssemblySelectComponentsWithDoFs::isActive()
{
    return getActiveAssembly() != nullptr;
}

// ================================================================================
// Select Joints of Component
// ================================================================================

DEF_STD_CMD_A(CmdAssemblySelectJointsOfComponent)

CmdAssemblySelectJointsOfComponent::CmdAssemblySelectJointsOfComponent()
    : Command("Assembly_SelectJointsOfComponent")
{
    sGroup = QT_TR_NOOP("Assembly");
    sMenuText = QT_TR_NOOP("Select Component Joints");
    sToolTipText = QT_TR_NOOP("Selects all joints referencing the selected component");
    sWhatsThis = "Assembly_SelectJointsOfComponent";
    sStatusTip = sToolTipText;
    sPixmap = "Assembly_SelectJointsOfComponent";
    eType = ForEdit;
}

void CmdAssemblySelectJointsOfComponent::activated(int iMsg)
{
    Q_UNUSED(iMsg);
    AssemblyObject* assembly = getActiveAssembly();
    if (!assembly) {
        return;
    }

    auto selection = Gui::Selection().getSelectionEx(
        "",
        App::DocumentObject::getClassTypeId(),
        Gui::ResolveMode::NoResolve
    );
    if (selection.empty()) {
        return;
    }

    std::set<App::DocumentObject*> components;

    for (auto& sel : selection) {
        const std::vector<std::string> subs = sel.getSubNames();
        std::string sub = subs.empty() ? "" : subs.front();

        if (App::DocumentObject* comp = getMovingPartFromSel(assembly, sel.getObject(), sub)) {
            components.insert(comp);
        }
    }

    if (components.empty()) {
        return;
    }

    std::vector<App::DocumentObject*> jointsToSelect;
    for (auto* comp : components) {
        std::vector<App::DocumentObject*> partJoints = assembly->getJointsOfPart(comp);
        jointsToSelect.insert(jointsToSelect.end(), partJoints.begin(), partJoints.end());
    }

    selectObjects(jointsToSelect);
}

bool CmdAssemblySelectJointsOfComponent::isActive()
{
    return getActiveAssembly() != nullptr && !Gui::Selection().getSelection().empty();
}


// ================================================================================
// Collision While Dragging
// ================================================================================

// The DragCollision preference, read from its group: AssemblyParams has the
// new value only once its own observer is told, and the commands below
// observe the group too
static long dragCollision()
{
    return App::GetApplication()
        .GetParameterGroupByPath("User parameter:BaseApp/Preferences/Mod/Assembly")
        ->GetInt("DragCollision", 0);
}

// One value of the DragCollision preference (docs/AssemblyInterference.md),
// checked while it is the one set
class CmdAssemblyDragCollisionMode: public Gui::CheckableCommand
{
public:
    CmdAssemblyDragCollisionMode(const char* name, AssemblyObject::DragCollision mode)
        : Gui::CheckableCommand(name)
        , mode(mode)
    {
        sGroup = QT_TR_NOOP("Assembly");
        sWhatsThis = name;
        // Off is no mode for the tool bar button to turn on
        eType = mode == AssemblyObject::DragCollision::Off ? NoDefaultAction : 0;
        bCanLog = false;
    }
    const char* className() const override
    {
        return "CmdAssemblyDragCollisionMode";
    }

protected:
    bool getOption() const override
    {
        return dragCollision() == static_cast<long>(mode);
    }
    void setOption(bool checked) override
    {
        // Unchecked as another mode is checked, by the group or by the menu's
        // radio buttons following it: that one has set the preference, or
        // will. Off has nothing to undo.
        if (checked) {
            AssemblyParams::setDragCollision(static_cast<long>(mode));
        }
        else if (getOption() && mode != AssemblyObject::DragCollision::Off) {
            AssemblyParams::setDragCollision(static_cast<long>(AssemblyObject::DragCollision::Off));
        }
    }

private:
    AssemblyObject::DragCollision mode;
};

/** The DragCollision preference on the tool bar: the button turns collision
 * while dragging on and off, in the mode its menu chose last (Highlight or
 * Stop); the menu has the three values. It follows the preference, as the
 * preference page and Python set it too, and the next drag reads it.
 */
class CmdAssemblyDragCollision: public Gui::GroupCommand
{
public:
    CmdAssemblyDragCollision()
        : GroupCommand("Assembly_DragCollision", StopIndex)
    {
        sGroup = QT_TR_NOOP("Assembly");
        sMenuText = QT_TR_NOOP("Collision While Dragging");
        sToolTipText = QT_TR_NOOP("What dragging a part does when it brings two components "
                                  "into collision");
        sWhatsThis = "Assembly_DragCollision";
        sStatusTip = sToolTipText;
        eType = 0;
        bCanLog = false;
        setCheckable(true);
        setExclusive(true);

        // The children's texts are translated in their own class's context
        auto off = new CmdAssemblyDragCollisionMode("Assembly_DragCollisionOff",
                                                    AssemblyObject::DragCollision::Off);
        off->setMenuText(QT_TRANSLATE_NOOP("CmdAssemblyDragCollisionMode", "Off"));
        off->setToolTipText(QT_TRANSLATE_NOOP("CmdAssemblyDragCollisionMode",
                                              "Dragging a part does not look for collisions"));
        off->setStatusTip(off->getToolTipText());
        addCommand(off);

        auto highlight = new CmdAssemblyDragCollisionMode("Assembly_DragCollisionHighlight",
                                                          AssemblyObject::DragCollision::Highlight);
        highlight->setMenuText(
            QT_TRANSLATE_NOOP("CmdAssemblyDragCollisionMode", "Highlight Collisions"));
        highlight->setToolTipText(QT_TRANSLATE_NOOP(
            "CmdAssemblyDragCollisionMode",
            "Dragging a part shows the components it brings into collision in red.\n"
            "Components interfering when the drag starts are left out."));
        highlight->setStatusTip(highlight->getToolTipText());
        highlight->setPixmap("Assembly_DragCollision");
        addCommand(highlight);

        auto stop = new CmdAssemblyDragCollisionMode("Assembly_DragCollisionStop",
                                                     AssemblyObject::DragCollision::Stop);
        stop->setMenuText(QT_TRANSLATE_NOOP("CmdAssemblyDragCollisionMode", "Stop at Collisions"));
        stop->setToolTipText(QT_TRANSLATE_NOOP(
            "CmdAssemblyDragCollisionMode",
            "Dragging a part stops it before it collides with another component,\n"
            "the two shown in red. Components interfering when the drag starts\n"
            "are left out."));
        stop->setStatusTip(stop->getToolTipText());
        // One icon: the button's checked state says whether it is on, its
        // menu in which mode
        stop->setPixmap("Assembly_DragCollision");
        addCommand(stop);

        hParam = App::GetApplication().GetParameterGroupByPath(
            "User parameter:BaseApp/Preferences/Mod/Assembly");
        hParam->Attach(this);
    }
    ~CmdAssemblyDragCollision() override
    {
        hParam->Detach(this);
    }
    const char* className() const override
    {
        return "CmdAssemblyDragCollision";
    }

    void OnChange(Base::Subject<const char*>& caller, const char* reason) override
    {
        (void)caller;
        if (reason && std::strcmp(reason, "DragCollision") == 0) {
            sync(_pcAction);
        }
    }

protected:
    enum
    {
        OffIndex,
        HighlightIndex,
        StopIndex
    };

    Gui::Action* createAction() override
    {
        Gui::Action* action = GroupCommand::createAction();
        if (auto group = qobject_cast<Gui::ActionGroup*>(action)) {
            // the button unchecks the mode it shows, leaving none checked
            // until Off is
            group->setExclusiveOptional(true);
        }
        sync(action);
        return action;
    }

    // Not a sync: invoke() asks this between the click checking the button
    // and activated() reading it
    bool isActive() override
    {
        return true;
    }

    void activated(int iMsg) override
    {
        if (triggerSource() == TriggerNone) {
            // From a script, Gui.runCommand("Assembly_DragCollision", n): the
            // value n, where a group would give its child n its own state
            if (iMsg >= OffIndex && iMsg <= StopIndex) {
                AssemblyParams::setDragCollision(iMsg);
            }
        }
        else {
            GroupCommand::activated(iMsg);
        }
        sync(_pcAction);
    }

private:
    void sync(Gui::Action* action)
    {
        auto group = qobject_cast<Gui::ActionGroup*>(action);
        if (!group) {
            return;
        }
        int index = OffIndex;
        switch (static_cast<AssemblyObject::DragCollision>(dragCollision())) {
            case AssemblyObject::DragCollision::Highlight:
                index = HighlightIndex;
                break;
            case AssemblyObject::DragCollision::Stop:
                index = StopIndex;
                break;
            default:
                break;
        }
        int i = 0;
        for (auto cmd : getCommands()) {
            if (auto child = cmd->getAction()) {
                child->setChecked(i == index, true);
            }
            ++i;
        }
        if (index != OffIndex && group->property("defaultAction").toInt() != index) {
            group->setProperty("defaultAction", index);
            setup(group);
        }
        group->setChecked(index != OffIndex, true);
    }

    ParameterGrp::handle hParam;
};


// ================================================================================
// Clear Collision Markers
// ================================================================================

// The assemblies whose collision markers the command clears: the one in edit,
// else those of the active document
static std::vector<AssemblyObject*> markedAssemblies()
{
    std::vector<AssemblyObject*> marked;
    if (auto* assembly = getActiveAssembly()) {
        if (!assembly->getCollisionMarkers().empty()) {
            marked.push_back(assembly);
        }
        return marked;
    }
    Gui::Document* doc = Gui::Application::Instance->activeDocument();
    if (!doc) {
        return marked;
    }
    for (auto* assembly : doc->getDocument()->getObjectsOfType<AssemblyObject>()) {
        if (!assembly->getCollisionMarkers().empty()) {
            marked.push_back(assembly);
        }
    }
    return marked;
}

DEF_STD_CMD_A(CmdAssemblyClearCollisionMarkers)

CmdAssemblyClearCollisionMarkers::CmdAssemblyClearCollisionMarkers()
    : Command("Assembly_ClearCollisionMarkers")
{
    sGroup = QT_TR_NOOP("Assembly");
    sMenuText = QT_TR_NOOP("Clear Collision Markers");
    sToolTipText =
        QT_TR_NOOP("Removes the arrows that mark where dragging brought components into collision");
    sWhatsThis = "Assembly_ClearCollisionMarkers";
    sStatusTip = sToolTipText;
    sPixmap = "Assembly_ClearCollisionMarkers";
    // The markers are not the document's: nothing to undo or record
    bCanLog = false;
}

void CmdAssemblyClearCollisionMarkers::activated(int iMsg)
{
    Q_UNUSED(iMsg);
    for (auto* assembly : markedAssemblies()) {
        assembly->clearCollisionMarkers();
    }
}

bool CmdAssemblyClearCollisionMarkers::isActive()
{
    return !markedAssemblies().empty();
}


// ================================================================================
// Command Creation
// ================================================================================

void AssemblyGui::CreateAssemblyCommands()
{
    Gui::CommandManager& rcCmdMgr = Gui::Application::Instance->commandManager();

    rcCmdMgr.addCommand(new CmdAssemblyLinkSelectLinked());
    rcCmdMgr.addCommand(new CmdAssemblySelectConflictingConstraints());
    rcCmdMgr.addCommand(new CmdAssemblySelectRedundantConstraints());
    rcCmdMgr.addCommand(new CmdAssemblySelectMalformedConstraints());
    rcCmdMgr.addCommand(new CmdAssemblySelectComponentsWithDoFs());
    rcCmdMgr.addCommand(new CmdAssemblySelectJointsOfComponent());
    rcCmdMgr.addCommand(new CmdAssemblyDragCollision());
    rcCmdMgr.addCommand(new CmdAssemblyClearCollisionMarkers());
}
