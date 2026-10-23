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

#include "AssemblyParams.h"
#include <boost/core/ignore_unused.hpp>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <algorithm>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <ranges>

// Reached transitively upstream, but not here.
#include <Precision.hxx>


#include <App/Application.h>
#include <App/Datums.h>
#include <App/Document.h>
#include <App/DocumentObjectGroup.h>
#include <App/FeaturePythonPyImp.h>
#include <App/ElementNamingUtils.h>
#include <App/Link.h>
#include <App/PropertyPythonObject.h>
#include <Base/Console.h>
#include <Base/Placement.h>
#include <Base/Rotation.h>
#include <Base/Tools.h>
#include <Base/Interpreter.h>

#include <Mod/Part/App/TopoShape.h>
#include <Mod/Part/App/AttachExtension.h>
#include <Mod/Part/App/DatumFeature.h>
#include <Mod/Part/App/PartFeature.h>

#include <OndselSolver/CREATE.h>
#include <OndselSolver/ASMTSimulationParameters.h>
#include <OndselSolver/ASMTAssembly.h>
#include <OndselSolver/ASMTMarker.h>
#include <OndselSolver/ASMTPart.h>
#include <OndselSolver/ASMTJoint.h>
#include <OndselSolver/ASMTAngleJoint.h>
#include <OndselSolver/ASMTFixedJoint.h>
#include <OndselSolver/ASMTGearJoint.h>
#include <OndselSolver/ASMTRevoluteJoint.h>
#include <OndselSolver/ASMTCylindricalJoint.h>
#include <OndselSolver/ASMTTranslationalJoint.h>
#include <OndselSolver/ASMTSphericalJoint.h>
#include <OndselSolver/ASMTParallelAxesJoint.h>
#include <OndselSolver/ASMTPerpendicularJoint.h>
#include <OndselSolver/ASMTPointInPlaneJoint.h>
#include <OndselSolver/ASMTPointInLineJoint.h>
#include <OndselSolver/ASMTLineInPlaneJoint.h>
#include <OndselSolver/ASMTPlanarJoint.h>
#include <OndselSolver/ASMTRevCylJoint.h>
#include <OndselSolver/ASMTCylSphJoint.h>
#include <OndselSolver/ASMTRackPinionJoint.h>
#include <OndselSolver/ASMTRotationLimit.h>
#include <OndselSolver/ASMTTranslationLimit.h>
#include <OndselSolver/ASMTRotationalMotion.h>
#include <OndselSolver/ASMTTranslationalMotion.h>
#include <OndselSolver/ASMTGeneralMotion.h>
#include <OndselSolver/ASMTScrewJoint.h>
#include <OndselSolver/ASMTSphSphJoint.h>
#include <OndselSolver/ASMTTime.h>
#include <OndselSolver/ASMTConstantGravity.h>
#include <OndselSolver/ExternalSystem.h>
#include <OndselSolver/DiagonalMatrix.h>
#include <OndselSolver/Part.h>
#include <OndselSolver/RedundantConstraint.h>
#include <OndselSolver/enum.h>

#include "AssemblyLink.h"
#include "AssemblyObject.h"
#include "AssemblyObjectPy.h"
#include "AssemblyUtils.h"
#include "Groups.h"
#include "Solver/Backend.h"
#include "Solver/OndselBackend.h"

FC_LOG_LEVEL_INIT("Assembly", true, true, true)

using namespace Assembly;
using namespace MbD;


namespace PartApp = Part;

namespace
{

struct RotationJointSide
{
    App::DocumentObject* joint = nullptr;
    const char* carrierRefName = nullptr;
    const char* carrierPlcName = nullptr;
};

Base::Placement getJointSideGlobalPlacement(
    App::DocumentObject* joint,
    const char* refName,
    const char* plcName
)
{
    if (!joint) {
        return {};
    }

    auto* ref = joint->getPropertyByName<App::PropertyXLinkSub>(refName);
    if (!ref) {
        return {};
    }

    return App::GeoFeature::getGlobalPlacement(nullptr, ref)
        * App::GeoFeature::getPlacementFromProp(joint, plcName);
}

bool axesAreCoaxial(const Base::Placement& plc1, const Base::Placement& plc2)
{
    Base::Vector3d axis1 = plc1.getRotation().multVec(Base::Vector3d(0, 0, 1));
    Base::Vector3d axis2 = plc2.getRotation().multVec(Base::Vector3d(0, 0, 1));
    if (axis1.Length() <= Precision::Confusion() || axis2.Length() <= Precision::Confusion()) {
        return false;
    }
    axis1.Normalize();
    axis2.Normalize();

    if (1.0 - std::abs(axis1 * axis2) > Precision::Confusion()) {
        return false;
    }

    const Base::Vector3d delta = plc2.getPosition() - plc1.getPosition();
    return delta.Cross(axis1).Length() <= Precision::Confusion();
}

bool gearJointNeedsCarrierMarker(App::DocumentObject* joint)
{
    const auto plc1 = getJointSideGlobalPlacement(joint, "Reference1", "Placement1");
    const auto plc2 = getJointSideGlobalPlacement(joint, "Reference2", "Placement2");
    return axesAreCoaxial(plc1, plc2);
}

bool isRotationCarrierJoint(JointType type)
{
    return type == JointType::Revolute || type == JointType::Cylindrical;
}

bool findRotationCarrierForGearSide(
    AssemblyObject* assembly,
    App::DocumentObject* gearJoint,
    const char* gearRefName,
    const char* gearPlcName,
    RotationJointSide& carrierSide
)
{
    auto* gearPart = getMovingPartFromRef(gearJoint, gearRefName);
    if (!gearPart) {
        return false;
    }

    const auto gearPlc = getJointSideGlobalPlacement(gearJoint, gearRefName, gearPlcName);
    bool foundCarrier = false;
    bool ambiguousCarrier = false;

    for (auto* joint : assembly->getJoints(false, true)) {
        if (!joint || joint == gearJoint || !getJointActivated(joint)) {
            continue;
        }
        if (!isRotationCarrierJoint(getJointType(joint))) {
            continue;
        }

        auto matchesSide = [&](const char* movingRefName,
                               const char* movingPlcName,
                               const char* carrierRefName,
                               const char* carrierPlcName) {
            if (getMovingPartFromRef(joint, movingRefName) != gearPart) {
                return false;
            }

            const auto rotationPlc = getJointSideGlobalPlacement(joint, movingRefName, movingPlcName);
            if (!axesAreCoaxial(gearPlc, rotationPlc)) {
                return false;
            }

            if (foundCarrier) {
                ambiguousCarrier = true;
                return false;
            }
            foundCarrier = true;
            carrierSide.joint = joint;
            carrierSide.carrierRefName = carrierRefName;
            carrierSide.carrierPlcName = carrierPlcName;
            return true;
        };

        matchesSide("Reference1", "Placement1", "Reference2", "Placement2");
        matchesSide("Reference2", "Placement2", "Reference1", "Placement1");
    }

    return foundCarrier && !ambiguousCarrier;
}

void setGearJointCarrierMarkerIfAvailable(
    AssemblyObject* assembly,
    App::DocumentObject* joint,
    const std::shared_ptr<Solver::Joint>& mbdJoint
)
{
    auto gearJoint = mbdJoint;
    if (!gearJoint || gearJoint->kind != Solver::JointKind::Gear) {
        return;
    }

    RotationJointSide side1;
    RotationJointSide side2;
    const bool hasCarrier1
        = findRotationCarrierForGearSide(assembly, joint, "Reference1", "Placement1", side1);
    const bool hasCarrier2
        = findRotationCarrierForGearSide(assembly, joint, "Reference2", "Placement2", side2);
    if (!hasCarrier1 || !hasCarrier2) {
        return;
    }

    auto* carrierPart1 = getMovingPartFromRef(side1.joint, side1.carrierRefName);
    auto* carrierPart2 = getMovingPartFromRef(side2.joint, side2.carrierRefName);
    if (!carrierPart1 || !carrierPart2
        || assembly->getMbDPart(carrierPart1) != assembly->getMbDPart(carrierPart2)) {
        return;
    }

    const std::string carrierMarkerName = joint->getFullName() + "-Carrier";
    Solver::Id fullMarkerNameK = assembly->handleOneSideOfJoint(
        side1.joint,
        side1.carrierRefName,
        side1.carrierPlcName,
        carrierMarkerName
    );
    if (fullMarkerNameK != Solver::NoId) {
        gearJoint->kind = Solver::JointKind::CoaxialGear;
        gearJoint->frameK = fullMarkerNameK;
    }
}

// A joint of the solver-neutral model, not yet in it: jointParts() adds the
// ones makeMbdJoint() keeps (where CREATE<ASMT...Joint>::With() was).
std::shared_ptr<Solver::Joint> makeJoint(Solver::JointKind kind)
{
    auto mbdJoint = std::make_shared<Solver::Joint>();
    mbdJoint->kind = kind;
    return mbdJoint;
}

// A redundant equation whose residual is still above this after the solve is a
// conflict: the solver dropped it to get a full-rank system, and the solution
// of the remaining equations does not satisfy it. Residuals are in model units
// (mm, OndselSolver's mbdUnits are all 1; mm^2 for the squared distances of
// DistancexyConstraintIJ) for displacement equations and dimensionless for
// direction cosines; a satisfied equation ends near 1e-12.
constexpr double conflictResidualTolerance = 1.0e-6;

// Re-evaluates the equation OndselSolver set aside as redundant at the solved
// configuration. RedundantConstraint stubs out every update, so the wrapped
// equation's aG is stale until asked here.
bool isConflicting(const std::shared_ptr<MbD::RedundantConstraint>& redundant)
{
    if (!redundant || !redundant->constraint) {
        return false;
    }
    try {
        redundant->constraint->postPosICIteration();
    }
    catch (...) {
        return false;
    }
    const double residual = redundant->constraint->aG;
    return !std::isfinite(residual) || std::abs(residual) > conflictResidualTolerance;
}

void appendUnique(std::vector<std::string>& names, const std::string& name)
{
    if (std::ranges::find(names, name) == names.end()) {
        names.push_back(name);
    }
}

}  // namespace


// ================================ Assembly Object ============================

PROPERTY_SOURCE(Assembly::AssemblyObject, App::Part)

AssemblyObject::AssemblyObject()
    : mbdAssembly(std::make_shared<ASMTAssembly>())
    , bundleFixed(false)
    , lastDoF(0)
    , lastHasConflict(false)
    , lastHasRedundancies(false)
    , lastHasPartialRedundancies(false)
    , lastHasMalformedConstraints(false)
    , lastSolverStatus(0)
{
    mbdAssembly->externalSystem->freecadAssemblyObject = this;

    // The workbench's Python decides "is this container a real assembly?" by
    // testing Type == "Assembly" (six sites in JointObject.py). Upstream stamps
    // it from CommandCreateAssembly.py, so an assembly created any other way --
    // a script, a test, an importer -- is not recognised as one, and joints
    // silently never solve. The type is a property of the class, not of how the
    // object was created, so set it here. Restoring a document still overwrites
    // it with the saved value, so old files are unaffected.
    Type.setValue("Assembly");

    lastDoF = numberOfComponents() * 6;
    signalSolverUpdate();
}

AssemblyObject::~AssemblyObject() = default;

PyObject* AssemblyObject::getPyObject()
{
    if (PythonObject.is(Py::_None())) {
        // ref counter is set to 1
        PythonObject = Py::Object(new AssemblyObjectPy(this), true);
    }
    return Py::new_reference_to(PythonObject);
}

App::DocumentObjectExecReturn* AssemblyObject::execute()
{
    App::DocumentObjectExecReturn* ret = App::Part::execute();

    ParameterGrp::handle hGrp = App::GetApplication().GetParameterGroupByPath(
        "User parameter:BaseApp/Preferences/Mod/Assembly"
    );
    if (hGrp->GetBool("SolveOnRecompute", Assembly::AssemblyParams::defaultSolveOnRecompute())) {
        solve(false);
    }
    return ret;
}

void AssemblyObject::onChanged(const App::Property* prop)
{
    if (prop == &Group) {
        for (auto* obj : getInList()) {
            if (auto* assemblyLink = freecad_cast<AssemblyLink*>(obj)) {
                assemblyLink->updateContents();
            }
        }
        updateSolveStatus();
    }
    App::Part::onChanged(prop);
}

