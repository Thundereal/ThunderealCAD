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
#include <functional>
#include <cstring>
#include <numbers>
#include <algorithm>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <ranges>
#include <atomic>
#include <thread>

// Reached transitively upstream, but not here.
#include <Precision.hxx>
#include <Standard_Failure.hxx>


#include <App/Application.h>
#include <App/Datums.h>
#include <App/Document.h>
#include <App/DocumentObjectGroup.h>
#include <App/FeaturePythonPyImp.h>
#include <App/GeoFeatureGroupExtension.h>
#include <App/ElementNamingUtils.h>
#include <App/Link.h>
#include <App/PropertyPythonObject.h>
#include <Base/Console.h>
#include <Base/Placement.h>
#include <Base/Rotation.h>
#include <Base/Tools.h>
#include <Base/ConsoleContext.h>
#include <Base/Interpreter.h>

#include <Mod/Part/App/TopoShape.h>
#include <Mod/Part/App/AttachExtension.h>
#include <Mod/Part/App/DatumFeature.h>
#include <Mod/Part/App/FeatureElement.h>
#include <Mod/Part/App/PartFeature.h>
#include <Mod/Part/App/PropertyTopoShape.h>

#include "AssemblyLink.h"
#include "AssemblyObject.h"
#include "AssemblyObjectPy.h"
#include "AssemblyUtils.h"
#include "CamJoint.h"
#include <App/DocumentObjectExtension.h>
#include "Groups.h"
#include "AssemblyGroup.h"
#include "Grounding.h"
#include "Joint.h"
#include "MassProperties.h"
#include "Physics.h"
#include "Simulation.h"
#include "Views.h"
#include "Solver/Backend.h"
#include "Solver/ReferenceBackend.h"
#include "Solver/Registry.h"
#include "Solver/RigidGroups.h"

FC_LOG_LEVEL_INIT("Assembly", true, true, true)

using namespace Assembly;


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

    if (int side = typedJointSide(joint, refName)) {
        // A C++ joint's frame is in its assembly's coordinate system, or
        // the flexible sub-assembly instance's holding its mirror
        auto* typed = static_cast<Joint*>(joint);
        auto* container = typed->getContainer();
        return (container ? container->globalPlacement() : Base::Placement())
            * typed->getFrame(side);
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
    : lastDoF(0)
    , lastHasConflict(false)
    , lastHasRedundancies(false)
    , lastHasPartialRedundancies(false)
    , lastHasMalformedConstraints(false)
    , lastSolverStatus(0)
{
    ADD_PROPERTY_TYPE(
        SolverBackend,
        (""),
        "Solver",
        App::Prop_None,
        "The solver backend this assembly is solved with, by name\n"
        "(AssemblyApp.getAvailableSolvers()). Empty: the one the preferences name."
    );
    ADD_PROPERTY_TYPE(
        Freeze,
        (false),
        "Assembly",
        App::Prop_NoRecompute,
        "Keep the components as one shape: the assembly is not solved, and a\n"
        "document linking to it loads that shape in place of the components."
    );
    // As Assembly3's: the one change a partially loaded document takes
    Freeze.setStatus(App::Property::PartialTrigger, true);
    ADD_PROPERTY_TYPE(InterferenceExcluded,
                      (nullptr),
                      "Interference",
                      App::Prop_NoRecompute,
                      "Components left out of the interference check and of collision while\n"
                      "dragging, or containers or groups of them (a fasteners group).");
    ADD_PROPERTY_TYPE(InterferenceIgnoreHidden,
                      (false),
                      "Interference",
                      App::Prop_NoRecompute,
                      "Leave the hidden components out of the interference check and of\n"
                      "collision while dragging.");
    ADD_PROPERTY_TYPE(InterferenceIgnoredPairs,
                      (),
                      "Interference",
                      App::Prop_NoRecompute,
                      "Pairs never reported, \"Name1|Name2\" by the objects' names: a\n"
                      "component's, or a container's or group's it is in.");
    ADD_PROPERTY_TYPE(InterferenceClearances,
                      (),
                      "Interference",
                      App::Prop_NoRecompute,
                      "Clearances pairs keep, \"Name1|Name2\" to mm: a pair closer than its\n"
                      "clearance is reported, and stops a drag as a collision does.");

    // The workbench's Python decides "is this container a real assembly?" by
    // testing Type == "Assembly" (six sites in JointObject.py). Upstream stamps
    // it from CommandCreateAssembly.py, so an assembly created any other way --
    // a script, a test, an importer -- is not recognised as one, and joints
    // silently never solve. The type is a property of the class, not of how the
    // object was created, so set it here. Restoring a document still overwrites
    // it with the saved value, so old files are unaffected.
    Type.setValue("Assembly");

    // Not upstream: an object already in another geometry group, in another
    // document or already a component is added as an owned link
    // (docs/AssemblyObjectModel.md sec 2). A file that saved another mode
    // restores it
    GroupMode.setValue(GroupModeAuto);

    lastDoF = numberOfComponents() * 6;
    signalSolverUpdate();
}

namespace
{
// Every frame a backend produces, kept so that playback needs no live
// solver; for a run on a worker thread, how far it got, and whether to stop
struct TrajectoryRecorder: Solver::TrajectorySink
{
    Solver::Trajectory frames;
    std::size_t width = 0;
    std::vector<double> pending;
    std::atomic<bool> cancel {false};
    std::atomic<std::size_t> count {0};
    std::atomic<double> last {0.0};

    bool frame(double time, const std::vector<Solver::BodyPose>& poses) override
    {
        if (cancel) {
            return false;
        }
        if (frames.times.empty()) {
            for (const auto& pose : poses) {
                frames.bodies.push_back(pose.body);
            }
        }
        else if (poses.size() != frames.bodies.size()) {
            return false;
        }
        frames.times.push_back(time);
        for (const auto& pose : poses) {
            frames.poses.push_back(pose.placement);
        }
        frames.values.insert(frames.values.end(), pending.begin(), pending.end());
        pending.clear();
        last = time;
        ++count;
        return true;
    }
    void channels(const std::vector<Solver::Channel>& channels) override
    {
        frames.channels = channels;
        for (const auto& channel : channels) {
            width += channel.width;
        }
    }
    void values(const std::vector<double>& values) override
    {
        pending = values;
        pending.resize(width, 0.0);
    }
};
}  // namespace

struct AssemblyObject::SimulationRun
{
    TrajectoryRecorder sink;
    std::unique_ptr<Solver::Session> session;
    Solver::SimulationSpec spec;
    Solver::Model model;
    std::unordered_map<App::DocumentObject*, MbDPartData> parts;
    App::DocumentObjectWeakPtrT sim {nullptr};
    std::atomic<bool> done {false};
    Solver::Status status = Solver::Status::Failed;
    std::thread thread;

    // A worker's warning reaches a Python observer under the GIL: the
    // thread waiting for it must not hold it
    void join()
    {
        if (!thread.joinable()) {
            return;
        }
        if (Py_IsInitialized() && PyGILState_Check()) {
            Base::PyGILStateRelease release;
            thread.join();
        }
        else {
            thread.join();
        }
    }
    ~SimulationRun()
    {
        sink.cancel = true;
        join();
    }
};

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

void AssemblyObject::handleChangedPropertyType(
    Base::XMLReader& reader,
    const char* TypeName,
    App::Property* prop
)
{
    // A file that stored the backend as an enumeration of the registered names
    // (sec 4.8 of docs/AssemblySolverInterface.md proposed one) keeps its choice.
    if (prop == &SolverBackend
        && std::strcmp(TypeName, App::PropertyEnumeration::getClassTypeId().getName()) == 0) {
        App::PropertyEnumeration value;
        value.Restore(reader);
        SolverBackend.setValue(value.isValid() ? value.getValueAsString() : "");
        return;
    }
    App::Part::handleChangedPropertyType(reader, TypeName, prop);
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
    else if (prop == &Freeze && !isRestoring() && getDocument()
             && !getDocument()->isPerformingTransaction()) {
        if (getDocument()->testStatus(App::Document::PartialDoc)) {
            FC_WARN(
                getFullName() << ": the document is partially loaded, open it on its own "
                                 "to take or drop the frozen shape"
            );
        }
        else if (Freeze.getValue()) {
            refreshFrozenShape();
            App::Part::onChanged(prop);
            // Nothing to recompute in a frozen assembly: saved touched, a
            // document linking to it would load it in full
            purgeTouched();
            return;
        }
        else if (auto parts = freecad_cast<PartsGroup*>(getAuxGroup(PartsRole))) {
            // Left for the user to remove, without the shape that is no
            // longer kept in step
            parts->Shape.setValue(::Part::TopoShape());
        }
    }
    App::Part::onChanged(prop);
}

void AssemblyObject::removePartsGroup()
{
    if (isFrozen()) {
        THROWM(Base::RuntimeError, "A frozen assembly keeps its Parts group");
    }
    App::Part::removePartsGroup();
}

void AssemblyObject::refreshFrozenShape()
{
    auto group = getAuxGroup(PartsRole);
    if (group && !group->isDerivedFrom<PartsGroup>()) {
        // A plain one, made before the assembly had a kind of its own
        App::Part::removePartsGroup();
        group = nullptr;
    }
    auto parts = static_cast<PartsGroup*>(group ? group : addPartsGroup());
    if (!parts) {
        return;
    }
    std::vector<::Part::TopoShape> shapes;
    for (auto obj : parts->Group.getValues()) {
        if (!obj || !obj->Visibility.getValue()) {
            continue;
        }
        auto shape = ::Part::Feature::getTopoShape(obj);
        if (!shape.isNull()) {
            shapes.push_back(shape);
        }
    }
    ::Part::TopoShape compound;
    if (!shapes.empty()) {
        compound.makECompound(shapes);
    }
    parts->Shape.setValue(compound);
    parts->purgeTouched();
}

int AssemblyObject::canLoadPartial() const
{
    return isFrozen() ? 1 : 0;
}

std::vector<std::string> AssemblyObject::getSubObjects(int reason) const
{
    if (reason == GS_DEFAULT && isFrozen()) {
        if (auto parts = freecad_cast<PartsGroup*>(getAuxGroup(PartsRole))) {
            return {std::string(parts->getNameInDocument()) + "."};
        }
    }
    return App::Part::getSubObjects(reason);
}

namespace
{
// A component as the Parts group takes it: not a joint's element binding, nor a
// group, nor anything without a place of its own, nor a simulation's own (a trace)
bool isComponentObject(const App::DocumentObject* obj)
{
    return obj && !obj->isDerivedFrom<::Part::ElementBinding>()
        && !obj->isDerivedFrom<::Part::Element>() && !isStudyChild(obj)
        && !App::GeoFeatureGroupExtension::isNonGeoGroup(obj)
        && (obj->isDerivedFrom<App::GeoFeature>()
            || obj->hasExtension(App::LinkBaseExtension::getExtensionClassTypeId()));
}

// A plain group holding a component at any depth, other than upstream's
// groups of an assembly
bool holdsComponents(const App::DocumentObject* obj)
{
    if (obj->isDerivedFrom<BomGroup>() || obj->isDerivedFrom<JointGroup>()
        || obj->isDerivedFrom<SimulationGroup>() || obj->isDerivedFrom<SnapshotGroup>()
        || obj->isDerivedFrom<ViewGroup>()) {
        return false;
    }
    auto group = App::GeoFeatureGroupExtension::getNonGeoGroup(obj);
    if (!group) {
        return false;
    }
    for (auto child : group->Group.getValues()) {
        if (child && (isComponentObject(child) || holdsComponents(child))) {
            return true;
        }
    }
    return false;
}
}  // namespace

bool AssemblyObject::getChildDefaultExport(App::DocumentObject* obj, int reason) const
{
    if (reason != GS_DEFAULT) {
        return App::Part::getChildDefaultExport(obj, reason);
    }
    if (!obj || !obj->isAttachedToDocument() || !Group.find(obj->getNameInDocument())) {
        return false;
    }
    // One in a plain group goes with the group
    if (App::GroupExtension::getGroupOfObject(obj)) {
        return false;
    }
    if (App::GeoFeatureGroupExtension::isNonGeoGroup(obj)) {
        return holdsComponents(obj);
    }
    // A feature taking another in (a cut's base) is not told apart: the one
    // taken in is normally hidden, and the shape follows visibility
    return isComponentObject(obj);
}

int AssemblyObject::solve(bool enableRedo)
{
    if (isFrozen()) {
        // Nothing moves in a frozen assembly
        return 0;
    }
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

    Solver::Result result;
    if (session) {
        result = session->solve({});
    }
    else {
        result.message = "no solver backend";
    }
    if (result.status != Solver::Status::Ok) {
        FC_ERR("Solve failed: " << result.message);
        lastSolverStatus = -1;
        updateSolveStatus();
        return -1;
    }
    lastSolverStatus = 0;

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
        std::unordered_set<Solver::Id> uniqueBodies;
        for (const auto& entry : objectPartMap) {
            if (entry.second.body < loadedBodies) {
                uniqueBodies.insert(entry.second.body);
            }
        }

        const int bundledParts = static_cast<int>(objectPartMap.size() - uniqueBodies.size());
        numberOfSolverBodies -= bundledParts;
    }

    // +1 because the assembly origin is also represented by a solver body.
    lastDoF = (1 + numberOfSolverBodies) * 6;

    Solver::Diagnostics diagnostics;
    if (session) {
        diagnostics = session->analyse();
    }
    if (!(diagnostics.known & Solver::Diagnostics::KnownDof)) {
        // Solve on demand when queried before the system is solved, but not from
        // within a solve: solve() calls this, so a failed solve would recurse
        // indefinitely. A solve that gets that far refreshes the status itself.
        if (!solveInProgress) {
            solve();
        }
        return;
    }

    // The session counts the bodies it was given; parts left out of the model
    // (not connected to ground) keep their 6 freedoms.
    lastDoF = diagnostics.dof + 6 * (numberOfSolverBodies - static_cast<int>(loadedBodies));

    // Joints map back to the document by their source; grounds, limits and
    // drivers are not joints of the document.
    auto collect = [&](const std::vector<Solver::Id>& ids, std::vector<std::string>& names) {
        for (Solver::Id id : ids) {
            std::string name = jointNameOf(id);
            if (!name.empty()) {
                appendUnique(names, name);
            }
        }
    };
    collect(diagnostics.redundant, lastRedundantJoints);
    collect(diagnostics.partiallyRedundant, lastPartialRedundantJoints);
    collect(diagnostics.conflicting, lastConflictingJoints);
    if (diagnostics.known & Solver::Diagnostics::KnownMalformed) {
        collect(diagnostics.malformed, lastMalformedJoints);
        lastHasMalformedConstraints = !lastMalformedJoints.empty();
    }

    lastHasRedundancies = !lastRedundantJoints.empty();
    lastHasPartialRedundancies = !lastPartialRedundantJoints.empty();
    lastHasConflict = !lastConflictingJoints.empty();

    signalSolverUpdate();
}

