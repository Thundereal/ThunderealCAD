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

#include <cmath>
#include <cstring>
#include <map>
#include <set>

#include <QPointer>

#include <Inventor/nodes/SoCamera.h>
#include <Inventor/nodes/SoOrthographicCamera.h>
#include <Inventor/nodes/SoPerspectiveCamera.h>
#include <Inventor/nodes/SoSeparator.h>

#include <App/Document.h>
#include <App/GroupExtension.h>
#include <Gui/Application.h>
#include <Gui/BitmapFactory.h>
#include <Gui/Command.h>
#include <Gui/Document.h>
#include <Gui/View3DInventor.h>
#include <Gui/View3DInventorViewer.h>
#include <Gui/ViewVisibility.h>
#include <Mod/PMI/App/Callout.h>
#include <Mod/PMI/App/View.h>

#include "ViewProviderCallout.h"
#include "ViewProviderView.h"

using namespace PMIGui;

PROPERTY_SOURCE(PMIGui::ViewProviderView, Gui::ViewProviderDocumentObject)

namespace
{
/// What a PMI view made active in a document holds: the view, the 3D view
/// it is active in, the camera from before any view was, whether it clips.
/// Per document, not per view: going from one view straight to another
/// keeps the camera from before the first.
struct Active
{
    const PMI::View* view = nullptr;
    QPointer<Gui::View3DInventor> mdi;
    SoCamera* saved = nullptr;
    bool clipped = false;
};

std::map<const App::Document*, Active>& actives()
{
    static std::map<const App::Document*, Active> map;
    return map;
}

/// The owner of a PMI view's layer of a 3D view's transient visibility
constexpr const char* VisibilityOwner = "PMI::View";
}  // namespace

void ViewProviderView::showObjects(Gui::View3DInventor* mdi, bool show)
{
    auto view = freecad_cast<PMI::View*>(getObject());
    if (!mdi || !view) {
        return;
    }
    std::vector<Gui::VisibilityEntry> hides;
    const std::vector<App::DocumentObject*> shown = view->Objects.getValues();
    if (show && !shown.empty()) {
        // Hidden: each object at the top of the tree it does not list, and
        // within a group holding one it lists, the rest of the group. What
        // it lists and the groups holding them stay, and so does the PMI
        std::set<App::DocumentObject*> keep(shown.begin(), shown.end());
        std::set<App::DocumentObject*> holders;
        for (auto obj : shown) {
            for (auto group = App::GroupExtension::getGroupOfObject(obj); group;
                 group = App::GroupExtension::getGroupOfObject(group)) {
                holders.insert(group);
            }
        }
        for (auto obj : view->getDocument()->getObjects()) {
            if (keep.count(obj) || holders.count(obj)
                || std::strncmp(obj->getTypeId().getName(), "PMI::", 5) == 0) {
                continue;
            }
            auto group = App::GroupExtension::getGroupOfObject(obj);
            bool top = !group && obj->getInList().empty();
            if (!top && !(group && holders.count(group))) {
                continue;
            }
            Gui::VisibilityEntry entry;
            entry.doc = obj->getDocument()->getName();
            entry.obj = obj->getNameInDocument();
            entry.rooted = false;
            entry.visible = false;
            hides.push_back(std::move(entry));
        }
    }
    mdi->getViewer()->setVisibilityLayer(VisibilityOwner, std::move(hides));
}

ViewProviderView::ViewProviderView() = default;

ViewProviderView::~ViewProviderView()
{
    auto obj = getObject();
    if (!obj) {
        return;
    }
    auto it = actives().find(obj->getDocument());
    if (it != actives().end() && it->second.view == obj) {
        if (it->second.saved) {
            it->second.saved->unref();
        }
        actives().erase(it);
    }
}

const PMI::View* ViewProviderView::active(const App::Document* doc)
{
    auto it = actives().find(doc);
    return it == actives().end() ? nullptr : it->second.view;
}

void ViewProviderView::attach(App::DocumentObject* obj)
{
    ViewProviderDocumentObject::attach(obj);
    addDisplayMaskMode(new SoSeparator, "View");
    _connActivated = getDocument()->signalActivatedViewProvider.connect(
        [this](const Gui::ViewProviderDocumentObject* vp, const char* name) {
            if (!name || std::strcmp(name, PMIVIEWKEY) != 0) {
                return;
            }
            // Only the view made active, or the one let go, acts
            if (vp == this) {
                activate();
            }
            else if (!vp && active(getObject()->getDocument()) == getObject()) {
                deactivate();
            }
        });
}

std::vector<std::string> ViewProviderView::getDisplayModes() const
{
    return {"View"};
}

void ViewProviderView::setDisplayMode(const char* mode)
{
    setDisplayMaskMode("View");
    ViewProviderDocumentObject::setDisplayMode(mode);
}

QIcon ViewProviderView::getIcon() const
{
    return mergeOverlayIcons(Gui::BitmapFactory().pixmap("PMI_View"));
}

bool ViewProviderView::onDelete(const std::vector<std::string>& subs)
{
    // Deleted while active: the camera and the clipping come back first
    if (active(getObject()->getDocument()) == getObject()) {
        deactivate();
    }
    return ViewProviderDocumentObject::onDelete(subs);
}