int AssemblyObject::solve(bool enableRedo)
{
    // updateSolveStatus() solves on demand; suppress that while a solve is running.
    Base::StateLocker lock(solveInProgress);

    ensureIdentityPlacements();

    syncGroundedJoints();

    collectMalformedJoints();

    solverModel = makeMbdAssembly();
    objectPartMap.clear();
    rebuildRigidClusters();
    syncActiveRigidGroupPlacements();
    motions.clear();

    auto groundedObjs = fixGroundedParts();
    if (groundedObjs.empty()) {
        // If no part fixed we can't solve.
        return -6;
    }

    std::vector<App::DocumentObject*> joints = getJoints();

    removeUnconnectedJoints(joints, groundedObjs);

    jointParts(joints);

    makeMbdModel();

    if (enableRedo) {
        savePlacementsForUndo();
    }

    try {
        mbdAssembly->runPreDrag();
        lastSolverStatus = 0;
    }
    catch (const std::exception& e) {
        FC_ERR("Solve failed: " << e.what());
        lastSolverStatus = -1;
        updateSolveStatus();
        return -1;
    }
    catch (...) {
        FC_ERR("Solve failed: unhandled exception");
        lastSolverStatus = -1;
        updateSolveStatus();
        return -1;
    }

    setNewPlacements();
    updateRigidPlacementCache();

    redrawJointPlacements(joints);

    updateSolveStatus();

    return 0;
}

void AssemblyObject::updateSolveStatus()
{
    lastRedundantJoints.clear();
    lastHasRedundancies = false;
    lastConflictingJoints.clear();
    lastHasConflict = false;
    lastPartialRedundantJoints.clear();
    lastHasPartialRedundancies = false;
    // Malformed joints are found while the model is built (solve()); a status
    // refresh without a solve keeps the last solve's list.
    lastHasMalformedConstraints = !lastMalformedJoints.empty();

    int numberOfSolverBodies = numberOfComponents();
    if (!objectPartMap.empty()) {
        std::unordered_set<MbD::ASMTPart*> uniqueParts;
        for (const auto& entry : objectPartMap) {
            if (entry.second.part) {
                uniqueParts.insert(entry.second.part.get());
            }
        }

        const int bundledParts = static_cast<int>(objectPartMap.size() - uniqueParts.size());
        numberOfSolverBodies -= bundledParts;
    }

    // +1 because the assembly origin is also represented by a solver body.
    lastDoF = (1 + numberOfSolverBodies) * 6;

    // Solve on demand when queried before the system is solved, but not from within
    // a solve: solve() calls this, so a failed solve would recurse indefinitely.
    if (!solveInProgress && (!mbdAssembly || !mbdAssembly->mbdSystem)) {
        solve();
    }

    if (!mbdAssembly || !mbdAssembly->mbdSystem) {
        return;
    }

    // Helper lambda to clean up the joint name from the solver
    auto cleanJointName = [](const std::string& rawName) -> std::string {
        // rawName is like : /OndselAssembly/ground_moves#Joint001
        size_t hashPos = rawName.find_last_of('#');
        if (hashPos != std::string::npos) {
            // Return the substring after the '#'
            return rawName.substr(hashPos + 1);
        }
        return rawName;
    };


    // Iterate through all joints and motions in the MBD system
    mbdAssembly->mbdSystem->jointsMotionsDo([&](std::shared_ptr<MbD::Joint> jm) {
        if (!jm) {
            return;
        }
        // OndselSolver removes redundancy per equation: during the first
        // assembly solve, rows of the Jacobian found linearly dependent are
        // wrapped in a RedundantConstraint and left out. A joint is one MbD
        // joint with one equation per constraint, so per joint:
        //  - redundant: at least one equation removed (what the list has always
        //    meant, and what the over-constrained message lists);
        //  - partially redundant: some, not all, of its equations removed;
        //  - conflicting: a removed equation is not satisfied at the solution,
        //    i.e. the joint contradicts the others instead of repeating them.
        int nEquations = 0;
        int nRedundant = 0;
        bool isJointConflicting = false;

        jm->constraintsDo([&](std::shared_ptr<MbD::Constraint> con) {
            if (!con) {
                return;
            }
            ++nEquations;
            if (auto redundant = std::dynamic_pointer_cast<MbD::RedundantConstraint>(con)) {
                ++nRedundant;
                if (isConflicting(redundant)) {
                    isJointConflicting = true;
                }
                // A removed equation takes no freedom away: the remaining ones
                // are full rank, so their count is the Jacobian's rank.
                return;
            }
            --lastDoF;
        });

        const std::string fullName = cleanJointName(jm->name);
        App::DocumentObject* docObj = getDocument()->getObject(fullName.c_str());

        // We only care about objects that are actual joints in the FreeCAD document.
        // This effectively filters out the grounding joints, which are named after parts.
        if (!docObj || !docObj->getPropertyByName("Reference1")) {
            return;
        }

        if (nRedundant > 0) {
            const std::string objName = docObj->getNameInDocument();
            appendUnique(lastRedundantJoints, objName);
            if (nRedundant < nEquations) {
                appendUnique(lastPartialRedundantJoints, objName);
            }
            if (isJointConflicting) {
                appendUnique(lastConflictingJoints, objName);
            }
        }
    });

    lastHasRedundancies = !lastRedundantJoints.empty();
    lastHasPartialRedundancies = !lastPartialRedundantJoints.empty();
    lastHasConflict = !lastConflictingJoints.empty();

    signalSolverUpdate();
}

int AssemblyObject::generateSimulation(App::DocumentObject* sim)
{
    solverModel = makeMbdAssembly();
    objectPartMap.clear();

    motions = getMotionsFromSimulation(sim);

    auto groundedObjs = fixGroundedParts();
    if (groundedObjs.empty()) {
        // If no part fixed we can't solve.
        return -6;
    }

    std::vector<App::DocumentObject*> joints = getJoints();

    removeUnconnectedJoints(joints, groundedObjs);

    jointParts(joints);

    create_mbdSimulationParameters(sim);

    makeMbdModel();

    try {
        mbdAssembly->runKINEMATIC();
    }
    catch (...) {
        Base::Console().Error("Generation of simulation failed\n");
        motions.clear();
        return -1;
    }

    motions.clear();

    return 0;
}

std::vector<App::DocumentObject*> AssemblyObject::getMotionsFromSimulation(App::DocumentObject* sim)
{
    if (!sim) {
        return {};
    }

    auto* prop = dynamic_cast<App::PropertyLinkList*>(sim->getPropertyByName("Group"));
    if (!prop) {
        return {};
    }

    return prop->getValue();
}

int Assembly::AssemblyObject::updateForFrame(size_t index)
{
    if (!mbdAssembly) {
        return -1;
    }

    auto nfrms = mbdAssembly->numberOfFrames();
    if (index >= nfrms) {
        return -1;
    }

    mbdAssembly->updateForFrame(index);
    setNewPlacements();
    auto jointDocs = getJoints();
    redrawJointPlacements(jointDocs);
    return 0;
}

size_t Assembly::AssemblyObject::numberOfFrames()
{
    return mbdAssembly->numberOfFrames();
}

bool AssemblyObject::requiresRigidSolveForMove(const std::vector<App::DocumentObject*>& movedParts)
{
    rebuildRigidClusters();

    return std::ranges::any_of(movedParts, [&](App::DocumentObject* part) {
        return getRigidRepresentative(part) != nullptr;
    });
}

void AssemblyObject::preDrag(std::vector<App::DocumentObject*> dragParts)
{
    dragStepCount = 0;
    dragStepFailures = 0;
    lastDragStepFailed = false;

    bundleFixed = true;
    bool hasUnconnectedDragPart = std::ranges::any_of(dragParts, [this](App::DocumentObject* part) {
        return part && !isPartConnected(part);
    });

    if (hasUnconnectedDragPart) {
        prepareMbdForIslandDrag(dragParts);
    }
    else {
        solve();
    }
    bundleFixed = false;

    draggedParts.clear();

    for (auto part : dragParts) {
        const bool isRigidClustered = getRigidRepresentative(part) != nullptr;

        // make sure no duplicate
        if (std::ranges::find(draggedParts, part) != draggedParts.end()) {
            continue;
        }

        // - Free-floating parts should not be added since they are ignored by the solver.
        // During ungrounded island dragging, prepareMbdForIslandDrag seeds the MBD system from
        // the dragged parts, so objectPartMap is the source of truth instead.
        // - Active rigid-cluster members are solver-connected through the shared MbD part.
        if (!isPartConnected(part) && (!objectPartMap.contains(part) || !isRigidClustered)) {
            continue;
        }

        // Rigid-cluster members stay draggable because they share one MbD part.
        if (isRigidClustered) {
            draggedParts.push_back(part);
        }

        Base::Placement plc;
        for (auto& pair : objectPartMap) {
            App::DocumentObject* parti = pair.first;
            if (parti != part) {
                continue;
            }
            plc = pair.second.offsetPlc;
        }
        if (!plc.isIdentity()) {
            // If not identity, then it's a bundled object. Some bundled objects may
            // have identity placement if they have the same position as the main object of
            // the bundle. But they're not going to be a problem.
            continue;
        }

        draggedParts.push_back(part);
    }

    applyDragMasses();
}

void AssemblyObject::applyDragMasses()
{
    // Every solver body is built with mass 1 and unit inertia (makeMbdPart), and
    // that stays so for the static solve above and the one postDrag() runs: in
    // an under-constrained assembly, OndselSolver's mass-weighted minimal motion
    // decides which part moves, so changing those masses would change what a
    // solve does. During the drag steps the GUI's heavier masses for the dragged
    // parts (setObjMasses) are what make the dragged part follow the mouse and
    // the others give way: PosICDragNewtonRaphson's weights come from
    // Part::fillqsuWeights, which scales them by m / max(m) and aJ / max(aJ).
    // So the masses go on the MbD bodies the drag steps solve, after the
    // pre-drag solve built them, and the ASMT model keeps mass 1.
    if (!mbdAssembly || !mbdAssembly->mbdSystem) {
        return;
    }

    for (auto* part : draggedParts) {
        const double mass = getObjMass(part);
        if (mass <= 0.0 || mass == 1.0) {
            continue;
        }
        auto it = objectPartMap.find(part);
        if (it == objectPartMap.end() || !it->second.part) {
            continue;
        }
        auto mbdPart = std::dynamic_pointer_cast<MbD::Part>(it->second.part->mbdObject);
        if (!mbdPart) {
            continue;
        }
        mbdPart->m = mass;
        // The base inertia is unit, so a heavier part is heavier in rotation too.
        mbdPart->aJ = std::make_shared<MbD::DiagonalMatrix<double>>(3, mass);
    }
}

void AssemblyObject::prepareMbdForIslandDrag(std::vector<App::DocumentObject*> dragParts)
{
    ensureIdentityPlacements();
    syncGroundedJoints();

    solverModel = makeMbdAssembly();
    objectPartMap.clear();
    motions.clear();

    auto seededParts = fixGroundedParts();
    for (auto* part : dragParts) {
        if (part) {
            seededParts.insert(part);
        }
    }

    std::vector<App::DocumentObject*> joints = getJoints();
    removeUnconnectedJoints(joints, seededParts);
    if (joints.empty()) {
        objectPartMap.clear();
        mbdAssembly.reset();
        return;
    }

    jointParts(joints);

    makeMbdModel();

    try {
        mbdAssembly->runPreDrag();
    }
    catch (const std::exception& e) {
        FC_ERR("Drag setup failed: " << e.what());
        objectPartMap.clear();
        mbdAssembly.reset();
        return;
    }
    catch (...) {
        FC_ERR("Drag setup failed: unhandled exception");
        objectPartMap.clear();
        mbdAssembly.reset();
        return;
    }
}