std::string AssemblyObject::jointNameOf(Solver::Id joint) const
{
    if (!solverModel || joint >= solverModel->joints.size()) {
        return {};
    }
    // The source is the joint's full name, like Document#Joint001.
    const std::string& rawName = solverModel->sourceName(solverModel->joints[joint].source);
    size_t hashPos = rawName.find_last_of('#');
    const std::string name = hashPos != std::string::npos ? rawName.substr(hashPos + 1) : rawName;
    App::DocumentObject* docObj = getDocument()->getObject(name.c_str());

    // Only objects that are actual joints in the document.
    if (!docObj
        || (!docObj->getPropertyByName("Reference1") && !docObj->isDerivedFrom<Joint>())) {
        return {};
    }
    return docObj->getNameInDocument();
}

std::vector<std::pair<std::string, double>> AssemblyObject::getSolverResiduals() const
{
    std::vector<std::pair<std::string, double>> out;
    if (!session || !solverModel) {
        return out;
    }
    std::vector<Base::Placement> poses;
    for (const auto& pose : session->poses()) {
        poses.push_back(pose.placement);
    }
    const std::vector<double> residuals = Solver::Reference::jointResiduals(*solverModel, poses);
    for (std::size_t j = 0; j < residuals.size(); ++j) {
        const std::string name = jointNameOf(static_cast<Solver::Id>(j));
        if (name.empty()) {
            continue;
        }
        auto it = std::ranges::find_if(out, [&](const auto& entry) { return entry.first == name; });
        if (it == out.end()) {
            out.emplace_back(name, residuals[j]);
        }
        else if (!(it->second >= residuals[j])) {
            it->second = residuals[j];
        }
    }
    return out;
}

std::vector<std::tuple<std::string, Base::Vector3d, double, int, double>> AssemblyObject::getSolverMetric() const
{
    std::vector<std::tuple<std::string, Base::Vector3d, double, int, double>> out;
    if (!solverModel) {
        return out;
    }
    std::vector<Solver::BodyMetric> metric;
    if (session) {
        metric = session->bodyMetric();
    }
    if (metric.size() != solverModel->bodies.size()) {
        metric = Solver::jointMetric(*solverModel);
    }
    for (const auto& [obj, data] : objectPartMap) {
        if (!obj || !obj->isAttachedToDocument() || data.body >= metric.size()) {
            continue;
        }
        // The object is the body times its offset: the body's centre in the
        // object's frame
        const Solver::BodyMetric& m = metric[data.body];
        Base::Vector3d centre;
        data.offsetPlc.inverse().multVec(m.centre, centre);
        out.emplace_back(obj->getNameInDocument(), centre, m.inertia, static_cast<int>(m.body), m.mass);
    }
    std::ranges::sort(out, [](const auto& a, const auto& b) {
        return std::get<0>(a) < std::get<0>(b);
    });
    return out;
}

int AssemblyObject::generateSimulation(App::DocumentObject* sim)
{
    if (simulationRun) {
        cancelSimulation();
        finishSimulation();
    }
    // The study the frames were of has none now
    if (auto* last = freecad_cast<Simulation*>(lastSimulation())) {
        last->ResultsStale.setValue(false);
    }
    trajectory.reset();
    trajectoryParts.clear();
    trajectorySimulation.reset();
    trajectoryModel.reset();
    trajectorySpec.reset();

    if (int built = buildSimulation(sim); built != 0) {
        return built;
    }
    makeMbdModel();
    return runSimulation(sim);
}

int AssemblyObject::buildSimulation(App::DocumentObject* sim)
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

    // Speed and Effort are a dynamic run's (docs/AssemblyDynamics.md sec 5.2);
    // a static one takes an Effort, a load, but a speed holds nothing still
    if (simulationSpec && simulationSpec->analysis != Solver::Analysis::Dynamic) {
        const bool statics = simulationSpec->analysis == Solver::Analysis::Static;
        auto refused = [&](Solver::MotorControl control) {
            return control == Solver::MotorControl::Speed
                || (!statics && control == Solver::MotorControl::Effort);
        };
        for (const auto& driver : solverModel->drivers) {
            if (refused(driver.rotationControl) || refused(driver.translationControl)) {
                Base::Console().error(
                    statics ? "Assembly: the motion of %s is a Speed, which a Static study "
                              "does not run\n"
                            : "Assembly: the motion of %s is a Speed or an Effort, which only "
                              "a Dynamic or a Static study runs\n",
                    solverModel->sourceName(driver.source).c_str());
                ++badFormulas;
            }
        }
    }

    if (badFormulas > 0) {
        // Each was reported by the build, with its position in the text.
        Base::Console().Error("Generation of simulation failed\n");
        motions.clear();
        return -1;
    }

    // A dynamic or static run, or a kinematic one with its reactions: the
    // bodies' masses, its force elements, and gravity in the assembly's
    // frame. The forces first: a part a spring alone holds becomes a body
    // there, which the masses then weigh.
    if (simulationSpec
        && (simulationSpec->analysis != Solver::Analysis::Kinematic || simulationSpec->reactions)) {
        addForces(*solverModel, sim);
        if (badFormulas > 0) {
            // A load's profile that does not parse, reported with it
            Base::Console().Error("Generation of simulation failed\n");
            motions.clear();
            return -1;
        }
        addDynamics(*solverModel);
        if (auto* typed = freecad_cast<Simulation*>(sim);
            typed && typed->GravityEnabled.getValue()) {
            const Base::Rotation turn =
                App::GeoFeature::getGlobalPlacement(this).getRotation().inverse();
            solverModel->dynamics->gravity = turn.multVec(typed->Gravity.getValue());
        }
    }
    return 0;
}

int AssemblyObject::runSimulation(App::DocumentObject* sim)
{
    TrajectoryRecorder sink;
    if (!session || !simulationSpec
        || session->simulate(*simulationSpec, sink) != Solver::Status::Ok) {
        Base::Console().Error("Generation of simulation failed\n");
        motions.clear();
        return -1;
    }
    keepFrames(sim, std::move(sink.frames), objectPartMap, *solverModel, *simulationSpec);
    resultsChanged = false;
    motions.clear();
    return 0;
}

void AssemblyObject::keepFrames(App::DocumentObject* sim,
                                Solver::Trajectory&& frames,
                                const std::unordered_map<App::DocumentObject*, MbDPartData>& parts,
                                const Solver::Model& model,
                                const Solver::SimulationSpec& spec)
{
    trajectory = std::make_unique<Solver::Trajectory>(std::move(frames));
    trajectoryParts = parts;
    trajectorySimulation = sim;
    trajectoryModel = std::make_unique<Solver::Model>(model);
    trajectorySpec = std::make_unique<Solver::SimulationSpec>(spec);
}

int AssemblyObject::startSimulation(App::DocumentObject* sim)
{
    if (simulationRun) {
        cancelSimulation();
        finishSimulation();
    }
    if (auto* last = freecad_cast<Simulation*>(lastSimulation())) {
        last->ResultsStale.setValue(false);
    }
    trajectory.reset();
    trajectoryParts.clear();
    trajectorySimulation.reset();
    trajectoryModel.reset();
    trajectorySpec.reset();

    if (int built = buildSimulation(sim); built != 0) {
        return built;
    }
    makeMbdModel();
    motions.clear();
    if (!session || !simulationSpec || !solverModel) {
        Base::Console().Error("Generation of simulation failed\n");
        return -1;
    }
    auto run = std::make_unique<SimulationRun>();
    // The run's own: the assembly's next solve builds another
    run->session = std::move(session);
    loadedBodies = 0;
    run->spec = *simulationSpec;
    run->model = *solverModel;
    run->parts = objectPartMap;
    run->sim = sim;
    SimulationRun* going = run.get();
    run->thread = std::thread([going]() {
        Base::ConsoleContext context("the simulation");
        try {
            going->status = going->session->simulate(going->spec, going->sink);
        }
        catch (const std::exception& e) {
            Base::Console().error("Assembly: the simulation failed: %s\n", e.what());
            going->status = Solver::Status::Failed;
        }
        catch (...) {
            going->status = Solver::Status::Failed;
        }
        going->done = true;
    });
    simulationRun = std::move(run);
    return 0;
}

AssemblyObject::SimulationProgress AssemblyObject::simulationProgress() const
{
    SimulationProgress progress;
    if (simulationRun) {
        progress.done = simulationRun->done;
        progress.running = !progress.done;
        progress.time = simulationRun->sink.last;
        progress.frames = simulationRun->sink.count;
    }
    return progress;
}

void AssemblyObject::cancelSimulation()
{
    if (simulationRun) {
        simulationRun->sink.cancel = true;
    }
}

int AssemblyObject::finishSimulation()
{
    if (!simulationRun) {
        return -1;
    }
    simulationRun->join();
    const std::unique_ptr<SimulationRun> run = std::move(simulationRun);
    if (run->status == Solver::Status::Cancelled) {
        Base::Console().message("Assembly: the simulation was cancelled\n");
        return -7;
    }
    App::DocumentObject* sim = *run->sim;
    if (run->status != Solver::Status::Ok || !sim) {
        Base::Console().Error("Generation of simulation failed\n");
        return -1;
    }
    keepFrames(sim, std::move(run->sink.frames), run->parts, run->model, run->spec);
    // The document may have changed while it ran: looked at after the next
    // recompute
    resultsChanged = true;
    return 0;
}

namespace
{
bool sameSpec(const Solver::SimulationSpec& a, const Solver::SimulationSpec& b)
{
    return a.analysis == b.analysis && a.maxStep == b.maxStep && a.restitution == b.restitution
        && a.tStart == b.tStart && a.tEnd == b.tEnd && a.tOutput == b.tOutput
        && a.errorTolerance == b.errorTolerance && a.hMin == b.hMin && a.hMax == b.hMax
        && a.reactions == b.reactions;
}
}  // namespace

