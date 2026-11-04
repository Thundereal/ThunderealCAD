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
#include <algorithm>
#include <cmath>

#include <QAction>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QMenu>
#include <QTimer>

#include <BRep_Tool.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#endif

#include <App/GeoFeature.h>
#include <App/PropertyLinks.h>
#include <Base/Color.h>
#include <Gui/Application.h>
#include <Gui/Inventor/Draggers/Gizmo.h>
#include <Gui/OnViewValue.h>
#include <Gui/QuantitySpinBox.h>
#include <Gui/ViewProviderDragger.h>
#include <Mod/Part/App/PartFeature.h>
#include <Mod/Surface/App/EdgeConditions.h>

#include "EdgeMarkers.h"


using namespace SurfaceGui;
namespace Geom = Surface::Geom;

namespace
{

/// The B-spline surface of the one face of `obj`, placed; null for none
Handle(Geom_BSplineSurface) surfaceOf(App::DocumentObject* obj)
{
    auto feature = dynamic_cast<Part::Feature*>(obj);
    if (!feature || !feature->isValid()) {
        return {};
    }
    TopExp_Explorer faces(feature->Shape.getValue(), TopAbs_FACE);
    if (!faces.More()) {
        return {};
    }
    return Handle(Geom_BSplineSurface)::DownCast(
        BRep_Tool::Surface(TopoDS::Face(faces.Current())));
}

/// The marker of the boundary of the surface of `obj` that the edge `sub` of
/// `source` lies on, `at` of the way along the edge as its curve runs; none
/// if it is not one edge, or on none
std::optional<Geom::BoundaryMarker> markerOnEdge(App::DocumentObject* obj,
                                                 App::DocumentObject* source,
                                                 const std::string& sub,
                                                 double at = 0.5)
{
    auto s = surfaceOf(obj);
    if (s.IsNull() || !source) {
        return std::nullopt;
    }
    TopoDS_Shape shape;
    try {
        shape = Part::Feature::getTopoShape(source, sub.c_str(), true).getShape();
    }
    catch (...) {
        return std::nullopt;
    }
    // an object given whole: its one edge
    if (!shape.IsNull() && shape.ShapeType() != TopAbs_EDGE) {
        TopExp_Explorer edges(shape, TopAbs_EDGE);
        if (!edges.More()) {
            return std::nullopt;
        }
        TopoDS_Shape edge = edges.Current();
        edges.Next();
        if (edges.More()) {
            return std::nullopt;
        }
        shape = edge;
    }
    if (shape.IsNull()) {
        return std::nullopt;
    }
    double first, last;
    Handle(Geom_Curve) c = BRep_Tool::Curve(TopoDS::Edge(shape), first, last);
    if (c.IsNull()) {
        return std::nullopt;
    }
    int b = Geom::boundaryOf(s, new Geom_TrimmedCurve(c, first, last), 1e-2);
    if (b < 0) {
        return std::nullopt;
    }
    // the boundary's parameter may run against the edge
    gp_Pnt start = Geom::boundaryMarker(s, Geom::Boundary(b), 0).middle;
    if (start.Distance(c->Value(last)) < start.Distance(c->Value(first))) {
        at = 1 - at;
    }
    return Geom::boundaryMarker(s, Geom::Boundary(b), at);
}

}  // namespace

EdgeMarkers::Where EdgeMarkers::onBoundary(App::DocumentObject* obj, Geom::Boundary boundary)
{
    App::DocumentObjectT objT(obj);
    return [objT, boundary]() -> std::optional<Geom::BoundaryMarker> {
        auto s = surfaceOf(objT.getObject());
        if (s.IsNull()) {
            return std::nullopt;
        }
        return Geom::boundaryMarker(s, boundary);
    };
}

EdgeMarkers::Where
EdgeMarkers::onEdge(App::DocumentObject* obj, App::PropertyLinkSub* link, double at)
{
    App::DocumentObjectT objT(obj);
    return [objT, link, at]() -> std::optional<Geom::BoundaryMarker> {
        auto obj = objT.getObject();
        if (!obj) {
            return std::nullopt;
        }
        const auto& subs = link->getSubValues();
        return markerOnEdge(obj,
                            link->getValue(),
                            subs.empty() ? std::string() : subs.front(),
                            at);
    };
}

EdgeMarkers::Where EdgeMarkers::when(std::function<bool()> shown, Where where)
{
    return [shown = std::move(shown), where = std::move(where)]() {
        return shown() ? where() : std::nullopt;
    };
}

EdgeMarkers::Where
EdgeMarkers::onListEdge(App::DocumentObject* obj, App::PropertyLinkSubList* list, int index)
{
    App::DocumentObjectT objT(obj);
    return [objT, list, index]() -> std::optional<Geom::BoundaryMarker> {
        auto obj = objT.getObject();
        if (!obj) {
            return std::nullopt;
        }
        const auto& objs = list->getValues();
        const auto& subs = list->getSubValues();
        const int n = int(std::min(objs.size(), subs.size()));
        const int i = index < 0 ? n + index : index;
        if (i < 0 || i >= n) {
            return std::nullopt;
        }
        return markerOnEdge(obj, objs[i], subs[i]);
    };
}