bool AssemblyObject::doDragStep()
{
    if (!mbdAssembly || draggedParts.empty()) {
        return false;
    }

    ++dragStepCount;
    lastDragStepFailed = true;  // until the step below gets through
    std::string failure;
    bool restoreAfterFailedDragStep = false;

    try {
        std::vector<std::shared_ptr<MbD::ASMTPart>> dragMbdParts;
        std::unordered_set<ASMTPart*> seenMbdParts;

        for (auto& part : draggedParts) {
            if (!part) {
                continue;
            }

            auto mbdPart = getMbDData(part).part;
            if (!mbdPart) {
                continue;
            }

            if (!seenMbdParts.insert(mbdPart.get()).second) {
                continue;
            }

            dragMbdParts.push_back(mbdPart);

            Base::Placement plc = getPlacementFromProp(part, "Placement");
            if (auto it = objectPartMap.find(part);
                it != objectPartMap.end() && !it->second.offsetPlc.isIdentity()) {
                plc = plc * it->second.offsetPlc.inverse();
            }
            Base::Vector3d pos = plc.getPosition();
            mbdPart->updateMbDFromPosition3D(
                std::make_shared<FullColumn<double>>(ListD {pos.x, pos.y, pos.z})
            );

            Base::Rotation rot = plc.getRotation();
            Base::Matrix4D mat;
            rot.getValue(mat);
            Base::Vector3d r0 = mat.getRow(0);
            Base::Vector3d r1 = mat.getRow(1);
            Base::Vector3d r2 = mat.getRow(2);
            mbdPart->updateMbDFromRotationMatrix(r0.x, r0.y, r0.z, r1.x, r1.y, r1.z, r2.x, r2.y, r2.z);
        }

        // Timing mbdAssembly->runDragStep()
        auto dragPartsVec = std::make_shared<std::vector<std::shared_ptr<ASMTPart>>>(dragMbdParts);
        // OndselSolver retries a failed step at half, a quarter... of the move;
        // when none converges it puts the parts back where the last step left them.
        if (!mbdAssembly->runDragStep(dragPartsVec)) {
            failure = "the solver did not converge";
            restoreAfterFailedDragStep = true;
        }
        // Timing the validation and placement setting
        else if (validateNewPlacements()) {
            setNewPlacements();
            updateRigidPlacementCache();

            auto joints = getJoints();
            for (auto* joint : joints) {
                if (joint->Visibility.getValue()) {
                    // redraw only the moving joint as its quite slow as its python code.
                    redrawJointPlacement(joint);
                }
            }
            lastDragStepFailed = false;
        }
        else {
            // The solver's state is that solution; nothing older to go back to.
            failure = "a grounded part would have moved";
        }
    }
    catch (const std::exception& e) {
        failure = e.what();
        restoreAfterFailedDragStep = true;
    }
    catch (...) {
        failure = "unknown exception";
        restoreAfterFailedDragStep = true;
    }

    if (lastDragStepFailed && restoreAfterFailedDragStep) {
        // The caller wrote the dragged parts' target Placement before calling;
        // put the document back on the last good step so it shows what the
        // solver holds and the next step starts from there.
        try {
            mbdAssembly->restorePosRot();
            setNewPlacements();
        }
        catch (...) {
        }
    }

    if (lastDragStepFailed) {
        // The drag goes on: the parts stay where the last good step put them
        // and the next mouse move tries again. Say so once per drag, not once
        // per mouse move; postDrag() gives the count.
        if (++dragStepFailures == 1) {
            Base::Console().warning(
                "Assembly: a drag step of %s failed (%s); the parts stay where the last "
                "good step left them.\n",
                getFullLabel().c_str(),
                failure.c_str()
            );
        }
        return false;
    }
    return true;
}

Base::Placement AssemblyObject::getMbdPlacement(std::shared_ptr<ASMTPart> mbdPart)
{
    if (!mbdPart) {
        return Base::Placement();
    }

    double x, y, z;
    mbdPart->getPosition3D(x, y, z);
    Base::Vector3d pos = Base::Vector3d(x, y, z);

    double q0, q1, q2, q3;
    mbdPart->getQuarternions(q3, q0, q1, q2);
    Base::Rotation rot = Base::Rotation(q0, q1, q2, q3);

    return Base::Placement(pos, rot);
}

bool AssemblyObject::validateNewPlacements()
{
    // First we check if a grounded object has moved. It can happen that they flip.
    auto groundedParts = getGroundedParts();
    for (auto* obj : groundedParts) {
        auto* propPlacement = obj->getPlacementProperty();
        if (propPlacement) {
            Base::Placement oldPlc = propPlacement->getValue();

            auto it = objectPartMap.find(obj);
            if (it != objectPartMap.end()) {
                std::shared_ptr<MbD::ASMTPart> mbdPart = it->second.part;
                Base::Placement newPlacement = getMbdPlacement(mbdPart);
                if (!it->second.offsetPlc.isIdentity()) {
                    newPlacement = newPlacement * it->second.offsetPlc;
                }

                if (!oldPlc.isSame(newPlacement, Precision::Confusion())) {
                    Base::Console().Warning(
                        "Assembly : Ignoring bad solve, a grounded object (%s) moved.\n",
                        obj->getFullLabel()
                    );
                    return false;
                }
            }
        }
    }

    // TODO: We could do further tests
    // For example check if the joints connectors are correctly aligned.
    return true;
}

void AssemblyObject::postDrag()
{
    if (!mbdAssembly) {
        return;
    }

    if (dragStepFailures > 1) {
        Base::Console().warning(
            "Assembly: %d of %d drag steps of %s failed.\n",
            dragStepFailures,
            dragStepCount,
            getFullLabel().c_str()
        );
    }

    mbdAssembly->runPostDrag();  // Do this after last drag
    purgeTouched();
}

void AssemblyObject::savePlacementsForUndo()
{
    previousPositions.clear();

    for (auto& pair : objectPartMap) {
        App::DocumentObject* obj = pair.first;
        if (!obj) {
            continue;
        }

        std::pair<App::DocumentObject*, Base::Placement> savePair;
        savePair.first = obj;

        // Check if the object has a "Placement" property
        auto* propPlc = obj->getPlacementProperty();
        if (!propPlc) {
            continue;
        }
        savePair.second = propPlc->getValue();

        previousPositions.push_back(savePair);
    }
}

void AssemblyObject::undoSolve()
{
    if (previousPositions.size() == 0) {
        return;
    }

    for (auto& pair : previousPositions) {
        App::DocumentObject* obj = pair.first;
        if (!obj) {
            continue;
        }

        // Check if the object has a "Placement" property
        auto* propPlacement = obj->getPlacementProperty();
        if (!propPlacement) {
            continue;
        }

        propPlacement->setValue(pair.second);
    }
    previousPositions.clear();

    // update joint placements:
    getJoints();
}

void AssemblyObject::clearUndo()
{
    previousPositions.clear();
}

void AssemblyObject::exportAsASMT(std::string fileName)
{
    solverModel = makeMbdAssembly();
    objectPartMap.clear();
    rebuildRigidClusters();
    fixGroundedParts();

    std::vector<App::DocumentObject*> joints = getJoints();

    jointParts(joints);

    makeMbdModel();

    mbdAssembly->outputFile(fileName);
}

void AssemblyObject::rebuildRigidClusters()
{
    rigidRepByPart.clear();
    rigidMembersByRep.clear();

    std::unordered_map<App::DocumentObject*, App::DocumentObject*> parent;

    auto findRoot = [&](App::DocumentObject* node, auto& self) -> App::DocumentObject* {
        auto [it, inserted] = parent.emplace(node, node);
        if (inserted || it->second == node) {
            return it->second;
        }

        it->second = self(it->second, self);
        return it->second;
    };

    auto unite = [&](App::DocumentObject* a, App::DocumentObject* b) {
        if (!a || !b) {
            return;
        }

        auto* const rootA = findRoot(a, findRoot);
        auto* const rootB = findRoot(b, findRoot);

        if (rootA != rootB) {
            parent[rootB] = rootA;
        }
    };

    for (auto* const rigidGroup : getRigidGroups()) {
        if (!rigidGroup) {
            continue;
        }

        auto* const prop = dynamic_cast<App::PropertyLinkList*>(
            rigidGroup->getPropertyByName("ObjectsToRigidGroup")
        );
        if (!prop) {
            continue;
        }

        const auto members = prop->getValues();
        if (members.size() < 2) {
            continue;
        }

        auto* const first = members.front();
        for (auto* const member : members | std::views::drop(1)) {
            unite(first, member);
        }
    }

    std::unordered_map<App::DocumentObject*, std::vector<App::DocumentObject*>> clusters;

    for (const auto& [node, _] : parent) {
        boost::ignore_unused(_);
        const auto root = findRoot(node, findRoot);
        clusters[root].push_back(node);
    }

    for (auto& [_, members] : clusters) {
        boost::ignore_unused(_);

        if (members.size() < 2) {
            continue;
        }

        const auto repIt = std::ranges::min_element(members, {}, [](App::DocumentObject* obj) {
            return obj ? std::string(obj->getNameInDocument()) : std::string();
        });

        if (repIt == members.end() || !*repIt) {
            continue;
        }

        auto* const rep = *repIt;

        for (auto* const member : members) {
            rigidRepByPart[member] = rep;
        }

        rigidMembersByRep[rep] = std::move(members);
    }
}

App::DocumentObject* AssemblyObject::getRigidRepresentative(App::DocumentObject* part) const
{
    if (!part) {
        return nullptr;
    }

    if (auto it = rigidRepByPart.find(part); it != rigidRepByPart.end()) {
        return it->second;
    }

    return nullptr;
}

const std::vector<App::DocumentObject*>* AssemblyObject::getRigidMembers(App::DocumentObject* part) const
{
    if (auto* rep = getRigidRepresentative(part); rep) {
        if (auto it = rigidMembersByRep.find(rep); it != rigidMembersByRep.end()) {
            return &it->second;
        }
    }

    return nullptr;
}

void AssemblyObject::syncActiveRigidGroupPlacements()
{
    for (const auto& [rep, members] : rigidMembersByRep) {
        if (!rep || members.size() < 2) {
            continue;
        }

        bool hasCompleteCache = true;
        std::vector<App::DocumentObject*> movedMembers;

        for (auto* member : members) {
            if (!member) {
                hasCompleteCache = false;
                break;
            }

            auto cacheIt = rigidPlacementCache.find(member);
            if (cacheIt == rigidPlacementCache.end()) {
                hasCompleteCache = false;
                break;
            }

            Base::Placement currentPlc = getPlacementFromProp(member, "Placement");
            if (!cacheIt->second.isSame(currentPlc)) {
                movedMembers.push_back(member);
            }
        }

        if (!hasCompleteCache) {
            for (auto* member : members) {
                if (!member) {
                    continue;
                }
                rigidPlacementCache[member] = getPlacementFromProp(member, "Placement");
            }
            continue;
        }

        if (movedMembers.empty()) {
            continue;
        }

        App::DocumentObject* driver = movedMembers.front();
        const Base::Placement oldDriverPlc = rigidPlacementCache.at(driver);
        const Base::Placement newDriverPlc = getPlacementFromProp(driver, "Placement");
        const Base::Placement delta = newDriverPlc * oldDriverPlc.inverse();

        for (auto* member : members) {
            if (!member || member == driver) {
                continue;
            }

            if (auto cacheIt = rigidPlacementCache.find(member);
                cacheIt != rigidPlacementCache.end()) {
                Base::Placement targetPlc = delta * cacheIt->second;
                auto* propPlacement = member->getPlacementProperty();
                if (propPlacement && !propPlacement->getValue().isSame(targetPlc)) {
                    propPlacement->setValue(targetPlc);
                    member->purgeTouched();
                }
            }
        }

        for (auto* member : members) {
            if (!member) {
                continue;
            }
            rigidPlacementCache[member] = getPlacementFromProp(member, "Placement");
        }
    }
}

void AssemblyObject::updateRigidPlacementCache()
{
    std::unordered_set<App::DocumentObject*> activeMembers;

    for (const auto& [rep, members] : rigidMembersByRep) {
        boost::ignore_unused(rep);
        for (auto* member : members) {
            if (!member) {
                continue;
            }
            activeMembers.insert(member);
            rigidPlacementCache[member] = getPlacementFromProp(member, "Placement");
        }
    }

    std::erase_if(rigidPlacementCache, [&](const auto& entry) {
        return !activeMembers.contains(entry.first);
    });
}

namespace
{
// A singular solve can return NaN or infinite placements. NaN coordinates defeat
// the bounding-box rejection in SoRayPickAction, so writing one into the document
// makes every ray pick hit everything; reject them at the solver/document boundary.
bool isFinitePlacement(const Base::Placement& plc)
{
    const Base::Vector3d& pos = plc.getPosition();
    double q0 {};
    double q1 {};
    double q2 {};
    double q3 {};
    plc.getRotation().getValue(q0, q1, q2, q3);
    return std::isfinite(pos.x) && std::isfinite(pos.y) && std::isfinite(pos.z) && std::isfinite(q0)
        && std::isfinite(q1) && std::isfinite(q2) && std::isfinite(q3);
}
}  // namespace