bool AssemblyObject::isSimulationStale(App::DocumentObject* sim)
{
    if (!sim || !trajectory || !trajectoryModel || !trajectorySpec || lastSimulation() != sim) {
        return false;
    }
    // A build replaces the session, the model, the spec and the parts the
    // last solve left, which the solve's status and a drag read: they are
    // put back after
    auto keptSession = std::move(session);
    auto keptModel = std::move(solverModel);
    auto keptSpec = std::move(simulationSpec);
    auto keptParts = objectPartMap;
    const std::size_t keptBodies = loadedBodies;
    const int keptBad = badFormulas;
    const bool wasChecking = checkingResults;
    checkingResults = true;
    bool stale = true;
    try {
        if (buildSimulation(sim) == 0 && solverModel && simulationSpec) {
            stale = !Solver::sameMechanism(*trajectoryModel, *solverModel, 1e-9)
                || !sameSpec(*trajectorySpec, *simulationSpec);
        }
    }
    catch (const Base::Exception& e) {
        e.reportException();
    }
    motions.clear();
    session = std::move(keptSession);
    solverModel = std::move(keptModel);
    simulationSpec = std::move(keptSpec);
    objectPartMap = std::move(keptParts);
    loadedBodies = keptBodies;
    badFormulas = keptBad;
    checkingResults = wasChecking;
    return stale;
}

void AssemblyObject::checkResults()
{
    if (!resultsChanged || checkingResults) {
        return;
    }
    resultsChanged = false;
    auto* sim = freecad_cast<Simulation*>(lastSimulation());
    if (!sim) {
        return;
    }
    const bool stale = isSimulationStale(sim);
    if (sim->ResultsStale.getValue() != stale) {
        sim->ResultsStale.setValue(stale);
    }
}

// Not upstream: whether a child of a Simulation is a motion. A child whose
// proxy is of another class is not, whatever its properties are called
// (docs/AssemblyDynamics.md sec 1: later children of a Simulation carry other
// proxies); one without a proxy -- a script's, or a Motion whose module did
// not load -- is read by its properties, as before.
static bool isMotionObject(App::DocumentObject* obj)
{
    if (!obj) {
        return false;
    }
    if (obj->isDerivedFrom<Motion>()) {
        return true;
    }
    if (auto* proxy = dynamic_cast<App::PropertyPythonObject*>(obj->getPropertyByName("Proxy"))) {
        Base::PyGILStateLocker lock;
        try {
            Py::Object value = proxy->getValue();
            if (!value.isNone()) {
                const Py::Object type = value.type();
                return type.hasAttr("__name__")
                    && Py::String(type.getAttr("__name__")).as_std_string() == "Motion";
            }
        }
        catch (Py::Exception&) {
            Base::PyException e;  // clears the Python error
            return false;
        }
    }
    return obj->getPropertyByName("Joint") && obj->getPropertyByName("MotionType")
        && obj->getPropertyByName("Formula");
}

std::vector<App::DocumentObject*> AssemblyObject::getMotionsFromSimulation(App::DocumentObject* sim)
{
    if (!sim) {
        return {};
    }

    auto* prop = sim->isDerivedFrom<Simulation>()
        ? &static_cast<Simulation*>(sim)->Group
        : dynamic_cast<App::PropertyLinkList*>(sim->getPropertyByName("Group"));
    if (!prop) {
        return {};
    }

    // Not upstream: only the Motion objects among the children.
    std::vector<App::DocumentObject*> motions;
    for (auto* obj : prop->getValue()) {
        if (isMotionObject(obj)) {
            motions.push_back(obj);
        }
    }
    return motions;
}

int Assembly::AssemblyObject::updateForFrame(size_t index)
{
    // Plays the last simulation's frame from the stored trajectory.
    if (!trajectory || index >= trajectory->times.size()) {
        return -1;
    }

    const std::size_t nBodies = trajectory->bodies.size();
    std::vector<Solver::BodyPose> poses;
    for (std::size_t i = 0; i < nBodies; ++i) {
        const Solver::Id body = trajectory->bodies[i];
        if (body >= poses.size()) {
            poses.resize(body + 1);
        }
        poses[body] = {body, trajectory->poses[index * nBodies + i]};
    }
    setNewPlacements(trajectoryParts, poses);
    auto jointDocs = getJoints();
    redrawJointPlacements(jointDocs);
    return 0;
}

size_t Assembly::AssemblyObject::numberOfFrames() const
{
    return trajectory ? trajectory->times.size() : 0;
}

bool AssemblyObject::requiresRigidSolveForMove(const std::vector<App::DocumentObject*>& movedParts)
{
    rebuildRigidClusters();

    return std::ranges::any_of(movedParts, [&](App::DocumentObject* part) {
        return getRigidRepresentative(part) != nullptr;
    });
}

void AssemblyObject::beginCollisionWatch()
{
    collisionWatch.reset();
    dragCollisions.clear();
    const long mode = AssemblyParams::getDragCollision();
    if (mode != static_cast<long>(DragCollision::Highlight)
        && mode != static_cast<long>(DragCollision::Stop)) {
        return;
    }
    dragCollisionMode = static_cast<DragCollision>(mode);
    ++collisionWatches;
    dragStopped = false;
    try {
        collisionWatch =
            std::make_unique<CollisionWatch>(this, AssemblyParams::getDragCollisionTolerance());
        if (collisionWatch->initialInterferences() > 0) {
            Base::Console().log("Assembly: %zu pairs of %s interfere already; the drag leaves "
                                "them out.\n",
                                collisionWatch->initialInterferences(),
                                getFullLabel().c_str());
        }
    }
    catch (const Base::Exception& e) {
        FC_WARN("No collision check for this drag: " << e.what());
        collisionWatch.reset();
    }
    catch (const Standard_Failure& e) {
        FC_WARN("No collision check for this drag: " << e.GetMessageString());
        collisionWatch.reset();
    }
}

void AssemblyObject::endCollisionWatch()
{
    collisionWatch.reset();
    dragCollisions.clear();
}

const std::vector<InterferenceResult>& AssemblyObject::checkDragCollisions()
{
    dragCollisions.clear();
    if (collisionWatch) {
        try {
            dragCollisions = collisionWatch->check();
        }
        catch (const Standard_Failure& e) {
            FC_WARN("The collision check of a drag step failed: " << e.GetMessageString());
        }
    }
    if (!stoppingDrag && dragCollisions.empty()) {
        dragStopped = false;
    }
    markDragCollisions();
    return dragCollisions;
}

void AssemblyObject::markDragCollisions()
{
    bool changed = false;
    for (const auto& hit : dragCollisions) {
        if (!hit.touched) {
            continue;
        }
        // This drag's marker of the pair moves to where they meet now: Stop
        // checks nearer the contact as it halves the step
        auto it = std::find_if(collisionMarkers.begin(),
                               collisionMarkers.end(),
                               [&](const CollisionMarker& marker) {
                                   return marker.watch == collisionWatches
                                       && marker.first.getObject() == hit.first
                                       && marker.second.getObject() == hit.second;
                               });
        if (it == collisionMarkers.end()) {
            CollisionMarker marker;
            marker.first = hit.first;
            marker.second = hit.second;
            marker.watch = collisionWatches;
            it = collisionMarkers.insert(collisionMarkers.end(), marker);
        }
        it->centre = hit.touchCentre;
        it->direction = hit.touchNormal;
        it->kind = hit.kind;
        it->volume = hit.volume;
        it->area = hit.touchArea;
        it->distance = hit.distance;
        it->clearance = hit.clearance;
        it->faces1 = hit.faceNames1;
        it->faces2 = hit.faceNames2;
        changed = true;
    }
    if (changed) {
        signalCollisionMarkers();
    }
}

void AssemblyObject::addCollisionMarker(const InterferenceResult& result)
{
    // Checked again where nothing moved: the same marker
    for (const auto& marker : collisionMarkers) {
        if (marker.first.getObject() == result.first && marker.second.getObject() == result.second
            && marker.centre.IsEqual(result.touchCentre, 1e-9) && marker.kind == result.kind) {
            return;
        }
    }
    CollisionMarker marker;
    marker.first = result.first;
    marker.second = result.second;
    marker.centre = result.touchCentre;
    marker.direction = result.touchNormal;
    marker.kind = result.kind;
    marker.volume = result.volume;
    marker.area = result.touchArea;
    marker.distance = result.distance;
    marker.clearance = result.clearance;
    marker.faces1 = result.faceNames1;
    marker.faces2 = result.faceNames2;
    // Of no drag
    marker.watch = 0;
    collisionMarkers.push_back(marker);
    signalCollisionMarkers();
}

void AssemblyObject::clearCollisionMarkers(bool checksOnly)
{
    const auto count = collisionMarkers.size();
    collisionMarkers.erase(std::remove_if(collisionMarkers.begin(),
                                          collisionMarkers.end(),
                                          [&](const CollisionMarker& marker) {
                                              return !checksOnly || marker.watch == 0;
                                          }),
                           collisionMarkers.end());
    if (collisionMarkers.size() != count) {
        signalCollisionMarkers();
    }
}

double AssemblyObject::stopAtDragCollision(const std::function<bool(double)>& poseAt)
{
    Base::StateLocker stopping(stoppingDrag);
    auto hits = dragCollisions;
    double lo = 0.0;
    double hi = 1.0;
    // How far the whole step moves the parts (mm): the first fraction posed
    // went (1 - t) of it from where the step's check had them
    double span = -1.0;
    const double tolerance = collisionWatch ? std::max(collisionWatch->tolerance(), 1e-6) : 0.0;
    auto tryAt = [&](double t) {
        const bool posed = poseAt(t);
        const bool clear = posed && checkDragCollisions().empty();
        if (posed && span < 0.0 && collisionWatch) {
            span = collisionWatch->travel() / (1.0 - t);
        }
        if (clear) {
            lo = t;
            return;
        }
        hi = t;
        if (posed) {
            hits = dragCollisions;
        }
    };
    if (dragStopped) {
        tryAt(1.0 / (1 << dragCollisionHalvings));
    }
    for (int i = 0; i < dragCollisionHalvings; ++i) {
        if (span >= 0.0 && span * (hi - lo) <= tolerance) {
            break;
        }
        tryAt(0.5 * (lo + hi));
    }
    dragStopped = true;
    poseAt(lo);
    dragCollisions = std::move(hits);
    return lo;
}

void AssemblyObject::preDrag(std::vector<App::DocumentObject*> dragParts)
{
    dragStepCount = 0;
    dragStepFailures = 0;
    lastDragStepFailed = false;
    beginCollisionWatch();

    // Bodies Fixed joints hold together are one body in the solve
    // (Solver::groupedRigidly, sec 6.21); a drag needs no bundling of its own.
    bool hasUnconnectedDragPart = std::ranges::any_of(dragParts, [this](App::DocumentObject* part) {
        return part && !isPartConnected(part);
    });

    if (hasUnconnectedDragPart) {
        prepareMbdForIslandDrag(dragParts);
    }
    else {
        solve();
    }

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
    // The GUI's heavier masses for the dragged parts (setObjMasses) become the
    // drag weights of their bodies; the session puts them on the solver's bodies
    // for the drag steps only (OndselSession::beginDrag says why), never into
    // what a static solve is built from.
    if (!session || !solverModel) {
        return;
    }

    std::vector<Solver::Id> bodies;
    for (auto* part : draggedParts) {
        auto it = objectPartMap.find(part);
        if (it == objectPartMap.end() || it->second.body >= loadedBodies) {
            continue;
        }
        const Solver::Id body = it->second.body;
        if (std::ranges::find(bodies, body) == bodies.end()) {
            bodies.push_back(body);
        }
        const double mass = getObjMass(part);
        if (mass <= 0.0 || mass == 1.0) {
            continue;
        }
        solverModel->bodies[body].dragWeight = mass;
    }

    if (session->update(*solverModel) != Solver::Status::Ok) {
        FC_LOG("Assembly: the solver did not take the drag weights");
    }
    session->beginDrag(bodies, {});
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
        session.reset();
        loadedBodies = 0;
        return;
    }

    jointParts(joints);

    makeMbdModel();

    Solver::Result result;
    if (session) {
        result = session->solve({});
    }
    else {
        result.message = "no solver backend";
    }
    if (result.status != Solver::Status::Ok) {
        FC_ERR("Drag setup failed: " << result.message);
        objectPartMap.clear();
        session.reset();
        loadedBodies = 0;
        return;
    }
}

