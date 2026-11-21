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

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <set>
#include <vector>

#include <QAction>
#include <QMenu>
#include <QPointer>

#include <Inventor/nodes/SoCamera.h>
#include <Inventor/nodes/SoOrthographicCamera.h>
#include <Inventor/nodes/SoPerspectiveCamera.h>
#include <Inventor/nodes/SoSeparator.h>

#include <App/Application.h>
#include <App/Document.h>
#include <App/GroupExtension.h>
#include <Gui/Application.h>
#include <Gui/BitmapFactory.h>
#include <Gui/Command.h>
#include <Gui/Document.h>
#include <Gui/MainWindow.h>
#include <Gui/View3DInventor.h>
#include <Gui/View3DInventorViewer.h>
#include <Gui/ViewVisibility.h>
#include <Gui/ViewerContext.h>
#include <Mod/PMI/App/Callout.h>
#include <Mod/PMI/App/View.h>

#include "ViewProviderCallout.h"
#include "ViewProviderView.h"

using namespace PMIGui;

PROPERTY_SOURCE(PMIGui::ViewProviderView, Gui::ViewProviderDocumentObject)

namespace
{
/// A PMI view made active in one view -- a desktop 3D view, or a browser's
/// (Gui::MirrorViewer): the view it is, the camera from before any PMI view
/// was active there, whether it clips. Per view, not per PMI view: going
/// from one PMI view straight to another keeps the camera from before the
/// first.
struct Active
{
    Gui::ViewerContext* viewer = nullptr;
    /// Expires with the view, a browser's going with its client
    std::weak_ptr<const void> life;
    /// The desktop 3D view, which has a camera and clipping of its own
    QPointer<Gui::View3DInventor> mdi;
    const App::Document* doc = nullptr;
    const PMI::View* view = nullptr;
    SoCamera* saved = nullptr;
    bool clipped = false;
};

std::vector<Active>& actives()
{
    static std::vector<Active> list;
    return list;
}

/// A view closed, a browser gone: what it held goes with it. Done where a
/// request comes in, never under a loop over the list
void prune()
{
    auto& list = actives();
    for (auto it = list.begin(); it != list.end();) {
        if (it->life.expired()) {
            if (it->saved) {
                it->saved->unref();
            }
            it = list.erase(it);
        }
        else {
            ++it;
        }
    }
}

Active* find(const Gui::ViewerContext* viewer)
{
    if (!viewer) {
        return nullptr;
    }
    for (auto& a : actives()) {
        if (a.viewer == viewer) {
            return &a;
        }
    }
    return nullptr;
}

/// The owner of a PMI view's layer of a view's transient visibility
constexpr const char* VisibilityOwner = "PMI::View";

Gui::VisibilityEntry hide(const App::DocumentObject* obj)
{
    Gui::VisibilityEntry entry;
    entry.doc = obj->getDocument()->getName();
    entry.obj = obj->getNameInDocument();
    entry.rooted = false;
    entry.visible = false;
    return entry;
}

/// What \a view hides in a view it is active in: every annotation it does
/// not show, and when it lists Objects, each object at the top of the tree
/// it does not list, and within a group holding one it lists, the rest of
/// the group. What it lists and the groups holding them stay
std::vector<Gui::VisibilityEntry> hidesOf(const PMI::View* view)
{
    std::vector<Gui::VisibilityEntry> hides;
    const std::vector<App::DocumentObject*> shown = view->Objects.getValues();
    std::set<App::DocumentObject*> keep(shown.begin(), shown.end());
    std::set<App::DocumentObject*> holders;
    for (auto obj : shown) {
        for (auto group = App::GroupExtension::getGroupOfObject(obj); group;
             group = App::GroupExtension::getGroupOfObject(group)) {
            holders.insert(group);
        }
    }
    for (auto obj : view->getDocument()->getObjects()) {
        if (auto callout = freecad_cast<PMI::Callout*>(obj)) {
            if (!callout->inView(view)) {
                hides.push_back(hide(obj));
            }
            continue;
        }
        if (shown.empty() || keep.count(obj) || holders.count(obj)
            || std::strncmp(obj->getTypeId().getName(), "PMI::", 5) == 0) {
            continue;
        }
        auto group = App::GroupExtension::getGroupOfObject(obj);
        bool top = !group && obj->getInList().empty();
        if (top || (group && holders.count(group))) {
            hides.push_back(hide(obj));
        }
    }
    return hides;
}

void relayer(const Active& a)
{
    a.viewer->setVisibilityLayer(VisibilityOwner, hidesOf(a.view));
}

/// Set the camera from \a view, and on the desktop its clipping. A
/// browser's camera is its own: it is asked to turn it (a section is not
/// streamed, so a browser's view does not clip)
void apply(Active& a)
{
    const PMI::View* view = a.view;
    if (!a.mdi) {
        const Base::Placement& pla = view->Camera.getValue();
        double q0, q1, q2, q3;
        pla.getRotation().getValue(q0, q1, q2, q3);
        const Base::Vector3d o = pla.getPosition();
        a.viewer->turnClientCamera(SbRotation(float(q0), float(q1), float(q2), float(q3)),
                                   SbVec3f(float(o.x), float(o.y), float(o.z)),
                                   float(view->Height.getValue()));
        return;
    }
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

/// Take back what \a a did to its view: the layer, the clipping, the camera
void restore(Active& a)
{
    a.viewer->setVisibilityLayer(VisibilityOwner, {});
    if (!a.mdi) {
        a.viewer->restoreClientCamera();
        return;
    }
    auto viewer = a.mdi->getViewer();
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

std::vector<Gui::View3DInventor*> views3D(Gui::Document* gdoc)
{
    std::vector<Gui::View3DInventor*> views;
    if (gdoc) {
        for (auto mdi : gdoc->getMDIViewsOfType(Gui::View3DInventor::getClassTypeId())) {
            views.push_back(static_cast<Gui::View3DInventor*>(mdi));
        }
    }
    return views;
}

Gui::View3DInventor* mdiOf(Gui::ViewerContext* viewer)
{
    auto gdoc = viewer ? viewer->getDocument() : nullptr;
    for (auto mdi : views3D(gdoc)) {
        if (mdi->getViewer() == viewer) {
            return mdi;
        }
    }
    return nullptr;
}

/// Mark \a action as one that changes only the view it runs in, which a
/// browser may run whatever its access (SceneContextMenu)
void viewerOnly(QAction* action)
{
    action->setProperty(Gui::ViewProviderDocumentObject::ViewerEntryProperty, true);
}
}  // namespace

ViewProviderView::ViewProviderView() = default;

ViewProviderView::~ViewProviderView()
{
    auto obj = getObject();
    auto& list = actives();
    for (auto it = list.begin(); it != list.end();) {
        if (it->view == obj) {
            if (it->saved) {
                it->saved->unref();
            }
            it = list.erase(it);
        }
        else {
            ++it;
        }
    }
}

bool ViewProviderView::newViewOnDoubleClick()
{
    return App::GetApplication()
        .GetParameterGroupByPath("User parameter:BaseApp/Preferences/Mod/PMI")
        ->GetBool("DoubleClickNewView", false);
}

Gui::ViewerContext* ViewProviderView::currentViewer(const App::Document* doc)
{
    if (auto viewer = Gui::ViewerContext::current()) {
        auto gdoc = viewer->getDocument();
        if (gdoc && gdoc->getDocument() == doc) {
            return viewer;
        }
    }
    auto gdoc = Gui::Application::Instance->getDocument(doc);
    auto mdi = gdoc ? qobject_cast<Gui::View3DInventor*>(gdoc->getActiveView()) : nullptr;
    return mdi ? mdi->getViewer() : nullptr;
}

const PMI::View* ViewProviderView::activeIn(const Gui::ViewerContext* viewer)
{
    prune();
    const Active* a = find(viewer);
    return a ? a->view : nullptr;
}

const PMI::View* ViewProviderView::active(const App::Document* doc)
{
    return activeIn(currentViewer(doc));
}

void ViewProviderView::relayerAll(const App::Document* doc)
{
    prune();
    for (const auto& a : actives()) {
        if (a.doc == doc) {
            relayer(a);
        }
    }
}

void ViewProviderView::activateIn(Gui::ViewerContext* viewer)
{
    auto view = freecad_cast<PMI::View*>(getObject());
    if (!view || !viewer) {
        return;
    }
    prune();
    Active* a = find(viewer);
    if (!a) {
        Active fresh;
        fresh.viewer = viewer;
        fresh.life = viewer->lifetime();
        fresh.mdi = mdiOf(viewer);
        fresh.doc = view->getDocument();
        actives().push_back(fresh);
        a = &actives().back();
        if (a->mdi) {
            SoCamera* cam = a->mdi->getViewer()->getSoRenderManager()->getCamera();
            a->saved = static_cast<SoCamera*>(cam->copy());
            a->saved->ref();
        }
    }
    a->view = view;
    // What it shows first: a view fitting everything fits what it shows
    relayer(*a);
    apply(*a);
}

void ViewProviderView::deactivateIn(Gui::ViewerContext* viewer)
{
    auto& list = actives();
    for (auto it = list.begin(); it != list.end(); ++it) {
        if (it->viewer == viewer) {
            Active a = *it;
            list.erase(it);
            restore(a);
            if (a.saved) {
                a.saved->unref();
            }
            return;
        }
    }
}

void ViewProviderView::toggleIn(Gui::ViewerContext* viewer)
{
    auto obj = getObject();
    if (!viewer || !obj) {
        return;
    }
    // A desktop 3D view says what is active in it with its active objects,
    // which the tree highlights: set there, and followed (follow())
    if (auto mdi = mdiOf(viewer)) {
        bool on = mdi->getActiveObject<App::DocumentObject*>(PMIVIEWKEY) != obj;
        mdi->setActiveObject(on ? obj : nullptr, PMIVIEWKEY);
        return;
    }
    if (activeIn(viewer) == obj) {
        deactivateIn(viewer);
    }
    else {
        activateIn(viewer);
    }
}

Gui::View3DInventor* ViewProviderView::showInNewView()
{
    auto obj = getObject();
    auto gdoc = getDocument();
    if (!obj || !gdoc) {
        return nullptr;
    }
    auto mdi = qobject_cast<Gui::View3DInventor*>(
        gdoc->createView(Gui::View3DInventor::getClassTypeId()));
    if (!mdi) {
        return nullptr;
    }
    mdi->setWindowTitle(QStringLiteral("%1 : %2[*]")
                            .arg(QString::fromUtf8(obj->getDocument()->Label.getValue()),
                                 QString::fromUtf8(obj->Label.getValue())));
    Gui::getMainWindow()->setActiveWindow(mdi);
    mdi->setActiveObject(obj, PMIVIEWKEY);
    return mdi;
}

void ViewProviderView::follow()
{
    auto obj = getObject();
    for (auto mdi : views3D(getDocument())) {
        auto viewer = mdi->getViewer();
        auto named = mdi->getActiveObject<App::DocumentObject*>(PMIVIEWKEY);
        if (named == obj) {
            if (activeIn(viewer) != obj) {
                activateIn(viewer);
            }
        }
        else if (!named && activeIn(viewer) == obj) {
            deactivateIn(viewer);
        }
    }
}

void ViewProviderView::applyEverywhere()
{
    for (auto& a : actives()) {
        if (a.view == getObject()) {
            apply(a);
        }
    }
}

void ViewProviderView::relayerEverywhere()
{
    for (const auto& a : actives()) {
        if (a.view == getObject()) {
            relayer(a);
        }
    }
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
            // Made active somewhere, or let go: which 3D view it was the
            // signal does not say, their active objects do
            if (vp == this || !vp) {
                follow();
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
    // Deleted while active: the cameras and the clipping come back first
    auto obj = getObject();
    for (auto mdi : views3D(getDocument())) {
        if (mdi->getActiveObject<App::DocumentObject*>(PMIVIEWKEY) == obj) {
            mdi->setActiveObject(nullptr, PMIVIEWKEY);
        }
    }
    std::vector<Gui::ViewerContext*> still;
    for (const auto& a : actives()) {
        if (a.view == obj) {
            still.push_back(a.viewer);
        }
    }
    for (auto viewer : still) {
        deactivateIn(viewer);
    }
    return ViewProviderDocumentObject::onDelete(subs);
}

bool ViewProviderView::doubleClicked()
{
    auto obj = getObject();
    auto gdoc = getDocument();
    if (!obj || !gdoc) {
        return false;
    }
    auto current = qobject_cast<Gui::View3DInventor*>(gdoc->getActiveView());
    if (newViewOnDoubleClick()
        && !(current && current->getActiveObject<App::DocumentObject*>(PMIVIEWKEY) == obj)) {
        // Active in a 3D view already: that one comes up; else a new one
        for (auto mdi : views3D(gdoc)) {
            if (mdi->getActiveObject<App::DocumentObject*>(PMIVIEWKEY) == obj) {
                Gui::getMainWindow()->setActiveWindow(mdi);
                return true;
            }
        }
        return showInNewView() != nullptr;
    }
    auto view = gdoc->setActiveView(this);
    if (!view) {
        return false;
    }
    if (view->getActiveObject<App::DocumentObject*>(PMIVIEWKEY) == obj) {
        Gui::Command::doCommand(Gui::Command::Gui,
                                "Gui.ActiveDocument.ActiveView.setActiveObject('%s', None)",
                                PMIVIEWKEY);
        return true;
    }
    Gui::Command::doCommand(Gui::Command::Gui,
                            "Gui.ActiveDocument.ActiveView.setActiveObject('%s', %s)",
                            PMIVIEWKEY,
                            obj->getFullName(true).c_str());
    return true;
}

void ViewProviderView::setupContextMenu(QMenu* menu, QObject* receiver, const char* member)
{
    auto obj = getObject();
    auto viewer = obj ? currentViewer(obj->getDocument()) : nullptr;
    if (viewer) {
        QAction* act = menu->addAction(activeIn(viewer) == obj ? QObject::tr("Let go")
                                                                : QObject::tr("Make active"));
        viewerOnly(act);
        std::weak_ptr<const void> life = viewer->lifetime();
        QObject::connect(act, &QAction::triggered, [this, viewer, life] {
            // The view may have gone while the menu was open
            if (!life.expired()) {
                toggleIn(viewer);
            }
        });
    }
    // A new 3D view is a window of the desktop's: not from a browser
    if (!Gui::ViewerContext::current()) {
        QAction* act = menu->addAction(QObject::tr("Show in new 3D view"));
        QObject::connect(act, &QAction::triggered, [this] {
            showInNewView();
        });
    }
    ViewProviderDocumentObject::setupContextMenu(menu, receiver, member);
}

void ViewProviderView::addViewsMenu(QMenu* menu, const PMI::Callout* callout)
{
    if (!callout || callout->Views.getValues().empty()) {
        return;
    }
    App::Document* doc = callout->getDocument();
    Gui::ViewerContext* viewer = currentViewer(doc);
    auto gdoc = Gui::Application::Instance->getDocument(doc);
    if (!viewer || !gdoc) {
        return;
    }
    QMenu* sub = menu->addMenu(QObject::tr("PMI views"));
    const PMI::View* now = activeIn(viewer);
    std::weak_ptr<const void> life = viewer->lifetime();
    for (auto obj : callout->Views.getValues()) {
        auto vp = freecad_cast<ViewProviderView*>(gdoc->getViewProvider(obj));
        if (!vp) {
            continue;
        }
        QAction* act = sub->addAction(QString::fromUtf8(obj->Label.getValue()));
        act->setCheckable(true);
        act->setChecked(now == obj);
        viewerOnly(act);
        QObject::connect(act, &QAction::triggered, [vp, viewer, life] {
            if (!life.expired()) {
                vp->toggleIn(viewer);
            }
        });
    }
    if (now) {
        auto vp = freecad_cast<ViewProviderView*>(gdoc->getViewProvider(
            const_cast<PMI::View*>(now)));
        QAction* act = sub->addAction(QObject::tr("Show all annotations"));
        viewerOnly(act);
        QObject::connect(act, &QAction::triggered, [vp, viewer, life] {
            if (vp && !life.expired() && activeIn(viewer) == vp->getObject()) {
                vp->toggleIn(viewer);
            }
        });
    }
    if (!Gui::ViewerContext::current()) {
        sub->addSeparator();
        QMenu* fresh = sub->addMenu(QObject::tr("In a new 3D view"));
        for (auto obj : callout->Views.getValues()) {
            auto vp = freecad_cast<ViewProviderView*>(gdoc->getViewProvider(obj));
            if (!vp) {
                continue;
            }
            QAction* act = fresh->addAction(QString::fromUtf8(obj->Label.getValue()));
            QObject::connect(act, &QAction::triggered, [vp] {
                vp->showInNewView();
            });
        }
    }
}

void ViewProviderView::updateData(const App::Property* prop)
{
    auto view = freecad_cast<PMI::View*>(getObject());
    if (view
        && (prop == &view->Camera || prop == &view->Height || prop == &view->Perspective
            || prop == &view->Clip || prop == &view->ClipPlane)) {
        applyEverywhere();
    }
    if (view && prop == &view->Objects) {
        relayerEverywhere();
        applyEverywhere();
    }
    ViewProviderDocumentObject::updateData(prop);
}
