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

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <QObject>

#include <App/DocumentObserver.h>
#include <Mod/Surface/App/Geom/EdgeCondition.h>

class QComboBox;
class QDoubleSpinBox;
class QWidget;

namespace App
{
class DocumentObject;
class PropertyLinkSub;
class PropertyLinkSubList;
}  // namespace App

namespace Gui
{
class GizmoContainer;
class LinearGizmo;
class OnViewValueSet;
class QuantitySpinBox;
class ViewProviderDragger;
}  // namespace Gui

namespace SurfaceGui
{

/** The edge conditions in the view (docs/Surface.md sec 8): an arrow at the
 * middle of each outer boundary, pointing into the surface. A click on it
 * steps the continuity G0 -> G1 -> G2 -> G0; dragged, or typed at once it is
 * active, it sets the weight -- the arrow as long as the weight times a
 * quarter of the boundary; its menu picks the continuity. It works the
 * panel's own boxes, so a change made here is the panel's change.
 */
class EdgeMarkers: public QObject
{
    Q_OBJECT

public:
    /// Where a marker stands, in world coordinates; none to hide it
    using Where = std::function<std::optional<Surface::Geom::BoundaryMarker>()>;

    /// Markers on the view of `obj`, their hidden boxes in `panel`
    EdgeMarkers(App::DocumentObject* obj, QWidget* panel);
    ~EdgeMarkers() override;

    void add(const QString& title, QComboBox* continuity, QDoubleSpinBox* weight, Where where);
    /// To the edit's view, once it is in edit
    void attach();
    /// Each marker where its boundary now is: after every recompute
    void place();

    /// A boundary of the B-spline surface of the face of `obj`
    static Where onBoundary(App::DocumentObject* obj, Surface::Geom::Boundary boundary);
    /// The boundary of the surface of `obj` that the edge `link` lies on
    static Where onEdge(App::DocumentObject* obj, App::PropertyLinkSub* link);
    /// The same for the edge at `index` of `list`, counted from its end when
    /// negative (-1 the last)
    static Where onListEdge(App::DocumentObject* obj, App::PropertyLinkSubList* list, int index);

private:
    struct Marker
    {
        Gui::LinearGizmo* gizmo;
        Gui::QuantitySpinBox* box;
        QComboBox* continuity;
        Where where;
    };
    void colour(const Marker& m);

    App::DocumentObjectT objT;
    Gui::ViewProviderDragger* vp = nullptr;
    QWidget* panel;
    std::vector<Marker> markers;
    bool syncing = false;
    // the values go before their container
    std::unique_ptr<Gui::GizmoContainer> container;
    std::unique_ptr<Gui::OnViewValueSet> values;
};

}  // namespace SurfaceGui