bool AssemblyObject::doDragStep()
{
    if (!session || draggedParts.empty()) {
        return false;
    }

    ++dragStepCount;
    lastDragStepFailed = true;  // until the step below gets through
    std::string failure;
    bool restoreAfterFailedDragStep = false;
    // Stop at a collision: the poses to take a colliding step back to
    std::vector<Solver::BodyPose> before;
    if (collisionWatch && dragCollisionMode == DragCollision::Stop) {
        before = session->poses();
    }

    try {
        // The target of each dragged body: the Placement the caller wrote on
        // its part, taken back to the body's frame for a bundled part.
        std::vector<Solver::BodyPose> targets;
        for (auto& part : draggedParts) {
            if (!part) {
                continue;
            }

            const Solver::Id body = getMbDData(part).body;
            if (body >= loadedBodies) {
                continue;
            }

            if (std::ranges::any_of(targets, [&](const auto& t) { return t.body == body; })) {
                continue;
            }

            Base::Placement plc = getPlacementFromProp(part, "Placement");
            if (auto it = objectPartMap.find(part);
                it != objectPartMap.end() && !it->second.offsetPlc.isIdentity()) {
                plc = plc * it->second.offsetPlc.inverse();
            }
            targets.push_back({body, plc});
        }

        // The session retries a failed step and, when none converges, puts the
        // bodies back where the last step left them.
        Solver::Result result = session->dragStep(targets);
        if (result.status != Solver::Status::Ok) {
            failure = result.message;
            restoreAfterFailedDragStep = true;
        }
        // Timing the validation and placement setting
        else if (auto poses = session->poses(); validateNewPlacements(poses)) {
            setNewPlacements(poses);
            updateRigidPlacementCache();

            auto joints = getJoints();
            for (auto* joint : joints) {
                if (joint->Visibility.getValue()) {
                    // redraw only the moving joint as its quite slow as its python code.
                    redrawJointPlacement(joint);
                }
            }
            lastDragStepFailed = false;

            if (collisionWatch && !checkDragCollisions().empty()
                && dragCollisionMode == DragCollision::Stop) {
                // The step goes only as far as it does without colliding: each
                // fraction of it solved from where the last good step left the
                // bodies, towards the targets moved that fraction of the way
                stopAtDragCollision([&](double t) {
                    if (session->setPoses(before) != Solver::Status::Ok) {
                        FC_WARN("The solver could not take back a step that collides");
                        if (t > 0.0) {
                            return false;
                        }
                    }
                    if (t <= 0.0) {
                        setNewPlacements(before);
                        updateRigidPlacementCache();
                        return true;
                    }
                    std::vector<Solver::BodyPose> partial;
                    for (const auto& target : targets) {
                        const Base::Placement from = target.body < before.size()
                            ? before[target.body].placement
                            : target.placement;
                        partial.push_back(
                            {target.body, Base::Placement::sclerp(from, target.placement, t)});
                    }
                    if (session->dragStep(partial).status != Solver::Status::Ok) {
                        return false;
                    }
                    auto poses = session->poses();
                    if (!validateNewPlacements(poses)) {
                        return false;
                    }
                    setNewPlacements(poses);
                    updateRigidPlacementCache();
                    return true;
                });
                for (auto* joint : joints) {
                    if (joint->Visibility.getValue()) {
                        redrawJointPlacement(joint);
                    }
                }
                const auto& hit = dragCollisions.front();
                failure = std::string("it would make ") + hit.first->getFullLabel()
                    + " collide with " + hit.second->getFullLabel();
                lastDragStepFailed = true;
            }
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

bool AssemblyObject::validateNewPlacements()
{
    return validateNewPlacements(session ? session->poses() : std::vector<Solver::BodyPose> {});
}

bool AssemblyObject::validateNewPlacements(const std::vector<Solver::BodyPose>& poses)
{
    // First we check if a grounded object has moved. It can happen that they flip.
    auto groundedParts = getGroundedParts();
    for (auto* obj : groundedParts) {
        auto* propPlacement = obj->getPlacementProperty();
        if (propPlacement) {
            Base::Placement oldPlc = propPlacement->getValue();

            auto it = objectPartMap.find(obj);
            if (it != objectPartMap.end()) {
                const Solver::Id body = it->second.body;
                Base::Placement newPlacement = body < poses.size() ? poses[body].placement
                                                                   : Base::Placement();
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
    endCollisionWatch();
    if (!session) {
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

    // Do this after last drag
    Solver::Result result = session->endDrag();
    if (result.status != Solver::Status::Ok) {
        FC_ERR("Post-drag solve failed: " << result.message);
    }
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

void AssemblyObject::addDynamics(Solver::Model& model) const
{
    std::map<Solver::Id, std::vector<MassProperties>> byBody;
    for (auto& props : massPropertiesOf(this)) {
        if (props.mass <= 0.0 || !props.obj) {
            continue;
        }
        auto it = objectPartMap.find(props.obj);
        if (it == objectPartMap.end() || it->second.body >= model.bodies.size()) {
            continue;
        }
        byBody[it->second.body].push_back(std::move(props));
    }
    Solver::Dynamics dynamics;
    for (const auto& [body, parts] : byBody) {
        const MassProperties total = totalOf(parts);
        Solver::Inertia inertia;
        inertia.body = body;
        inertia.mass = total.mass;
        inertia.com = total.com;
        inertia.inertia = total.inertia;
        switch (total.origin) {
            case MassProperties::Origin::Override:
                inertia.origin = Solver::Inertia::Origin::Override;
                break;
            case MassProperties::Origin::Assumed:
                inertia.origin = Solver::Inertia::Origin::AssumedDensity;
                break;
            default:
                inertia.origin = Solver::Inertia::Origin::Shape;
                break;
        }
        // From the assembly's frame into the body's
        dynamics.inertias.push_back(Solver::moved(inertia, model.bodies[body].placement.inverse()));
    }
    // Kept: the force elements and the initial velocities addForces() put
    // in first
    if (model.dynamics) {
        dynamics.forces = std::move(model.dynamics->forces);
        dynamics.initial = std::move(model.dynamics->initial);
    }
    model.dynamics = std::move(dynamics);
}

void AssemblyObject::addForces(Solver::Model& model, App::DocumentObject* sim)
{
    auto* typed = freecad_cast<Simulation*>(sim);
    if (!typed) {
        return;
    }
    if (!model.dynamics) {
        model.dynamics = Solver::Dynamics();
    }
    // A frame at a placement on a component (on its body, as the component
    // sits there), or on the ground in the assembly's coordinates
    auto frameOn = [&](App::DocumentObject* component,
                       const Base::Placement& placement,
                       const std::string& name) {
        Solver::Frame frame;
        frame.label = model.addSource(name);
        Base::Placement local = placement;
        if (component) {
            const MbDPartData data = getMbDData(component);
            frame.body = data.body;
            local = data.offsetPlc * local;
        }
        frame.local = local;
        return model.addFrame(frame);
    };
    auto frameAt =
        [&](App::DocumentObject* component, const Base::Vector3d& point, const std::string& name) {
            return frameOn(component, Base::Placement(point, Base::Rotation()), name);
        };
    // Where a frame of the model is, in the assembly's coordinates
    auto placementOf = [&](Solver::Id id) {
        const Solver::Frame& frame = model.frames[id];
        if (frame.body == Solver::NoId || frame.body >= model.bodies.size()) {
            return frame.local;
        }
        return model.bodies[frame.body].placement * frame.local;
    };
    // A placement in the assembly's coordinates, relative to a component as
    // it sits in the model (the ground's: as it is)
    auto relativeTo = [&](App::DocumentObject* component, const Base::Placement& placement) {
        if (!component) {
            return placement;
        }
        const MbDPartData data = getMbDData(component);
        if (data.body == Solver::NoId || data.body >= model.bodies.size()) {
            return placement;
        }
        return (model.bodies[data.body].placement * data.offsetPlc).inverse() * placement;
    };
    // A force or a torque on its frame, its opposite on its reaction's
    // frame there, or the ground's
    auto addLoad = [&](AppliedLoad* load) {
        const std::string name = load->getFullName();
        Base::Placement local;
        auto* component = load->getEnd(local);
        if (!component) {
            Base::Console().warning(
                "Assembly: the load %s acts on no component, and does nothing\n",
                load->getFullLabel().c_str());
            return;
        }
        Base::Vector3d direction = load->Direction.getValue();
        if (direction.Length() < 1e-12) {
            Base::Console().warning("Assembly: the load %s has no direction, and does nothing\n",
                                    load->getFullLabel().c_str());
            return;
        }
        direction.Normalize();
        // The magnitude in internal units (mN, mN mm) times the profile, one
        // formula whose text is the record
        std::string profile = load->Profile.getValue();
        profile.erase(0, profile.find_first_not_of(" \t"));
        profile.erase(profile.find_last_not_of(" \t") + 1);
        std::ostringstream text;
        text.precision(17);
        text << load->Magnitude.getValue() << "*(" << (profile.empty() ? "1" : profile) << ")";
        Solver::FormulaError error;
        Solver::ForceElement force;
        force.magnitude.text = text.str();
        force.magnitude.tree = Solver::FormulaTree::parse(force.magnitude.text, &error);
        force.magnitude.function = Solver::Formula::functionOf(force.magnitude.tree);
        if (!force.magnitude.tree) {
            ++badFormulas;
            Base::Console().error("Assembly: the profile of %s is not valid: %s: %s\n",
                                  load->getFullLabel().c_str(),
                                  error.describe().c_str(),
                                  profile.c_str());
            return;
        }
        force.source = model.addSource(name);
        force.kind =
            load->isTorque() ? Solver::ForceKind::AppliedTorque : Solver::ForceKind::AppliedForce;
        force.frameI = frameOn(component, local, name + "-1");
        auto* reaction = load->Reaction.getValue();
        force.frameJ =
            frameOn(reaction, relativeTo(reaction, placementOf(force.frameI)), name + "-2");
        force.direction = direction;
        force.followsFrame = load->DirectionAxes.getValue() == 0;
        force.id = static_cast<Solver::Id>(model.dynamics->forces.size());
        model.dynamics->forces.push_back(std::move(force));
    };
    // A bushing at its frame, holding its component to the other's frame
    // there, or the ground's
    auto addBushing = [&](Bushing* bushing) {
        const std::string name = bushing->getFullName();
        Base::Placement local;
        auto* component = bushing->getEnd(local);
        if (!component) {
            Base::Console().warning(
                "Assembly: the bushing %s is on no component, and does nothing\n",
                bushing->getFullLabel().c_str());
            return;
        }
        Solver::ForceElement force;
        force.source = model.addSource(name);
        force.kind = Solver::ForceKind::Bushing;
        force.frameI = frameOn(component, local, name + "-1");
        auto* other = bushing->Other.getValue();
        force.frameJ = frameOn(other, relativeTo(other, placementOf(force.frameI)), name + "-2");
        auto put = [](std::array<double, 6>& to,
                      const Base::Vector3d& along,
                      const Base::Vector3d& about) {
            to = {along.x, along.y, along.z, about.x, about.y, about.z};
        };
        put(force.k6,
            bushing->TranslationalStiffness.getValue(),
            bushing->RotationalStiffness.getValue());
        put(force.c6,
            bushing->TranslationalDamping.getValue(),
            bushing->RotationalDamping.getValue());
        put(force.preload6, bushing->Preload.getValue(), bushing->PreloadTorque.getValue());
        force.id = static_cast<Solver::Id>(model.dynamics->forces.size());
        model.dynamics->forces.push_back(std::move(force));
    };
    for (auto* element : typed->Elements.getValues()) {
        if (auto* load = freecad_cast<AppliedLoad*>(element)) {
            addLoad(load);
            continue;
        }
        if (auto* bushing = freecad_cast<Bushing*>(element)) {
            addBushing(bushing);
            continue;
        }
        // On its joint's frames, as the joint's model record has them
        if (auto* torsion = freecad_cast<TorsionSpring*>(element)) {
            auto* joint = torsion->Joint.getValue();
            const std::string jointName = joint ? joint->getFullName() : std::string();
            const Solver::Joint* held = nullptr;
            for (const auto& record : model.joints) {
                if ((record.kind == Solver::JointKind::Revolute
                     || record.kind == Solver::JointKind::Cylindrical)
                    && model.sourceName(record.source) == jointName) {
                    held = &record;
                    break;
                }
            }
            if (!held) {
                Base::Console().warning(
                    "Assembly: the torsion spring %s is on no revolute or cylindrical joint "
                    "the simulation runs, and does nothing\n",
                    torsion->getFullLabel().c_str());
                continue;
            }
            Solver::ForceElement force;
            force.source = model.addSource(torsion->getFullName());
            force.kind = Solver::ForceKind::TorsionSpring;
            force.frameI = held->frameI;
            force.frameJ = held->frameJ;
            force.k = torsion->Stiffness.getValue();
            force.rest = Base::toRadians(torsion->FreeAngle.getValue());
            force.preload = torsion->Preload.getValue();
            force.c = torsion->Damping.getValue();
            force.id = static_cast<Solver::Id>(model.dynamics->forces.size());
            model.dynamics->forces.push_back(force);
            continue;
        }
        auto* spring = freecad_cast<SpringDamper*>(element);
        if (!spring) {
            continue;
        }
        const std::string name = spring->getFullName();
        Solver::ForceElement force;
        force.source = model.addSource(name);
        force.kind = Solver::ForceKind::SpringDamper;
        // An end on an element is the point its last recompute took
        Base::Vector3d point1;
        Base::Vector3d point2;
        auto* component1 = spring->getEnd(1, point1);
        auto* component2 = spring->getEnd(2, point2);
        force.frameI = frameAt(component1, point1, name + "-1");
        force.frameJ = frameAt(component2, point2, name + "-2");
        force.k = spring->Stiffness.getValue();
        force.rest = spring->FreeLength.getValue();
        force.preload = spring->Preload.getValue();
        force.c = spring->Damping.getValue();
        force.id = static_cast<Solver::Id>(model.dynamics->forces.size());
        model.dynamics->forces.push_back(force);
    }

    // The initial velocities, a Dynamic study's children: a static run
    // starts and ends at rest
    if (typed->AnalysisType.getValue() != 1) {
        return;
    }
    for (auto* child : typed->Group.getValues()) {
        auto* condition = freecad_cast<InitialCondition*>(child);
        if (!condition) {
            continue;
        }
        const std::string label = condition->getFullLabel();
        Solver::InitialVelocity initial;
        if (!condition->isJointRate()) {
            auto* component = condition->Component.getValue();
            if (!component) {
                Base::Console().warning(
                    "Assembly: the initial velocity %s is of no component, and does nothing\n",
                    label.c_str());
                continue;
            }
            initial.source = model.addSource(condition->getFullName());
            initial.kind = Solver::InitialVelocity::Kind::Body;
            initial.frameI = frameOn(component, Base::Placement(), condition->getFullName());
            initial.v = condition->LinearVelocity.getValue();
            initial.w = condition->AngularVelocity.getValue() * (std::numbers::pi / 180.0);
            model.dynamics->initial.push_back(initial);
            continue;
        }
        // On its joint's frames, as the joint's model record has them, and
        // not on a joint a motion of that kind drives already
        auto* joint = condition->Joint.getValue();
        const std::string jointName = joint ? joint->getFullName() : std::string();
        const bool angular = condition->MotionType.getValue() == 0;
        const Solver::Joint* held = nullptr;
        for (const auto& record : model.joints) {
            const bool fits = record.kind == Solver::JointKind::Cylindrical
                || record.kind
                    == (angular ? Solver::JointKind::Revolute : Solver::JointKind::Prismatic);
            if (fits && model.sourceName(record.source) == jointName) {
                held = &record;
                break;
            }
        }
        if (!held) {
            Base::Console().warning(
                angular ? "Assembly: the initial velocity %s is on no revolute or cylindrical "
                          "joint the simulation runs, and does nothing\n"
                        : "Assembly: the initial velocity %s is on no slider or cylindrical "
                          "joint the simulation runs, and does nothing\n",
                label.c_str());
            continue;
        }
        const auto driven = std::ranges::find_if(model.drivers, [&](const Solver::Driver& d) {
            return model.sourceName(d.source) == jointName
                && (d.kind == Solver::DriverKind::Both
                    || d.kind
                        == (angular ? Solver::DriverKind::RotationZ
                                    : Solver::DriverKind::TranslationZ));
        });
        if (driven != model.drivers.end()) {
            Base::Console().warning(
                "Assembly: the initial velocity %s is on a joint a motion drives, which "
                "sets its rate; it does nothing\n",
                label.c_str());
            continue;
        }
        initial.source = model.addSource(condition->getFullName());
        initial.kind = angular ? Solver::InitialVelocity::Kind::RotationZ
                               : Solver::InitialVelocity::Kind::TranslationZ;
        initial.frameI = held->frameI;
        initial.frameJ = held->frameJ;
        // An angular rate's internal unit is the degree per second
        initial.rate =
            angular ? Base::toRadians(condition->Rate.getValue()) : condition->Rate.getValue();
        model.dynamics->initial.push_back(initial);
    }
}

std::vector<AssemblyObject::BodyInertia> AssemblyObject::getBodyInertias()
{
    // Built as a solve builds it, as exportAsASMT does
    solverModel = makeMbdAssembly();
    objectPartMap.clear();
    rebuildRigidClusters();
    fixGroundedParts();
    std::vector<App::DocumentObject*> joints = getJoints();
    jointParts(joints);
    makeMbdModel();
    addDynamics(*solverModel);

    std::vector<BodyInertia> result;
    for (const auto& inertia : solverModel->dynamics->inertias) {
        std::vector<App::DocumentObject*> objects;
        for (const auto& [obj, data] : objectPartMap) {
            if (data.body == inertia.body) {
                objects.push_back(obj);
            }
        }
        std::sort(objects.begin(), objects.end(), [](auto* a, auto* b) {
            return std::string(a->getNameInDocument()) < b->getNameInDocument();
        });
        result.push_back(
            {inertia, solverModel->bodies[inertia.body].placement, std::move(objects)});
    }
    return result;
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

    if (!session || session->exportNative(fileName) != Solver::Status::Ok) {
        Base::Console().error(
            "Assembly: the solver backend could not export %s\n",
            fileName.c_str()
        );
    }
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

double AssemblyObject::timeOfFrame(size_t index) const
{
    return trajectory && index < trajectory->times.size() ? trajectory->times[index] : 0.0;
}

bool AssemblyObject::placementAtFrame(App::DocumentObject* obj,
                                      size_t index,
                                      Base::Placement& placement) const
{
    if (!trajectory || index >= trajectory->times.size()) {
        return false;
    }
    auto it = trajectoryParts.find(obj);
    if (it == trajectoryParts.end()) {
        return false;
    }
    const auto& bodies = trajectory->bodies;
    auto at = std::find(bodies.begin(), bodies.end(), it->second.body);
    if (at == bodies.end()) {
        return false;
    }
    placement = trajectory->poses[index * bodies.size() + (at - bodies.begin())];
    if (!it->second.offsetPlc.isIdentity()) {
        placement = placement * it->second.offsetPlc;
    }
    return isFinitePlacement(placement);
}

void AssemblyObject::setNewPlacements()
{
    if (session) {
        setNewPlacements(session->poses());
    }
}

namespace
{
// Whether a solve left the placement where it was, to rounding: within 1e-12
// of the position's size (at least 1 mm) and 1e-12 rad. A solve that moves
// nothing still hands back every pose through the solver's own coordinates,
// their last bits changed, and writing those moved every part and its
// copies -- a solve after a solve, as a recompute after a joint was made,
// paid the write of all of them for nothing.
bool sameToRounding(const Base::Placement& was, const Base::Placement& now)
{
    if (was.isSame(now)) {
        return true;
    }
    const Base::Vector3d& p = was.getPosition();
    if ((now.getPosition() - p).Length() > 1e-12 * std::max(1.0, p.Length())) {
        return false;
    }
    double x {}, y {}, z {}, w {};
    (was.getRotation().inverse() * now.getRotation()).getValue(x, y, z, w);
    return 2.0 * std::atan2(std::sqrt(x * x + y * y + z * z), std::abs(w)) <= 1e-12;
}
}  // namespace

void AssemblyObject::setNewPlacements(const std::vector<Solver::BodyPose>& poses)
{
    setNewPlacements(objectPartMap, poses);
}

void AssemblyObject::setNewPlacements(
    const std::unordered_map<App::DocumentObject*, MbDPartData>& parts,
    const std::vector<Solver::BodyPose>& poses
)
{
    // The copies that follow the components, found once per assembly: a scan
    // of the assembly's children for each part moved made a drag step of a
    // chain of 200 parts spend 0.3 s here, five times the solve.
    std::unordered_map<App::Part*, Joint::Copies> copies;
    bool moved = false;
    auto copiesOf = [&](App::Part* assembly) -> const Joint::Copies& {
        auto it = copies.find(assembly);
        if (it == copies.end()) {
            it = copies.emplace(assembly, Joint::copiesOf(assembly)).first;
        }
        return it->second;
    };
    for (auto& pair : parts) {
        App::DocumentObject* obj = pair.first;
        const Solver::Id body = pair.second.body;

        // poses[i] is body i's; a body without a pose is skipped.
        if (!obj || body >= poses.size() || poses[body].body != body) {
            continue;
        }

        // Check if the object has a "Placement" property
        auto* propPlacement = obj->getPlacementProperty();
        if (!propPlacement) {
            continue;
        }


        Base::Placement newPlacement = poses[body].placement;
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
        if (!sameToRounding(propPlacement->getValue(), newPlacement)) {
            const Base::Placement delta = newPlacement * propPlacement->getValue().inverse();
            propPlacement->setValue(newPlacement);
            obj->purgeTouched();
            moved = true;
            // The copies that follow it: in this assembly, and, for a part
            // of a flexible sub-assembly instance, in the instance too, the
            // move then said in each one's coordinate system
            auto* container = Base::freecad_dynamic_cast<App::Part>(
                App::GeoFeatureGroupExtension::getGroupOfObject(obj));
            if (container && container != this) {
                Joint::relocateCopies(copiesOf(container), obj, delta);
                // Down the placements, not the global ones: those throw
                // while this assembly recomputes, as a solve on recompute is
                Base::Placement inThis;
                for (App::DocumentObject* step = container; step && step != this;
                     step = App::GeoFeatureGroupExtension::getGroupOfObject(step)) {
                    inThis = getPlacementFromProp(step, "Placement") * inThis;
                }
                Joint::relocateCopies(copiesOf(this), obj, inThis * delta * inThis.inverse());
            }
            else {
                Joint::relocateCopies(copiesOf(this), obj, delta);
            }
        }
    }
    // The shapes the assembly cached by its components' paths are where the
    // components stood: the cache waits for the assembly to change, and a
    // placement written here touches nothing. The mass properties read
    // through it, and a dynamic run after a played frame had its inertias
    // where the parts had been.
    if (moved) {
        if (auto* cache = ::Part::PropertyShapeCache::get(this, false)) {
            Base::PyGILStateLocker lock;
            cache->setPyObject(Py_None);
        }
    }
}

void AssemblyObject::redrawJointPlacements(std::vector<App::DocumentObject*> joints)
{
    // Notify the joint objects that the transform of the coin object changed.
    // An instance joint has no markers: its multiplied joint draws them, once
    std::vector<App::DocumentObject*> done;
    for (auto* joint : joints) {
        if (auto typed = freecad_cast<Joint*>(joint)) {
            if (auto multiplier = typed->getMultiplier()) {
                joint = multiplier;
            }
        }
        if (!joint || std::find(done.begin(), done.end(), joint) != done.end()) {
            continue;
        }
        done.push_back(joint);
        redrawJointPlacement(joint);
    }
}

void AssemblyObject::redrawJointPlacement(App::DocumentObject* joint)
{
    if (!joint) {
        return;
    }
    // A C++ joint's view draws its markers in C++: told by a signal, not a
    // Python call per joint each solve and drag step
    if (auto typed = freecad_cast<Joint*>(joint)) {
        typed->signalFramesMoved();
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
    badFormulas = 0;

    // A build starts with nothing solved, as it always did: a status query or
    // playback before the model is handed over finds no session.
    session.reset();
    loadedBodies = 0;
    return assembly;
}

void AssemblyObject::makeMbdModel()
{
    loadedBodies = 0;
    Solver::Backend* backend = Solver::Registry::instance().select(this);
    // A dynamic or static run, or a kinematic one with its reactions, on the
    // first backend that runs dynamics, whichever the assembly solves with
    if (simulationSpec
        && (simulationSpec->analysis != Solver::Analysis::Kinematic || simulationSpec->reactions)
        && !(backend && backend->capabilities().dynamics)) {
        backend = nullptr;
        auto& registry = Solver::Registry::instance();
        for (const auto& name : registry.names()) {
            auto* candidate = registry.find(name);
            if (candidate && candidate->capabilities().dynamics) {
                backend = candidate;
                break;
            }
        }
        if (!backend) {
            FC_ERR("Assembly: no solver backend runs a dynamic analysis (Chrono's is not built)");
            session.reset();
            return;
        }
    }
    session = backend
        ? Solver::groupedRigidly(Solver::centredOnJoints(backend->createSession()))
        : nullptr;
    if (!session) {
        FC_ERR("Assembly: no solver backend is registered");
        return;
    }
    if (session->load(*solverModel) != Solver::Status::Ok) {
        FC_ERR("Assembly: the solver backend '" << backend->name() << "' did not take the model");
        return;
    }
    loadedBodies = solverModel->bodies.size();
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

namespace
{
/// Upstream's groups an assembly owns, in a file not converted: the ones its
/// deletion deletes (the simulation group stays)
bool ownedLegacyGroup(const App::DocumentObject* obj)
{
    return obj && (obj->is<JointGroup>() || obj->is<ViewGroup>() || obj->is<BomGroup>());
}
}  // namespace

std::vector<App::DocumentObject*> AssemblyObject::getOwnedObjects() const
{
    auto owned = App::Part::getOwnedObjects();
    for (auto obj : Group.getValues()) {
        if (ownedLegacyGroup(obj)) {
            owned.push_back(obj);
        }
    }
    return owned;
}

App::DocumentObject* AssemblyObject::getOwnedCounterpart(const App::DocumentObject* owned,
                                                         const App::DocumentObject* liveOwner,
                                                         App::OwnedPaste& how) const
{
    auto live = freecad_cast<const AssemblyObject*>(liveOwner);
    if (live && ownedLegacyGroup(owned)) {
        how = App::OwnedPaste::FollowOwner;
        for (auto obj : live->Group.getValues()) {
            if (obj && obj->getTypeId() == owned->getTypeId()) {
                return obj;
            }
        }
        return nullptr;
    }
    return App::Part::getOwnedCounterpart(owned, liveOwner, how);
}

// Not upstream: whether an object's proxy is of one of the classes, in the
// module: upstream's Python objects, in a file not converted or made by a
// script, take their role from it
static bool proxyClassIn(const App::DocumentObject* obj,
                         const char* module,
                         std::initializer_list<const char*> classes)
{
    auto* proxy = dynamic_cast<App::PropertyPythonObject*>(obj->getPropertyByName("Proxy"));
    if (!proxy) {
        return false;
    }
    Base::PyGILStateLocker lock;
    try {
        Py::Object value = proxy->getValue();
        if (value.isNone()) {
            return false;
        }
        const Py::Object type = value.type();
        if (!type.hasAttr("__module__")
            || Py::String(type.getAttr("__module__")).as_std_string() != module) {
            return false;
        }
        const std::string name = Py::String(type.getAttr("__name__")).as_std_string();
        for (const char* cls : classes) {
            if (name == cls) {
                return true;
            }
        }
    }
    catch (Py::Exception&) {
        Base::PyException e;  // clears the Python error
    }
    return false;
}

bool AssemblyObject::isAuxGroupAllowed(const App::AuxGroup* group,
                                       const App::DocumentObject* obj) const
{
    if (!group || !obj || obj->isDerivedFrom<App::AuxGroup>()) {
        return false;
    }
    const auto& role = group->getRole();
    if (role == AssemblyGroup::JointsRole) {
        // Not a multiplied joint's instance joints: the joint stands for them
        if (auto joint = freecad_cast<const Joint*>(obj)) {
            return !joint->isInstance();
        }
        return obj->isDerivedFrom<GroundedJoint>()
            || obj->isDerivedFrom<RigidGroup>()
            || proxyClassIn(obj, "JointObject", {"Joint", "GroundedJoint", "RigidGroupJoint"});
    }
    if (role == AssemblyGroup::PhysicsRole) {
        return obj->isDerivedFrom<PhysicsItem>();
    }
    if (role == AssemblyGroup::SimulationsRole) {
        return obj->isDerivedFrom<Simulation>()
            || proxyClassIn(obj, "CommandCreateSimulation", {"Simulation"});
    }
    if (role == AssemblyGroup::ViewsRole) {
        return obj->isDerivedFrom<ExplodedView>()
            || proxyClassIn(obj, "CommandCreateView", {"ExplodedView"});
    }
    if (role == App::Part::PartsRole) {
        // The components only: not a joint's element binding, nor what a group
        // of its own holds (a simulation's motion, a view's step, a snapshot
        // in upstream's group), nor upstream's groups
        if (!isComponentObject(obj) || App::GroupExtension::getGroupOfObject(obj)) {
            return false;
        }
    }
    return App::Part::isAuxGroupAllowed(group, obj);
}

App::DocumentObject* AssemblyObject::getAssemblyGroup(const char* role, bool create)
{
    if (!role) {
        return nullptr;
    }
    if (auto group = getAuxGroup(role)) {
        return group;
    }
    // Upstream's, in a file not converted
    Base::Type legacy;
    const char* label = nullptr;
    if (strcmp(role, AssemblyGroup::JointsRole) == 0) {
        legacy = JointGroup::getClassTypeId();
        label = "Joints";
    }
    else if (strcmp(role, AssemblyGroup::SimulationsRole) == 0) {
        legacy = SimulationGroup::getClassTypeId();
        label = "Simulations";
    }
    else if (strcmp(role, AssemblyGroup::ViewsRole) == 0) {
        legacy = ViewGroup::getClassTypeId();
        label = "Exploded Views";
    }
    else if (strcmp(role, AssemblyGroup::PhysicsRole) == 0) {
        // New with the dynamics phase: upstream has none
        label = "Physics";
    }
    else {
        return nullptr;
    }
    for (auto child : Group.getValues()) {
        if (child && !legacy.isBad() && child->getTypeId() == legacy) {
            return child;
        }
    }
    if (!create || !getDocument()) {
        return nullptr;
    }
    auto group = static_cast<AssemblyGroup*>(getDocument()->addObject("Assembly::AssemblyGroup", label));
    // The role first: a group with none is a user's, whose label is unique
    group->Role.setValue(role);
    group->Label.setValue(label);
    // A group the assembly makes is its own child, whatever its GroupMode
    auto children = Group.getValues();
    children.push_back(group);
    Group.setValues(children);
    group->setOwner(this);
    return group;
}

App::DocumentObject* AssemblyObject::getJointGroup() const
{
    return const_cast<AssemblyObject*>(this)->getAssemblyGroup(AssemblyGroup::JointsRole);
}

std::vector<App::DocumentObject*> AssemblyObject::getJointGroupMembers() const
{
    auto group = getJointGroup();
    auto prop = group ? group->getPropertyByName<App::PropertyLinkList>("Group") : nullptr;
    std::vector<App::DocumentObject*> members;
    if (!prop) {
        return members;
    }
    for (auto obj : prop->getValues()) {
        // A multiplied joint is its instance joints, one per pair
        auto joint = freecad_cast<Joint*>(obj);
        auto instances = joint ? joint->getInstances() : std::vector<Joint*>();
        if (instances.empty()) {
            members.push_back(obj);
        }
        else {
            members.insert(members.end(), instances.begin(), instances.end());
        }
    }
    return members;
}

App::DocumentObject* AssemblyObject::getExplodedViewGroup() const
{
    return const_cast<AssemblyObject*>(this)->getAssemblyGroup(AssemblyGroup::ViewsRole);
}

std::vector<App::DocumentObject*> AssemblyObject::getJoints(bool delBadJoints, bool subJoints)
{
    std::vector<App::DocumentObject*> joints = {};

    const auto jointGroupMembers = getJointGroupMembers();

    Base::PyGILStateLocker lock;
    for (auto joint : jointGroupMembers) {
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
        if (!part1 || !part2 || part1 == part2) {
            // Remove incomplete joints. Left-over when the user deletes a part.
            // Remove incoherent joints (self-pointing joints)
            if (delBadJoints) {
                getDocument()->removeObject(joint->getNameInDocument());
            }
            continue;
        }

        if (joint->isDerivedFrom<Joint>()) {
            joints.push_back(joint);
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

    const auto jointGroupMembers = getJointGroupMembers();

    Base::PyGILStateLocker lock;
    for (auto* joint : jointGroupMembers) {
        const bool typed = joint && joint->isDerivedFrom<Joint>();
        if (!joint || (!typed && !joint->getPropertyByName("Reference1"))) {
            continue;
        }
        auto* suppressed = dynamic_cast<App::PropertyBool*>(joint->getPropertyByName("Suppressed"));
        if (!suppressed || suppressed->getValue()) {
            continue;
        }
        auto* proxy = dynamic_cast<App::PropertyPythonObject*>(joint->getPropertyByName("Proxy"));
        if (!typed && (!proxy || !proxy->getValue().hasAttr("setJointConnectors"))) {
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
    if (int side = typedJointSide(joint, propName)) {
        // Up to date, or the element must resolve, or the element binding keep
        // a copy of it
        auto* typed = static_cast<Joint*>(joint);
        App::DocumentObject* component = nullptr;
        Base::Placement placement;
        Base::Placement local;
        return typed->getStoredSide(side, component, placement, local)
            || typed->getSide(side).isValid();
    }
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

    const auto jointGroupMembers = getJointGroupMembers();

    Base::PyGILStateLocker lock;
    for (auto obj : jointGroupMembers) {
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

    const auto jointGroupMembers = getJointGroupMembers();

    Base::PyGILStateLocker lock;
    for (auto const obj : jointGroupMembers) {
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

    // By name, not in the set's order, which follows the objects' addresses:
    // the order the solver's bodies are made in is the order of its
    // columns, and a solve whose columns came in another order could end
    // in other last bits from one run to the next.
    std::vector<std::pair<std::string, App::DocumentObject*>> ordered;
    for (auto obj : groundedParts) {
        if (obj) {
            ordered.emplace_back(obj->getFullName(), obj);
        }
    }
    std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });
    for (auto& [name, obj] : ordered) {
        Base::Placement plc = getPlacementFromProp(obj, "Placement");
        std::string str = name;
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

namespace
{
/** Who is joined to whom, built once per query from the joints' sides.
 *
 * The walks it serves were a scan of every joint, resolving both sides of
 * each, per part visited, with a linear search of the parts reached, and
 * answered "connected without this joint" by writing Suppressed on joints
 * and back (docs/AssemblyAudit.md sec 4.3, F3): O(P*J) a query, and a joint
 * built O(N) queries. Here: each joint's sides once, then a walk of the
 * parts reached, leaving out the joints asked to.
 */
class Connectivity
{
public:
    using RigidMembers
        = std::function<const std::vector<App::DocumentObject*>*(App::DocumentObject*)>;

    struct Edge
    {
        App::DocumentObject* other;
        App::DocumentObject* joint;
        App::PropertyXLinkSub* ref;
    };

    Connectivity(AssemblyObject* assembly,
                 const std::vector<App::DocumentObject*>& joints,
                 RigidMembers rigidMembers)
        : rigidMembers(std::move(rigidMembers))
    {
        for (auto* joint : joints) {
            if (!joint) {
                continue;
            }
            auto* obj1 = getMovingPartFromRef(joint, "Reference1");
            auto* obj2 = getMovingPartFromRef(joint, "Reference2");
            parts[joint] = {obj1, obj2};
            if (!obj1 || !obj2 || !assembly->isJointTypeConnecting(joint)) {
                continue;
            }
            const bool typed = joint->isDerivedFrom<Joint>();
            auto* ref1 = dynamic_cast<App::PropertyXLinkSub*>(joint->getPropertyByName("Reference1"));
            auto* ref2 = dynamic_cast<App::PropertyXLinkSub*>(joint->getPropertyByName("Reference2"));
            if (typed || ref2) {
                edges[obj1].push_back({obj2, joint, ref2});
            }
            if (typed || ref1) {
                edges[obj2].push_back({obj1, joint, ref1});
            }
        }
    }

    /// The two parts a joint moves, as getMovingPartFromRef() gives them
    std::pair<App::DocumentObject*, App::DocumentObject*> partsOf(App::DocumentObject* joint) const
    {
        auto it = parts.find(joint);
        return it == parts.end()
            ? std::pair<App::DocumentObject*, App::DocumentObject*>(nullptr, nullptr)
            : it->second;
    }

    /// The connecting joints of a part
    std::vector<App::DocumentObject*> jointsOf(App::DocumentObject* part) const
    {
        std::vector<App::DocumentObject*> res;
        auto it = edges.find(part);
        if (it != edges.end()) {
            for (const auto& edge : it->second) {
                res.push_back(edge.joint);
            }
        }
        return res;
    }

    /** What the seeds reach, through the joints but the excluded, and the
     * rigid groups: the seeds first, then in the order reached, each with
     * the reference it was reached by
     */
    std::vector<ObjRef> reach(const std::vector<ObjRef>& seeds,
                              const std::unordered_set<App::DocumentObject*>& excluded = {}) const
    {
        std::vector<ObjRef> res;
        std::unordered_set<App::DocumentObject*> seen;
        for (const auto& seed : seeds) {
            if (seed.obj && seen.insert(seed.obj).second) {
                res.push_back(seed);
            }
        }
        for (size_t i = 0; i < res.size(); ++i) {
            auto* part = res[i].obj;
            auto it = edges.find(part);
            if (it != edges.end()) {
                for (const auto& edge : it->second) {
                    if (!excluded.count(edge.joint) && seen.insert(edge.other).second) {
                        res.push_back({edge.other, edge.ref});
                    }
                }
            }
            if (const auto* members = rigidMembers ? rigidMembers(part) : nullptr) {
                for (auto* member : *members) {
                    if (member && seen.insert(member).second) {
                        res.push_back({member, nullptr});
                    }
                }
            }
        }
        return res;
    }

    /// What the seeds reach, as a set
    std::unordered_set<App::DocumentObject*>
    reachSet(const std::unordered_set<App::DocumentObject*>& seeds,
             const std::unordered_set<App::DocumentObject*>& excluded = {}) const
    {
        std::vector<ObjRef> start;
        start.reserve(seeds.size());
        for (auto* seed : seeds) {
            start.push_back({seed, nullptr});
        }
        std::unordered_set<App::DocumentObject*> res;
        for (const auto& ref : reach(start, excluded)) {
            res.insert(ref.obj);
        }
        return res;
    }

private:
    RigidMembers rigidMembers;
    std::unordered_map<App::DocumentObject*, std::vector<Edge>> edges;
    std::unordered_map<App::DocumentObject*,
                       std::pair<App::DocumentObject*, App::DocumentObject*>>
        parts;
};
}  // namespace

bool AssemblyObject::isJointConnectingPartToGround(App::DocumentObject* joint, const char* propname)
{
    if (!joint || !isJointTypeConnecting(joint)) {
        return false;
    }

    App::DocumentObject* part = getMovingPartFromRef(joint, propname);
    if (!part) {
        return false;
    }

    // Not grounded itself, connected, and still connected with the part's
    // other joints left out: this joint takes it to ground
    auto grounded = getGroundedParts();
    if (grounded.count(part)) {
        return false;
    }
    const Connectivity index(this, getJoints(), [this](App::DocumentObject* obj) {
        return getRigidMembers(obj);
    });
    if (!index.reachSet(grounded).count(part)) {
        return false;
    }
    std::unordered_set<App::DocumentObject*> others;
    for (auto* jointi : index.jointsOf(part)) {
        if (jointi != joint) {
            others.insert(jointi);
        }
    }
    return index.reachSet(grounded, others).count(part) > 0;
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
    const Connectivity index(this, joints, [this](App::DocumentObject* obj) {
        return getRigidMembers(obj);
    });
    const auto connected = index.reachSet(groundedObjs);
    joints.erase(
        std::remove_if(
            joints.begin(),
            joints.end(),
            [&](App::DocumentObject* joint) {
                auto [obj1, obj2] = index.partsOf(joint);
                return !connected.count(obj1) || !connected.count(obj2);
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
    const Connectivity index(this, joints, [this](App::DocumentObject* obj) {
        return getRigidMembers(obj);
    });
    std::unordered_set<App::DocumentObject*> known;
    for (const auto& ref : connectedParts) {
        known.insert(ref.obj);
    }
    for (const auto& ref : index.reach({{currentObj, nullptr}})) {
        if (known.insert(ref.obj).second) {
            connectedParts.push_back(ref);
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

        const bool typed = joint->isDerivedFrom<Joint>();
        if (obj1 == part) {
            auto* ref = dynamic_cast<App::PropertyXLinkSub*>(joint->getPropertyByName("Reference2"));
            if (!ref && !typed) {
                continue;
            }
            connectedParts.push_back({obj2, ref});
        }
        else if (obj2 == part) {
            auto* ref = dynamic_cast<App::PropertyXLinkSub*>(joint->getPropertyByName("Reference1"));
            if (!ref && !typed) {
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

    return getGroundedParts().count(obj) > 0;
}

bool AssemblyObject::isPartConnected(App::DocumentObject* obj)
{
    if (!obj) {
        return false;
    }

    if (!connectedCache) {
        const auto grounded = getGroundedParts();
        const Connectivity index(this, getJoints(), [this](App::DocumentObject* part) {
            return getRigidMembers(part);
        });
        auto reached = index.reachSet(grounded);
        reached.insert(grounded.begin(), grounded.end());
        connectedCache = std::move(reached);
    }
    return connectedCache->count(obj) > 0;
}

void AssemblyObject::onSettingDocument()
{
    App::Part::onSettingDocument();
    App::Document* doc = getDocument();
    if (!doc) {
        return;
    }
    connConnectivityChanged = doc->signalChangedObject.connect(
        [this](const App::DocumentObject&, const App::Property& prop) {
            if (!connectedCache) {
                return;
            }
            const char* name = prop.getName();
            if (name
                && (std::strcmp(name, "Placement") == 0 || std::strcmp(name, "Shape") == 0
                    || std::strcmp(name, "Visibility") == 0)) {
                return;
            }
            connectedCache.reset();
        }
    );
    auto drop = [this](const App::DocumentObject&) {
        connectedCache.reset();
    };
    connConnectivityNew = doc->signalNewObject.connect(drop);
    connConnectivityDeleted = doc->signalDeletedObject.connect(drop);
    // The stale flag (docs/AssemblyDynamics.md sec 8.4): a change while
    // frames are kept is looked at after the recompute that follows it
    connResultsChanged = doc->signalChangedObject.connect(
        [this](const App::DocumentObject&, const App::Property& prop) {
            if (!trajectory || checkingResults) {
                return;
            }
            const char* name = prop.getName();
            if (name && std::strcmp(name, "ResultsStale") == 0) {
                return;
            }
            resultsChanged = true;
        });
    connResultsRecomputed = doc->signalRecomputed.connect(
        [this](const App::Document&, const std::vector<App::DocumentObject*>&) {
            checkResults();
        });
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
    if (auto* typed = freecad_cast<Simulation*>(sim)) {
        switch (typed->AnalysisType.getValue()) {
            case 1:
                mbdSim->analysis = Solver::Analysis::Dynamic;
                break;
            case 2:
                mbdSim->analysis = Solver::Analysis::Static;
                break;
            default:
                mbdSim->analysis = Solver::Analysis::Kinematic;
                break;
        }
        mbdSim->tStart = typed->TimeStart.getValue();
        mbdSim->tEnd = typed->TimeEnd.getValue();
        mbdSim->tOutput = typed->TimeStepOutput.getValue();
        mbdSim->hMin = 1.0e-9;
        mbdSim->hMax = 1.0;
        mbdSim->errorTolerance = typed->GlobalErrorTolerance.getValue();
        mbdSim->restitution = typed->Restitution.getValue();
        mbdSim->reactions =
            mbdSim->analysis == Solver::Analysis::Kinematic && typed->Reactions.getValue();
        simulationSpec = std::move(mbdSim);
        return;
    }
    // Upstream's Python simulation, by its property names
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

        // Not upstream: the curve and the offset come with the frames
        // (makeMbdJoint, addCamCurve).
        case JointType::Cam:
            return makeJoint(Solver::JointKind::PointOnCurve);

        default:
            return nullptr;
    }
}

std::shared_ptr<Solver::Joint> AssemblyObject::makeMbdJointDistance(App::DocumentObject* joint)
{
    DistanceType type = getDistanceType(joint, &jcsSwapped);

    std::string elt1 = getElementFromProp(joint, jcsRef(1));
    std::string elt2 = getElementFromProp(joint, jcsRef(2));
    auto* obj1 = getLinkedObjFromRef(joint, jcsRef(1));
    auto* obj2 = getLinkedObjFromRef(joint, jcsRef(2));

    // Sec 6.11: a joint the task would refuse (distanceJointRefusal) and that
    // is already in a document solves as before, said once; one of a sub-type
    // the role mappings added solves with its mapping, logged once.
    const std::string refusal = distanceJointRefusal(joint, type);
    if (!refusal.empty()) {
        if (warnedRefusedDistance.insert(joint->getFullName() + "/" + distanceTypeName(type))
                .second) {
            Base::Console().warning(
                "Assembly: the Distance joint %s is between elements (%s) the solver has "
                "no mapping for: %s. It is solved as before; new joints of this kind are "
                "refused.\n",
                joint->getFullName().c_str(),
                distanceTypeName(type),
                refusal.c_str()
            );
        }
    }
    else if (const char* meaning = distanceMeaning(type)) {
        const std::string key = "mapped:" + joint->getFullName() + "/" + distanceTypeName(type);
        if (warnedRefusedDistance.insert(key).second) {
            Base::Console().log(
                "Assembly: the Distance joint %s (%s) is solved as %s; before the role "
                "mappings (2026-10-23) it was a planar distance.\n",
                joint->getFullName().c_str(),
                distanceTypeName(type),
                meaning
            );
        }
    }
    // What the default case has always given.
    auto planarDefault = [&]() {
        auto mbdJoint = makeJoint(Solver::JointKind::Planar);
        mbdJoint->value = getJointDistance(joint);
        return mbdJoint;
    };

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

        // Audit sec 8.1 decision 7 (not upstream): these three fell to the
        // default Planar(d).
        case DistanceType::LineCylinder: {
            // I is the cylinder (its JCS on the axis), J the line (its JCS z
            // along it): the line parallel to the axis, d + r from it.
            auto mbdJoint = makeJoint(Solver::JointKind::LineLineDistance);
            mbdJoint->value = getJointDistance(joint) + getFaceRadius(obj1, elt1);
            return mbdJoint;
        }

        case DistanceType::LineSphere: {
            // The sphere's centre d + r from the line. PointLineDistance takes
            // the line from frame I and the point from J, so the line (J after
            // getDistanceType's ordering) becomes I.
            double radius = getFaceRadius(obj1, elt1);
            swapJCS(joint);
            auto mbdJoint = makeJoint(Solver::JointKind::PointLineDistance);
            mbdJoint->value = getJointDistance(joint) + radius;
            return mbdJoint;
        }

        case DistanceType::CurvePlane: {
            // I is the plane, J the curve, whose JCS is the curve's plane (a
            // circle's centre and normal; for another planar curve its plane
            // through its centre of mass, UtilsAssembly.findPlacement): the
            // two planes parallel, d apart, as PlanePlane.
            auto mbdJoint = makeJoint(Solver::JointKind::Planar);
            mbdJoint->value = getJointDistance(joint);
            return mbdJoint;
        }

        // Sec 6.11, the role mappings (not upstream): each reuses an existing
        // mapping with one element read the way that mapping reads its
        // kind. A cone stands for its axis, as a cylinder of radius 0 (its
        // JCS is on the axis, at the apex, z along it); a torus for its axis,
        // radius 0, as CylinderTorus and TorusSphere read it; a circle for its
        // axis and radius, as LineCircle and CircleCircle read it.
        case DistanceType::PlaneCone: {
            // As PlaneCylinder: the cone's axis in a plane d off the plane.
            auto mbdJoint = makeJoint(Solver::JointKind::LineInPlane);
            mbdJoint->value = getJointDistance(joint);
            return mbdJoint;
        }

        case DistanceType::CylinderCone: {
            // As CylinderCylinder: the cone's axis parallel, d + r1 from the
            // cylinder's axis.
            auto mbdJoint = makeJoint(Solver::JointKind::LineLineDistance);
            mbdJoint->value = getJointDistance(joint) + getFaceRadius(obj1, elt1);
            return mbdJoint;
        }

        case DistanceType::ConeCone:
        case DistanceType::ConeTorus:
        case DistanceType::LineCone:
        case DistanceType::LineTorus: {
            // Two axes (or an axis and the line) parallel, d apart.
            auto mbdJoint = makeJoint(Solver::JointKind::LineLineDistance);
            mbdJoint->value = getJointDistance(joint);
            return mbdJoint;
        }

        case DistanceType::ConeSphere: {
            // As CylinderSphere: the sphere's centre d + r2 from the cone's axis.
            auto mbdJoint = makeJoint(Solver::JointKind::PointLineDistance);
            mbdJoint->value = getJointDistance(joint) + getFaceRadius(obj2, elt2);
            return mbdJoint;
        }

        case DistanceType::PointCone:
        case DistanceType::PointTorus: {
            // As PointCylinder: the point d from the axis.
            auto mbdJoint = makeJoint(Solver::JointKind::PointLineDistance);
            mbdJoint->value = getJointDistance(joint);
            return mbdJoint;
        }

        case DistanceType::CurveCylinder: {
            if (!refusal.empty()) {
                return planarDefault();
            }
            // As CylinderCylinder: I the cylinder, J the circle (its centre
            // and normal).
            auto mbdJoint = makeJoint(Solver::JointKind::LineLineDistance);
            mbdJoint->value = getJointDistance(joint) + getFaceRadius(obj1, elt1)
                + getEdgeRadius(obj2, elt2);
            return mbdJoint;
        }

        case DistanceType::CurveSphere: {
            if (!refusal.empty()) {
                return planarDefault();
            }
            // As CylinderSphere, the circle's axis frame I: swapped, as
            // LineSphere.
            double radius = getFaceRadius(obj1, elt1) + getEdgeRadius(obj2, elt2);
            swapJCS(joint);
            auto mbdJoint = makeJoint(Solver::JointKind::PointLineDistance);
            mbdJoint->value = getJointDistance(joint) + radius;
            return mbdJoint;
        }

        case DistanceType::CurveCone:
        case DistanceType::CurveTorus: {
            if (!refusal.empty()) {
                return planarDefault();
            }
            // As LineCircle: the axes parallel, d + r2 apart.
            auto mbdJoint = makeJoint(Solver::JointKind::LineLineDistance);
            mbdJoint->value = getJointDistance(joint) + getEdgeRadius(obj2, elt2);
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
            // Only Other gets here (audit sec 8.1 decision 7, sec 6.11): between
            // two elements it is refused for a new joint and warned about above;
            // with a whole object on a side it is the objects' planar distance.
            return planarDefault();
        }
    }
}

std::vector<std::shared_ptr<Solver::Joint>> AssemblyObject::makeMbdJoint(App::DocumentObject* joint)
{
    if (!joint) {
        return {};
    }

    JointType jointType = getJointType(joint);

    jcsSwapped = false;
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
        fullMarkerNameI = handleOneSideOfJoint(joint, jcsRef(1), jcsPlc(1));
        fullMarkerNameJ = handleOneSideOfJoint(joint, jcsRef(2), jcsPlc(2));
    }
    if (fullMarkerNameI == Solver::NoId || fullMarkerNameJ == Solver::NoId) {
        return {};
    }

    mbdJoint->source = solverModel->addSource(joint->getFullName());
    mbdJoint->frameI = fullMarkerNameI;
    mbdJoint->frameJ = fullMarkerNameJ;
    // Not upstream: a cam holds its follower on the cam's curve (CamJoint.cpp).
    if (jointType == JointType::Cam
        && !addCamCurve(
            joint,
            *solverModel,
            *mbdJoint,
            getMbDData(getMovingPartFromRef(joint, jcsRef(1))).offsetPlc
        )) {
        return {};
    }

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

                // In the model only: the document keeps what the user typed.
                if ((minLength > maxLength) && minEnabled && maxEnabled) {
                    std::swap(minLength, maxLength);
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
                // In the model only: the document keeps what the user typed.
                if ((minAngle > maxAngle) && minEnabled && maxEnabled) {
                    std::swap(minAngle, maxAngle);
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

    // A motion's joint, type and formula: typed on the C++ motion, by name
    // on upstream's Python one. False when it is not a motion or has none
    struct MotionData
    {
        App::DocumentObject* joint = nullptr;
        std::string type;
        std::string formula;
        Solver::MotorControl control = Solver::MotorControl::Position;
    };
    auto readMotion = [](App::DocumentObject* motion, MotionData& data) {
        if (auto* typed = freecad_cast<Motion*>(motion)) {
            data.joint = typed->Joint.getValue();
            data.type = typed->MotionType.getValueAsString();
            data.formula = typed->Formula.getValue();
            data.control = static_cast<Solver::MotorControl>(typed->Control.getValue());
            return true;
        }
        auto* pJoint = dynamic_cast<App::PropertyXLinkSub*>(motion->getPropertyByName("Joint"));
        auto* pType = dynamic_cast<App::PropertyEnumeration*>(motion->getPropertyByName("MotionType"));
        auto* pFormula = dynamic_cast<App::PropertyString*>(motion->getPropertyByName("Formula"));
        if (!pJoint || !pType || !pFormula) {
            return false;
        }
        data.joint = pJoint->getValue();
        data.type = pType->getValueAsString();
        data.formula = pFormula->getValue();
        return true;
    };

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

    // An Effort's formula is in N m or N, as a user writes it; the model's
    // is in internal units, mN mm and mN (docs/AssemblyDynamics.md sec 8.1)
    auto internal = [](Solver::Formula& formula, const MotionData& data) {
        if (data.control == Solver::MotorControl::Effort) {
            formula = Solver::scaledFormula(formula, data.type == "Angular" ? 1e6 : 1e3);
        }
    };

    // Parsed here, once, in the workbench's grammar (Solver/Formula.h): a bad
    // formula is reported against its motion instead of failing in the solver.
    auto setFormula = [this](Solver::Formula& target, std::string text, App::DocumentObject* motion) {
        Solver::FormulaError error;
        target.tree = Solver::FormulaTree::parse(text, &error);
        target.function = Solver::Formula::functionOf(target.tree);
        if (!target.tree) {
            ++badFormulas;
            Base::Console().error(
                "Assembly: the formula of %s is not valid: %s: %s\n",
                motion->getFullLabel().c_str(),
                error.describe().c_str(),
                text.c_str()
            );
        }
        target.text = std::move(text);
    };

    // Add motions if needed
    for (auto* motion : motions) {
        if (std::ranges::find(done, motion) != done.end()) {
            continue;  // don't process twice (can happen in case of cylindrical)
        }

        MotionData data;
        if (!readMotion(motion, data) || data.joint != joint || data.formula.empty()) {
            continue;
        }
        std::string formula = data.formula;
        std::string motionType = data.type;

        replaceInitialValue(formula, joint, motionType);
        Solver::Formula parsed;
        setFormula(parsed, formula, motion);
        internal(parsed, data);

        // check if there is a second motion as cylindrical can have both,
        // in which case the solver needs a general motion.
        for (auto* motion2 : motions) {
            MotionData data2;
            if (motion2 == motion || !readMotion(motion2, data2) || data2.joint != joint
                || data2.formula.empty()) {
                continue;
            }
            std::string formula2 = data2.formula;
            std::string motionType2 = data2.type;
            if (motionType2 == motionType) {
                continue;  // only if both motions are different. ie one angular and one linear.
            }

            replaceInitialValue(formula2, joint, motionType2);
            Solver::Formula parsed2;
            setFormula(parsed2, formula2, motion2);
            internal(parsed2, data2);

            Solver::Driver ASMTmotion;
            ASMTmotion.source = solverModel->addSource(joint->getFullName());
            ASMTmotion.frameI = fullMarkerNameI;
            ASMTmotion.frameJ = fullMarkerNameJ;
            ASMTmotion.kind = Solver::DriverKind::Both;
            ASMTmotion.translation = motionType == "Angular" ? parsed2 : parsed;
            ASMTmotion.rotation = motionType == "Angular" ? parsed : parsed2;
            ASMTmotion.translationControl = motionType == "Angular" ? data2.control : data.control;
            ASMTmotion.rotationControl = motionType == "Angular" ? data.control : data2.control;
            solverModel->addDriver(ASMTmotion);

            done.push_back(motion2);
        }

        if (motionType == "Angular") {
            Solver::Driver ASMTmotion;
            ASMTmotion.source = solverModel->addSource(joint->getFullName());
            ASMTmotion.frameI = fullMarkerNameI;
            ASMTmotion.frameJ = fullMarkerNameJ;
            ASMTmotion.kind = Solver::DriverKind::RotationZ;
            ASMTmotion.rotation = parsed;
            ASMTmotion.rotationControl = data.control;
            solverModel->addDriver(ASMTmotion);
        }
        else if (motionType == "Linear") {
            Solver::Driver ASMTmotion;
            ASMTmotion.source = solverModel->addSource(joint->getFullName());
            ASMTmotion.frameI = fullMarkerNameI;
            ASMTmotion.frameJ = fullMarkerNameJ;
            ASMTmotion.kind = Solver::DriverKind::TranslationZ;
            ASMTmotion.translation = parsed;
            ASMTmotion.translationControl = data.control;
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
    if (int side = typedJointSide(joint, propRefName)) {
        // A C++ joint: the frame relative to its component, as the joint
        // kept it on its last recompute; resolved from the geometry only when
        // the joint is not up to date
        auto* typed = static_cast<Joint*>(joint);
        App::DocumentObject* component = nullptr;
        Base::Placement componentPlacement;
        Base::Placement plc;
        if (!typed->getStoredSide(side, component, componentPlacement, plc)) {
            const auto jointSide = typed->getSide(side);
            if (!jointSide.isValid()) {
                Base::Console().Warning(
                    "The %s side of Joint %s is bad.\n",
                    side == 1 ? "first" : "second",
                    joint->getFullName()
                );
                return Solver::NoId;
            }
            component = jointSide.component;
            plc = jointSide.placement.inverse() * jointSide.frame;
        }
        MbDPartData data = getMbDData(component);
        if (!data.offsetPlc.isIdentity()) {
            plc = data.offsetPlc * plc;
        }
        std::string markerNameCopy = markerName.empty() ? joint->getFullName() : markerName;
        auto mbdMarker = makeMbdMarker(markerNameCopy, plc);
        mbdMarker.body = data.body;
        return solverModel->addFrame(mbdMarker);
    }

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

    App::DocumentObject* part1 = getMovingPartFromRef(joint, jcsRef(1));
    App::DocumentObject* obj1 = getObjFromJointRef(joint, jcsRef(1));
    Base::Placement plc1 = getPlacementFromProp(joint, jcsPlc(1));

    App::DocumentObject* obj2 = getObjFromJointRef(joint, jcsRef(2));
    Base::Placement plc2 = getPlacementFromProp(joint, jcsPlc(2));

    if (!part1 || !obj1) {
        Base::Console().Warning("%s of Joint %s is bad.\n", jcsRef(1), joint->getFullName());
        return;
    }

    // For the pinion nothing special needed :
    markerNameJ = handleOneSideOfJoint(joint, jcsRef(2), jcsPlc(2));

    // For the rack we need to change the placement :
    // make the pinion plc relative to the rack placement.
    auto* ref1 = dynamic_cast<App::PropertyXLinkSub*>(joint->getPropertyByName(jcsRef(1)));
    auto* ref2 = dynamic_cast<App::PropertyXLinkSub*>(joint->getPropertyByName(jcsRef(2)));
    // A C++ joint: both frames are in the assembly's coordinate system, which
    // plays the part of the rack object's below
    const bool typed = joint->isDerivedFrom<Joint>();
    Base::Placement rack_global_plc;
    if (typed) {
        auto* typedJoint = static_cast<Joint*>(joint);
        plc1 = typedJoint->getSide(typedJointSide(joint, jcsRef(1))).frame;
        plc2 = typedJoint->getSide(typedJointSide(joint, jcsRef(2))).frame;
    }
    else {
        if (!ref1 || !ref2) {
            return;
        }
        Base::Placement pinion_global_plc = getGlobalPlacement(obj2, ref2);
        plc2 = pinion_global_plc * plc2;
        rack_global_plc = getGlobalPlacement(obj1, ref1);
        plc2 = rack_global_plc.inverse() * plc2;
    }

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
    if (typed) {
        plc1 = getPlacementFromProp(part1, "Placement").inverse() * plc1;
    }
    else if (obj1->getNameInDocument() != part1->getNameInDocument()) {
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

void AssemblyObject::swapJCS(const App::DocumentObject* /*joint*/)
{
    // Upstream swaps Reference1/2 and Placement1/2 in the document here. A
    // solve must not write the document (docs/AssemblySolverInterface.md
    // sec 6.3 step 7), so the swap lives in the model being built instead.
    jcsSwapped = !jcsSwapped;
}

const char* AssemblyObject::jcsRef(int side) const
{
    return ((side == 1) != jcsSwapped) ? "Reference1" : "Reference2";
}

const char* AssemblyObject::jcsPlc(int side) const
{
    return ((side == 1) != jcsSwapped) ? "Placement1" : "Placement2";
}

bool AssemblyObject::isMbDJointValid(App::DocumentObject* joint)
{
    // The members of a rigid group share one MbD part, so a joint between two
    // of them would be self referencing.
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

    // What the part reaches without the joint, and is not connected to
    // ground without it: the joint is left out of the walks, not suppressed
    const Connectivity index(this, getJoints(), [this](App::DocumentObject* obj) {
        return getRigidMembers(obj);
    });
    std::unordered_set<App::DocumentObject*> excluded;
    if (joint) {
        excluded.insert(joint);
    }
    const auto connected = index.reachSet(getGroundedParts(), excluded);
    std::vector<ObjRef> downstreamParts;
    for (const auto& ref : index.reach({{part, nullptr}}, excluded)) {
        if (ref.obj != part && !connected.count(ref.obj)) {
            downstreamParts.push_back(ref);
        }
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
