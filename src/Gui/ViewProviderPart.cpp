/***************************************************************************
 *   Copyright (c) 2006 Werner Mayer <wmayer[at]users.sourceforge.net>     *
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
# include <Inventor/nodes/SoMaterial.h>
# include <QMenu>
#endif

#include <App/AuxGroup.h>
#include <App/AutoTransaction.h>
#include <App/Document.h>
#include <App/DocumentObject.h>
#include <App/Part.h>

#include "ViewProviderPart.h"
#include "ActionFunction.h"
#include "Application.h"
#include "BitmapFactory.h"
#include "Command.h"
#include "MDIView.h"
#include "ViewParams.h"
#include "TaskElementColors.h"
#include "Document.h"
#include "Control.h"
#include "ViewProviderLink.h"

using namespace Gui;


PROPERTY_SOURCE_WITH_EXTENSIONS(Gui::ViewProviderPart, Gui::ViewProviderGeometryObject)


/**
 * Creates the view provider for an object group.
 */
ViewProviderPart::ViewProviderPart()
{
    initExtension(this);

    ADD_PROPERTY_TYPE(OverrideMaterial, (false), 0, App::Prop_None, "Override part material");

    ADD_PROPERTY(OverrideColorList,());

    // What the part lays over what it holds is the part's, and these are
    // names over it (docs/ShapeAppearanceDesign.md sec 14.6.4)
    looks.overrideMaterial = &OverrideMaterial;
    looks.shapeAppearance = &ShapeAppearance;
    looks.overrideColorList = &OverrideColorList;
    looks.others = {&ShapeColor, &Transparency};

    sPixmap = "Geofeaturegroup.svg";
}

void ViewProviderPart::attach(App::DocumentObject *obj)
{
    inherited::attach(obj);
    // In a document being read the looks are in the file, and are taken
    // when the read of this object is done (finishRestoring())
    auto doc = obj ? obj->getDocument() : nullptr;
    if (!isRestoring() && doc && !doc->testStatus(App::Document::Restoring))
        looks.bind(obj, false);
}

void ViewProviderPart::beforeDelete()
{
    looks.unbind();
    inherited::beforeDelete();
}

ViewProviderPart::~ViewProviderPart() = default;

App::PropertyLinkSub *ViewProviderPart::getColoredElementsProperty() const {
    if(!getObject())
        return 0;
    return Base::freecad_dynamic_cast<App::PropertyLinkSub>(
            getObject()->getPropertyByName("ColoredElements"));
}

/**
 * TODO
 * Whenever a property of the group gets changed then the same property of all
 * associated view providers of the objects of the object group get changed as well.
 */
void ViewProviderPart::onChanged(const App::Property* prop) {
    looks.onChanged(prop, isRestoring());
    if (prop == &OverrideMaterial)
        pcShapeMaterial->setOverride(OverrideMaterial.getValue());
    else if(!isRestoring()) {
        if(prop == &OverrideColorList)
            applyColors();
    }

    inherited::onChanged(prop);
}

void ViewProviderPart::updateData(const App::Property *prop) {
    looks.updateData(prop);
    if(prop && !isRestoring() && !pcObject->isRestoring()) {
        auto obj = Base::freecad_dynamic_cast<App::Part>(getObject());
        if(prop == getColoredElementsProperty()) 
            applyColors();
        else if (obj && prop == &obj->Type) {
            if (obj->Type.getStrValue() == "Assembly")
                sPixmap = "Geoassembly.svg";
            else
                sPixmap = "Geofeaturegroup.svg";
            signalChangeIcon();
        }
    }
    inherited::updateData(prop);
}

void ViewProviderPart::setupContextMenu(QMenu* menu, QObject* receiver, const char* member)
{
    auto func = new Gui::ActionFunction(menu);
    QAction* act = menu->addAction(QObject::tr("Active object"));
    act->setCheckable(true);
    act->setChecked(isActivePart());
    func->trigger(act, [this](){
        this->doubleClicked();
    });

    if(getColoredElementsProperty()) {
        act = menu->addAction(QObject::tr("Override colors..."), receiver, member);
        act->setData(QVariant((int)ViewProvider::Color));
    }

    if (auto part = Base::freecad_dynamic_cast<App::Part>(getObject())) {
        act = menu->addAction(QObject::tr("Parts group"));
        act->setCheckable(true);
        act->setChecked(part->getAuxGroup(App::Part::PartsRole) != nullptr);
        act->setToolTip(QObject::tr("Show the components in a group of their own"));
        func->trigger(act, [this]() {
            this->togglePartsGroup();
        });
    }

    inherited::setupContextMenu(menu, receiver, member);
}

void ViewProviderPart::togglePartsGroup()
{
    auto part = Base::freecad_dynamic_cast<App::Part>(getObject());
    if (!part)
        return;
    App::AutoTransaction committer("Toggle Parts group");
    try {
        if (auto group = part->getAuxGroup(App::Part::PartsRole)) {
            group->setOwner(nullptr);
            part->getDocument()->removeObject(group->getNameInDocument());
            return;
        }
        auto doc = part->getDocument();
        auto group = static_cast<App::AuxGroup*>(doc->addObject("App::PartAuxGroup", "Parts"));
        // The role first: a group with none is a user's, whose label is unique
        group->Role.setValue(App::Part::PartsRole);
        group->Label.setValue("Parts");
        // A group the part makes is its own child, whatever its GroupMode
        auto children = part->Group.getValues();
        children.push_back(group);
        part->Group.setValues(children);
        group->setOwner(part);
    }
    catch (Base::Exception &e) {
        e.ReportException();
        committer.close(true);
    }
}