bool ViewProviderView::doubleClicked()
{
    auto doc = Gui::Application::Instance->activeDocument();
    if (!doc) {
        doc = getDocument();
    }
    auto view = doc ? doc->setActiveView(this) : nullptr;
    if (!view) {
        return false;
    }
    if (view->getActiveObject<App::DocumentObject*>(PMIVIEWKEY) == getObject()) {
        Gui::Command::doCommand(Gui::Command::Gui,
                                "Gui.ActiveDocument.ActiveView.setActiveObject('%s', None)",
                                PMIVIEWKEY);
        return true;
    }
    Gui::Command::doCommand(Gui::Command::Gui,
                            "Gui.ActiveDocument.ActiveView.setActiveObject('%s', %s)",
                            PMIVIEWKEY,
                            getObject()->getFullName(true).c_str());
    return true;
}

void ViewProviderView::activate()
{
    auto view = freecad_cast<PMI::View*>(getObject());
    auto mdi = qobject_cast<Gui::View3DInventor*>(getDocument()->getActiveView());
    if (!view || !mdi) {
        return;
    }
    Active& a = actives()[view->getDocument()];
    if (a.mdi && a.mdi != mdi) {
        // Active in another 3D view before: that one gets its camera back
        showObjects(a.mdi, false);
        if (a.clipped) {
            a.mdi->getViewer()->toggleClippingPlane(0);
        }
        if (a.saved) {
            a.mdi->getViewer()->getSoRenderManager()->getCamera()->copyFieldValues(a.saved);
            a.saved->unref();
            a.saved = nullptr;
        }
        a.clipped = false;
    }
    a.view = view;
    a.mdi = mdi;
    if (!a.saved) {
        SoCamera* cam = mdi->getViewer()->getSoRenderManager()->getCamera();
        a.saved = static_cast<SoCamera*>(cam->copy());
        a.saved->ref();
    }
    // Its objects first: a view fitting everything fits what it shows
    showObjects(mdi, true);
    apply();
    refilter();
}

void ViewProviderView::deactivate()
{
    auto it = actives().find(getObject()->getDocument());
    if (it == actives().end()) {
        return;
    }
    Active a = it->second;
    actives().erase(it);
    if (a.mdi) {
        auto viewer = a.mdi->getViewer();
        showObjects(a.mdi, false);
        if (a.clipped) {
            viewer->toggleClippingPlane(0);
        }
        if (a.saved) {
            if (viewer->getSoRenderManager()->getCamera()->getTypeId() != a.saved->getTypeId()) {
                viewer->setCameraType(a.saved->getTypeId());
            }
            viewer->getSoRenderManager()->getCamera()->copyFieldValues(a.saved);
        }
    }
    if (a.saved) {
        a.saved->unref();
    }
    refilter();
}

void ViewProviderView::apply()
{
    auto view = freecad_cast<PMI::View*>(getObject());
    if (!view) {
        return;
    }
    auto it = actives().find(view->getDocument());
    if (it == actives().end() || it->second.view != view || !it->second.mdi) {
        return;
    }
    Active& a = it->second;
    auto viewer = a.mdi->getViewer();
    SoType want = view->Perspective.getValue() ? SoPerspectiveCamera::getClassTypeId()
                                               : SoOrthographicCamera::getClassTypeId();
    if (viewer->getSoRenderManager()->getCamera()->getTypeId() != want) {
        viewer->setCameraType(want);
    }
    SoCamera* cam = viewer->getSoRenderManager()->getCamera();
    const Base::Placement& pla = view->Camera.getValue();
    double q0, q1, q2, q3;
    pla.getRotation().getValue(q0, q1, q2, q3);
    SbRotation rot {float(q0), float(q1), float(q2), float(q3)};
    cam->orientation.setValue(rot);
    if (view->Height.getValue() <= 0.0) {
        viewer->viewAll();
    }
    else {
        Base::Vector3d o = pla.getPosition();
        SbVec3f z;
        rot.multVec(SbVec3f(0, 0, 1), z);
        float dist = cam->focalDistance.getValue();
        cam->position.setValue(SbVec3f(float(o.x), float(o.y), float(o.z)) + z * dist);
        if (cam->isOfType(SoOrthographicCamera::getClassTypeId())) {
            static_cast<SoOrthographicCamera*>(cam)->height = float(view->Height.getValue());
        }
        else if (cam->isOfType(SoPerspectiveCamera::getClassTypeId())) {
            // The height shows at the focal distance
            auto persp = static_cast<SoPerspectiveCamera*>(cam);
            persp->heightAngle = float(2 * std::atan2(view->Height.getValue() / 2, dist));
        }
    }
    if (a.clipped) {
        viewer->toggleClippingPlane(0);
        a.clipped = false;
    }
    if (view->Clip.getValue()) {
        viewer->toggleClippingPlane(1, false, true, view->ClipPlane.getValue());
        a.clipped = true;
    }
}

void ViewProviderView::refilter()
{
    auto obj = getObject();
    if (!obj) {
        return;
    }
    for (auto vp : getDocument()->getViewProvidersOfType(ViewProviderCallout::getClassTypeId())) {
        static_cast<ViewProviderCallout*>(vp)->refilter();
    }
}

void ViewProviderView::updateData(const App::Property* prop)
{
    auto view = freecad_cast<PMI::View*>(getObject());
    if (view && active(view->getDocument()) == view
        && (prop == &view->Camera || prop == &view->Height || prop == &view->Perspective
            || prop == &view->Clip || prop == &view->ClipPlane)) {
        apply();
    }
    if (view && active(view->getDocument()) == view && prop == &view->Objects) {
        auto it = actives().find(view->getDocument());
        showObjects(it->second.mdi, true);
        apply();
    }
    ViewProviderDocumentObject::updateData(prop);
}
