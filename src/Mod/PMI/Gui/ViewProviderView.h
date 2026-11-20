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

namespace App
{
class Document;
}
namespace PMI
{
class View;
}

namespace Gui
{
class View3DInventor;
}

namespace PMIGui
{

/// The key of the active PMI view in a 3D view
static const char PMIVIEWKEY[] = "pmiview";

/** A PMI view (docs/GDandT.md sec 6.3). Double-clicked, it is made active in
 * the 3D view: the camera is set from it, its clipping applied, and only its
 * annotations show. Let go, the camera and clipping come back; the
 * annotations all show again.
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
    bool onDelete(const std::vector<std::string>& subs) override;
    QIcon getIcon() const override;

    /// The PMI view active in \a doc, or null
    static const PMI::View* active(const App::Document* doc);

private:
    void activate();
    void deactivate();
    void apply();
    void refilter();
    /// Its Objects, in the 3D view it is active in: what it does not list
    /// hidden there, transiently (sec 6.3); \a show false takes that back
    void showObjects(Gui::View3DInventor* mdi, bool show);

    fastsignals::scoped_connection _connActivated;
};

}  // namespace PMIGui

#endif  // PMIGUI_VIEWPROVIDERVIEW_H
