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


#ifndef PMIGUI_VIEWPROVIDERVIEW_H
#define PMIGUI_VIEWPROVIDERVIEW_H

#include <fastsignals/signal.h>

#include <Gui/ViewProviderDocumentObject.h>

#include <Mod/PMI/PMIGlobal.h>

class QMenu;

namespace App
{
class Document;
}
namespace PMI
{
class Callout;
class View;
}

namespace Gui
{
class View3DInventor;
class ViewerContext;
}

namespace PMIGui
{

/// The key of the active PMI view in a 3D view
static const char PMIVIEWKEY[] = "pmiview";

/** A PMI view (docs/GDandT.md sec 6.3). Made active in a view -- a desktop
 * 3D view, or a browser's view of a served document -- the camera is set
 * from it, its clipping applied, and only its annotations and its Objects
 * show there. Let go, the camera and clipping come back; everything shows
 * again. Each view keeps its own: two 3D views may show two PMI views side
 * by side, and what one hides the others do not.
 */
class PMIGuiExport ViewProviderView: public Gui::ViewProviderDocumentObject
{
    PROPERTY_HEADER_WITH_OVERRIDE(PMIGui::ViewProviderView);

public:
    ViewProviderView();
    ~ViewProviderView() override;

    void attach(App::DocumentObject* obj) override;
    void updateData(const App::Property* prop) override;
    std::vector<std::string> getDisplayModes() const override;
    void setDisplayMode(const char* mode) override;
    bool doubleClicked() override;
    void setupContextMenu(QMenu* menu, QObject* receiver, const char* member) override;
    bool onDelete(const std::vector<std::string>& subs) override;
    QIcon getIcon() const override;

    /// The PMI view active in the view \a doc is looked at through now --
    /// a browser's while its request runs (Gui::ViewerScope), else the
    /// document's active 3D view -- or null
    static const PMI::View* active(const App::Document* doc);
    /// The PMI view active in \a viewer, or null
    static const PMI::View* activeIn(const Gui::ViewerContext* viewer);
    /// The view \a doc is looked at through now (see active())
    static Gui::ViewerContext* currentViewer(const App::Document* doc);
    /// What each view \a doc's PMI views are active in hides, made again:
    /// an annotation added, or taken into or out of a view
    static void relayerAll(const App::Document* doc);
    /// The menu of the PMI views \a callout is in, for its context menu:
    /// each made active or let go in the view the menu was opened in; on
    /// the desktop, each also in a new 3D view
    static void addViewsMenu(QMenu* menu, const PMI::Callout* callout);

    /// Make it active in \a viewer, or let it go there when it is already
    void toggleIn(Gui::ViewerContext* viewer);
    /// Make it active in a new 3D view of the document, placed as an
    /// additional view is (a split by default), named after it
    Gui::View3DInventor* showInNewView();
    /// Whether a double click opens a new 3D view (preferences, PMI)
    static bool newViewOnDoubleClick();

private:
    void activateIn(Gui::ViewerContext* viewer);
    void deactivateIn(Gui::ViewerContext* viewer);
    /// The desktop 3D views' active objects, followed: made active where
    /// one names it now, let go where none names it any more
    void follow();
    void applyEverywhere();
    void relayerEverywhere();

    fastsignals::scoped_connection _connActivated;
};

}  // namespace PMIGui

#endif  // PMIGUI_VIEWPROVIDERVIEW_H