void AssemblyObject::setNewPlacements()
{
    for (auto& pair : objectPartMap) {
        App::DocumentObject* obj = pair.first;
        std::shared_ptr<ASMTPart> mbdPart = pair.second.part;

        if (!obj || !mbdPart) {
            continue;
        }

        // Check if the object has a "Placement" property
        auto* propPlacement = obj->getPlacementProperty();
        if (!propPlacement) {
            continue;
        }


        Base::Placement newPlacement = getMbdPlacement(mbdPart);
        if (!pair.second.offsetPlc.isIdentity()) {
            newPlacement = newPlacement * pair.second.offsetPlc;
        }
        if (!isFinitePlacement(newPlacement)) {
            Base::Console().warning(
                "Assembly: solver returned a non-finite placement for '%s'; keeping its "
                "previous position.\n",
                obj->getFullName().c_str()
            );
            continue;
        }
        if (!propPlacement->getValue().isSame(newPlacement)) {
            propPlacement->setValue(newPlacement);
            obj->purgeTouched();
        }
    }
}

void AssemblyObject::redrawJointPlacements(std::vector<App::DocumentObject*> joints)
{
    // Notify the joint objects that the transform of the coin object changed.
    for (auto* joint : joints) {
        if (!joint) {
            continue;
        }
        redrawJointPlacement(joint);
    }
}

void AssemblyObject::redrawJointPlacement(App::DocumentObject* joint)
{
    if (!joint) {
        return;
    }

    Base::PyGILStateLocker lock;

    try {
        auto* proxy = dynamic_cast<App::PropertyPythonObject*>(joint->getPropertyByName("Proxy"));

        if (!proxy) {
            return;
        }

        Py::Object jointPy = proxy->getValue();

        if (!jointPy.hasAttr("redrawJointPlacements")) {
            return;
        }

        Py::Object attr = jointPy.getAttr("redrawJointPlacements");
        if (attr.ptr() && attr.isCallable()) {
            Py::Tuple args(1);
            args.setItem(0, Py::asObject(joint->getPyObject()));
            Py::Callable(attr).apply(args);
        }
    }
    catch (Py::Exception&) {
        // Callers run inside Qt event handlers, which cannot propagate C++ exceptions
        // out of the joint's Python callback. Report the error and keep redrawing the
        // remaining joints.
        Base::PyException e;
        e.reportException();
    }
}

std::unique_ptr<Solver::Model> AssemblyObject::makeMbdAssembly()
{
    auto assembly = std::make_unique<Solver::Model>();
    simulationSpec.reset();

    // Status, drag and playback still read the ASMT model (until step 4 of
    // sec 6.3): a build starts it empty, as it always did, until
    // makeMbdModel() hands it the model built.
    mbdAssembly = Solver::Ondsel::toAsmt(*assembly).assembly;
    mbdAssembly->externalSystem->freecadAssemblyObject = this;
    return assembly;
}

void AssemblyObject::makeMbdModel()
{
    ParameterGrp::handle hPgr = App::GetApplication().GetParameterGroupByPath(
        "User parameter:BaseApp/Preferences/Mod/Assembly"
    );

    auto asmt = Solver::Ondsel::toAsmt(
        *solverModel,
        simulationSpec.get(),
        hPgr->GetBool("LogSolverDebug", Assembly::AssemblyParams::defaultLogSolverDebug())
    );
    mbdAssembly = asmt.assembly;
    mbdAssembly->externalSystem->freecadAssemblyObject = this;
    for (auto& entry : objectPartMap) {
        MbDPartData& data = entry.second;
        data.part = data.body < asmt.parts.size() ? asmt.parts[data.body] : nullptr;
    }

    // Every model handed to the solver, as ASMT text: the identity gate's
    // view of what a solve, a drag or a simulation runs on.
    std::string dumpFile = hPgr->GetASCII("SolverDumpFile", "");
    if (!dumpFile.empty()) {
        mbdAssembly->outputFile(dumpFile);
    }
}

App::DocumentObject* AssemblyObject::getJointOfPartConnectingToGround(
    App::DocumentObject* part,
    std::string& name,
    const std::vector<App::DocumentObject*>& excludeJoints
)
{
    if (!part) {
        return nullptr;
    }

    std::vector<App::DocumentObject*> joints = getJointsOfPart(part);

    for (auto joint : joints) {
        if (!joint) {
            continue;
        }

        if (std::ranges::find(excludeJoints, joint) != excludeJoints.end()) {
            continue;
        }

        App::DocumentObject* part1 = getMovingPartFromRef(joint, "Reference1");
        App::DocumentObject* part2 = getMovingPartFromRef(joint, "Reference2");
        if (!part1 || !part2) {
            continue;
        }

        if (part == part1 && isJointConnectingPartToGround(joint, "Reference1")) {
            name = "Reference1";
            return joint;
        }
        if (part == part2 && isJointConnectingPartToGround(joint, "Reference2")) {
            name = "Reference2";
            return joint;
        }
    }
    return nullptr;
}

App::DocumentObject* AssemblyObject::getJointOfPartForUngroundedDrag(
    App::DocumentObject* part,
    std::string& name
)
{
    if (!part) {
        return nullptr;
    }

    std::vector<App::DocumentObject*> joints = getJointsOfPart(part);
    for (auto* joint : joints) {
        if (!joint || !isJointTypeConnecting(joint)) {
            continue;
        }
        if (getJointType(joint) == JointType::Fixed) {
            continue;
        }

        App::DocumentObject* part1 = getMovingPartFromRef(joint, "Reference1");
        App::DocumentObject* part2 = getMovingPartFromRef(joint, "Reference2");
        if (!part1 || !part2) {
            continue;
        }

        std::string refName;
        if (part == part1) {
            refName = "Reference1";
        }
        else if (part == part2) {
            refName = "Reference2";
        }
        else {
            continue;
        }

        name = refName;
        return joint;
    }

    return nullptr;
}

template<typename T>
T* AssemblyObject::getGroup()
{
    App::Document* doc = getDocument();

    std::vector<DocumentObject*> groups = doc->getObjectsOfType(T::getClassTypeId());
    if (groups.empty()) {
        return nullptr;
    }
    for (auto group : groups) {
        if (hasObject(group)) {
            return freecad_cast<T*>(group);
        }
    }
    return nullptr;
}

JointGroup* AssemblyObject::getJointGroup() const
{
    return Assembly::getJointGroup(this);
}

ViewGroup* AssemblyObject::getExplodedViewGroup() const
{
    App::Document* doc = getDocument();

    std::vector<DocumentObject*> viewGroups = doc->getObjectsOfType(ViewGroup::getClassTypeId());
    if (viewGroups.empty()) {
        return nullptr;
    }
    for (auto viewGroup : viewGroups) {
        if (hasObject(viewGroup)) {
            return freecad_cast<ViewGroup*>(viewGroup);
        }
    }
    return nullptr;
}

std::vector<App::DocumentObject*> AssemblyObject::getJoints(bool delBadJoints, bool subJoints)
{
    std::vector<App::DocumentObject*> joints = {};

    JointGroup* jointGroup = getJointGroup();
    if (!jointGroup) {
        return {};
    }

    Base::PyGILStateLocker lock;
    for (auto joint : jointGroup->getObjects()) {
        if (!joint) {
            continue;
        }

        auto* prop = dynamic_cast<App::PropertyBool*>(joint->getPropertyByName("Suppressed"));
        if (joint->isError() || !prop || prop->getValue()) {
            // Filter grounded joints and deactivated joints.
            continue;
        }

        auto* part1 = getMovingPartFromRef(joint, "Reference1");
        auto* part2 = getMovingPartFromRef(joint, "Reference2");
        if (!part1 || !part2 || part1->getFullName() == part2->getFullName()) {
            // Remove incomplete joints. Left-over when the user deletes a part.
            // Remove incoherent joints (self-pointing joints)
            if (delBadJoints) {
                getDocument()->removeObject(joint->getNameInDocument());
            }
            continue;
        }

        auto proxy = dynamic_cast<App::PropertyPythonObject*>(joint->getPropertyByName("Proxy"));
        if (proxy) {
            if (proxy->getValue().hasAttr("setJointConnectors")) {
                joints.push_back(joint);
            }
        }
    }

    // add sub assemblies joints.
    if (subJoints) {
        for (auto& assembly : getSubAssemblies()) {
            auto subJoints = assembly->getJoints();
            joints.insert(joints.end(), subJoints.begin(), subJoints.end());
        }
    }

    return joints;
}

void AssemblyObject::collectMalformedJoints()
{
    // getJoints() drops these silently, and handleOneSideOfJoint() only warns,
    // so the solve goes on without them and nothing told the user. An
    // incomplete joint -- a reference not set yet, as while the joint task is
    // open -- is not malformed: it is skipped, as before. Malformed is a joint
    // whose references are set but cannot be used: both on the same part, or a
    // sub-element that no longer resolves.
    lastMalformedJoints.clear();

    JointGroup* jointGroup = getJointGroup();
    if (!jointGroup) {
        return;
    }

    Base::PyGILStateLocker lock;
    for (auto* joint : jointGroup->getObjects()) {
        if (!joint || !joint->getPropertyByName("Reference1")) {
            continue;
        }
        auto* suppressed = dynamic_cast<App::PropertyBool*>(joint->getPropertyByName("Suppressed"));
        if (!suppressed || suppressed->getValue()) {
            continue;
        }
        auto* proxy = dynamic_cast<App::PropertyPythonObject*>(joint->getPropertyByName("Proxy"));
        if (!proxy || !proxy->getValue().hasAttr("setJointConnectors")) {
            continue;
        }

        auto* part1 = getMovingPartFromRef(joint, "Reference1");
        auto* part2 = getMovingPartFromRef(joint, "Reference2");
        if (!part1 || !part2) {
            continue;  // incomplete
        }

        if (part1 == part2 || !isJointRefUsable(joint, "Reference1")
            || !isJointRefUsable(joint, "Reference2")) {
            appendUnique(lastMalformedJoints, joint->getNameInDocument());
        }
    }
}

bool AssemblyObject::isJointRefUsable(App::DocumentObject* joint, const char* propName)
{
    if (!getObjFromJointRef(joint, propName)) {
        return false;
    }

    // The element the reference names must exist: after a topology change a
    // joint keeps its last placement while pointing at a face that is gone.
    const auto* prop = joint->getPropertyByName<App::PropertyXLinkSub>(propName);
    const auto& subs = prop->getSubValues();
    const char* element = Data::findElementName(subs.front().c_str());
    if (!element || !*element) {
        return true;  // a whole object (datum, LCS): nothing more to resolve
    }
    try {
        return !PartApp::Feature::getShape(prop->getValue(), subs.front().c_str(), true).IsNull();
    }
    catch (...) {
        return false;
    }
}

std::vector<App::DocumentObject*> AssemblyObject::getGroundedJoints()
{
    std::vector<App::DocumentObject*> joints = {};

    JointGroup* jointGroup = getJointGroup();
    if (!jointGroup) {
        return {};
    }

    Base::PyGILStateLocker lock;
    for (auto obj : jointGroup->getObjects()) {
        if (!obj) {
            continue;
        }

        auto* propObj = dynamic_cast<App::PropertyLink*>(obj->getPropertyByName("ObjectToGround"));

        if (propObj) {
            joints.push_back(obj);
        }
    }

    return joints;
}

std::vector<App::DocumentObject*> AssemblyObject::getRigidGroups()
{
    std::vector<App::DocumentObject*> rigid_groups {};

    JointGroup* jointGroup = getJointGroup();
    if (!jointGroup) {
        return {};
    }

    Base::PyGILStateLocker lock;
    for (auto const obj : jointGroup->getObjects()) {
        if (!obj || obj->isError()) {
            continue;
        }

        if (auto* prop = dynamic_cast<App::PropertyBool*>(obj->getPropertyByName("Suppressed"));
            prop == nullptr || prop->getValue()) {
            continue;
        }

        if (auto* prop
            = dynamic_cast<App::PropertyLinkList*>(obj->getPropertyByName("ObjectsToRigidGroup"));
            prop) {
            const std::vector<App::DocumentObject*> rawMembers = prop->getValues();
            std::vector<App::DocumentObject*> validMembers;
            validMembers.reserve(rawMembers.size());
            std::unordered_set<App::DocumentObject*> seen;

            for (auto* const member : rawMembers) {
                if (!member || member->isError()) {
                    continue;
                }

                // Keep only parts that belong to this assembly and have placement.
                if (!hasObject(member) || member->getPropertyByName("Placement") == nullptr) {
                    continue;
                }

                // Ignore duplicates.
                if (!seen.insert(member).second) {
                    continue;
                }

                validMembers.push_back(member);
            }

            // Ignore entire rigid group if it has less than 2 members remaining.
            if (validMembers.size() < 2) {
                continue;
            }

            if (validMembers.size() != rawMembers.size()) {
                prop->setValue(validMembers);
            }

            rigid_groups.emplace_back(obj);
        }
    }

    return rigid_groups;
}