std::vector<App::DocumentObject*> ViewProviderPart::claimChildren() const
{
    auto children = inherited::claimChildren();
    auto part = Base::freecad_dynamic_cast<App::Part>(getObject());
    if (!part)
        return children;
    auto origin = part->Origin.getValue();
    bool hasOrigin = false;
    std::vector<App::AuxGroup*> groups;
    std::vector<App::DocumentObject*> rest;
    for (auto child : children) {
        auto group = Base::freecad_dynamic_cast<App::AuxGroup>(child);
        if (group && group->getOwner() == part)
            groups.push_back(group);
        else if (child == origin && origin)
            hasOrigin = true;
        else
            rest.push_back(child);
    }
    if (groups.empty())
        return children;
    std::stable_sort(groups.begin(), groups.end(),
            [](const App::AuxGroup *a, const App::AuxGroup *b) {
                return App::Part::getAuxGroupRank(a) < App::Part::getAuxGroupRank(b);
            });
    std::vector<App::DocumentObject*> res;
    res.reserve(children.size());
    if (hasOrigin)
        res.push_back(origin);
    res.insert(res.end(), groups.begin(), groups.end());
    res.insert(res.end(), rest.begin(), rest.end());
    return res;
}

bool ViewProviderPart::isActivePart(const char* key)
{
    auto activeDoc = Gui::Application::Instance->activeDocument();
    if (!activeDoc)
        activeDoc = getDocument();
    auto activeView = activeDoc->setActiveView(this);
    if (!activeView)
        return false;

    return activeView->getActiveObject<App::DocumentObject*>(key) == this->getObject();
}

bool ViewProviderPart::doubleClicked()
{
    //make the part the active one

    //first, check if the part is already active.
    auto activeDoc = Gui::Application::Instance->activeDocument();
    if(!activeDoc)
        activeDoc = getDocument();
    auto activeView = activeDoc->setActiveView(this);
    if(!activeView)
        return false;

    App::SubObjectT sel(getObject(), "");
    auto sels = Selection().getSelectionT("*",ResolveMode::NoResolve,true);
    if (sels.size() == 1 && sels[0].getSubObject() == getObject())
        sel = sels[0];

    if (activeView->isActiveObject(sel.getObject(),PARTKEY,sel.getSubName().c_str())) {
        //active part double-clicked. Deactivate.
        Gui::Command::doCommand(Gui::Command::Gui,
                "Gui.ActiveDocument.ActiveView.setActiveObject('%s', None)",
                PARTKEY);
    } else {
        //set new active part
        Gui::Command::doCommand(Gui::Command::Gui,
                "Gui.ActiveDocument.ActiveView.setActiveObject('%s', %s, u'%s')",
                PARTKEY, sel.getObjectPython().c_str(), sel.getSubName().c_str());
    }

    return true;
}

bool ViewProviderPart::setEdit(int ModNum)
{
    if (ModNum == ViewProvider::Color) {
        TaskView::TaskDialog *dlg = Control().activeDialog();
        if (dlg) {
            Control().showDialog(dlg);
            return false;
        }
        Selection().clearSelection();
        return true;
    }
    return inherited::setEdit(ModNum);
}

void ViewProviderPart::setEditViewer(Gui::ViewerContext* viewer, int ModNum)
{
    if (ModNum == ViewProvider::Color) {
        Gui::Control().showDialog(new TaskElementColors(this));
        return;
    }
    return inherited::setEditViewer(viewer,ModNum);
}

std::map<std::string, App::Color> ViewProviderPart::getElementColors(const char *subname) const {
    std::map<std::string, App::Color> res;
    if(!getObject())
        return res;
    auto prop = getColoredElementsProperty();
    if(!prop)
        return res;
    const auto mat = ShapeAppearance.getBase();
    return ViewProviderLink::getElementColorsFrom(*this,subname,*prop,
            OverrideColorList, OverrideMaterial.getValue(), &mat);
}

void ViewProviderPart::setElementColors(const std::map<std::string, App::Color> &colorMap) {
    if(!getObject())
        return;
    auto prop = getColoredElementsProperty();
    if(!prop)
        return;
    ViewProviderLink::setElementColorsTo(*this,colorMap,*prop,OverrideColorList, 
            &OverrideMaterial, &ShapeAppearance);
}

void ViewProviderPart::applyColors() {
    prevColorOverride = ViewProviderLink::applyColorsTo(*this, prevColorOverride);
}

void ViewProviderPart::buildChildren3D() {
    ViewProviderGeoFeatureGroupExtension::buildChildren3D();
    if(getObject() && !getObject()->isRestoring())
        applyColors();
}

void ViewProviderPart::finishRestoring() {
    inherited::finishRestoring();
    looks.bind(getObject(), true);
    applyColors();
}

// Python feature -----------------------------------------------------------------------

namespace Gui {
/// @cond DOXERR
PROPERTY_SOURCE_TEMPLATE(Gui::ViewProviderPartPython, Gui::ViewProviderPart)
/// @endcond

// explicit template instantiation
template class GuiExport ViewProviderFeaturePythonT<ViewProviderPart>;
}
