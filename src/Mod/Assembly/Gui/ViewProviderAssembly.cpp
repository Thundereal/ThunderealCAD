// SPDX-License-Identifier: LGPL-2.1-or-later
/****************************************************************************
 *                                                                          *
 *   Copyright (c) 2023 Ondsel <development@ondsel.com>                     *
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


#include <Mod/Assembly/App/AssemblyParams.h>
#include <boost/core/ignore_unused.hpp>
#include <QCursor>
#include <QMessageBox>
#include <QToolTip>
#include <QTimer>
#include <QMenu>
#include <QString>
#include <vector>
#include <sstream>
#include <iostream>
#include <Inventor/nodes/SoCone.h>
#include <Inventor/actions/SoRayPickAction.h>
#include <Inventor/nodes/SoCamera.h>
#include <Inventor/nodes/SoCoordinate3.h>
#include <Inventor/nodes/SoIndexedFaceSet.h>
#include <Inventor/SoPickedPoint.h>
#include <Inventor/SoRenderManager.h>
#include <Inventor/nodes/SoCylinder.h>
#include <Inventor/nodes/SoFaceSet.h>
#include <Inventor/nodes/SoMaterial.h>
#include <Inventor/nodes/SoPickStyle.h>
#include <Inventor/nodes/SoPolygonOffset.h>
#include <Inventor/nodes/SoRotation.h>
#include <Inventor/nodes/SoSeparator.h>
#include <Inventor/nodes/SoShapeHints.h>
#include <Inventor/draggers/SoDragger.h>
#include <Inventor/events/SoKeyboardEvent.h>
#include <Inventor/nodes/SoSwitch.h>
#include <Inventor/nodes/SoTransform.h>
#include <Inventor/nodes/SoTranslation.h>
#include <Inventor/sensors/SoFieldSensor.h>
#include <Inventor/sensors/SoSensor.h>


#include <chrono>
#include <set>
#include <algorithm>
#include <iterator>
#include <Inventor/SoPath.h>
#include <Inventor/details/SoDetail.h>

// Reached transitively upstream, but not here.
#include <Precision.hxx>

#include <Base/Color.h>
#include <App/Link.h>
#include <App/Document.h>
#include <App/DocumentObject.h>
#include <App/Part.h>

#include <Base/Tools.h>

#include <App/AutoTransaction.h>
#include <Base/Exception.h>
#include <Gui/ActionFunction.h>
#include <App/DocumentObserver.h>
#include <App/GeoFeatureGroupExtension.h>
#include <Gui/Application.h>
#include <Gui/BitmapFactory.h>
#include <Gui/CommandT.h>
#include <Gui/Control.h>
#include <Gui/Document.h>
#include <Gui/Inventor/Draggers/SoTransformDragger.h>
#include <Gui/Inventor/So3DAnnotation.h>
#include <Gui/Inventor/SoAutoZoomTranslation.h>
#include <Gui/MDIView.h>
#include <Gui/MainWindow.h>
#include <Gui/View3DInventor.h>
#include <Gui/View3DInventorViewer.h>
#include <Gui/ViewProviderLink.h>
#include <Gui/ViewProviderGeometryObject.h>
#include <Gui/ViewParams.h>
#include <Gui/SoFCSelectionAction.h>

#include <Mod/Assembly/App/AssemblyGroup.h>
#include <Mod/Assembly/App/AssemblyLink.h>
#include <Mod/Assembly/App/AssemblyObject.h>
#include <Mod/Assembly/App/AssemblyUtils.h>
#include <Mod/Assembly/App/Groups.h>
#include <Mod/Assembly/App/Joint.h>
#include <Mod/PartDesign/App/Body.h>

#include "TaskAssemblyMessages.h"

#include "ViewProviderAssembly.h"
#include "ViewProviderAssemblyPy.h"

#include <Gui/Utilities.h>


using namespace Assembly;
using namespace AssemblyGui;

void printPlacement(Base::Placement plc, const char* name)
{
    Base::Vector3d pos = plc.getPosition();
    Base::Vector3d axis;
    double angle;
    Base::Rotation rot = plc.getRotation();
    rot.getRawValue(axis, angle);
    Base::Console().Warning(
        "placement %s : position (%.1f, %.1f, %.1f) - axis (%.1f, %.1f, %.1f) angle %.1f\n",
        name,
        pos.x,
        pos.y,
        pos.z,
        axis.x,
        axis.y,
        axis.z,
        angle
    );
}

PROPERTY_SOURCE(AssemblyGui::ViewProviderAssembly, Gui::ViewProviderPart)

ViewProviderAssembly::ViewProviderAssembly()
    : SelectionObserver(false)
    , dragMode(DragMode::None)
    , canStartDragging(false)
    , partMoving(false)
    , enableMovement(true)
    , moveOnlyPreselected(false)
    , moveInCommand(true)
    , ctrlPressed(false)
    , forceSolveOnMoveForRigid(false)
    , ungroundedJointDrag(false)
    , lastClickTime(0)
    , jointVisibilitiesBackup({})
    , docsToMove({})
{
    m_preTransactionConn = App::GetApplication().signalBeforeOpenTransaction.connect(
        std::bind(&ViewProviderAssembly::slotAboutToOpenTransaction, this, std::placeholders::_1)
    );
    m_startSaveConn = App::GetApplication().signalStartSaveDocument.connect(
        std::bind(&ViewProviderAssembly::slotStartSave, this, std::placeholders::_1, std::placeholders::_2)
    );
    m_deletedObjectConn = App::GetApplication().signalDeletedObject.connect(
        std::bind(&ViewProviderAssembly::slotDeletedObject, this, std::placeholders::_1)
    );
}

ViewProviderAssembly::~ViewProviderAssembly()
{
    m_preTransactionConn.disconnect();
    m_startSaveConn.disconnect();
    m_deletedObjectConn.disconnect();
    // (the edit's three: unsetEdit() releases them, and may not have run)
    connectSolverUpdate.disconnect();
    connectActivatedVP.disconnect();
    QObject::disconnect(workbenchConnection);

    updateTaskPanel(false);
    clearDragCollisions();
    if (collisionSwitch) {
        pcRoot->removeChild(collisionSwitch);
        collisionSwitch->unref();
        collisionSwitch = nullptr;
    }
    connectCollisionMarkers.disconnect();
    if (markerGroup) {
        pcRoot->removeChild(markerGroup);
        markerGroup->unref();
        markerGroup = nullptr;
    }
};

void ViewProviderAssembly::attach(App::DocumentObject* obj)
{
    ViewProviderPart::attach(obj);
    if (auto* assembly = freecad_cast<AssemblyObject*>(obj)) {
        connectCollisionMarkers = assembly->signalCollisionMarkers.connect([this] {
            showCollisionMarkers();
        });
    }
}

void ViewProviderAssembly::showCollisionMarkers()
{
    const auto& markers = getObject<AssemblyObject>()->getCollisionMarkers();
    if (!markerGroup) {
        if (markers.empty()) {
            return;
        }
        // In the assembly's coordinates: after its transform in pcRoot
        markerGroup = new Gui::So3DAnnotation;
        markerGroup->ref();
        markerGroup->setName("AssemblyCollisionMarkers");
        // After the other annotations: the dragger (-1), the labels (1)
        markerGroup->priority = 2;
        pcRoot->addChild(markerGroup);
    }
    markerGroup->removeAllChildren();
    markerArrows = nullptr;
    markerHover = nullptr;
    hoveredMarker = -1;
    if (markers.empty()) {
        return;
    }
    // Not picked by the selection: hoverCollisionMarker() picks the arrows
    auto* pick = new SoPickStyle;
    pick->style = SoPickStyle::UNPICKABLE;
    markerGroup->addChild(pick);
    markerHover = new SoSeparator;
    markerGroup->addChild(markerHover);
    markerArrows = new SoSeparator;
    // Picked with the auto zoom's scale, which a cached box does not have
    markerArrows->pickCulling = SoSeparator::OFF;
    markerGroup->addChild(markerArrows);
    // Red for a collision, amber for components that only touch
    auto* collision = new SoMaterial;
    collision->diffuseColor.setValue(1.0F, 0.1F, 0.05F);
    collision->emissiveColor.setValue(0.6F, 0.0F, 0.0F);
    collision->ref();
    auto* contact = new SoMaterial;
    contact->diffuseColor.setValue(1.0F, 0.65F, 0.0F);
    contact->emissiveColor.setValue(0.6F, 0.35F, 0.0F);
    contact->ref();
    // In units of the auto zoom's, about 1.5% of the view's height: the head
    // 2.4 long, its tip at where the two met, the shaft 3.6 behind it
    constexpr float headLength = 2.4F;
    constexpr float shaftLength = 3.6F;
    for (const auto& marker : markers) {
        auto* arrow = new SoSeparator;
        arrow->pickCulling = SoSeparator::OFF;
        arrow->addChild(marker.kind == Assembly::InterferenceResult::Kind::Contact ? contact
                                                                                   : collision);
        auto* at = new SoTranslation;
        at->translation.setValue(Base::convertTo<SbVec3f>(marker.centre));
        arrow->addChild(at);
        arrow->addChild(new Gui::SoAutoZoomTranslation);
        // Its +Y along the arrow, towards the tip: into the component hit
        Base::Vector3d towards = -marker.direction;
        if (towards.Length() < 1e-12) {
            towards = Base::Vector3d(0.0, 0.0, -1.0);
        }
        auto* turn = new SoRotation;
        turn->rotation.setValue(
            SbRotation(SbVec3f(0.0F, 1.0F, 0.0F), Base::convertTo<SbVec3f>(towards.Normalize())));
        arrow->addChild(turn);
        auto* headAt = new SoTranslation;
        headAt->translation.setValue(0.0F, -0.5F * headLength, 0.0F);
        arrow->addChild(headAt);
        auto* head = new SoCone;
        head->height = headLength;
        head->bottomRadius = 1.0F;
        arrow->addChild(head);
        auto* shaftAt = new SoTranslation;
        shaftAt->translation.setValue(0.0F, -0.5F * (headLength + shaftLength), 0.0F);
        arrow->addChild(shaftAt);
        auto* shaft = new SoCylinder;
        shaft->height = shaftLength;
        shaft->radius = 0.35F;
        arrow->addChild(shaft);
        markerArrows->addChild(arrow);
    }
    collision->unref();
    contact->unref();
}

bool ViewProviderAssembly::hoverCollisionMarker(const SbVec2s& cursorPos,
                                                Gui::ViewerContext* viewer)
{
    int index = -1;
    if (markerArrows && markerArrows->getNumChildren() > 0) {
        auto* camera = viewer->getSoRenderManager()->getCamera();
        if (camera) {
            // The arrows alone, where the view has them: under the camera
            // and the assembly's placement
            auto* root = new SoSeparator;
            root->ref();
            root->addChild(camera);
            auto* placed = new SoTransform;
            const Base::Placement global =
                App::GeoFeature::getGlobalPlacement(getObject<AssemblyObject>());
            double q0 {}, q1 {}, q2 {}, q3 {};
            global.getRotation().getValue(q0, q1, q2, q3);
            placed->translation.setValue(Base::convertTo<SbVec3f>(global.getPosition()));
            placed->rotation.setValue(static_cast<float>(q0),
                                      static_cast<float>(q1),
                                      static_cast<float>(q2),
                                      static_cast<float>(q3));
            root->addChild(placed);
            root->addChild(markerArrows);
            SoRayPickAction pick(viewer->getViewportRegion());
            pick.setPoint(cursorPos);
            pick.setRadius(viewer->getPickRadius());
            pick.apply(root);
            if (const SoPickedPoint* point = pick.getPickedPoint()) {
                const SoPath* path = point->getPath();
                for (int i = 0; i + 1 < path->getLength(); ++i) {
                    if (path->getNode(i) == markerArrows) {
                        index = markerArrows->findChild(path->getNode(i + 1));
                        break;
                    }
                }
            }
            root->unref();
        }
    }
    if (index != hoveredMarker) {
        hoveredMarker = index;
        showMarkerFaces(index);
        if (index >= 0) {
            QToolTip::showText(QCursor::pos(), markerText(index));
        }
        else {
            QToolTip::hideText();
        }
    }
    return index >= 0;
}

void ViewProviderAssembly::showMarkerFaces(int index)
{
    if (!markerHover) {
        return;
    }
    markerHover->removeAllChildren();
    const auto& markers = getObject<AssemblyObject>()->getCollisionMarkers();
    if (index < 0 || index >= static_cast<int>(markers.size())) {
        return;
    }
    auto* assembly = getObject<AssemblyObject>();
    const auto& marker = markers[index];
    // The faces in the assembly's coordinates, meshed as they are shown
    std::vector<Base::Vector3d> points;
    std::vector<int32_t> indices;
    for (const auto* names : {&marker.faces1, &marker.faces2}) {
        for (const auto& name : *names) {
            Part::TopoShape face;
            try {
                face = Part::Feature::getTopoShape(assembly,
                                                   name.c_str(),
                                                   true,
                                                   nullptr,
                                                   nullptr,
                                                   true,
                                                   false);
            }
            catch (const Base::Exception&) {
                continue;
            }
            if (face.isNull()) {
                continue;
            }
            std::vector<Base::Vector3d> facePoints;
            std::vector<Data::ComplexGeoData::Facet> facets;
            face.getFaces(facePoints, facets, 0.1);
            const auto offset = static_cast<int32_t>(points.size());
            points.insert(points.end(), facePoints.begin(), facePoints.end());
            for (const auto& facet : facets) {
                indices.insert(indices.end(),
                               {offset + static_cast<int32_t>(facet.I1),
                                offset + static_cast<int32_t>(facet.I2),
                                offset + static_cast<int32_t>(facet.I3),
                                -1});
            }
        }
    }
    if (points.empty()) {
        return;
    }
    auto* hints = new SoShapeHints;
    hints->vertexOrdering = SoShapeHints::UNKNOWN_ORDERING;
    markerHover->addChild(hints);
    // The preselection's colour, see-through: the faces are inside the parts
    auto* material = new SoMaterial;
    const unsigned long rgba = Gui::ViewParams::getHighlightColor();
    const SbColor colour(static_cast<float>((rgba >> 24) & 0xff) / 255.0F,
                         static_cast<float>((rgba >> 16) & 0xff) / 255.0F,
                         static_cast<float>((rgba >> 8) & 0xff) / 255.0F);
    material->diffuseColor.setValue(colour);
    material->emissiveColor.setValue(colour * 0.5F);
    material->transparency.setValue(0.3F);
    markerHover->addChild(material);
    auto* coords = new SoCoordinate3;
    coords->point.setNum(static_cast<int>(points.size()));
    SbVec3f* out = coords->point.startEditing();
    for (std::size_t i = 0; i < points.size(); ++i) {
        out[i] = Base::convertTo<SbVec3f>(points[i]);
    }
    coords->point.finishEditing();
    markerHover->addChild(coords);
    auto* faces = new SoIndexedFaceSet;
    faces->coordIndex.setValues(0, static_cast<int>(indices.size()), indices.data());
    markerHover->addChild(faces);
}

QString ViewProviderAssembly::markerText(int index) const
{
    const auto& markers = getObject<AssemblyObject>()->getCollisionMarkers();
    if (index < 0 || index >= static_cast<int>(markers.size())) {
        return {};
    }
    const auto& marker = markers[index];
    auto label = [](const App::DocumentObjectT& ref) {
        auto* obj = ref.getObject();
        return obj ? QString::fromUtf8(obj->Label.getValue()) : tr("(deleted)");
    };
    const QString first = label(marker.first);
    const QString second = label(marker.second);
    QString text;
    using Kind = Assembly::InterferenceResult::Kind;
    if (marker.kind == Kind::Contact) {
        text = tr("%1 touches %2").arg(first, second);
        if (marker.area > 0.0) {
            text += QLatin1String("\n") + tr("Over %1 mm^2").arg(marker.area, 0, 'g', 4);
        }
    }
    else if (marker.kind == Kind::Clearance) {
        text = tr("%1 and %2 are closer than %3 mm")
                   .arg(first, second)
                   .arg(marker.clearance, 0, 'g', 4);
        if (marker.distance > 0.0 && marker.distance < marker.clearance) {
            text += QLatin1String("\n") + tr("%1 mm apart").arg(marker.distance, 0, 'g', 4);
        }
        if (marker.area > 0.0) {
            text += QLatin1String("\n") + tr("Over %1 mm^2").arg(marker.area, 0, 'g', 4);
        }
    }
    else {
        text = marker.watch != 0 ? tr("%1 collided with %2 while dragging").arg(first, second)
                                 : tr("%1 interferes with %2").arg(first, second);
        if (marker.volume > 0.0) {
            text += QLatin1String("\n") + tr("%1 mm^3 in common").arg(marker.volume, 0, 'g', 4);
        }
    }
    if (!marker.faces1.empty() || !marker.faces2.empty()) {
        text += QLatin1String("\n") + tr("Click to select the faces");
    }
    return text;
}

void ViewProviderAssembly::selectMarkerFaces(int index)
{
    auto* assembly = getObject<AssemblyObject>();
    const auto& markers = assembly->getCollisionMarkers();
    if (index < 0 || index >= static_cast<int>(markers.size())) {
        return;
    }
    // Through the assembly from the document's root
    App::DocumentObject* root = assembly;
    std::string prefix;
    const auto parents = assembly->getParents();
    if (!parents.empty()) {
        root = parents.front().first;
        prefix = parents.front().second;
    }
    Gui::Selection().clearSelection();
    for (const auto* names : {&markers[index].faces1, &markers[index].faces2}) {
        for (const auto& name : *names) {
            Gui::Selection().addSelection(root->getDocument()->getName(),
                                          root->getNameInDocument(),
                                          (prefix + name).c_str());
        }
    }
}

QIcon ViewProviderAssembly::getIcon() const
{
    auto* assembly = getObject<Assembly::AssemblyObject>();
    if (assembly && assembly->isFrozen()) {
        return Gui::BitmapFactory().pixmap("Assembly_Frozen.svg");
    }
    return Gui::BitmapFactory().pixmap("Geoassembly.svg");
}

void ViewProviderAssembly::setupContextMenu(QMenu* menu, QObject* receiver, const char* member)
{
    auto func = new Gui::ActionFunction(menu);

    QAction* act = menu->addAction(QObject::tr("Active object"));
    act->setCheckable(true);
    act->setChecked(isActivePart(ASSEMBLYKEY));
    func->trigger(act, [this]() { this->doubleClicked(); });

    if (auto* assembly = getObject<Assembly::AssemblyObject>()) {
        bool frozen = assembly->isFrozen();
        act = menu->addAction(QObject::tr("Freeze"));
        act->setCheckable(true);
        act->setChecked(frozen);
        act->setToolTip(
            QObject::tr(
                "Keep the components as one shape: the assembly is not solved, and a "
                "document linking to it loads that shape in place of the components"
            )
        );
        func->trigger(act, [this]() { this->toggleFreeze(); });
        if (frozen) {
            act = menu->addAction(QObject::tr("Refresh frozen shape"));
            act->setToolTip(QObject::tr("Take the shape of the components again"));
            func->trigger(act, [this]() { this->refreshFrozenShape(); });
        }
        act = menu->addAction(QObject::tr("Parts group"));
        act->setCheckable(true);
        act->setChecked(assembly->getAuxGroup(App::Part::PartsRole) != nullptr);
        // A frozen assembly keeps it
        act->setEnabled(!frozen);
        act->setToolTip(QObject::tr("Show the components in a group of their own"));
        func->trigger(act, [this]() { this->togglePartsGroup(); });
    }

    ViewProviderDragger::setupContextMenu(menu, receiver, member);  // NOLINT
}

void ViewProviderAssembly::toggleFreeze()
{
    auto* assembly = getObject<Assembly::AssemblyObject>();
    if (!assembly) {
        return;
    }
    bool freeze = !assembly->isFrozen();
    App::AutoTransaction committer(freeze ? "Freeze assembly" : "Unfreeze assembly");
    try {
        assembly->Freeze.setValue(freeze);
    }
    catch (Base::Exception& e) {
        e.ReportException();
        committer.close(true);
    }
}

void ViewProviderAssembly::refreshFrozenShape()
{
    auto* assembly = getObject<Assembly::AssemblyObject>();
    if (!assembly) {
        return;
    }
    App::AutoTransaction committer("Refresh frozen shape");
    try {
        assembly->refreshFrozenShape();
    }
    catch (Base::Exception& e) {
        e.ReportException();
        committer.close(true);
    }
}

bool ViewProviderAssembly::doubleClicked()
{
    if (isInEditMode()) {
        autoCollapseOnDeactivation = true;
        getDocument()->setEditRestore(false);
        getDocument()->resetEdit();
    }
    else {
        // assure the Assembly workbench
        if (App::GetApplication()
                .GetUserParameter()
                .GetGroup("BaseApp")
                ->GetGroup("Preferences")
                ->GetGroup("Mod/Assembly")
                ->GetBool("SwitchToWB", Assembly::AssemblyParams::defaultSwitchToWB())) {
            Gui::Command::assureWorkbench("AssemblyWorkbench");
        }

        // Part is not 'Active' so we enter edit mode to make it so.
        getDocument()->setEdit(this);
    }

    Gui::Selection().clearSelection();
    return true;
}

bool ViewProviderAssembly::canDragObject(App::DocumentObject* obj) const
{
    // The user should not be able to drag the joint group out of the assembly
    return obj && !obj->is<Assembly::JointGroup>() && !obj->is<Assembly::AssemblyGroup>();
}

bool ViewProviderAssembly::canDragObjectToTarget(App::DocumentObject* obj, App::DocumentObject* target) const
{
    // If a solid is removed from the assembly, its joints need to be removed.
    bool prompted = false;
    auto* assemblyPart = getObject<AssemblyObject>();

    // If target is null then it's being dropped on a doc.
    if (target && assemblyPart->hasObject(target)) {
        // If the obj stays in assembly then its ok.
        return true;
    }

    // Combine the joints and groundedJoints vectors into one for simplicity.
    std::vector<App::DocumentObject*> allJoints = assemblyPart->getJoints();
    std::vector<App::DocumentObject*> groundedJoints = assemblyPart->getGroundedJoints();
    allJoints.insert(allJoints.end(), groundedJoints.begin(), groundedJoints.end());

    for (auto joint : allJoints) {
        // getLinkObjFromProp returns nullptr if the property doesn't exist.
        App::DocumentObject* part1 = getMovingPartFromRef(joint, "Reference1");
        App::DocumentObject* part2 = getMovingPartFromRef(joint, "Reference2");
        App::DocumentObject* obj1 = getObjFromJointRef(joint, "Reference1");
        App::DocumentObject* obj2 = getObjFromJointRef(joint, "Reference2");
        App::DocumentObject* obj3 = getObjFromProp(joint, "ObjectToGround");
        if (obj == obj1 || obj == obj2 || obj == part1 || obj == part2 || obj == obj3) {
            if (!prompted) {
                prompted = true;
                QMessageBox msgBox(Gui::getMainWindow());
                msgBox.setText(tr("The object is associated to one or more joints."));
                msgBox.setInformativeText(
                    tr("Do you want to move the object and delete associated joints?")
                );
                msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
                msgBox.setDefaultButton(QMessageBox::No);
                int ret = msgBox.exec();

                if (ret == QMessageBox::No) {
                    return false;
                }
            }
            Gui::Command::doCommand(
                Gui::Command::Gui,
                "App.activeDocument().removeObject('%s')",
                joint->getNameInDocument()
            );
        }
    }
    return true;
}

void ViewProviderAssembly::updateData(const App::Property* prop)
{
    auto* obj = static_cast<Assembly::AssemblyObject*>(pcObject);
    if (prop == &obj->Group) {
        // Defer the icon update until the event loop is idle.
        // This ensures the assembly has had a chance to recompute its
        // connectivity state before we query it.

        // We can't capture the raw 'obj' pointer because it may be deleted
        // by the time the timer fires. Instead, we capture the names of the
        // document and the object, and look them up again.
        if (!obj->getDocument()) {
            return;  // Should not happen, but a good safeguard
        }
        const std::string docName = obj->getDocument()->getName();
        const std::string objName = obj->getNameInDocument();

        QTimer::singleShot(0, [docName, objName]() {
            // Re-acquire the document and the object safely.
            App::Document* doc = App::GetApplication().getDocument(docName.c_str());
            if (!doc) {
                return;  // Document was closed
            }

            auto* pcObj = doc->getObject(objName.c_str());
            auto* obj = static_cast<Assembly::AssemblyObject*>(pcObj);

            // Now we can safely check if the object still exists and is attached.
            if (!obj || !obj->isAttachedToDocument()) {
                return;
            }

            std::vector<App::DocumentObject*> joints = obj->getJoints();
            for (auto* joint : joints) {
                Gui::ViewProvider* jointVp = Gui::Application::Instance->getViewProvider(joint);
                if (jointVp) {
                    jointVp->signalChangeIcon();
                }
            }
        });
    }
    else {
        if (prop == &obj->Freeze) {
            signalChangeIcon();
        }
        Gui::ViewProviderPart::updateData(prop);
    }
}

std::vector<App::DocumentObject*> ViewProviderAssembly::claimChildren3D() const
{
    auto children = Gui::ViewProviderPart::claimChildren3D();
    auto* assembly = getObject<Assembly::AssemblyObject>();
    auto* parts = assembly && assembly->isFrozen()
        ? assembly->getAuxGroup(App::Part::PartsRole)
        : nullptr;
    if (!parts || !parts->isDerivedFrom<Assembly::PartsGroup>()) {
        return children;
    }
    const auto& components = parts->Group.getValues();
    std::set<App::DocumentObject*> frozen(components.begin(), components.end());
    children.erase(
        std::remove_if(
            children.begin(),
            children.end(),
            [&frozen](App::DocumentObject* obj) { return frozen.count(obj) > 0; }
        ),
        children.end()
    );
    return children;
}

bool ViewProviderAssembly::setEdit(int mode)
{
    if (mode == ViewProvider::Default) {
        // Ask that this edit mode be restored. For example if it is quit to edit a sketch.
        getDocument()->setEditRestore(true);
        autoCollapseOnDeactivation = false;

        // Set the part as 'Activated' ie bold in the tree.
        Gui::Command::doCommand(
            Gui::Command::Gui,
            "appDoc = App.getDocument('%s')\n"
            "Gui.getDocument(appDoc).ActiveView.setActiveObject('%s', "
            "appDoc.getObject('%s'))",
            this->getObject()->getDocument()->getName(),
            ASSEMBLYKEY,
            this->getObject()->getNameInDocument()
        );

        setDragger();
        attachSelection();

        updateTaskPanel(true);

        auto* assembly = getObject<AssemblyObject>();
        // An edit entered again without the last one having released its
        // connections would leave those live with nothing holding them: the
        // main window's kept calling a provider that was gone, at the next
        // change of workbench after the assembly was deleted.
        connectSolverUpdate.disconnect();
        connectActivatedVP.disconnect();
        QObject::disconnect(workbenchConnection);
        connectSolverUpdate = assembly->signalSolverUpdate.connect([this] {
            UpdateSolverInformation();
        });

        connectActivatedVP = getDocument()->signalActivatedViewProvider.connect(
            std::bind(
                &ViewProviderAssembly::slotActivatedVP,
                this,
                std::placeholders::_1,
                std::placeholders::_2
            )
        );

        workbenchConnection = QObject::connect(
            Gui::getMainWindow(),
            &Gui::MainWindow::workbenchActivated,
            [this](const QString& name) { this->onWorkbenchActivated(name); }
        );

        assembly->recomputeFeature(true);

        return true;
    }
    return ViewProviderPart::setEdit(mode);
}

void ViewProviderAssembly::unsetEdit(int mode)
{
    if (mode == ViewProvider::Default) {
        canStartDragging = false;
        partMoving = false;
        docsToMove.clear();

        unsetDragger();
        detachSelection();

        // The edit's connections go with the edit, before anything that
        // leaves early: the two below were released only at the end, past
        // the return for a document with no active view, and the main
        // window's not here at all.
        connectSolverUpdate.disconnect();
        connectActivatedVP.disconnect();
        QObject::disconnect(workbenchConnection);

        // Check if the view is still active before trying to deactivate the assembly.
        auto activeView = getDocument()->getActiveView();
        if (!activeView) {
            return;
        }

        // Set the part as not 'Activated' ie not bold in the tree.
        if (isActivePart(ASSEMBLYKEY)) {
            Gui::Command::doCommand(
                Gui::Command::Gui,
                "appDoc = App.getDocument('%s')\n"
                "Gui.getDocument(appDoc).ActiveView.setActiveObject('%s', None)",
                this->getObject()->getDocument()->getName(),
                ASSEMBLYKEY
            );
        }

        updateTaskPanel(false);

        return;
    }
    ViewProviderPart::unsetEdit(mode);
}

void ViewProviderAssembly::slotActivatedVP(const Gui::ViewProviderDocumentObject* vp, const char* name)
{
    if (name && strcmp(name, ASSEMBLYKEY) == 0) {

        // If the new active VP is NOT this assembly (meaning we lost activation or it was cleared)
        if (vp != this && isInEditMode()) {
            autoCollapseOnDeactivation = true;
            getDocument()->setEditRestore(false);
            getDocument()->resetEdit();
        }
    }
}

void ViewProviderAssembly::setDragger()
{
    // Create the dragger coin object
    assert(!asmDragger);
    asmDragger = new Gui::SoTransformDragger();
    // ViewParams is a static API in this fork, not upstream's instance().
    asmDragger->setAxisColors(
        Gui::ViewParams::getAxisXColor(),
        Gui::ViewParams::getAxisYColor(),
        Gui::ViewParams::getAxisZColor()
    );
    asmDragger->draggerSize.setValue(Gui::ViewParams::getDraggerScale());

    asmDraggerSwitch = new SoSwitch(SO_SWITCH_NONE);
    asmDraggerSwitch->addChild(asmDragger);

    pcRoot->insertChild(asmDraggerSwitch, 0);
    asmDraggerSwitch->ref();
    asmDragger->ref();
}

void ViewProviderAssembly::unsetDragger()
{
    pcRoot->removeChild(asmDraggerSwitch);
    if (asmDragger) {
        asmDragger->unref();
        asmDragger = nullptr;
    }
    if (asmDraggerSwitch) {
        asmDraggerSwitch->unref();
        asmDraggerSwitch = nullptr;
    }
}

void ViewProviderAssembly::setEditViewer(Gui::ViewerContext* viewer, int ModNum)
{
    ViewProviderPart::setEditViewer(viewer, ModNum);

    if (asmDragger && viewer) {
        asmDragger->setUpAutoScale(viewer->getSoRenderManager()->getCamera());
    }
}

bool ViewProviderAssembly::isInEditMode() const
{
    return asmDragger != nullptr;
}

App::DocumentObject* ViewProviderAssembly::getActivePart() const
{
    auto activeView = getDocument()->getActiveView();
    if (!activeView) {
        return nullptr;
    }
    return activeView->getActiveObject<App::DocumentObject*>(ASSEMBLYKEY);
}

bool ViewProviderAssembly::keyPressed(bool pressed, int key)
{
    if (key == SoKeyboardEvent::ESCAPE) {
        if (isInEditMode()) {
            if (Gui::Control().activeDialog(nullptr)) {
                return true;
            }

            ParameterGrp::handle hPgr = App::GetApplication().GetParameterGroupByPath(
                "User parameter:BaseApp/Preferences/Mod/Assembly"
            );

            return !hPgr->GetBool("LeaveEditWithEscape", Assembly::AssemblyParams::defaultLeaveEditWithEscape());
        }
    }

    if (key == SoKeyboardEvent::LEFT_CONTROL || key == SoKeyboardEvent::RIGHT_CONTROL) {
        ctrlPressed = pressed;
    }
    return false;  // handle all other key events
}

bool ViewProviderAssembly::mouseMove(const SbVec2s& cursorPos, Gui::ViewerContext* viewer)
{
    try {
        return tryMouseMove(cursorPos, viewer);
    }
    catch (const Base::Exception& e) {
        Base::Console().Warning("%s\n", e.what());
        return false;
    }
}

bool ViewProviderAssembly::tryMouseMove(const SbVec2s& cursorPos, Gui::ViewerContext* viewer)
{
    if (!isInEditMode()) {
        return false;
    }

    // Over a collision marker: its faces highlighted, its tooltip shown
    if (!partMoving && !canStartDragging) {
        hoverCollisionMarker(cursorPos, viewer);
    }

    // Initialize or cancel the dragging of parts
    if (canStartDragging) {
        canStartDragging = false;

        if (enableMovement && getSelectedObjectsWithinAssembly()) {
            initMove(cursorPos, viewer);
        }
    }

    // Do the dragging of parts
    if (partMoving) {
        Base::Vector3d newPos, newPosRot;
        Base::Vector3d ungroundedDragTranslation;
        Base::Placement dragJcsGlobalPlc = jcsGlobalPlc;
        if (ungroundedJointDrag) {
            SbVec3f vec = viewer->getPointOnFocalPlane(cursorPos);
            Base::Vector3d currentUngroundedDragPosition = Base::convertTo<Base::Vector3d>(vec);
            ungroundedDragTranslation = currentUngroundedDragPosition - initialUngroundedDragPosition;
            dragJcsGlobalPlc.setPosition(jcsGlobalPlc.getPosition() + ungroundedDragTranslation);
        }

        if (dragMode == DragMode::RotationOnPlane) {
            SbVec3f vec = viewer->getPointOnXYPlaneOfPlacement(cursorPos, jcsGlobalPlc);
            newPosRot = Base::Vector3d(vec[0], vec[1], vec[2]);
        }
        else if (dragMode == DragMode::TranslationOnAxis) {
            Base::Vector3d zAxis = jcsGlobalPlc.getRotation().multVec(Base::Vector3d(0., 0., 1.));
            Base::Vector3d pos = jcsGlobalPlc.getPosition();
            SbVec3f axisCenter(pos.x, pos.y, pos.z);
            SbVec3f axis(zAxis.x, zAxis.y, zAxis.z);
            SbVec3f vec = viewer->getPointOnLine(cursorPos, axisCenter, axis);
            newPos = Base::Vector3d(vec[0], vec[1], vec[2]);
        }
        else if (dragMode == DragMode::TranslationOnAxisAndRotationOnePlane) {
            SbVec3f vec = viewer->getPointOnXYPlaneOfPlacement(cursorPos, jcsGlobalPlc);
            newPosRot = Base::Vector3d(vec[0], vec[1], vec[2]);

            Base::Vector3d zAxis = jcsGlobalPlc.getRotation().multVec(Base::Vector3d(0., 0., 1.));
            Base::Vector3d pos = jcsGlobalPlc.getPosition();
            SbVec3f axisCenter(pos.x, pos.y, pos.z);
            SbVec3f axis(zAxis.x, zAxis.y, zAxis.z);
            vec = viewer->getPointOnLine(cursorPos, axisCenter, axis);
            newPos = Base::Vector3d(vec[0], vec[1], vec[2]);
        }
        else if (dragMode == DragMode::TranslationOnPlane) {
            SbVec3f vec = viewer->getPointOnXYPlaneOfPlacement(cursorPos, jcsGlobalPlc);
            newPos = Base::Vector3d(vec[0], vec[1], vec[2]);
        }
        else {
            SbVec3f vec = viewer->getPointOnFocalPlane(cursorPos);
            newPos = Base::Vector3d(vec[0], vec[1], vec[2]);
        }

        // Cursor deltas are world-space but the placements written below are in the
        // assembly's local frame, so rotate them into it. Identity for an unrotated
        // assembly. Mirrors the asmPlc correction in draggerMotionCallback.
        Base::Rotation asmInvRot
            = App::GeoFeature::getGlobalPlacement(getObject<AssemblyObject>()).getRotation().inverse();

        // A move that would collide is taken back towards these
        std::vector<std::pair<App::PropertyPlacement*, Base::Placement>> previous;
        for (auto& objToMove : docsToMove) {
            App::DocumentObject* obj = objToMove.obj;
            auto* propPlacement = obj->getPlacementProperty();
            if (propPlacement) {
                previous.emplace_back(propPlacement, propPlacement->getValue());
                Base::Placement plc = objToMove.plc;
                if (ungroundedJointDrag) {
                    plc.setPosition(plc.getPosition() + ungroundedDragTranslation);
                    if (obj != docsToMove[0].obj) {
                        propPlacement->setValue(plc);
                        continue;
                    }
                }

                if (dragMode == DragMode::RotationOnPlane) {
                    Base::Vector3d center = jcsGlobalPlc.getPosition();
                    Base::Vector3d norm = jcsGlobalPlc.getRotation().multVec(
                        Base::Vector3d(0., 0., -1.)
                    );
                    double angle
                        = (newPosRot - center).GetAngleOriented(initialPositionRot - center, norm);
                    Base::Rotation zRotation = Base::Rotation(Base::Vector3d(0., 0., 1.), angle);
                    Base::Placement rotatedGlovalJcsPlc = dragJcsGlobalPlc
                        * Base::Placement(Base::Vector3d(), zRotation);
                    Base::Placement jcsPlcRelativeToPart = plc.inverse() * dragJcsGlobalPlc;
                    plc = rotatedGlovalJcsPlc * jcsPlcRelativeToPart.inverse();
                }
                else if (dragMode == DragMode::Ball) {
                    Base::Vector3d center = jcsGlobalPlc.getPosition();
                    // Vectors from joint center to initial click and current drag position
                    Base::Vector3d u = initialPosition - center;
                    Base::Vector3d v = newPos - center;

                    // Ensure vectors are valid to prevent singularities
                    if (u.Length() > Precision::Confusion() && v.Length() > Precision::Confusion()) {
                        // Calculate rotation that moves vector u to v
                        Base::Rotation rot;
                        rot.setValue(u, v);

                        // Apply this rotation to the global joint placement (around the joint center)
                        Base::Placement rotatedGlobalJcsPlc = dragJcsGlobalPlc;
                        rotatedGlobalJcsPlc.setRotation(rot * dragJcsGlobalPlc.getRotation());

                        // Calculate the initial offset of the part relative to the joint
                        // and apply the new global joint placement to find the new part placement.
                        Base::Placement jcsPlcRelativeToPart = plc.inverse() * dragJcsGlobalPlc;
                        plc = rotatedGlobalJcsPlc * jcsPlcRelativeToPart.inverse();
                    }
                }
                else if (dragMode == DragMode::TranslationOnAxis) {
                    Base::Vector3d pos = plc.getPosition() + (newPos - initialPosition);
                    plc.setPosition(pos);
                }
                else if (dragMode == DragMode::TranslationOnAxisAndRotationOnePlane) {
                    Base::Vector3d delta = newPos - initialPosition;
                    Base::Vector3d pos = plc.getPosition() + delta;
                    plc.setPosition(pos);

                    Base::Placement newJcsGlobalPlc = dragJcsGlobalPlc;
                    newJcsGlobalPlc.setPosition(dragJcsGlobalPlc.getPosition() + delta);

                    Base::Vector3d center = jcsGlobalPlc.getPosition() + delta;
                    Base::Vector3d norm = newJcsGlobalPlc.getRotation().multVec(
                        Base::Vector3d(0., 0., -1.)
                    );

                    Base::Vector3d projInitialPositionRot
                        = initialPositionRot.ProjectToPlane(center, norm);
                    boost::ignore_unused(projInitialPositionRot);
                    double angle
                        = (newPosRot - center).GetAngleOriented(initialPositionRot - center, norm);
                    Base::Rotation zRotation = Base::Rotation(Base::Vector3d(0., 0., 1.), angle);
                    Base::Placement rotatedGlovalJcsPlc = newJcsGlobalPlc
                        * Base::Placement(Base::Vector3d(), zRotation);
                    Base::Placement jcsPlcRelativeToPart = plc.inverse() * newJcsGlobalPlc;
                    plc = rotatedGlovalJcsPlc * jcsPlcRelativeToPart.inverse();
                }
                else if (dragMode == DragMode::TranslationOnPlane) {
                    Base::Vector3d pos = plc.getPosition() + (newPos - initialPosition);
                    plc.setPosition(pos);
                }
                else {  // DragMode::Translation / TranslationNoSolve
                    Base::Vector3d delta = asmInvRot.multVec(newPos - prevPosition);

                    Base::Vector3d pos = propPlacement->getValue().getPosition() + delta;
                    plc.setPosition(pos);
                }
                propPlacement->setValue(plc);
            }
        }

        prevPosition = newPos;

        auto* assemblyPart = getObject<AssemblyObject>();
        ParameterGrp::handle hGrp = App::GetApplication().GetParameterGroupByPath(
            "User parameter:BaseApp/Preferences/Mod/Assembly"
        );
        bool solveOnMove = hGrp->GetBool("SolveOnMove", Assembly::AssemblyParams::defaultSolveOnMove());
        // HACK: Re-solve assembly to update rigid groups while dragging.
        using DragCollision = AssemblyObject::DragCollision;
        const bool stopAtCollision = assemblyPart->getDragCollisionMode() == DragCollision::Stop;
        // Where the move puts the moved parts
        auto placementsMoved = [&]() {
            std::vector<Base::Placement> placements;
            for (auto& [prop, plc] : previous) {
                placements.push_back(prop->getValue());
            }
            return placements;
        };
        // Stop: as far along the move as the parts go without colliding, the
        // moved parts' placements interpolated, and, with solve, the assembly
        // solved for each fraction
        auto stopAlongMove = [&](const std::vector<Base::Placement>& wanted, bool solve) {
            assemblyPart->stopAtDragCollision([&](double t) {
                for (std::size_t i = 0; i < previous.size(); ++i) {
                    const auto& [prop, plc] = previous[i];
                    prop->setValue(t <= 0.0 ? plc : Base::Placement::sclerp(plc, wanted[i], t));
                }
                return !solve || assemblyPart->solve() == 0;
            });
            showDragStopped();
        };
        if (solveOnMove && (dragMode != DragMode::TranslationNoSolve || forceSolveOnMoveForRigid)) {
            if (forceSolveOnMoveForRigid) {
                // A whole solve per move, and, stopped, per fraction of it
                const auto wanted = placementsMoved();
                assemblyPart->solve();
                if (!assemblyPart->checkDragCollisions().empty() && stopAtCollision) {
                    stopAlongMove(wanted, true);
                }
            }
            else if (!assemblyPart->doDragStep() && assemblyPart->getLastDragStepFailed()) {
                if (stopAtCollision && !assemblyPart->getDragCollisions().empty()) {
                    showDragStopped();
                }
                else {
                    // The parts stay where the last good step left them; tell the
                    // user why the part stopped following the mouse.
                    Gui::getMainWindow()->showMessage(
                        tr("The assembly solver could not follow this move"),
                        3000);
                }
            }
        }
        else {
            if (assemblyPart->getDragCollisionMode() != DragCollision::Off
                && !assemblyPart->checkDragCollisions().empty() && stopAtCollision) {
                stopAlongMove(placementsMoved(), false);
            }
            assemblyPart->redrawJointPlacements(assemblyPart->getJoints());
        }
        showDragCollisions();
    }
    return false;
}

void ViewProviderAssembly::showDragStopped()
{
    const auto& hits = getObject<AssemblyObject>()->getDragCollisions();
    if (hits.empty()) {
        return;
    }
    Gui::getMainWindow()->showMessage(
        tr("Stopped: %1 would collide with %2")
            .arg(QString::fromUtf8(hits.front().first->Label.getValue()),
                 QString::fromUtf8(hits.front().second->Label.getValue())),
        3000);
}

void ViewProviderAssembly::showDragCollisions()
{
    auto* assembly = getObject<AssemblyObject>();
    // Each component's faces in a crossing, of all its pairs; the whole of it
    // in a pair where nothing crosses
    std::vector<std::pair<App::DocumentObject*, CollisionNode>> colliding;
    for (const auto& hit : assembly->getDragCollisions()) {
        for (const auto& [obj, faces] : {std::make_pair(hit.first, &hit.touchFaces1),
                                         std::make_pair(hit.second, &hit.touchFaces2)}) {
            auto it = std::find_if(colliding.begin(), colliding.end(), [&](const auto& entry) {
                return entry.first == obj;
            });
            if (it == colliding.end()) {
                it = colliding.insert(colliding.end(), {obj, CollisionNode {}});
            }
            if (faces->empty()) {
                it->second.whole = true;
            }
            it->second.faces.insert(it->second.faces.end(), faces->begin(), faces->end());
        }
    }
    for (auto& [obj, wanted] : colliding) {
        if (wanted.whole) {
            wanted.faces.clear();
        }
        std::sort(wanted.faces.begin(), wanted.faces.end());
        wanted.faces.erase(std::unique(wanted.faces.begin(), wanted.faces.end()),
                           wanted.faces.end());
    }
    if (colliding.empty()) {
        if (collisionSwitch) {
            collisionSwitch->whichChild = SO_SWITCH_NONE;
        }
        return;
    }
    if (!collisionSwitch) {
        // In the assembly's coordinates: after its transform in pcRoot
        collisionSwitch = new SoSwitch(SO_SWITCH_NONE);
        collisionSwitch->ref();
        collisionSwitch->setName("AssemblyCollisions");
        auto* look = new SoSeparator;
        auto* pick = new SoPickStyle;
        pick->style = SoPickStyle::UNPICKABLE;
        look->addChild(pick);
        // In front of the component it covers
        auto* offset = new SoPolygonOffset;
        offset->factor = -1.0F;
        offset->units = -1.0F;
        look->addChild(offset);
        auto* hints = new SoShapeHints;
        hints->vertexOrdering = SoShapeHints::UNKNOWN_ORDERING;
        look->addChild(hints);
        auto* material = new SoMaterial;
        material->diffuseColor.setValue(1.0F, 0.15F, 0.1F);
        material->emissiveColor.setValue(0.45F, 0.0F, 0.0F);
        material->transparency.setValue(0.35F);
        look->addChild(material);
        collisionGroup = new SoSeparator;
        look->addChild(collisionGroup);
        collisionSwitch->addChild(look);
        pcRoot->addChild(collisionSwitch);
    }
    collisionGroup->removeAllChildren();
    for (const auto& [obj, wanted] : colliding) {
        auto it = collisionNodes.find(obj);
        if (it != collisionNodes.end()
            && (it->second.whole != wanted.whole || it->second.faces != wanted.faces)) {
            it->second.node->unref();
            collisionNodes.erase(it);
            it = collisionNodes.end();
        }
        if (it == collisionNodes.end()) {
            std::vector<Base::Vector3d> points;
            if (!assembly->getCollisionMesh(obj, points, wanted.whole ? nullptr : &wanted.faces)
                || points.empty()) {
                continue;
            }
            auto* node = new SoSeparator;
            node->ref();
            auto* transform = new SoTransform;
            node->addChild(transform);
            auto* coords = new SoCoordinate3;
            coords->point.setNum(static_cast<int>(points.size()));
            SbVec3f* out = coords->point.startEditing();
            for (std::size_t i = 0; i < points.size(); ++i) {
                out[i].setValue(static_cast<float>(points[i].x),
                                static_cast<float>(points[i].y),
                                static_cast<float>(points[i].z));
            }
            coords->point.finishEditing();
            node->addChild(coords);
            auto* faces = new SoFaceSet;
            const int count = static_cast<int>(points.size() / 3);
            faces->numVertices.setNum(count);
            int32_t* numbers = faces->numVertices.startEditing();
            std::fill(numbers, numbers + count, 3);
            faces->numVertices.finishEditing();
            node->addChild(faces);
            CollisionNode drawn = wanted;
            drawn.node = node;
            drawn.transform = transform;
            it = collisionNodes.emplace(obj, std::move(drawn)).first;
        }
        // The component's placement in the assembly, through its containers
        Base::Placement plc;
        for (auto* step = obj; step && step != assembly;
             step = App::GeoFeatureGroupExtension::getGroupOfObject(step)) {
            if (auto* prop = step->getPlacementProperty()) {
                plc = prop->getValue() * plc;
            }
        }
        const Base::Vector3d pos = plc.getPosition();
        double q0 {}, q1 {}, q2 {}, q3 {};
        plc.getRotation().getValue(q0, q1, q2, q3);
        it->second.transform->translation.setValue(static_cast<float>(pos.x),
                                                   static_cast<float>(pos.y),
                                                   static_cast<float>(pos.z));
        it->second.transform->rotation.setValue(static_cast<float>(q0),
                                                static_cast<float>(q1),
                                                static_cast<float>(q2),
                                                static_cast<float>(q3));
        collisionGroup->addChild(it->second.node);
    }
    collisionSwitch->whichChild = SO_SWITCH_ALL;
}

void ViewProviderAssembly::clearDragCollisions()
{
    if (collisionSwitch) {
        collisionSwitch->whichChild = SO_SWITCH_NONE;
    }
    if (collisionGroup) {
        collisionGroup->removeAllChildren();
    }
    for (auto& entry : collisionNodes) {
        entry.second.node->unref();
    }
    collisionNodes.clear();
}

bool ViewProviderAssembly::mouseButtonPressed(
    int Button,
    bool pressed,
    const SbVec2s& cursorPos,
    const Gui::ViewerContext* viewer
)
{
    Q_UNUSED(cursorPos);
    Q_UNUSED(viewer);

    if (!isInEditMode()) {
        return false;
    }

    // Left Mouse button ****************************************************
    // On a collision marker: its faces selected
    if (Button == 1 && pressed && hoveredMarker >= 0) {
        selectMarkerFaces(hoveredMarker);
        markerPressed = true;
        return true;
    }
    // and its release too, which the navigation takes for a click on nothing
    // and clears the selection with
    if (Button == 1 && !pressed && markerPressed) {
        markerPressed = false;
        return true;
    }
    if (Button == 1) {
        if (pressed && !getDraggerVisibility()) {
            // Check for double-click
            auto now = std::chrono::steady_clock::now();
            long nowMillis
                = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
            if (nowMillis - lastClickTime < 500) {
                auto* joint = getSelectedJoint();
                if (joint) {
                    // Double-click detected
                    // We start by clearing selection such that the second click selects the joint
                    // and not the assembly.
                    Gui::Selection().clearSelection();
                    // singleShot timer to make sure this happens after the release of the click.
                    // Else the release will trigger a removeSelection of what
                    // doubleClickedIn3dView adds to the selection.
                    // (by the object, looked up when the timer fires: the
                    // provider may be gone by then, and it is no QObject to
                    // give the timer as its context)
                    App::DocumentObjectT self(getObject());
                    QTimer::singleShot(50, Gui::getMainWindow(), [self]() {
                        auto vp = dynamic_cast<ViewProviderAssembly*>(
                            Gui::Application::Instance->getViewProvider(self.getObject()));
                        if (vp) {
                            vp->doubleClickedIn3dView();
                        }
                    });
                    return true;
                }
            }
            // First click detected
            lastClickTime = nowMillis;

            canStartDragging = true;
        }
        else {  // Button 1 released
            // release event is not received when user click on a part for selection.
            // So we use SelectionObserver to know if something got selected.

            canStartDragging = false;
            if (partMoving) {
                endMove();
                return true;
            }
        }
    }
    return false;
}

void ViewProviderAssembly::doubleClickedIn3dView()
{
    // Double clicking on a joint should start editing it.
    auto* joint = getSelectedJoint();

    if (joint) {
        std::string obj_name = joint->getNameInDocument();
        std::string doc_name = joint->getDocument()->getName();

        std::string cmd = "import JointObject\n"
                          "obj = App.getDocument('"
            + doc_name + "').getObject('" + obj_name
            + "')\n"
              "Gui.Control.showDialog(JointObject.TaskAssemblyCreateJoint(0, obj))";

        Gui::Command::runCommand(Gui::Command::App, cmd.c_str());
    }
}

bool ViewProviderAssembly::canDragObjectIn3d(App::DocumentObject* obj) const
{
    if (!obj) {
        return false;
    }

    if (auto* asmLink = dynamic_cast<Assembly::AssemblyLink*>(obj)) {
        if (!asmLink->isRigid()) {
            return false;
        }
    }

    auto* assemblyPart = getObject<AssemblyObject>();

    // Check if the selected object is a child of the assembly
    if (!assemblyPart->hasObject(obj, true)) {
        // hasObject does not detect LinkElements (see
        // https://github.com/FreeCAD/FreeCAD/issues/16113) the following block can be removed if
        // the issue is fixed :
        auto* linkEl = dynamic_cast<App::LinkElement*>(obj);
        if (linkEl) {
            auto* linkGroup = linkEl->getLinkGroup();
            if (assemblyPart->hasObject(linkGroup, true)) {
                return true;
            }
        }
        return false;
    }

    auto* propPlacement = obj->getPlacementProperty();
    if (!propPlacement) {
        return false;
    }

    // We have to exclude Grounded joints as they happen to have a Placement prop
    auto* propLink = dynamic_cast<App::PropertyLink*>(obj->getPropertyByName("ObjectToGround"));
    if (propLink) {
        return false;
    }

    // We have to exclude grounded objects as they should not move.
    if (assemblyPart->isPartGrounded(obj)) {
        return false;
    }
    return true;
}

App::DocumentObject* ViewProviderAssembly::getSelectedJoint()
{
    auto sel = Gui::Selection().getSelectionEx("", App::DocumentObject::getClassTypeId());

    if (sel.size() == 1) {  // Handle double click only if only one obj selected.
        App::DocumentObject* obj = sel[0].getObject();
        if (obj) {
            auto* prop = dynamic_cast<App::PropertyBool*>(obj->getPropertyByName("EnableLengthMin"));
            if (prop) {
                return obj;
            }
        }
    }
    return nullptr;
}

bool ViewProviderAssembly::getSelectedObjectsWithinAssembly(bool addPreselection, bool onlySolids)
{
    // check the current selection, and check if any of the selected objects are within this
    // App::Part
    //  If any, put them into the vector docsToMove and return true.
    //  Get the document
    docsToMove.clear();

    // Get the assembly object for this ViewProvider
    auto* assemblyPart = getObject<AssemblyObject>();

    if (!assemblyPart) {
        return false;
    }

    if (!moveOnlyPreselected) {
        for (auto& selObj : Gui::Selection().getSelectionEx(
                 "",
                 App::DocumentObject::getClassTypeId(),
                 Gui::ResolveMode::NoResolve
             )) {
            // getSubNames() returns ["Body001.Pad.Face14", "Body002.Pad.Face7"]
            //  if you have several objects within the same assembly selected.

            std::vector<std::string> objsSubNames = selObj.getSubNames();
            for (auto& subNamesStr : objsSubNames) {
                std::vector<std::string> subNames = Base::Tools::splitSubName(subNamesStr);
                if (subNames.empty()) {
                    continue;
                }
                if (onlySolids && subNames.back() != "") {
                    continue;
                }

                App::DocumentObject* selRoot = selObj.getObject();
                App::DocumentObject* obj = getObjFromRef(selRoot, subNamesStr);
                if (!obj) {
                    // In case of sub-assembly, the jointgroup would trigger the dragger.
                    continue;
                }

                collectMovableObjects(selRoot, subNamesStr, obj, onlySolids);
            }
        }
    }

    // This function is called before the selection is updated. So if a user click and drag a part
    // it is not selected at that point. So we need to get the preselection too.
    if (addPreselection && Gui::Selection().hasPreselection()) {

        App::DocumentObject* selRoot = Gui::Selection().getPreselection().Object.getObject();
        std::string sub = Gui::Selection().getPreselection().pSubName;

        App::DocumentObject* obj = getMovingPartFromSel(assemblyPart, selRoot, sub);
        if (canDragObjectIn3d(obj)) {

            bool alreadyIn = false;
            for (auto& movingObj : docsToMove) {
                App::DocumentObject* obji = movingObj.obj;
                if (obji == obj) {
                    alreadyIn = true;
                    break;
                }
            }

            if (!alreadyIn) {
                auto* pPlc = obj->getPlacementProperty();
                if (!ctrlPressed && !moveOnlyPreselected) {
                    docsToMove.clear();
                }

                docsToMove.emplace_back(obj, pPlc->getValue(), selRoot, sub);
            }
        }
    }

    return !docsToMove.empty();
}

void ViewProviderAssembly::collectMovableObjects(
    App::DocumentObject* selRoot,
    const std::string& subNamePrefix,
    App::DocumentObject* currentObject,
    bool onlySolids
)
{
    // Get the AssemblyObject for context
    auto* assemblyPart = getObject<AssemblyObject>();

    // Handling of special case: flexible AssemblyLink
    auto* asmLink = dynamic_cast<Assembly::AssemblyLink*>(currentObject);
    if (asmLink && !asmLink->isRigid()) {
        std::vector<App::DocumentObject*> children = asmLink->Group.getValues();
        for (auto* child : children) {
            // Recurse on children, appending the child's name to the subName prefix
            std::string newSubNamePrefix = subNamePrefix + child->getNameInDocument() + ".";
            if (child->isDerivedFrom<App::Link>() && child->isLinkGroup()) {
                auto* link = static_cast<App::Link*>(child);
                std::vector<App::DocumentObject*> elts = link->ElementList.getValues();
                for (auto* elt : elts) {
                    std::string eltSubNamePrefix = newSubNamePrefix + elt->getNameInDocument() + ".";
                    collectMovableObjects(selRoot, eltSubNamePrefix, elt, onlySolids);
                }
            }
            else {
                collectMovableObjects(selRoot, newSubNamePrefix, child, onlySolids);
            }
        }
        return;
    }

    // Base case: This is not a flexible link, process it as a potential movable part.
    if (onlySolids
        && !(
            currentObject->isDerivedFrom<App::Part>() || currentObject->isDerivedFrom<Part::Feature>()
            || currentObject->isDerivedFrom<App::Link>()
            || currentObject->isDerivedFrom<App::LinkElement>()
        )) {
        return;
    }

    App::DocumentObject* part = getMovingPartFromSel(assemblyPart, selRoot, subNamePrefix);

    if (onlySolids && assemblyPart->isPartConnected(part)) {
        return;  // No dragger for connected parts.
    }

    if (canDragObjectIn3d(part)) {
        auto* pPlc = part->getPlacementProperty();
        if (pPlc) {
            docsToMove.emplace_back(part, pPlc->getValue(), selRoot, subNamePrefix);
        }
    }
}

ViewProviderAssembly::DragMode ViewProviderAssembly::findDragMode()
{
    ungroundedJointDrag = false;

    auto addPartsToMove = [&](const std::vector<Assembly::ObjRef>& refs) {
        for (auto& partRef : refs) {
            auto obj = partRef.obj;
            auto ref = partRef.ref;
            if (!obj || !ref) {
                continue;
            }

            auto* pPlc = obj->getPlacementProperty();
            if (!pPlc) {
                continue;
            }

            App::DocumentObject* selRoot = ref->getValue();
            if (!selRoot) {
                continue;
            }
            std::vector<std::string> subs = ref->getSubValues();
            if (subs.empty()) {
                continue;
            }

            docsToMove.emplace_back(obj, pPlc->getValue(), selRoot, subs[0]);
        }
    };

    if (docsToMove.size() == 1) {
        auto* assemblyPart = getObject<AssemblyObject>();
        std::string pName;
        movingJoint = assemblyPart->getJointOfPartConnectingToGround(docsToMove[0].obj, pName);

        if (!movingJoint) {
            movingJoint = assemblyPart->getJointOfPartForUngroundedDrag(docsToMove[0].obj, pName);
            if (!movingJoint) {
                // In this case the user is moving an object that is not grounded
                // Then we want to also move other parts that may be connected to it.
                // In particular for case of flexible subassemblies or it looks really weird
                std::vector<Assembly::ObjRef> connectedParts
                    = assemblyPart->getDownstreamParts(docsToMove[0].obj, nullptr);

                addPartsToMove(connectedParts);
                return DragMode::TranslationNoSolve;
            }

            ungroundedJointDrag = true;
            std::vector<Assembly::ObjRef> connectedParts
                = assemblyPart->getDownstreamParts(docsToMove[0].obj, nullptr);
            addPartsToMove(connectedParts);
        }

        JointType jointType = getJointType(movingJoint);
        if (jointType == JointType::Fixed) {
            // If fixed joint we need to find the upstream joint to find move mode.
            // For example : Gnd -(revolute)- A -(fixed)- B : if user try to move B, then we should
            // actually move A
            movingJoint = nullptr;  // reinitialize because getUpstreamMovingPart will call
            // getJointOfPartConnectingToGround again which will find the same joint.
            auto* upPart = assemblyPart->getUpstreamMovingPart(docsToMove[0].obj, movingJoint, pName);
            if (!movingJoint) {
                return DragMode::Translation;
            }
            docsToMove.clear();
            if (!upPart) {
                return DragMode::None;
            }

            auto* pPlc = upPart->getPlacementProperty();
            if (pPlc && movingJoint->isDerivedFrom<Assembly::Joint>()) {
                // A C++ joint's side is a component of the assembly
                docsToMove.emplace_back(upPart, pPlc->getValue(), upPart, "");
            }
            else if (pPlc) {
                auto* ref = dynamic_cast<App::PropertyXLinkSub*>(
                    movingJoint->getPropertyByName(pName.c_str())
                );

                App::DocumentObject* selRoot = ref->getValue();
                if (!selRoot) {
                    return DragMode::None;
                }
                std::vector<std::string> subs = ref->getSubValues();
                if (subs.empty()) {
                    return DragMode::None;
                }

                docsToMove.emplace_back(upPart, pPlc->getValue(), selRoot, subs[0]);
            }

            jointType = getJointType(movingJoint);
        }

        const char* plcPropName = (pName == "Reference1") ? "Placement1" : "Placement2";

        if (int side = Assembly::typedJointSide(movingJoint, pName.c_str())) {
            // A C++ joint's frame is in the assembly's coordinate system
            Base::Placement asmPlc = App::GeoFeature::getGlobalPlacement(getObject<AssemblyObject>());
            jcsGlobalPlc = asmPlc * static_cast<Assembly::Joint*>(movingJoint)->getSide(side).frame;
            jcsPlc = jcsGlobalPlc;
        }
        else {
            // jcsPlc is relative to the Object
            jcsPlc = App::GeoFeature::getPlacementFromProp(movingJoint, plcPropName);

            // Make jcsGlobalPlc relative to the origin of the doc
            auto* ref = dynamic_cast<App::PropertyXLinkSub*>(movingJoint->getPropertyByName(pName.c_str()));
            if (!ref) {
                return DragMode::Translation;
            }
            Base::Placement asmPlc = App::GeoFeature::getGlobalPlacement(getObject<AssemblyObject>());
            Base::Placement global_plc = asmPlc * App::GeoFeature::getGlobalPlacement(nullptr, ref);
            jcsGlobalPlc = global_plc * jcsPlc;
        }

        if (!ungroundedJointDrag) {
            // Add downstream parts so that they move together
            std::vector<Assembly::ObjRef> downstreamParts
                = assemblyPart->getDownstreamParts(docsToMove[0].obj, movingJoint);
            addPartsToMove(downstreamParts);
        }

        if (jointType == JointType::Revolute) {
            return DragMode::RotationOnPlane;
        }
        else if (jointType == JointType::Slider) {
            return DragMode::TranslationOnAxis;
        }
        else if (jointType == JointType::Cylindrical) {
            return DragMode::TranslationOnAxisAndRotationOnePlane;
        }
        else if (jointType == JointType::Ball) {
            return DragMode::Ball;
        }
        else if (jointType == JointType::Distance) {
            //  depends on the type of distance. For example plane-plane:
            DistanceType distanceType = getDistanceType(movingJoint);
            if (distanceType == DistanceType::PlanePlane || distanceType == DistanceType::Other) {
                return DragMode::TranslationOnPlane;
            }
        }
    }
    return DragMode::Translation;
}

void ViewProviderAssembly::initMove(const SbVec2s& cursorPos, Gui::ViewerContext* viewer)
{
    try {
        tryInitMove(cursorPos, viewer);
    }
    catch (const Base::Exception& e) {
        Base::Console().Warning("%s\n", e.what());
    }
}

void ViewProviderAssembly::tryInitMove(const SbVec2s& cursorPos, Gui::ViewerContext* viewer)
{
    dragMode = findDragMode();
    if (dragMode == DragMode::None) {
        return;
    }

    auto* assemblyPart = getObject<AssemblyObject>();
    // When the user drag parts, we switch off all joints visibility and only show the movingjoint
    jointVisibilitiesBackup.clear();
    auto joints = assemblyPart->getJoints();
    for (auto* joint : joints) {
        if (!joint) {
            continue;
        }
        bool visible = joint->Visibility.getValue();
        jointVisibilitiesBackup.push_back({joint, visible});
        if (movingJoint == joint) {
            if (!visible) {
                joint->Visibility.setValue(true);
            }
        }
        else if (visible) {
            joint->Visibility.setValue(false);
        }
        joint->purgeTouched();
    }

    SbVec3f vec;
    if (ungroundedJointDrag) {
        vec = viewer->getPointOnFocalPlane(cursorPos);
        initialUngroundedDragPosition = Base::Vector3d(vec[0], vec[1], vec[2]);
    }

    if (dragMode == DragMode::RotationOnPlane) {
        vec = viewer->getPointOnXYPlaneOfPlacement(cursorPos, jcsGlobalPlc);
        initialPositionRot = Base::Vector3d(vec[0], vec[1], vec[2]);
    }
    else if (dragMode == DragMode::TranslationOnAxis) {
        Base::Vector3d zAxis = jcsGlobalPlc.getRotation().multVec(Base::Vector3d(0., 0., 1.));
        Base::Vector3d pos = jcsGlobalPlc.getPosition();
        SbVec3f axisCenter(pos.x, pos.y, pos.z);
        SbVec3f axis(zAxis.x, zAxis.y, zAxis.z);
        vec = viewer->getPointOnLine(cursorPos, axisCenter, axis);
        initialPosition = Base::Vector3d(vec[0], vec[1], vec[2]);
    }
    else if (dragMode == DragMode::TranslationOnAxisAndRotationOnePlane) {
        vec = viewer->getPointOnXYPlaneOfPlacement(cursorPos, jcsGlobalPlc);
        initialPositionRot = Base::Vector3d(vec[0], vec[1], vec[2]);

        Base::Vector3d zAxis = jcsGlobalPlc.getRotation().multVec(Base::Vector3d(0., 0., 1.));
        Base::Vector3d pos = jcsGlobalPlc.getPosition();
        SbVec3f axisCenter(pos.x, pos.y, pos.z);
        SbVec3f axis(zAxis.x, zAxis.y, zAxis.z);
        vec = viewer->getPointOnLine(cursorPos, axisCenter, axis);
        initialPosition = Base::Vector3d(vec[0], vec[1], vec[2]);
    }
    else if (dragMode == DragMode::TranslationOnPlane) {
        vec = viewer->getPointOnXYPlaneOfPlacement(cursorPos, jcsGlobalPlc);
        initialPosition = Base::Vector3d(vec[0], vec[1], vec[2]);
    }
    else {
        vec = viewer->getPointOnFocalPlane(cursorPos);
        initialPosition = Base::Vector3d(vec[0], vec[1], vec[2]);
        prevPosition = initialPosition;
    }

    if (moveInCommand) {
        getDocument()->openCommand(tr("Move part").toStdString().c_str());
    }
    partMoving = true;

    // prevent selection while moving
    viewer->setSelectionEnabled(false);

    ParameterGrp::handle hGrp = App::GetApplication().GetParameterGroupByPath(
        "User parameter:BaseApp/Preferences/Mod/Assembly"
    );
    bool solveOnMove = hGrp->GetBool("SolveOnMove", Assembly::AssemblyParams::defaultSolveOnMove());
    std::vector<App::DocumentObject*> dragParts;
    for (auto& movingObj : docsToMove) {
        dragParts.push_back(movingObj.obj);
    }
    forceSolveOnMoveForRigid = assemblyPart->requiresRigidSolveForMove(dragParts);

    if (solveOnMove && (dragMode != DragMode::TranslationNoSolve || forceSolveOnMoveForRigid)) {
        objectMasses.clear();
        for (auto& movingObj : docsToMove) {
            objectMasses.push_back({movingObj.obj, 10.0});
        }

        assemblyPart->setObjMasses(objectMasses);
        assemblyPart->preDrag(dragParts);
    }
    else {
        // preDrag() starts the collision watch for a solved drag
        assemblyPart->beginCollisionWatch();
        assemblyPart->redrawJointPlacements(assemblyPart->getJoints());
    }
}

void ViewProviderAssembly::endMove()
{
    docsToMove.clear();
    partMoving = false;
    canStartDragging = false;
    forceSolveOnMoveForRigid = false;

    auto* assemblyPart = getObject<AssemblyObject>();
    auto joints = assemblyPart->getJoints();
    for (auto pair : jointVisibilitiesBackup) {
        bool visible = pair.first->Visibility.getValue();
        if (visible != pair.second) {
            pair.first->Visibility.setValue(pair.second);
            pair.first->purgeTouched();
        }
    }

    movingJoint = nullptr;

    // enable selection after the move
    auto* view = dynamic_cast<Gui::View3DInventor*>(getDocument()->getActiveView());
    if (view) {
        view->getViewer()->setSelectionEnabled(true);
    }

    ParameterGrp::handle hGrp = App::GetApplication().GetParameterGroupByPath(
        "User parameter:BaseApp/Preferences/Mod/Assembly"
    );
    bool solveOnMove = hGrp->GetBool("SolveOnMove", Assembly::AssemblyParams::defaultSolveOnMove());
    if (solveOnMove) {
        assemblyPart->postDrag();
        assemblyPart->setObjMasses({});
    }
    assemblyPart->endCollisionWatch();
    clearDragCollisions();

    if (moveInCommand) {
        getDocument()->commitCommand();
    }
}

void ViewProviderAssembly::initMoveDragger()
{
    setDraggerVisibility(true);

    // find the placement for the dragger.
    App::DocumentObject* part = docsToMove[0].obj;

    draggerInitPlc
        = App::GeoFeature::getGlobalPlacement(part, docsToMove[0].rootObj, docsToMove[0].sub);
    std::vector<App::DocumentObject*> listOfObjs;
    std::vector<App::PropertyXLinkSub*> listOfRefs;
    for (auto& movingObj : docsToMove) {
        listOfObjs.push_back(movingObj.obj);
        listOfRefs.push_back(movingObj.ref);
    }
    Base::Vector3d pos = getCenterOfBoundingBox(docsToMove);
    draggerInitPlc.setPosition(pos);

    setDraggerPlacement(draggerInitPlc);
    asmDragger->addStartCallback(draggerStartCallback, this);
    asmDragger->addMotionCallback(draggerMotionCallback, this);
    asmDragger->addFinishCallback(draggerFinishCallback, this);
}

void ViewProviderAssembly::endMoveDragger()
{
    if (getDraggerVisibility()) {
        asmDragger->removeStartCallback(draggerStartCallback, this);
        asmDragger->removeMotionCallback(draggerMotionCallback, this);
        asmDragger->removeFinishCallback(draggerFinishCallback, this);
        setDraggerVisibility(false);
    }
}

void ViewProviderAssembly::draggerStartCallback(void* data, SoDragger* d)
{
    boost::ignore_unused(d);
    static_cast<ViewProviderAssembly*>(data)->getObject<AssemblyObject>()->beginCollisionWatch();
}

void ViewProviderAssembly::draggerFinishCallback(void* data, SoDragger* d)
{
    boost::ignore_unused(d);
    auto sudoThis = static_cast<ViewProviderAssembly*>(data);
    auto* assembly = sudoThis->getObject<AssemblyObject>();
    assembly->endCollisionWatch();
    sudoThis->clearDragCollisions();

    // Stopped short of where the dragger was let go: back to the parts
    if (sudoThis->docsToMove.empty()) {
        return;
    }
    const auto& moved = sudoThis->docsToMove.front();
    auto* pPlc = moved.obj->getPlacementProperty();
    if (!pPlc) {
        return;
    }
    Base::Placement movePlc = pPlc->getValue() * moved.plc.inverse();
    Base::Placement asmPlc = App::GeoFeature::getGlobalPlacement(assembly);
    if (!asmPlc.isIdentity()) {
        movePlc = asmPlc * movePlc * asmPlc.inverse();
    }
    sudoThis->setDraggerPlacement(movePlc * sudoThis->draggerInitPlc);
}

void ViewProviderAssembly::draggerMotionCallback(void* data, SoDragger* d)
{
    boost::ignore_unused(d);
    auto sudoThis = static_cast<ViewProviderAssembly*>(data);

    Base::Placement draggerPlc = sudoThis->getDraggerPlacement();
    Base::Placement movePlc = draggerPlc * sudoThis->draggerInitPlc.inverse();

    // Transform the global delta `movePlc` in case the assembly is transformed.
    Base::Placement asmPlc = App::GeoFeature::getGlobalPlacement(sudoThis->getObject<AssemblyObject>());
    if (!asmPlc.isIdentity()) {
        movePlc = asmPlc.inverse() * movePlc * asmPlc;
    }

    // A move that would collide is taken back towards these
    std::vector<std::pair<App::PropertyPlacement*, Base::Placement>> previous;
    std::vector<Base::Placement> wanted;
    for (auto& movingObj : sudoThis->docsToMove) {
        App::DocumentObject* obj = movingObj.obj;

        auto* pPlc = obj->getPlacementProperty();
        if (pPlc) {
            previous.emplace_back(pPlc, pPlc->getValue());
            wanted.push_back(movePlc * movingObj.plc);
            pPlc->setValue(wanted.back());
        }
    }

    // Stop: as far along the move as the parts go without colliding
    auto* assembly = sudoThis->getObject<AssemblyObject>();
    if (!assembly->checkDragCollisions().empty()
        && assembly->getDragCollisionMode() == AssemblyObject::DragCollision::Stop) {
        assembly->stopAtDragCollision([&](double t) {
            for (std::size_t i = 0; i < previous.size(); ++i) {
                const auto& [prop, plc] = previous[i];
                prop->setValue(t <= 0.0 ? plc : Base::Placement::sclerp(plc, wanted[i], t));
            }
            return true;
        });
        sudoThis->showDragStopped();
    }
    sudoThis->showDragCollisions();
}

void ViewProviderAssembly::onSelectionChanged(const Gui::SelectionChanges& msg)
{
    // onSelectionChanged is called from both Selection.cpp and SelectionObserver.
    // In the case where you have nested assemblies, that would cause issues. See #27532
    bool singleAssembly
        = getDocument()->getDocument()->getObjectsOfType<Assembly::AssemblyObject>().size() == 1;
    if (!isInEditMode() && !singleAssembly) {
        return;
    }

    // Joint components isolation
    if (msg.Type == Gui::SelectionChanges::AddSelection) {
        auto selection = Gui::Selection().getSelection();
        if (selection.size() == 1) {
            App::DocumentObject* obj = selection[0].pObject;
            if (obj
                && (obj->getPropertyByName("JointType") || obj->getPropertyByName("ObjectToGround"))) {
                isolateJointReferences(obj);
                return;
            }
            else if (explodeTemporarily(obj)) {
                return;
            }
        }
        else {
            clearIsolate();
            clearTemporaryExplosion();
        }
    }
    if (msg.Type == Gui::SelectionChanges::ClrSelection
        || msg.Type == Gui::SelectionChanges::RmvSelection) {
        clearIsolate();
        clearTemporaryExplosion();
    }

    if (!isInEditMode()) {
        return;
    }

    if (msg.Type == Gui::SelectionChanges::AddSelection
        || msg.Type == Gui::SelectionChanges::ClrSelection
        || msg.Type == Gui::SelectionChanges::RmvSelection) {
        canStartDragging = false;
    }

    if (msg.Type == Gui::SelectionChanges::AddSelection) {
        // If selected object is a single solid show dragger and init dragger move

        if (enableMovement && getSelectedObjectsWithinAssembly(false, true)) {
            initMoveDragger();
        }
    }
    if (msg.Type == Gui::SelectionChanges::ClrSelection
        || msg.Type == Gui::SelectionChanges::RmvSelection) {
        if (enableMovement) {
            endMoveDragger();
        }
    }
}

bool ViewProviderAssembly::onDelete(const std::vector<std::string>& subNames)
{
    // Delete the assembly groups when assembly is deleted
    for (auto obj : getObject()->getOutList()) {
        // Not upstream: an aux group shows the assembly's own children
        // (docs/AssemblyObjectModel.md sec 3); what it shows goes with it
        if (obj->is<Assembly::AssemblyGroup>()) {
            Gui::Command::doCommand(
                Gui::Command::Doc,
                "doc = App.getDocument(\"%s\")\n"
                "group = doc.getObject(\"%s\")\n"
                "for member in group.Group:\n"
                "    if hasattr(member, 'removeObjectsFromDocument'):\n"
                "        member.removeObjectsFromDocument()\n"
                "    doc.removeObject(member.Name)\n"
                "doc.removeObject(group.Name)\n",
                obj->getDocument()->getName(),
                obj->getNameInDocument()
            );
            continue;
        }
        if (obj->is<Assembly::JointGroup>() || obj->is<Assembly::ViewGroup>()
            || obj->is<Assembly::BomGroup>()) {

            // Delete the group content first.
            Gui::Command::doCommand(
                Gui::Command::Doc,
                "doc = App.getDocument(\"%s\")\n"
                "objName = \"%s\"\n"
                "doc.getObject(objName).removeObjectsFromDocument()\n"
                "doc.removeObject(objName)\n",
                obj->getDocument()->getName(),
                obj->getNameInDocument()
            );
        }
    }

    return ViewProviderPart::onDelete(subNames);
}

bool ViewProviderAssembly::canDelete(App::DocumentObject* objBeingDeleted) const
{
    bool res = ViewProviderPart::canDelete(objBeingDeleted);
    if (res) {
        // If a component is deleted, then we delete the joints as well.
        auto* assemblyPart = getObject<AssemblyObject>();

        std::vector<App::DocumentObject*> objToDel;
        std::vector<App::DocumentObject*> objsBeingDeleted;
        objsBeingDeleted.push_back(objBeingDeleted);

        auto addSubComponents
            = std::function<void(AssemblyLink*, std::vector<App::DocumentObject*>&)> {};
        addSubComponents = [&](AssemblyLink* asmLink, std::vector<App::DocumentObject*>& objs) {
            std::vector<App::DocumentObject*> assemblyLinkGroup = asmLink->Group.getValues();
            for (auto* obj : assemblyLinkGroup) {
                auto* subAsmLink = freecad_cast<AssemblyLink*>(obj);
                auto* link = dynamic_cast<App::Link*>(obj);
                if (subAsmLink || link) {
                    if (std::ranges::find(objs, obj) == objs.end()) {
                        objs.push_back(obj);
                        if (subAsmLink && !asmLink->isRigid()) {
                            addSubComponents(subAsmLink, objs);
                        }
                    }
                }
            }
        };

        auto* asmLink = dynamic_cast<Assembly::AssemblyLink*>(objBeingDeleted);
        if (asmLink && !asmLink->isRigid()) {
            addSubComponents(asmLink, objsBeingDeleted);
        }

        for (auto* obj : objsBeingDeleted) {
            // List its joints
            std::vector<App::DocumentObject*> joints = assemblyPart->getJointsOfObj(obj);
            for (auto* joint : joints) {
                if (std::ranges::find(objToDel, joint) == objToDel.end()) {
                    objToDel.push_back(joint);
                }
            }
            joints = assemblyPart->getJointsOfPart(obj);
            for (auto* joint : joints) {
                if (std::ranges::find(objToDel, joint) == objToDel.end()) {
                    objToDel.push_back(joint);
                }
            }

            // List its grounded joints
            std::vector<App::DocumentObject*> inList = obj->getInList();
            for (auto* parent : inList) {
                if (!parent) {
                    continue;
                }

                if (parent->getPropertyByName("ObjectToGround")) {
                    if (std::ranges::find(objToDel, parent) == objToDel.end()) {
                        objToDel.push_back(parent);
                    }
                }
            }
        }

        // Deletes them.
        for (auto* joint : objToDel) {
            if (joint && joint->getNameInDocument() != nullptr) {
                Gui::Command::doCommand(
                    Gui::Command::Doc,
                    "App.getDocument(\"%s\").removeObject(\"%s\")",
                    joint->getDocument()->getName(),
                    joint->getNameInDocument()
                );
            }
        }
    }
    return res;
}

void ViewProviderAssembly::setDraggerVisibility(bool val)
{
    asmDraggerSwitch->whichChild = val ? SO_SWITCH_ALL : SO_SWITCH_NONE;
}
bool ViewProviderAssembly::getDraggerVisibility()
{
    if (!isInEditMode()) {
        return false;
    }

    return asmDraggerSwitch->whichChild.getValue() == SO_SWITCH_ALL;
}

void ViewProviderAssembly::setDraggerPlacement(Base::Placement plc)
{
    asmDragger->rotation.setValue(Base::convertTo<SbRotation>(plc.getRotation()));
    asmDragger->translation.setValue(Base::convertTo<SbVec3f>(plc.getPosition()));
}

Base::Placement ViewProviderAssembly::getDraggerPlacement()
{
    return {
        Base::convertTo<Base::Vector3d>(asmDragger->translation.getValue()),
        Base::convertTo<Base::Rotation>(asmDragger->rotation.getValue())
    };
}

Gui::SoTransformDragger* ViewProviderAssembly::getDragger()
{
    return asmDragger;
}

PyObject* ViewProviderAssembly::getPyObject()
{
    if (!pyViewObject) {
        pyViewObject = new ViewProviderAssemblyPy(this);
    }
    pyViewObject->IncRef();
    return pyViewObject;
}

void ViewProviderAssembly::applyIsolationRecursively(
    App::DocumentObject* current,
    std::set<App::DocumentObject*>& isolateSet,
    IsolateMode mode,
    std::set<App::DocumentObject*>& visited
)
{
    if (!current || !visited.insert(current).second) {
        return;  // Object is null or already processed
    }

    bool isolate = isolateSet.count(current);

    if (auto* group = dynamic_cast<App::DocumentObjectGroup*>(current)) {
        for (auto* child : group->Group.getValues()) {
            applyIsolationRecursively(child, isolateSet, mode, visited);
        }
        return;
    }
    else if (auto* part = dynamic_cast<App::Part*>(current)) {
        // As App::Part currently don't have material override
        // (there is in LinkStage and RealThunder said he'll try to PR later)
        // we have to recursively apply to children of App::Parts.

        // If Part is in isolateSet, then all its children should be added to isolateSet
        if (isolate) {
            for (auto* child : part->Group.getValues()) {
                isolateSet.insert(child);
            }
        }
        for (auto* child : part->Group.getValues()) {
            applyIsolationRecursively(child, isolateSet, mode, visited);
        }
        return;
    }

    auto* vp = Gui::Application::Instance->getViewProvider(current);
    auto* vpl = dynamic_cast<Gui::ViewProviderLink*>(vp);
    auto* vpg = dynamic_cast<Gui::ViewProviderGeometryObject*>(vp);
    if (!vpl && !vpg) {
        return;  // we process only geometric objects and links.
    }

    // Backup the initial values.
    ComponentState state;
    state.visibility = current->Visibility.getValue();
    if (vpl) {
        state.selectable = vpl->Selectable.getValue();
    }
    else {
        state.selectable = vpg->Selectable.getValue();
    }
    stateBackup[current] = state;

    if (mode == IsolateMode::Hidden) {
        current->Visibility.setValue(isolate);
        return;
    }

    if (isolate && !state.visibility) {  // force visibility for isolated objects
        current->Visibility.setValue(true);
    }

    if (vpl) {
        vpl->Selectable.setValue(isolate);
    }
    else if (vpg) {
        vpg->Selectable.setValue(isolate);
    }

    if (!isolate) {
        float trans = mode == IsolateMode::Transparent ? 0.8 : 1.0;
        // Upstream reads this from App::MaterialAppearance::getDefaultAppearance(), which this
        // fork does not have; the same preference is reachable through ViewParams.
        Base::Color transparentColor;
        transparentColor.setPackedValue(Gui::ViewParams::getDefaultShapeColor());
        // setTransparency means what it says since the alpha convention flip
        // (Base/Color.h); before that this line quietly meant the opposite.
        transparentColor.setTransparency(trans);
        std::map<std::string, Base::Color> colorMap;
        colorMap["Face"] = transparentColor;  // The "Face" wildcard targets all faces

        Gui::SoSelectionElementAction action(Gui::SoSelectionElementAction::Color, true);
        action.swapColors(colorMap);
        action.apply(vp->getRoot());
    }
}

void ViewProviderAssembly::isolateComponents(std::set<App::DocumentObject*>& isolateSet, IsolateMode mode)
{
    if (!stateBackup.empty()) {
        clearIsolate();
    }

    auto* assembly = getObject<AssemblyObject>();
    if (!assembly) {
        return;
    }

    std::vector<App::DocumentObject*> topLevelChildren = assembly->Group.getValues();

    std::set<App::DocumentObject*> visited;
    for (auto* child : topLevelChildren) {
        applyIsolationRecursively(child, isolateSet, mode, visited);
    }
}

void ViewProviderAssembly::isolateJointReferences(App::DocumentObject* joint, IsolateMode mode)
{
    if (!joint || isolatedJoint == joint) {
        return;
    }

    clearIsolate();

    if (auto* prop = joint->getPropertyByName<App::PropertyLink>("ObjectToGround")) {
        auto* groundedObj = prop->getValue();

        isolatedJoint = joint;
        isolatedJointVisibilityBackup = joint->Visibility.getValue();
        joint->Visibility.setValue(true);

        std::set<App::DocumentObject*> isolateSet = {groundedObj};
        isolateComponents(isolateSet, mode);
        return;
    }

    App::DocumentObject* part1 = getMovingPartFromRef(joint, "Reference1");
    App::DocumentObject* part2 = getMovingPartFromRef(joint, "Reference2");
    if (!part1 || !part2) {
        return;
    }

    isolatedJoint = joint;
    isolatedJointVisibilityBackup = joint->Visibility.getValue();
    if (!isolatedJointVisibilityBackup) {
        joint->Visibility.setValue(true);
    }

    std::set<App::DocumentObject*> isolateSet = {part1, part2};
    isolateComponents(isolateSet, mode);

    highlightJointElements(joint);
}

void ViewProviderAssembly::clearIsolate()
{
    if (isolatedJoint) {
        if (!isolatedJointVisibilityBackup) {
            isolatedJoint->Visibility.setValue(false);
        }
        isolatedJoint = nullptr;

        clearJointElementHighlight();
    }

    for (const auto& pair : stateBackup) {
        App::DocumentObject* component = pair.first;
        const ComponentState& state = pair.second;
        if (!component || !component->isAttachedToDocument()) {
            continue;
        }

        component->Visibility.setValue(state.visibility);
        auto* vp = Gui::Application::Instance->getViewProvider(component);
        if (auto* vpl = dynamic_cast<Gui::ViewProviderLink*>(vp)) {
            vpl->Selectable.setValue(state.selectable);
        }
        else if (auto* vpg = dynamic_cast<Gui::ViewProviderGeometryObject*>(vp)) {
            vpg->Selectable.setValue(state.selectable);
        }
        Gui::SoSelectionElementAction action(Gui::SoSelectionElementAction::Color, true);
        action.apply(vp->getRoot());
    }

    stateBackup.clear();
}

void ViewProviderAssembly::highlightJointElements(App::DocumentObject* joint)
{
    clearJointElementHighlight();

    // Assembly's own colour once one is set. 0, which is what the setting is
    // while it is not, is upstream's rule: the 3D view's preselection colour
    // when that is stored, and this red when it is not.
    uint32_t packedColor = static_cast<uint32_t>(Assembly::AssemblyParams::getJointHighlightColor());
    if (packedColor == 0) {
        SbColor defaultHighlightColor(0.8f, 0.1f, 0.1f);
        ParameterGrp::handle hGrp = Gui::WindowParameter::getDefaultParameter()->GetGroup("View");
        packedColor = hGrp->GetUnsigned("HighlightColor", defaultHighlightColor.getPackedValue());
    }
    Base::Color highlightColor(packedColor);

    std::set<std::string> processedElements;

    const char* refNames[] = {"Reference1", "Reference2"};
    for (const char* refName : refNames) {
        auto* propRef = dynamic_cast<App::PropertyXLinkSub*>(joint->getPropertyByName(refName));
        if (!propRef || propRef->getSubValues().empty()) {
            continue;
        }
        const auto& el = propRef->getSubValues()[0];

        if (el.empty() || processedElements.count(el)) {
            continue;
        }
        processedElements.insert(el);  // Mark as processed.

        auto* path = static_cast<SoFullPath*>(new SoPath(20));
        SoDetail* detail = nullptr;

        if (this->getDetailPath(el.c_str(), path, true, detail)) {
            const char* elementNameCStr = Data::findElementName(el.c_str());
            if (!elementNameCStr || !elementNameCStr[0]) {
                continue;
            }
            std::string elementName(elementNameCStr);

            Gui::SoSelectionElementAction action(Gui::SoSelectionElementAction::Color, true);

            std::map<std::string, Base::Color> colorMap;
            colorMap[elementName] = highlightColor;
            action.swapColors(colorMap);

            path->ref();
            action.apply(path);
        }
        delete detail;
    }
}

void ViewProviderAssembly::clearJointElementHighlight()
{
    Gui::SoSelectionElementAction action(Gui::SoSelectionElementAction::Color, true);
    // An empty color map tells nodes to clear their secondary color.
    action.apply(this->getRoot());
}

void ViewProviderAssembly::slotAboutToOpenTransaction(const std::string& cmdName)
{
    Q_UNUSED(cmdName);
    this->clearIsolate();
    this->clearTemporaryExplosion();
}

void ViewProviderAssembly::slotStartSave(const App::Document& doc, const std::string& filename)
{
    Q_UNUSED(filename);

    // Isolation and temporary explosion mutate persisted state (Selectable,
    // Visibility, Placements) and hold their restore data only in memory, so clear
    // both before serialization rather than writing the transient state to file.
    Gui::Document* guiDoc = getDocument();
    if (!guiDoc || guiDoc->getDocument() != &doc) {
        return;  // not our document
    }
    this->clearIsolate();
    this->clearTemporaryExplosion();
}

void ViewProviderAssembly::slotDeletedObject(const App::DocumentObject& obj)
{
    // Isolation and temporary explosion hold raw DocumentObject restore targets, and
    // objects can be deleted without a transaction opening (undo/redo, removeObject,
    // document close). Drop stale references so a later clear does not dereference
    // freed memory. Pointer identity only -- obj may already be half-destroyed.
    auto* deleted = const_cast<App::DocumentObject*>(&obj);

    stateBackup.erase(deleted);

    if (isolatedJoint == deleted) {
        clearJointElementHighlight();
        isolatedJoint = nullptr;
    }

    if (temporaryExplosion == deleted) {
        temporaryExplosion = nullptr;
    }
}

bool ViewProviderAssembly::explodeTemporarily(App::DocumentObject* explodedView)
{
    if (!explodedView || temporaryExplosion == explodedView) {
        return false;
    }

    clearTemporaryExplosion();

    Base::PyGILStateLocker lock;

    App::PropertyPythonObject* proxy = explodedView
        ? dynamic_cast<App::PropertyPythonObject*>(explodedView->getPropertyByName("Proxy"))
        : nullptr;

    if (!proxy) {
        return false;
    }

    Py::Object jointPy = proxy->getValue();

    if (!jointPy.hasAttr("explodeTemporarily")) {
        return false;
    }

    Py::Object attr = jointPy.getAttr("explodeTemporarily");
    if (attr.ptr() && attr.isCallable()) {
        Py::Tuple args(1);
        args.setItem(0, Py::asObject(explodedView->getPyObject()));
        Py::Callable(attr).apply(args);
        temporaryExplosion = explodedView;
        temporaryExplosion->purgeTouched();
        return true;
    }

    return false;
}

void ViewProviderAssembly::clearTemporaryExplosion()
{
    if (!temporaryExplosion) {
        return;
    }

    Base::PyGILStateLocker lock;

    App::PropertyPythonObject* proxy = temporaryExplosion
        ? dynamic_cast<App::PropertyPythonObject*>(temporaryExplosion->getPropertyByName("Proxy"))
        : nullptr;

    if (!proxy) {
        return;
    }

    Py::Object jointPy = proxy->getValue();

    if (!jointPy.hasAttr("restoreAssembly")) {
        return;
    }

    Py::Object attr = jointPy.getAttr("restoreAssembly");
    if (attr.ptr() && attr.isCallable()) {
        Py::Tuple args(1);
        args.setItem(0, Py::asObject(temporaryExplosion->getPyObject()));
        Py::Callable(attr).apply(args);
        temporaryExplosion->purgeTouched();
        temporaryExplosion = nullptr;
    }
}

// UTILS
Base::Vector3d ViewProviderAssembly::getCenterOfBoundingBox(const std::vector<MovingObject>& movingObjs)
{
    int count = 0;
    Base::Vector3d center;  // feujhzef

    for (auto& movingObj : movingObjs) {
        Gui::ViewProvider* viewProvider = Gui::Application::Instance->getViewProvider(movingObj.obj);
        if (!viewProvider) {
            continue;
        }

        const Base::BoundBox3d& boundingBox = viewProvider->getBoundingBox();
        if (!boundingBox.IsValid()) {
            continue;
        }

        Base::Vector3d bboxCenter = boundingBox.GetCenter();

        // bboxCenter does not take into account obj global placement
        Base::Placement plc(bboxCenter, Base::Rotation());
        // Change plc to be relative to the object placement.
        Base::Placement objPlc = App::GeoFeature::getPlacementFromProp(movingObj.obj, "Placement");
        plc = objPlc.inverse() * plc;
        // Change plc to be relative to the origin of the document.
        Base::Placement global_plc
            = App::GeoFeature::getGlobalPlacement(movingObj.obj, movingObj.rootObj, movingObj.sub);
        plc = global_plc * plc;
        bboxCenter = plc.getPosition();

        center += bboxCenter;
        ++count;
    }

    if (count > 0) {
        center /= static_cast<double>(count);
    }

    return center;
}

inline QString objListHelper(const AssemblyObject* assembly, const std::vector<std::string>& names)
{
    if (!assembly) {
        return QString();
    }
    App::Document* doc = assembly->getDocument();

    std::vector<App::DocumentObject*> joints;
    for (const auto& name : names) {
        if (auto* obj = doc->getObject(name.c_str())) {
            joints.push_back(obj);
        }
    }

    QString results;
    if (joints.size() < 3) {  // The 3 is a bit heuristic... more than that and we shift formats
        for (const auto joint : joints) {
            if (!results.isEmpty()) {
                results.append(QStringLiteral(", "));
            }
            results.append(
                QStringLiteral("%1").arg(QString::fromLatin1(joint->Label.getStrValue().c_str()))
            );
        }
    }
    else {
        const int numToShow = 2;
        int more = joints.size() - numToShow;
        for (int i = 0; i < numToShow; ++i) {
            results.append(QStringLiteral("%1, ").arg(
                QString::fromLatin1(joints[i]->Label.getStrValue().c_str())
            ));
        }
        results.append(ViewProviderAssembly::tr("and %1 more").arg(more));
    }
    return results;
}

void ViewProviderAssembly::UpdateSolverInformation()
{
    // Updates Solver Information with the Last solver execution at AssemblyObject level
    auto* assembly = getObject<AssemblyObject>();

    int dofs = assembly->getLastDoF();
    bool hasConflicts = assembly->getLastHasConflicts();
    bool hasRedundancies = assembly->getLastHasRedundancies();
    bool hasMalformed = assembly->getLastHasMalformedConstraints();
    // Partially redundant joints are a subset of the redundant ones (a joint
    // with some, not all, of its equations removed); say so when that is all
    // there is.
    bool onlyPartiallyRedundant = hasRedundancies
        && assembly->getLastPartiallyRedundant().size() == assembly->getLastRedundant().size();

    if (assembly->isEmpty()) {
        signalSetUp(QStringLiteral("empty"), tr("Empty Assembly"), QString(), QString());
    }
    else if (hasConflicts) {  // over-constrained, and the extra joints disagree
        signalSetUp(
            QStringLiteral("conflicting_constraints"),
            tr("Over-constrained:") + QLatin1String(" "),
            QStringLiteral("#conflicting"),
            QStringLiteral("(%1)").arg(objListHelper(assembly, assembly->getLastConflicting()))
        );
    }
    else if (hasMalformed) {  // malformed joints
        signalSetUp(
            QStringLiteral("malformed_constraints"),
            tr("Malformed joints:") + QLatin1String(" "),
            QStringLiteral("#malformed"),
            QStringLiteral("(%1)").arg(objListHelper(assembly, assembly->getLastMalformed()))
        );
    }
    else if (hasRedundancies && !onlyPartiallyRedundant) {
        signalSetUp(
            QStringLiteral("redundant_constraints"),
            tr("Redundant joints:") + QLatin1String(" "),
            QStringLiteral("#redundant"),
            QStringLiteral("(%1)").arg(objListHelper(assembly, assembly->getLastRedundant()))
        );
    }
    else if (hasRedundancies) {
        signalSetUp(
            QStringLiteral("partially_redundant_constraints"),
            tr("Partially redundant:") + QLatin1String(" "),
            QStringLiteral("#redundant"),
            QStringLiteral("(%1)").arg(objListHelper(assembly, assembly->getLastPartiallyRedundant()))
        );
    }
    else if (assembly->getLastSolverStatus() != 0) {
        signalSetUp(
            QStringLiteral("solver_failed"),
            tr("Solver failed to converge"),
            QStringLiteral(""),
            QStringLiteral("")
        );
    }
    else if (dofs > 0) {
        signalSetUp(
            QStringLiteral("under_constrained"),
            tr("Under-constrained:") + QLatin1String(" "),
            QStringLiteral("#dofs"),
            tr("%n Degrees of Freedom", "", dofs)
        );
    }
    else {
        signalSetUp(QStringLiteral("fully_constrained"), tr("Fully constrained"), QString(), QString());
    }
}

void ViewProviderAssembly::onWorkbenchActivated(const QString& name)
{
    bool isAssemblyWb = (name == QLatin1String("AssemblyWorkbench"));
    updateTaskPanel(isAssemblyWb);
}

void ViewProviderAssembly::updateTaskPanel(bool show)
{
    Gui::TaskView::TaskView* taskView = Gui::Control().taskPanel();
    if (!taskView) {
        return;
    }

    if (show && !taskSolver) {
        taskSolver = new TaskAssemblyMessages(this);
        taskView->addContextualPanel(taskSolver, this->getObject()->getDocument());
        UpdateSolverInformation();
    }
    else if (!show && taskSolver) {
        taskView->removeContextualPanel(taskSolver, this->getObject()->getDocument());
        taskSolver = nullptr;
    }
}