std::vector<App::DocumentObject*> AssemblyObject::getJointsOfObj(App::DocumentObject* obj)
{
    if (!obj) {
        return {};
    }

    std::vector<App::DocumentObject*> joints = getJoints();
    std::vector<App::DocumentObject*> jointsOf;

    for (auto joint : joints) {
        App::DocumentObject* obj1 = getObjFromJointRef(joint, "Reference1");
        App::DocumentObject* obj2 = getObjFromJointRef(joint, "Reference2");
        if (obj == obj1 || obj == obj2) {
            jointsOf.push_back(joint);
        }
    }

    return jointsOf;
}

std::vector<App::DocumentObject*> AssemblyObject::getJointsOfPart(App::DocumentObject* part)
{
    if (!part) {
        return {};
    }

    std::vector<App::DocumentObject*> joints = getJoints();
    std::vector<App::DocumentObject*> jointsOf;

    for (auto joint : joints) {
        App::DocumentObject* part1 = getMovingPartFromRef(joint, "Reference1");
        App::DocumentObject* part2 = getMovingPartFromRef(joint, "Reference2");
        if (part == part1 || part == part2) {
            jointsOf.push_back(joint);
        }
    }
    return jointsOf;
}

std::unordered_set<App::DocumentObject*> AssemblyObject::getGroundedParts()
{
    std::unordered_set<App::DocumentObject*> groundedSet;
    std::vector<App::DocumentObject*> allParts = getAssemblyComponents(this);
    for (auto part : allParts) {
        if (part) {
            auto propPlc = part->getPlacementProperty();
            if (propPlc && propPlc->isReadOnly()) {
                groundedSet.insert(part);
            }
        }
    }

    // We also need to add all the root-level datums objects that are not attached.
    std::vector<App::DocumentObject*> objs = Group.getValues();
    for (auto* obj : objs) {
        if (obj->isDerivedFrom<App::LocalCoordinateSystem>()
            || obj->isDerivedFrom<App::DatumElement>()
            || obj->isDerivedFrom<PartApp::Datum>()) {
            // This fork's datums (Part::Datum) carry AttachExtension; App's own
            // coordinate systems and datums do not, so the lookup must not throw.
            auto* pcAttach = obj->getExtensionByType<PartApp::AttachExtension>(true);
            if (pcAttach) {
                // If it's a Part datums, we check if it's attached. If yes then we ignore it.
                std::string mode = pcAttach->MapMode.getValueAsString();
                if (mode != "Deactivated") {
                    continue;
                }
            }
            groundedSet.insert(obj);
        }
    }

    // Origin is not in Group so we add it separately
    groundedSet.insert(Origin.getValue());

    // Propagate grounding through active rigid clusters.
    std::vector<App::DocumentObject*> groundedSnapshot(groundedSet.begin(), groundedSet.end());
    for (auto* groundedObj : groundedSnapshot) {
        if (!groundedObj) {
            continue;
        }

        if (const auto* members = getRigidMembers(groundedObj)) {
            for (auto* member : *members) {
                if (member) {
                    groundedSet.insert(member);
                }
            }
        }
    }

    return groundedSet;
}

std::unordered_set<App::DocumentObject*> AssemblyObject::fixGroundedParts()
{
    auto groundedParts = getGroundedParts();

    for (auto obj : groundedParts) {
        if (!obj) {
            continue;
        }

        Base::Placement plc = getPlacementFromProp(obj, "Placement");
        std::string str = obj->getFullName();
        fixGroundedPart(obj, plc, str);
    }
    return groundedParts;
}

void AssemblyObject::fixGroundedPart(App::DocumentObject* obj, Base::Placement& plc, std::string& name)
{
    if (!obj) {
        return;
    }

    std::string markerName1 = "marker-" + obj->getFullName();
    auto mbdMarker1 = makeMbdMarker(markerName1, plc);
    Solver::Id markerId1 = solverModel->addFrame(mbdMarker1);  // on the ground

    Solver::Id mbdPart = getMbDPart(obj);

    std::string markerName2 = "FixingMarker";
    Base::Placement basePlc = Base::Placement();
    auto mbdMarker2 = makeMbdMarker(markerName2, basePlc);
    mbdMarker2.body = mbdPart;
    Solver::Id markerId2 = solverModel->addFrame(mbdMarker2);

    solverModel->bodies[mbdPart].grounded = true;

    Solver::Ground mbdJoint;
    mbdJoint.source = solverModel->addSource(name);
    mbdJoint.frameI = markerId1;
    mbdJoint.frameJ = markerId2;

    solverModel->addGround(mbdJoint);
}

bool AssemblyObject::isJointConnectingPartToGround(App::DocumentObject* joint, const char* propname)
{
    if (!joint || !isJointTypeConnecting(joint)) {
        return false;
    }

    App::DocumentObject* part = getMovingPartFromRef(joint, propname);
    if (!part) {
        return false;
    }

    // Check if the part is grounded.
    bool isGrounded = isPartGrounded(part);
    if (isGrounded) {
        return false;
    }

    // Check if the part is disconnected even with the joint
    bool isConnected = isPartConnected(part);
    if (!isConnected) {
        return false;
    }

    // to know if a joint is connecting to ground we disable all the other joints
    std::vector<App::DocumentObject*> jointsOfPart = getJointsOfPart(part);
    std::vector<bool> activatedStates;

    for (auto jointi : jointsOfPart) {
        if (jointi->getFullName() == joint->getFullName()) {
            continue;
        }

        activatedStates.push_back(getJointActivated(jointi));
        setJointActivated(jointi, false);
    }

    isConnected = isPartConnected(part);

    // restore activation states
    for (auto jointi : jointsOfPart) {
        if (jointi->getFullName() == joint->getFullName() || activatedStates.empty()) {
            continue;
        }

        setJointActivated(jointi, activatedStates[0]);
        activatedStates.erase(activatedStates.begin());
    }

    return isConnected;
}

bool AssemblyObject::isJointTypeConnecting(App::DocumentObject* joint)
{
    if (!joint) {
        return false;
    }

    JointType jointType = getJointType(joint);
    return jointType != JointType::RackPinion && jointType != JointType::Screw
        && jointType != JointType::Gears && jointType != JointType::Belt;
}


bool AssemblyObject::isObjInSetOfObjRefs(App::DocumentObject* obj, const std::vector<ObjRef>& set)
{
    if (!obj) {
        return false;
    }

    for (const auto& pair : set) {
        if (pair.obj == obj) {
            return true;
        }
    }
    return false;
}

void AssemblyObject::removeUnconnectedJoints(
    std::vector<App::DocumentObject*>& joints,
    std::unordered_set<App::DocumentObject*> groundedObjs
)
{
    std::vector<ObjRef> connectedParts;

    // Initialize connectedParts with groundedObjs
    for (auto* groundedObj : groundedObjs) {
        connectedParts.push_back({groundedObj, nullptr});
    }

    // Perform a traversal from each grounded object
    for (auto* groundedObj : groundedObjs) {
        traverseAndMarkConnectedParts(groundedObj, connectedParts, joints);
    }

    // Filter out unconnected joints
    joints.erase(
        std::remove_if(
            joints.begin(),
            joints.end(),
            [&](App::DocumentObject* joint) {
                App::DocumentObject* obj1 = getMovingPartFromRef(joint, "Reference1");
                App::DocumentObject* obj2 = getMovingPartFromRef(joint, "Reference2");
                return (
                    !isObjInSetOfObjRefs(obj1, connectedParts)
                    || !isObjInSetOfObjRefs(obj2, connectedParts)
                );
            }
        ),
        joints.end()
    );
}

void AssemblyObject::traverseAndMarkConnectedParts(
    App::DocumentObject* currentObj,
    std::vector<ObjRef>& connectedParts,
    const std::vector<App::DocumentObject*>& joints
)
{
    // getConnectedParts returns the objs connected to the currentObj by any joint
    auto connectedObjs = getConnectedParts(currentObj, joints);
    for (auto& nextObjRef : connectedObjs) {
        if (!isObjInSetOfObjRefs(nextObjRef.obj, connectedParts)) {
            // Create a new ObjRef with the nextObj and a nullptr for PropertyXLinkSub*
            connectedParts.push_back(nextObjRef);
            traverseAndMarkConnectedParts(nextObjRef.obj, connectedParts, joints);
        }
    }
}

std::vector<ObjRef> AssemblyObject::getConnectedParts(
    App::DocumentObject* part,
    const std::vector<App::DocumentObject*>& joints
)
{
    if (!part) {
        return {};
    }

    std::vector<ObjRef> connectedParts;

    for (auto joint : joints) {
        if (!isJointTypeConnecting(joint)) {
            continue;
        }

        App::DocumentObject* obj1 = getMovingPartFromRef(joint, "Reference1");
        App::DocumentObject* obj2 = getMovingPartFromRef(joint, "Reference2");

        if (!obj1 || !obj2) {
            continue;
        }

        if (obj1 == part) {
            auto* ref = dynamic_cast<App::PropertyXLinkSub*>(joint->getPropertyByName("Reference2"));
            if (!ref) {
                continue;
            }
            connectedParts.push_back({obj2, ref});
        }
        else if (obj2 == part) {
            auto* ref = dynamic_cast<App::PropertyXLinkSub*>(joint->getPropertyByName("Reference1"));
            if (!ref) {
                continue;
            }
            connectedParts.push_back({obj1, ref});
        }
    }

    // Add rigid-cluster neighbors as fixed-like connectivity edges.
    if (const auto* members = getRigidMembers(part)) {
        for (auto* member : *members) {
            if (!member || member == part || isObjInSetOfObjRefs(member, connectedParts)) {
                continue;
            }
            connectedParts.push_back({member, nullptr});
        }
    }

    return connectedParts;
}

bool AssemblyObject::isPartGrounded(App::DocumentObject* obj)
{
    if (!obj) {
        return false;
    }

    auto groundedObjs = getGroundedParts();

    for (auto* groundedObj : groundedObjs) {
        if (groundedObj->getFullName() == obj->getFullName()) {
            return true;
        }
    }

    return false;
}

bool AssemblyObject::isPartConnected(App::DocumentObject* obj)
{
    if (!obj) {
        return false;
    }

    auto groundedObjs = getGroundedParts();
    std::vector<App::DocumentObject*> joints = getJoints();

    std::vector<ObjRef> connectedParts;

    // Initialize connectedParts with groundedObjs
    for (auto* groundedObj : groundedObjs) {
        connectedParts.push_back({groundedObj, nullptr});
    }

    // Perform a traversal from each grounded object
    for (auto* groundedObj : groundedObjs) {
        traverseAndMarkConnectedParts(groundedObj, connectedParts, joints);
    }

    for (auto& objRef : connectedParts) {
        if (obj == objRef.obj) {
            return true;
        }
    }

    return false;
}

void AssemblyObject::jointParts(std::vector<App::DocumentObject*> joints)
{
    for (auto* joint : joints) {
        if (!joint) {
            continue;
        }

        std::vector<std::shared_ptr<Solver::Joint>> mbdJoints = makeMbdJoint(joint);
        for (auto& mbdJoint : mbdJoints) {
            solverModel->addJoint(*mbdJoint);
        }
    }
}

void Assembly::AssemblyObject::create_mbdSimulationParameters(App::DocumentObject* sim)
{
    auto mbdSim = std::make_unique<Solver::SimulationSpec>();
    if (!sim) {
        return;
    }
    auto valueOf = [](DocumentObject* docObj, const char* propName) {
        auto* prop = dynamic_cast<App::PropertyFloat*>(docObj->getPropertyByName(propName));
        if (!prop) {
            return 0.0;
        }
        return prop->getValue();
    };
    mbdSim->tStart = valueOf(sim, "aTimeStart");
    mbdSim->tEnd = valueOf(sim, "bTimeEnd");
    mbdSim->tOutput = valueOf(sim, "cTimeStepOutput");
    mbdSim->hMin = 1.0e-9;
    mbdSim->hMax = 1.0;
    mbdSim->errorTolerance = valueOf(sim, "fGlobalErrorTolerance");
    simulationSpec = std::move(mbdSim);
}