EdgeMarkers::EdgeMarkers(App::DocumentObject* obj, QWidget* panel)
    : objT(obj)
    , panel(panel)
{
    vp = dynamic_cast<Gui::ViewProviderDragger*>(
        Gui::Application::Instance->getViewProvider(obj));
    if (!vp || !Gui::GizmoContainer::isEnabled()) {
        return;
    }
    values = std::make_unique<Gui::OnViewValueSet>();
    container = Gui::GizmoContainer::create({}, vp);
}

EdgeMarkers::~EdgeMarkers()
{
    // the values let go of the handles before the container deletes them
    values.reset();
    container.reset();
}

void EdgeMarkers::add(const QString& title,
                      QComboBox* continuity,
                      QDoubleSpinBox* weight,
                      Where where)
{
    if (!container) {
        return;
    }
    // the handle's own box, hidden, kept with the panel's
    auto box = new Gui::QuantitySpinBox(panel);
    // a name of its own where two markers work one weight
    QString boxName = weight->objectName() + QLatin1String("InView");
    int same = 0;
    for (const auto& m : markers) {
        if (m.box->objectName().startsWith(boxName)) {
            ++same;
        }
    }
    box->setObjectName(same ? boxName + QString::number(same + 1) : boxName);
    box->hide();
    box->setDecimals(weight->decimals());
    box->setRange(weight->minimum(), weight->maximum());
    box->setSingleStep(weight->singleStep());
    box->setValue(weight->value());
    connect(weight, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this, box](double v) {
        if (!syncing) {
            syncing = true;
            box->setValue(v);
            syncing = false;
        }
    });
    connect(box,
            qOverload<double>(&Gui::QuantitySpinBox::valueChanged),
            this,
            [this, weight](double v) {
                if (!syncing) {
                    syncing = true;
                    weight->setValue(v);
                    syncing = false;
                }
            });

    auto gizmo = new Gui::LinearGizmo(box);
    // a click steps the continuity; after the click, not in the dragger's
    // own event
    gizmo->setClickCallback([continuity]() {
        QTimer::singleShot(0, continuity, [continuity]() {
            continuity->setCurrentIndex((continuity->currentIndex() + 1) % continuity->count());
        });
    });
    int index = values->addValue(box, title, Gui::OnViewValueSet::Place::AtHandle, gizmo);
    values->setMenu(index, [continuity](QMenu& menu, Gui::Gizmo*) {
        for (int i = 0; i < continuity->count(); ++i) {
            QAction* act = menu.addAction(continuity->itemText(i));
            act->setCheckable(true);
            act->setChecked(continuity->currentIndex() == i);
            QObject::connect(act, &QAction::triggered, continuity, [continuity, i]() {
                continuity->setCurrentIndex(i);
            });
        }
    });
    container->appendGizmo(gizmo);
    // a weight in twentieths, whatever the zoom
    gizmo->setValueStep(0.05);
    markers.push_back({gizmo, box, continuity, std::move(where)});
    colour(markers.back());
    connect(continuity, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, gizmo]() {
        for (const auto& m : markers) {
            if (m.gizmo == gizmo) {
                colour(m);
            }
        }
    });
}

void EdgeMarkers::colour(const Marker& m)
{
    // the theme's for position alone; tangent and curvature their own
    switch (m.continuity->currentIndex()) {
        case 1:
            m.gizmo->setColor(Base::Color(0.95f, 0.7f, 0.1f));
            break;
        case 2:
            m.gizmo->setColor(Base::Color(0.2f, 0.75f, 0.3f));
            break;
        default:
            m.gizmo->setColor(std::nullopt);
            break;
    }
}

void EdgeMarkers::attach()
{
    if (!values) {
        return;
    }
    place();
    values->attachToEdit(vp, container.get());
    // the view is told of the edit after the panel is made
    QTimer::singleShot(0, this, [this]() {
        if (values) {
            values->attachToEdit(vp, container.get());
            place();
        }
    });
}

void EdgeMarkers::place()
{
    if (!container) {
        return;
    }
    // in the shape's own frame, as the container has its gizmos
    Base::Placement toShape;
    if (auto feature = dynamic_cast<App::GeoFeature*>(objT.getObject())) {
        toShape = feature->Placement.getValue().inverse();
    }
    for (const auto& m : markers) {
        if (m.gizmo->isDragging()) {
            continue;
        }
        auto at = m.where ? m.where() : std::nullopt;
        if (!at || at->length <= 0) {
            m.gizmo->setVisibility(false);
            continue;
        }
        Base::Vector3d pos(at->middle.X(), at->middle.Y(), at->middle.Z());
        Base::Vector3d dir(at->inward.X(), at->inward.Y(), at->inward.Z());
        toShape.multVec(pos, pos);
        toShape.getRotation().multVec(dir, dir);
        double factor = at->length / 4;
        if (std::abs(m.gizmo->getMultFactor() - factor) > 1e-9 * factor) {
            m.gizmo->setMultFactor(factor);
        }
        m.gizmo->Gizmo::setDraggerPlacement(pos, dir);
        m.gizmo->setVisibility(true);
    }
    if (values) {
        values->placeAnchors(container->getWorldMatrix());
        values->refresh();
    }
}

#include "moc_EdgeMarkers.cpp"