std::shared_ptr<Solver::Joint> AssemblyObject::makeMbdJointOfType(App::DocumentObject* joint, JointType type)
{
    switch (type) {
        case JointType::Fixed:
            if (bundleFixed) {
                return nullptr;
            }
            return makeJoint(Solver::JointKind::Fixed);

        case JointType::Revolute:
            return makeJoint(Solver::JointKind::Revolute);

        case JointType::Cylindrical:
            return makeJoint(Solver::JointKind::Cylindrical);

        case JointType::Slider:
            return makeJoint(Solver::JointKind::Prismatic);

        case JointType::Ball:
            return makeJoint(Solver::JointKind::Spherical);

        case JointType::Distance:
            return makeMbdJointDistance(joint);

        case JointType::Parallel:
            return makeJoint(Solver::JointKind::ParallelZ);

        case JointType::Perpendicular:
            return makeJoint(Solver::JointKind::PerpendicularZ);

        case JointType::Angle: {
            double angle = fabs(Base::toRadians(getJointAngle(joint)));
            if (fmod(angle, 2 * std::numbers::pi) < Precision::Confusion()) {
                return makeJoint(Solver::JointKind::ParallelZ);
            }
            auto mbdJoint = makeJoint(Solver::JointKind::AngleZ);
            mbdJoint->value = angle;
            return mbdJoint;
        }

        case JointType::RackPinion: {
            auto mbdJoint = makeJoint(Solver::JointKind::RackPinion);
            mbdJoint->value = getJointDistance(joint);
            return mbdJoint;
        }

        case JointType::Screw: {
            int slidingIndex = slidingPartIndex(joint);
            if (slidingIndex == 0) {  // invalid this joint needs a slider
                return nullptr;
            }

            if (slidingIndex != 1) {
                swapJCS(joint);  // make sure that sliding is first.
            }

            auto mbdJoint = makeJoint(Solver::JointKind::Screw);
            mbdJoint->value = getJointDistance(joint);
            return mbdJoint;
        }

        case JointType::Gears: {
            auto mbdJoint = makeJoint(Solver::JointKind::Gear);
            mbdJoint->value = getJointDistance(joint);
            mbdJoint->value2 = getJointDistance2(joint);
            return mbdJoint;
        }

        case JointType::Belt: {
            auto mbdJoint = makeJoint(Solver::JointKind::Gear);
            mbdJoint->value = getJointDistance(joint);
            mbdJoint->value2 = -getJointDistance2(joint);
            return mbdJoint;
        }

        default:
            return nullptr;
    }
}

std::shared_ptr<Solver::Joint> AssemblyObject::makeMbdJointDistance(App::DocumentObject* joint)
{
    DistanceType type = getDistanceType(joint);

    std::string elt1 = getElementFromProp(joint, "Reference1");
    std::string elt2 = getElementFromProp(joint, "Reference2");
    auto* obj1 = getLinkedObjFromRef(joint, "Reference1");
    auto* obj2 = getLinkedObjFromRef(joint, "Reference2");

    switch (type) {
        case DistanceType::PointPoint: {
            // Point to point distance, or ball joint if distance=0.
            double distance = getJointDistance(joint);
            if (distance < Precision::Confusion()) {
                return makeJoint(Solver::JointKind::Spherical);
            }
            auto mbdJoint = makeJoint(Solver::JointKind::PointPointDistance);
            mbdJoint->value = distance;
            return mbdJoint;
        }

        // Edge - edge cases
        case DistanceType::LineLine: {
            auto mbdJoint = makeJoint(Solver::JointKind::LineLineDistance);
            mbdJoint->value = getJointDistance(joint);
            return mbdJoint;
        }

        case DistanceType::LineCircle: {
            auto mbdJoint = makeJoint(Solver::JointKind::LineLineDistance);
            mbdJoint->value = getJointDistance(joint) + getEdgeRadius(obj2, elt2);
            return mbdJoint;
        }

        case DistanceType::CircleCircle: {
            auto mbdJoint = makeJoint(Solver::JointKind::LineLineDistance);
            mbdJoint->value = getJointDistance(joint) + getEdgeRadius(obj1, elt1)
                + getEdgeRadius(obj2, elt2);
            return mbdJoint;
        }

        // Face - Face cases
        case DistanceType::PlanePlane: {
            auto mbdJoint = makeJoint(Solver::JointKind::Planar);
            mbdJoint->value = getJointDistance(joint);
            return mbdJoint;
        }

        case DistanceType::PlaneCylinder: {
            auto mbdJoint = makeJoint(Solver::JointKind::LineInPlane);
            mbdJoint->value = getJointDistance(joint) + getFaceRadius(obj2, elt2);
            return mbdJoint;
        }

        case DistanceType::PlaneSphere: {
            auto mbdJoint = makeJoint(Solver::JointKind::PointInPlane);
            mbdJoint->value = getJointDistance(joint) + getFaceRadius(obj2, elt2);
            return mbdJoint;
        }

        case DistanceType::PlaneTorus: {
            auto mbdJoint = makeJoint(Solver::JointKind::Planar);
            mbdJoint->value = getJointDistance(joint);
            return mbdJoint;
        }

        case DistanceType::CylinderCylinder: {
            auto mbdJoint = makeJoint(Solver::JointKind::LineLineDistance);
            mbdJoint->value = getJointDistance(joint) + getFaceRadius(obj1, elt1)
                + getFaceRadius(obj2, elt2);
            return mbdJoint;
        }

        case DistanceType::CylinderSphere: {
            auto mbdJoint = makeJoint(Solver::JointKind::PointLineDistance);
            mbdJoint->value = getJointDistance(joint) + getFaceRadius(obj1, elt1)
                + getFaceRadius(obj2, elt2);
            return mbdJoint;
        }

        case DistanceType::CylinderTorus: {
            auto mbdJoint = makeJoint(Solver::JointKind::LineLineDistance);
            mbdJoint->value = getJointDistance(joint) + getFaceRadius(obj1, elt1)
                + getFaceRadius(obj2, elt2);
            return mbdJoint;
        }

        case DistanceType::TorusTorus: {
            auto mbdJoint = makeJoint(Solver::JointKind::Planar);
            mbdJoint->value = getJointDistance(joint);
            return mbdJoint;
        }

        case DistanceType::TorusSphere: {
            auto mbdJoint = makeJoint(Solver::JointKind::PointLineDistance);
            mbdJoint->value = getJointDistance(joint) + getFaceRadius(obj1, elt1)
                + getFaceRadius(obj2, elt2);
            return mbdJoint;
        }

        case DistanceType::SphereSphere: {
            auto mbdJoint = makeJoint(Solver::JointKind::PointPointDistance);
            mbdJoint->value = getJointDistance(joint) + getFaceRadius(obj1, elt1)
                + getFaceRadius(obj2, elt2);
            return mbdJoint;
        }

        // Point - Face cases
        case DistanceType::PointPlane: {
            auto mbdJoint = makeJoint(Solver::JointKind::PointInPlane);
            mbdJoint->value = getJointDistance(joint);
            return mbdJoint;
        }

        case DistanceType::PointCylinder: {
            auto mbdJoint = makeJoint(Solver::JointKind::PointLineDistance);
            mbdJoint->value = getJointDistance(joint) + getFaceRadius(obj1, elt1);
            return mbdJoint;
        }

        case DistanceType::PointSphere: {
            auto mbdJoint = makeJoint(Solver::JointKind::PointPointDistance);
            mbdJoint->value = getJointDistance(joint) + getFaceRadius(obj1, elt1);
            return mbdJoint;
        }

        // Edge - Face cases
        case DistanceType::LinePlane: {
            auto mbdJoint = makeJoint(Solver::JointKind::LineInPlane);
            mbdJoint->value = getJointDistance(joint);
            return mbdJoint;
        }

        // Point - Edge cases
        case DistanceType::PointLine: {
            auto mbdJoint = makeJoint(Solver::JointKind::PointLineDistance);
            mbdJoint->value = getJointDistance(joint);
            return mbdJoint;
        }

        case DistanceType::PointCurve: {
            // For other curves we do a point in plane-of-the-curve.
            // Maybe it would be best tangent / distance to the conic?
            // For arcs and circles we could use ASMTRevSphJoint. But is it better than
            // pointInPlane?
            auto mbdJoint = makeJoint(Solver::JointKind::PointInPlane);
            mbdJoint->value = getJointDistance(joint);
            return mbdJoint;
        }

        default: {
            // by default we make a planar joint.
            auto mbdJoint = makeJoint(Solver::JointKind::Planar);
            mbdJoint->value = getJointDistance(joint);
            return mbdJoint;
        }
    }
}

std::vector<std::shared_ptr<Solver::Joint>> AssemblyObject::makeMbdJoint(App::DocumentObject* joint)
{
    if (!joint) {
        return {};
    }

    JointType jointType = getJointType(joint);

    std::shared_ptr<Solver::Joint> mbdJoint = makeMbdJointOfType(joint, jointType);
    if (!mbdJoint || !isMbDJointValid(joint)) {
        return {};
    }
    if ((jointType == JointType::Gears || jointType == JointType::Belt)
        && gearJointNeedsCarrierMarker(joint)) {
        setGearJointCarrierMarkerIfAvailable(this, joint, mbdJoint);
    }

    Solver::Id fullMarkerNameI = Solver::NoId, fullMarkerNameJ = Solver::NoId;
    if (jointType == JointType::RackPinion) {
        getRackPinionMarkers(joint, fullMarkerNameI, fullMarkerNameJ);
    }
    else {
        fullMarkerNameI = handleOneSideOfJoint(joint, "Reference1", "Placement1");
        fullMarkerNameJ = handleOneSideOfJoint(joint, "Reference2", "Placement2");
    }
    if (fullMarkerNameI == Solver::NoId || fullMarkerNameJ == Solver::NoId) {
        return {};
    }

    mbdJoint->source = solverModel->addSource(joint->getFullName());
    mbdJoint->frameI = fullMarkerNameI;
    mbdJoint->frameJ = fullMarkerNameJ;

    // Add limits if needed. We do not add if this is a simulation or their might clash.
    if (motions.empty()) {
        if (jointType == JointType::Slider || jointType == JointType::Cylindrical) {
            auto* pLenMin = dynamic_cast<App::PropertyFloat*>(joint->getPropertyByName("LengthMin"));
            auto* pLenMax = dynamic_cast<App::PropertyFloat*>(joint->getPropertyByName("LengthMax"));
            auto* pMinEnabled = dynamic_cast<App::PropertyBool*>(
                joint->getPropertyByName("EnableLengthMin")
            );
            auto* pMaxEnabled = dynamic_cast<App::PropertyBool*>(
                joint->getPropertyByName("EnableLengthMax")
            );

            if (pLenMin && pLenMax && pMinEnabled && pMaxEnabled) {  // Make sure properties do exist
                // Swap the values if necessary.
                bool minEnabled = pMinEnabled->getValue();
                bool maxEnabled = pMaxEnabled->getValue();
                double minLength = pLenMin->getValue();
                double maxLength = pLenMax->getValue();

                if ((minLength > maxLength) && minEnabled && maxEnabled) {
                    pLenMin->setValue(maxLength);
                    pLenMax->setValue(minLength);
                    minLength = maxLength;
                    maxLength = pLenMax->getValue();

                    pMinEnabled->setValue(maxEnabled);
                    pMaxEnabled->setValue(minEnabled);
                    minEnabled = maxEnabled;
                    maxEnabled = pMaxEnabled->getValue();
                }

                if (minEnabled) {
                    Solver::Limit limit;
                    limit.source = solverModel->addSource(joint->getFullName());
                    limit.frameI = fullMarkerNameI;
                    limit.frameJ = fullMarkerNameJ;
                    limit.kind = Solver::LimitKind::TranslationZ;
                    limit.min = minLength;
                    limit.tolerance = 1.0e-9;
                    solverModel->addLimit(limit);
                }

                if (maxEnabled) {
                    Solver::Limit limit2;
                    limit2.source = solverModel->addSource(joint->getFullName());
                    limit2.frameI = fullMarkerNameI;
                    limit2.frameJ = fullMarkerNameJ;
                    limit2.kind = Solver::LimitKind::TranslationZ;
                    limit2.max = maxLength;
                    limit2.tolerance = 1.0e-9;
                    solverModel->addLimit(limit2);
                }
            }
        }
        if (jointType == JointType::Revolute || jointType == JointType::Cylindrical) {
            auto* pRotMin = dynamic_cast<App::PropertyFloat*>(joint->getPropertyByName("AngleMin"));
            auto* pRotMax = dynamic_cast<App::PropertyFloat*>(joint->getPropertyByName("AngleMax"));
            auto* pMinEnabled = dynamic_cast<App::PropertyBool*>(
                joint->getPropertyByName("EnableAngleMin")
            );
            auto* pMaxEnabled = dynamic_cast<App::PropertyBool*>(
                joint->getPropertyByName("EnableAngleMax")
            );

            if (pRotMin && pRotMax && pMinEnabled && pMaxEnabled) {  // Make sure properties do exist
                // Swap the values if necessary.
                bool minEnabled = pMinEnabled->getValue();
                bool maxEnabled = pMaxEnabled->getValue();
                double minAngle = pRotMin->getValue();
                double maxAngle = pRotMax->getValue();
                if ((minAngle > maxAngle) && minEnabled && maxEnabled) {
                    pRotMin->setValue(maxAngle);
                    pRotMax->setValue(minAngle);
                    minAngle = maxAngle;
                    maxAngle = pRotMax->getValue();

                    pMinEnabled->setValue(maxEnabled);
                    pMaxEnabled->setValue(minEnabled);
                    minEnabled = maxEnabled;
                    maxEnabled = pMaxEnabled->getValue();
                }

                if (minEnabled) {
                    Solver::Limit limit;
                    limit.source = solverModel->addSource(joint->getFullName());
                    limit.frameI = fullMarkerNameI;
                    limit.frameJ = fullMarkerNameJ;
                    limit.kind = Solver::LimitKind::RotationZ;
                    limit.min = Base::toRadians(minAngle);
                    limit.tolerance = 1.0e-9;
                    solverModel->addLimit(limit);
                }

                if (maxEnabled) {
                    Solver::Limit limit2;
                    limit2.source = solverModel->addSource(joint->getFullName());
                    limit2.frameI = fullMarkerNameI;
                    limit2.frameJ = fullMarkerNameJ;
                    limit2.kind = Solver::LimitKind::RotationZ;
                    limit2.max = Base::toRadians(maxAngle);
                    limit2.tolerance = 1.0e-9;
                    solverModel->addLimit(limit2);
                }
            }
        }
    }
    std::vector<App::DocumentObject*> done;

    auto replaceInitialValue =
        [](std::string& form, App::DocumentObject* jnt, const std::string& mType) {
            if (form.find("initialValue") != std::string::npos) {
                double val = getJointCurrentValue(jnt, mType == "Angular");

                std::ostringstream out;
                out.precision(10);
                out << val;
                std::string valStr = out.str();

                size_t pos;
                while ((pos = form.find("initialValue")) != std::string::npos) {
                    form.replace(pos, 12, valStr);
                }
            }
        };

    // Add motions if needed
    for (auto* motion : motions) {
        if (std::ranges::find(done, motion) != done.end()) {
            continue;  // don't process twice (can happen in case of cylindrical)
        }

        auto* pJoint = dynamic_cast<App::PropertyXLinkSub*>(motion->getPropertyByName("Joint"));
        if (!pJoint) {
            continue;
        }
        App::DocumentObject* motionJoint = pJoint->getValue();
        if (joint != motionJoint) {
            continue;
        }

        auto* pType = dynamic_cast<App::PropertyEnumeration*>(motion->getPropertyByName("MotionType"));
        auto* pFormula = dynamic_cast<App::PropertyString*>(motion->getPropertyByName("Formula"));
        if (!pType || !pFormula) {
            continue;
        }
        std::string formula = pFormula->getValue();
        if (formula == "") {
            continue;
        }
        std::string motionType = pType->getValueAsString();

        replaceInitialValue(formula, joint, motionType);

        // check if there is a second motion as cylindrical can have both,
        // in which case the solver needs a general motion.
        for (auto* motion2 : motions) {
            pJoint = dynamic_cast<App::PropertyXLinkSub*>(motion2->getPropertyByName("Joint"));
            if (!pJoint) {
                continue;
            }
            motionJoint = pJoint->getValue();
            if (joint != motionJoint || motion2 == motion) {
                continue;
            }

            auto* pType2 = dynamic_cast<App::PropertyEnumeration*>(
                motion2->getPropertyByName("MotionType")
            );
            auto* pFormula2 = dynamic_cast<App::PropertyString*>(motion2->getPropertyByName("Formula"));
            if (!pType2 || !pFormula2) {
                continue;
            }
            std::string formula2 = pFormula2->getValue();
            if (formula2 == "") {
                continue;
            }
            std::string motionType2 = pType2->getValueAsString();
            if (motionType2 == motionType) {
                continue;  // only if both motions are different. ie one angular and one linear.
            }

            replaceInitialValue(formula2, joint, motionType2);

            Solver::Driver ASMTmotion;
            ASMTmotion.source = solverModel->addSource(joint->getFullName());
            ASMTmotion.frameI = fullMarkerNameI;
            ASMTmotion.frameJ = fullMarkerNameJ;
            ASMTmotion.kind = Solver::DriverKind::Both;
            ASMTmotion.translation.text = motionType == "Angular" ? formula2 : formula;
            ASMTmotion.rotation.text = motionType == "Angular" ? formula : formula2;
            solverModel->addDriver(ASMTmotion);

            done.push_back(motion2);
        }

        if (motionType == "Angular") {
            Solver::Driver ASMTmotion;
            ASMTmotion.source = solverModel->addSource(joint->getFullName());
            ASMTmotion.frameI = fullMarkerNameI;
            ASMTmotion.frameJ = fullMarkerNameJ;
            ASMTmotion.kind = Solver::DriverKind::RotationZ;
            ASMTmotion.rotation.text = formula;
            solverModel->addDriver(ASMTmotion);
        }
        else if (motionType == "Linear") {
            Solver::Driver ASMTmotion;
            ASMTmotion.source = solverModel->addSource(joint->getFullName());
            ASMTmotion.frameI = fullMarkerNameI;
            ASMTmotion.frameJ = fullMarkerNameJ;
            ASMTmotion.kind = Solver::DriverKind::TranslationZ;
            ASMTmotion.translation.text = formula;
            solverModel->addDriver(ASMTmotion);
        }
    }

    return {mbdJoint};
}

Solver::Id AssemblyObject::handleOneSideOfJoint(
    App::DocumentObject* joint,
    const char* propRefName,
    const char* propPlcName,
    const std::string& markerName
)
{
    App::DocumentObject* part = getMovingPartFromRef(joint, propRefName);
    App::DocumentObject* obj = getObjFromJointRef(joint, propRefName);

    if (!part || !obj) {
        Base::Console()
            .Warning("The property %s of Joint %s is bad.\n", propRefName, joint->getFullName());
        return Solver::NoId;
    }

    MbDPartData data = getMbDData(part);
    Solver::Id mbdPart = data.body;
    Base::Placement plc = getPlacementFromProp(joint, propPlcName);
    // Now we have plc which is the JCS placement, but its relative to the Object, not to the
    // containing Part.
    auto* ref = dynamic_cast<App::PropertyXLinkSub*>(joint->getPropertyByName(propRefName));
    if (!ref) {
        return Solver::NoId;
    }

    // This plc adjustment should be necessary only if obj != part. But for some objects like
    // draft links, we can have obj == part and still need to get global placement to adjust
    // by the element placement.
    Base::Placement obj_global_plc = getGlobalPlacement(nullptr, ref);
    plc = obj_global_plc * plc;
    // Note part is supposed to be root of ref, so we could use part.Placement directly.
    Base::Placement part_global_plc = getGlobalPlacement(part, ref);
    plc = part_global_plc.inverse() * plc;

    // check if we need to add an offset in case of bundled parts.
    if (!data.offsetPlc.isIdentity()) {
        plc = data.offsetPlc * plc;
    }

    std::string markerNameCopy = markerName.empty() ? joint->getFullName() : markerName;
    auto mbdMarker = makeMbdMarker(markerNameCopy, plc);
    mbdMarker.body = mbdPart;

    return solverModel->addFrame(mbdMarker);
}

void AssemblyObject::getRackPinionMarkers(
    App::DocumentObject* joint,
    Solver::Id& markerNameI,
    Solver::Id& markerNameJ
)
{
    // ASMT rack pinion joint must get the rack as I and pinion as J.
    // - rack marker has to have Z axis parallel to pinion Z axis.
    // - rack marker has to have X axis parallel to the sliding axis.
    // The user will have selected the sliding marker so we need to transform it.
    // And we need to detect which marker is the rack.

    int slidingIndex = slidingPartIndex(joint);
    if (slidingIndex == 0) {
        return;
    }

    if (slidingIndex != 1) {
        swapJCS(joint);  // make sure that rack is first.
    }

    App::DocumentObject* part1 = getMovingPartFromRef(joint, "Reference1");
    App::DocumentObject* obj1 = getObjFromJointRef(joint, "Reference1");
    Base::Placement plc1 = getPlacementFromProp(joint, "Placement1");

    App::DocumentObject* obj2 = getObjFromJointRef(joint, "Reference2");
    Base::Placement plc2 = getPlacementFromProp(joint, "Placement2");

    if (!part1 || !obj1) {
        Base::Console().Warning("Reference1 of Joint %s is bad.\n", joint->getFullName());
        return;
    }

    // For the pinion nothing special needed :
    markerNameJ = handleOneSideOfJoint(joint, "Reference2", "Placement2");

    // For the rack we need to change the placement :
    // make the pinion plc relative to the rack placement.
    auto* ref1 = dynamic_cast<App::PropertyXLinkSub*>(joint->getPropertyByName("Reference1"));
    auto* ref2 = dynamic_cast<App::PropertyXLinkSub*>(joint->getPropertyByName("Reference2"));
    if (!ref1 || !ref2) {
        return;
    }
    Base::Placement pinion_global_plc = getGlobalPlacement(obj2, ref2);
    plc2 = pinion_global_plc * plc2;
    Base::Placement rack_global_plc = getGlobalPlacement(obj1, ref1);
    plc2 = rack_global_plc.inverse() * plc2;

    // The rot of the rack placement should be the same as the pinion, but with X axis along the
    // slider axis.
    Base::Rotation rot = plc2.getRotation();
    // the yaw of rot has to be the same as plc1
    Base::Vector3d currentZAxis = rot.multVec(Base::Vector3d(0, 0, 1));
    Base::Vector3d currentXAxis = rot.multVec(Base::Vector3d(1, 0, 0));
    Base::Vector3d targetXAxis = plc1.getRotation().multVec(Base::Vector3d(0, 0, 1));

    // Calculate the angle between the current X axis and the target X axis
    double yawAdjustment = currentXAxis.GetAngle(targetXAxis);

    // Determine the direction of the yaw adjustment using cross product
    Base::Vector3d crossProd = currentXAxis.Cross(targetXAxis);
    if (currentZAxis * crossProd < 0) {  // If cross product is in opposite direction to Z axis
        yawAdjustment = -yawAdjustment;
    }

    // Create a yaw rotation around the Z axis
    Base::Rotation yawRotation(currentZAxis, yawAdjustment);

    // Combine the initial rotation with the yaw adjustment
    Base::Rotation adjustedRotation = yawRotation * rot;
    plc1.setRotation(adjustedRotation);

    // Then end of processing similar to handleOneSideOfJoint :
    MbDPartData data1 = getMbDData(part1);
    Solver::Id mbdPart = data1.body;
    if (obj1->getNameInDocument() != part1->getNameInDocument()) {
        plc1 = rack_global_plc * plc1;

        Base::Placement part_global_plc = getGlobalPlacement(part1, ref1);
        plc1 = part_global_plc.inverse() * plc1;
    }
    // check if we need to add an offset in case of bundled parts.
    if (!data1.offsetPlc.isIdentity()) {
        plc1 = data1.offsetPlc * plc1;
    }

    std::string markerName = joint->getFullName();
    auto mbdMarker = makeMbdMarker(markerName, plc1);
    mbdMarker.body = mbdPart;

    markerNameI = solverModel->addFrame(mbdMarker);
}

int AssemblyObject::slidingPartIndex(App::DocumentObject* joint)
{
    App::DocumentObject* part1 = getMovingPartFromRef(joint, "Reference1");
    App::DocumentObject* part2 = getMovingPartFromRef(joint, "Reference2");

    // Compare the JCS in global coordinates: the slider and this joint may reference
    // different sub-objects of the same part.
    Base::Placement plc1 = getJointSideGlobalPlacement(joint, "Reference1", "Placement1");
    Base::Placement plc2 = getJointSideGlobalPlacement(joint, "Reference2", "Placement2");

    int slidingFound = 0;
    for (auto* jt : getJoints()) {
        if (getJointType(jt) == JointType::Slider) {
            App::DocumentObject* jpart1 = getMovingPartFromRef(jt, "Reference1");
            App::DocumentObject* jpart2 = getMovingPartFromRef(jt, "Reference2");
            int found = 0;
            Base::Placement plcjt, plci;
            if (jpart1 == part1 || jpart1 == part2) {
                found = (jpart1 == part1) ? 1 : 2;
                plci = (jpart1 == part1) ? plc1 : plc2;
                plcjt = getJointSideGlobalPlacement(jt, "Reference1", "Placement1");
            }
            else if (jpart2 == part1 || jpart2 == part2) {
                found = (jpart2 == part1) ? 1 : 2;
                plci = (jpart2 == part1) ? plc1 : plc2;
                plcjt = getJointSideGlobalPlacement(jt, "Reference2", "Placement2");
            }

            if (found != 0) {
                // Check that the Z axes of plcjt and plci are parallel. Do not compare
                // pitch/roll: a yaw offset on the slider JCS (e.g. to align the rack) rotates
                // about its local Z axis, which keeps the axis but changes pitch and roll
                // whenever that axis is not the global Z.
                Base::Vector3d axisjt = plcjt.getRotation().multVec(Base::Vector3d(0, 0, 1));
                Base::Vector3d axisi = plci.getRotation().multVec(Base::Vector3d(0, 0, 1));
                if (axisjt.Cross(axisi).Length() < Precision::Confusion()) {
                    slidingFound = found;
                }
            }
        }
    }
    return slidingFound;
}

bool AssemblyObject::isMbDJointValid(App::DocumentObject* joint)
{
    // When dragging a part, we are bundling fixed parts together.
    // This may lead to a conflicting joint that is self referencing a MbD part.
    // The solver crash when fed such a bad joint. So we make sure it does not happen.
    App::DocumentObject* part1 = getMovingPartFromRef(joint, "Reference1");
    App::DocumentObject* part2 = getMovingPartFromRef(joint, "Reference2");
    if (!part1 || !part2) {
        return false;
    }

    // If this joint is self-referential it must be ignored.
    if (getMbDPart(part1) == getMbDPart(part2)) {
        Base::Console().Warning(
            "Assembly: Ignoring joint (%s) because its parts are connected by a fixed "
            "joint bundle. This joint is a conflicting or redundant constraint.\n",
            joint->getFullLabel()
        );
        return false;
    }
    return true;
}

AssemblyObject::MbDPartData AssemblyObject::getMbDData(App::DocumentObject* part)
{
    auto it = objectPartMap.find(part);
    if (it != objectPartMap.end()) {
        // part has been associated with an ASMTPart before
        return it->second;
    }

    // Associate objects that belong to an active rigid cluster.
    if (auto* rep = getRigidRepresentative(part)) {
        Base::Placement repPlc = getPlacementFromProp(rep, "Placement");

        Solver::Id mbdPart = Solver::NoId;
        const auto repMapped = objectPartMap.find(rep);
        if (repMapped != objectPartMap.end()) {
            mbdPart = repMapped->second.body;
        }
        else {
            std::string repName = rep->getFullName();
            mbdPart = makeMbdPart(repName, repPlc);
            objectPartMap[rep] = {mbdPart, Base::Placement()};
        }

        if (const auto* members = getRigidMembers(rep)) {
            for (auto* member : *members) {
                if (!member || objectPartMap.find(member) != objectPartMap.end()) {
                    continue;
                }

                Base::Placement memberPlc = getPlacementFromProp(member, "Placement");
                objectPartMap[member] = {mbdPart, repPlc.inverse() * memberPlc};
            }
        }

        auto mapped = objectPartMap.find(part);
        if (mapped != objectPartMap.end()) {
            return mapped->second;
        }
    }

    // part has not been associated with an ASMTPart before
    std::string str = part->getFullName();
    Base::Placement plc = getPlacementFromProp(part, "Placement");
    Solver::Id mbdPart = makeMbdPart(str, plc);
    MbDPartData data = {mbdPart, Base::Placement()};
    objectPartMap[part] = data;  // Store the association

    // Associate other objects connected with fixed joints
    if (bundleFixed) {
        auto addConnectedFixedParts = [&](App::DocumentObject* currentPart, auto& self) -> void {
            std::vector<App::DocumentObject*> joints = getJointsOfPart(currentPart);
            for (auto* joint : joints) {
                JointType jointType = getJointType(joint);
                if (jointType == JointType::Fixed) {
                    App::DocumentObject* part1 = getMovingPartFromRef(joint, "Reference1");
                    App::DocumentObject* part2 = getMovingPartFromRef(joint, "Reference2");
                    App::DocumentObject* partToAdd = currentPart == part1 ? part2 : part1;

                    if (objectPartMap.find(partToAdd) != objectPartMap.end()) {
                        // already added
                        continue;
                    }

                    Base::Placement plci = getPlacementFromProp(partToAdd, "Placement");
                    MbDPartData partData = {mbdPart, plc.inverse() * plci};
                    objectPartMap[partToAdd] = partData;  // Store the association

                    // Recursively call for partToAdd
                    self(partToAdd, self);
                }
            }
        };

        addConnectedFixedParts(part, addConnectedFixedParts);
    }
    return data;
}

Solver::Id AssemblyObject::getMbDPart(App::DocumentObject* part)
{
    if (!part) {
        return Solver::NoId;
    }
    return getMbDData(part).body;
}

Solver::Id AssemblyObject::makeMbdPart(std::string& name, Base::Placement plc, double mass)
{
    Solver::Body mbdPart;
    mbdPart.source = solverModel->addSource(name);
    mbdPart.mass.mass = mass;
    mbdPart.placement = plc;

    return solverModel->addBody(mbdPart);
}

Solver::Frame AssemblyObject::makeMbdMarker(std::string& name, Base::Placement& plc)
{
    Solver::Frame mbdMarker;
    mbdMarker.label = solverModel->addSource(name);
    mbdMarker.local = plc;

    return mbdMarker;
}

std::vector<ObjRef> AssemblyObject::getDownstreamParts(
    App::DocumentObject* part,
    App::DocumentObject* joint
)
{
    if (!part) {
        return {};
    }

    // First we deactivate the joint
    bool state = false;
    if (joint) {
        state = getJointActivated(joint);
        setJointActivated(joint, false);
    }

    std::vector<App::DocumentObject*> joints = getJoints();

    std::vector<ObjRef> connectedParts = {{part, nullptr}};
    traverseAndMarkConnectedParts(part, connectedParts, joints);

    std::vector<ObjRef> downstreamParts;
    for (auto& parti : connectedParts) {
        if (!isPartConnected(parti.obj) && (parti.obj != part)) {
            downstreamParts.push_back(parti);
        }
    }

    if (joint) {
        setJointActivated(joint, state);
    }

    return downstreamParts;
}

App::DocumentObject* AssemblyObject::getUpstreamMovingPart(
    App::DocumentObject* part,
    App::DocumentObject*& joint,
    std::string& name,
    std::vector<App::DocumentObject*> excludeJoints
)
{
    if (!part || isPartGrounded(part)) {
        return nullptr;
    }

    excludeJoints.push_back(joint);

    joint = getJointOfPartConnectingToGround(part, name, excludeJoints);
    JointType jointType = getJointType(joint);
    if (jointType != JointType::Fixed) {
        return part;
    }

    part = getMovingPartFromRef(joint, name == "Reference1" ? "Reference2" : "Reference1");

    return getUpstreamMovingPart(part, joint, name);
}

double AssemblyObject::getObjMass(App::DocumentObject* obj)
{
    if (!obj) {
        return 0.0;
    }

    for (auto& pair : objMasses) {
        if (pair.first == obj) {
            return pair.second;
        }
    }
    return 1.0;
}

void AssemblyObject::setObjMasses(std::vector<std::pair<App::DocumentObject*, double>> objectMasses)
{
    objMasses = objectMasses;
}

std::vector<AssemblyLink*> AssemblyObject::getSubAssemblies()
{
    std::vector<AssemblyLink*> subAssemblies = {};

    App::Document* doc = getDocument();

    std::vector<DocumentObject*> assemblies = doc->getObjectsOfType(
        Assembly::AssemblyLink::getClassTypeId()
    );
    for (auto assembly : assemblies) {
        if (hasObject(assembly)) {
            subAssemblies.push_back(freecad_cast<AssemblyLink*>(assembly));
        }
    }

    return subAssemblies;
}

void AssemblyObject::ensureIdentityPlacements()
{
    std::vector<App::DocumentObject*> group = Group.getValues();
    for (auto* obj : group) {
        // When used in assembly, link groups must have identity placements.
        // A generated array is one component and keeps its own.
        if (isElementLinkGroup(obj)) {
            auto* link = dynamic_cast<App::Link*>(obj);
            auto* pPlc = obj->getPlacementProperty();
            if (!pPlc || !link) {
                continue;
            }

            Base::Placement plc = pPlc->getValue();
            if (plc.isIdentity()) {
                continue;
            }

            pPlc->setValue(Base::Placement());
            obj->purgeTouched();

            // To keep the LinkElement positions, we apply plc to their placements
            std::vector<App::DocumentObject*> elts = link->ElementList.getValues();
            for (auto* elt : elts) {
                pPlc = elt->getPlacementProperty();
                pPlc->setValue(plc * pPlc->getValue());
                elt->purgeTouched();
            }
        }
    }
}

void AssemblyObject::syncGroundedJoints()
{
    if (App::GetApplication().isRestoring()) {
        return;
    }

    std::vector<App::DocumentObject*> groundedJoints = getGroundedJoints();
    std::map<App::DocumentObject*, App::DocumentObject*> groundedMap;
    for (auto gJoint : groundedJoints) {
        auto propObj = dynamic_cast<App::PropertyLink*>(gJoint->getPropertyByName("ObjectToGround"));
        if (propObj && propObj->getValue()) {
            groundedMap[propObj->getValue()] = gJoint;
        }
    }

    std::vector<App::DocumentObject*> allParts = getAssemblyComponents(this);

    for (auto part : allParts) {
        if (!part) {
            continue;
        }
        auto propPlc = part->getPlacementProperty();
        if (!propPlc) {
            continue;
        }

        bool isReadOnly = propPlc->isReadOnly();
        auto it = groundedMap.find(part);
        bool hasJoint = (it != groundedMap.end());

        // Create grounding joint if placement is locked but no joint exists
        if (isReadOnly && !hasJoint) {
            Base::PyGILStateLocker lock;
            try {
                std::string docName = getDocument()->getName();
                std::string asmName = getNameInDocument();
                std::string partName = part->getNameInDocument();
                std::string code = "import FreeCAD\n"
                                   "try:\n"
                                   "    import JointObject\n"
                                   "    import UtilsAssembly\n"
                                   "    doc = FreeCAD.getDocument('"
                    + docName
                    + "')\n"
                      "    asm = doc.getObject('"
                    + asmName
                    + "')\n"
                      "    part = doc.getObject('"
                    + partName
                    + "')\n"
                      "    jg = UtilsAssembly.getJointGroup(asm)\n"
                      "    if jg:\n"
                      "        j = jg.newObject('App::FeaturePython', 'GroundedJoint')\n"
                      "        JointObject.GroundedJoint(j, part)\n"
                      "        if hasattr(JointObject, 'ViewProviderGroundedJoint') and getattr(j, "
                      "'ViewObject', None):\n"
                      "            JointObject.ViewProviderGroundedJoint(j.ViewObject)\n"
                      "        j.recompute()\n"
                      "except Exception as e:\n"
                      "    FreeCAD.Console.PrintError(str(e) + '\\n')\n";
                Base::Interpreter().runString(code.c_str());
            }
            catch (...) {
            }
        }
        // Delete grounding joint if placement lock was lifted
        else if (!isReadOnly && hasJoint) {
            getDocument()->removeObject(it->second->getNameInDocument());
        }
    }
}

int AssemblyObject::numberOfComponents() const
{
    return getAssemblyComponents(this).size();
}

bool AssemblyObject::isEmpty() const
{
    return numberOfComponents() == 0;
}
