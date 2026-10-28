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


#pragma once

#include <fastsignals/signal.h>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <Mod/Assembly/AssemblyGlobal.h>

#include <App/DocumentObserver.h>
#include <App/FeaturePython.h>
#include <App/Part.h>
#include <App/PropertyLinks.h>

#include "Interference.h"
#include "Solver/Model.h"

namespace App
{
class PropertyXLinkSub;
}  // namespace App

namespace Base
{
class Placement;
class Rotation;
}  // namespace Base


namespace Assembly
{

namespace Solver
{
class Session;
struct BodyPose;
struct SimulationSpec;
struct Trajectory;
}

class AssemblyLink;
class JointGroup;
class ViewGroup;
enum class JointType;


struct ObjRef
{
    App::DocumentObject* obj;
    App::PropertyXLinkSub* ref;
};

class AssemblyExport AssemblyObject: public App::Part
{
    PROPERTY_HEADER_WITH_OVERRIDE(Assembly::AssemblyObject);

public:
    AssemblyObject();
    ~AssemblyObject() override;

    PyObject* getPyObject() override;

    // The solver backend this assembly asks for by name (Solver::Registry);
    // empty: the preference Mod/Assembly/SolverBackend decides.
    App::PropertyString SolverBackend;
    /** Frozen (docs/AssemblyObjectModel.md sec 3.1): the components are kept
     * as one shape in the Parts group, the assembly is not solved, and a
     * document linking to it restores that shape and its elements only
     */
    App::PropertyBool Freeze;
    /** The interference check's and collision while dragging's rules
     * (docs/AssemblyInterference.md): the components left out -- or
     * containers or groups of them, as a fasteners group -- and the hidden
     * ones, if asked; the pairs never reported, each "Name1|Name2" by the
     * objects' names (a component's, or a container's or group's it is in);
     * and the clearances pairs keep, "Name1|Name2" to mm, a pair closer than
     * its clearance being reported as such
     */
    App::PropertyLinkListHidden InterferenceExcluded;
    App::PropertyBool InterferenceIgnoreHidden;
    App::PropertyStringList InterferenceIgnoredPairs;
    App::PropertyMap InterferenceClearances;

    /// returns the type name of the ViewProvider
    const char* getViewProviderName() const override
    {
        return "AssemblyGui::ViewProviderAssembly";
    }

    App::DocumentObjectExecReturn* execute() override;
    void onChanged(const App::Property* prop) override;
    void handleChangedPropertyType(
        Base::XMLReader& reader,
        const char* TypeName,
        App::Property* prop
    ) override;
    /* Solve the assembly. It will update first the joints, solve, update placements of the parts
    and redraw the joints Args : enableRedo : This store initial positions to enable undo while
    being in an active transaction (joint creation).*/
    int solve(bool enableRedo = false);
    int generateSimulation(App::DocumentObject* sim);
    int updateForFrame(size_t index);
    size_t numberOfFrames() const;
    // The last simulation's frame: its time (s), and the Placement it gives obj,
    // as playback would write it (false when the simulation does not move obj,
    // or there is no such frame)
    double timeOfFrame(size_t index) const;
    bool placementAtFrame(App::DocumentObject* obj, size_t index, Base::Placement& placement) const;
    // The last simulation's frames, with the values a dynamic run reports
    // (Solver::Channel); null before one
    const Solver::Trajectory* lastTrajectory() const
    {
        return trajectory.get();
    }
    void preDrag(std::vector<App::DocumentObject*> dragParts);
    /* One drag step: solves for the Placement the caller wrote on the dragged parts.
    Returns false when no drag is in progress or the step failed; a failed step leaves
    the parts where the last good step put them, and the drag can go on. */
    bool doDragStep();
    void postDrag();
    // Whether the last drag step failed, and the counts for the current drag.
    bool getLastDragStepFailed() const
    {
        return lastDragStepFailed;
    }
    int getDragStepFailures() const
    {
        return dragStepFailures;
    }
    int getDragStepCount() const
    {
        return dragStepCount;
    }

    // Collision while dragging (docs/AssemblyInterference.md), as the
    // DragCollision preference asks: off, highlight, or stop. preDrag() starts
    // watching the components and postDrag() ends it; a move without a solve
    // (the 3D view's) starts and ends it itself.
    enum class DragCollision
    {
        Off = 0,
        Highlight = 1,
        Stop = 2
    };
    void beginCollisionWatch();
    void endCollisionWatch();
    DragCollision getDragCollisionMode() const
    {
        return collisionWatch ? dragCollisionMode : DragCollision::Off;
    }
    // The pairs the components' placements in the document now bring into
    // collision (none without a watch), kept for getDragCollisions(). With
    // Stop, doDragStep() takes a step that would collide only as far as it
    // goes without, and the pairs are those the rest would have brought into
    // collision.
    const std::vector<InterferenceResult>& checkDragCollisions();
    // Stop at a collision: the move from where the last good step left the
    // parts (0) to where this one takes them (1) collides, and is halved
    // towards the farthest fraction that does not, dragCollisionHalvings times
    // at most, and no more once what is left of it moves the parts less than
    // the watch's tolerance (how far the step moves them, the first fraction
    // posed tells). Pushed on from where the last step stopped -- against
    // what stopped it, most often -- the least fraction (1/2^halvings) is
    // tried first: colliding, the parts stay, for one check, not all. poseAt(t) puts the parts at
    // fraction t, in the solver and the document both, and says whether it could. Leaves the parts
    // at the fraction found, which it returns, the collisions those of the nearest fraction that
    // collides.
    double stopAtDragCollision(const std::function<bool(double)>& poseAt);
    static constexpr int dragCollisionHalvings = 8;
    const std::vector<InterferenceResult>& getDragCollisions() const
    {
        return dragCollisions;
    }
    // Where drags brought components into collision, kept until cleared and
    // not saved: one for each pair a drag brought into collision, where they
    // met at the last check that found them (InterferenceResult's touch), for
    // the view to mark with an arrow. In the assembly's coordinates.
    struct CollisionMarker
    {
        App::DocumentObjectT first;
        App::DocumentObjectT second;
        Base::Vector3d centre;
        // Out of the component hit, towards the other
        Base::Vector3d direction;
        // The watch (beginCollisionWatch()) that found it
        int watch = 0;
        // What it marks, as the check had it (InterferenceResult): a
        // collision, a contact (components that only touch) or a pair
        // closer than its clearance, its volume (Exact), area, distance
        // and the clearance asked of it; the faces where the two meet, by
        // their names from the assembly ("A.B.Face3")
        InterferenceResult::Kind kind = InterferenceResult::Kind::Interference;
        double volume = 0.0;
        double area = 0.0;
        double distance = 0.0;
        double clearance = 0.0;
        std::vector<std::string> faces1;
        std::vector<std::string> faces2;
    };
    const std::vector<CollisionMarker>& getCollisionMarkers() const
    {
        return collisionMarkers;
    }
    // All of them, or with checks only, those of no drag (the interference
    // check's)
    void clearCollisionMarkers(bool checksOnly = false);
    // A marker of what the interference check found, a collision, a contact
    // or a clearance, where the result's touch is, unless the pair has one of
    // that kind there already
    void addCollisionMarker(const InterferenceResult& result);
    // The markers added, moved or cleared
    fastsignals::signal<void()> signalCollisionMarkers;
    // The triangles of a component the watch checks, or of some of its faces,
    // to draw a collision with (CollisionWatch::meshOf)
    bool getCollisionMesh(const App::DocumentObject* obj,
                          std::vector<Base::Vector3d>& points,
                          const std::vector<std::pair<int, int>>* faces = nullptr) const
    {
        return collisionWatch && collisionWatch->meshOf(obj, points, faces);
    }
    void savePlacementsForUndo();
    void undoSolve();
    void clearUndo();

    void exportAsASMT(std::string fileName);
    /** The model's dynamics block (docs/AssemblyDynamics.md sec 6.1): each
     * body's mass properties, from its components' (massPropertiesOf()),
     * summed in the assembly's frame and given in the body's. Of the model
     * the last build made: objectPartMap says which components a body is.
     */
    void addDynamics(Solver::Model& model) const;
    /// The force elements a study uses (Simulation::Elements), as frames and
    /// ForceElement records of the model's dynamics block; a component held
    /// by nothing else becomes a body of its own
    void addForces(Solver::Model& model, App::DocumentObject* sim);
    /// A model built, as for a solve, with its dynamics block: each body's
    /// inertia, and the components that make it up
    struct BodyInertia
    {
        Solver::Inertia inertia;
        Base::Placement placement;  // the body's frame
        std::vector<App::DocumentObject*> objects;
    };
    std::vector<BodyInertia> getBodyInertias();
    bool requiresRigidSolveForMove(const std::vector<App::DocumentObject*>& movedParts);

    bool validateNewPlacements();
    void setNewPlacements();
    static void redrawJointPlacements(std::vector<App::DocumentObject*> joints);
    static void redrawJointPlacement(App::DocumentObject* joint);

    // This makes sure that LinkGroups or sub-assemblies have identity placements.
    void ensureIdentityPlacements();
    // Make sure grounded joints reflect Placement read-only states
    void syncGroundedJoints();

    // Ondsel Solver interface. The builder emits a solver-neutral model
    // (Solver::Model, docs/AssemblySolverInterface.md), which the Ondsel adapter
    // turns into the ASMT model the solver runs.
    std::unique_ptr<Solver::Model> makeMbdAssembly();
    void create_mbdSimulationParameters(App::DocumentObject* sim);
    Solver::Id makeMbdPart(
        std::string& name,
        Base::Placement plc = Base::Placement(),
        double mass = 1.0
    );
    Solver::Id getMbDPart(App::DocumentObject* obj);
    // To help the solver, during dragging, we are bundling parts connected by a fixed joint.
    // So several assembly components are bundled in a single solver body.
    // So we need to store the plc of each bundled object relative to the bundle origin (first obj
    // of objectPartMap).
    struct MbDPartData
    {
        Solver::Id body = Solver::NoId;
        Base::Placement offsetPlc;  // This is the offset within the bundled parts
    };
    MbDPartData getMbDData(App::DocumentObject* part);
    Solver::Frame makeMbdMarker(std::string& name, Base::Placement& plc);
    std::vector<std::shared_ptr<Solver::Joint>> makeMbdJoint(App::DocumentObject* joint);
    std::shared_ptr<Solver::Joint> makeMbdJointOfType(App::DocumentObject* joint, JointType jointType);
    std::shared_ptr<Solver::Joint> makeMbdJointDistance(App::DocumentObject* joint);
    Solver::Id handleOneSideOfJoint(
        App::DocumentObject* joint,
        const char* propRefName,
        const char* propPlcName,
        const std::string& markerName = std::string()
    );
    void getRackPinionMarkers(
        App::DocumentObject* joint,
        Solver::Id& markerNameI,
        Solver::Id& markerNameJ
    );
    int slidingPartIndex(App::DocumentObject* joint);

    void jointParts(std::vector<App::DocumentObject*> joints);
    /** The assembly's group of a role (AssemblyGroup::JointsRole, ...):
     * its aux group (docs/AssemblyObjectModel.md sec 3), else upstream's
     * group of that role (JointGroup, SimulationGroup, ViewGroup) in a file
     * not converted, else with create a new aux group
     */
    App::DocumentObject* getAssemblyGroup(const char* role, bool create = false);
    /// The joint group, either kind, or null
    App::DocumentObject* getJointGroup() const;
    /// What the joint group lists, either kind
    std::vector<App::DocumentObject*> getJointGroupMembers() const;
    App::DocumentObject* getExplodedViewGroup() const;

    /// The roles of the assembly's aux groups: joints, grounding and rigid
    /// groups in Joints, simulations in Simulations, exploded views in Views
    bool isAuxGroupAllowed(const App::AuxGroup* group, const App::DocumentObject* obj) const override;
    /// A frozen assembly keeps its Parts group
    void removePartsGroup() override;

    /** @name Freeze */
    //@{
    bool isFrozen() const
    {
        return Freeze.getValue();
    }
    /// Take the compound of the components again, into the Parts group
    /// (made if there is none)
    void refreshFrozenShape();
    /// A frozen assembly is restored with its groups and elements, its
    /// components and joints created only
    int canLoadPartial() const override;
    /// Frozen, the shape is the Parts group's
    std::vector<std::string> getSubObjects(int reason = GS_DEFAULT) const override;
    //@}

    /** The shape and the export are the components, and the user's plain
     * groups of them, whatever else refers to them: a joint, its element
     * links and the groups name the components without taking their place.
     * The tree's children stay as the view claims them. 'Toggle export' on a
     * child (ExportMode "Child Query") starts from this.
     */
    bool getChildDefaultExport(App::DocumentObject* obj, int reason) const override;
    template<typename T>
    T* getGroup();

    std::vector<App::DocumentObject*> getJoints(bool delBadJoints = false, bool subJoints = true);
    std::vector<App::DocumentObject*> getGroundedJoints();
    std::vector<App::DocumentObject*> getRigidGroups();
    std::vector<App::DocumentObject*> getJointsOfObj(App::DocumentObject* obj);
    std::vector<App::DocumentObject*> getJointsOfPart(App::DocumentObject* part);
    App::DocumentObject* getJointOfPartConnectingToGround(
        App::DocumentObject* part,
        std::string& name,
        const std::vector<App::DocumentObject*>& excludeJoints = {}
    );
    App::DocumentObject* getJointOfPartForUngroundedDrag(App::DocumentObject* part, std::string& name);
    std::unordered_set<App::DocumentObject*> getGroundedParts();
    std::unordered_set<App::DocumentObject*> fixGroundedParts();
    void fixGroundedPart(App::DocumentObject* obj, Base::Placement& plc, std::string& jointName);

    bool isJointConnectingPartToGround(App::DocumentObject* joint, const char* partPropName);
    bool isJointTypeConnecting(App::DocumentObject* joint);

    bool isObjInSetOfObjRefs(App::DocumentObject* obj, const std::vector<ObjRef>& pairs);
    void removeUnconnectedJoints(
        std::vector<App::DocumentObject*>& joints,
        std::unordered_set<App::DocumentObject*> groundedObjs
    );
    void traverseAndMarkConnectedParts(
        App::DocumentObject* currentPart,
        std::vector<ObjRef>& connectedParts,
        const std::vector<App::DocumentObject*>& joints
    );
    std::vector<ObjRef> getConnectedParts(
        App::DocumentObject* part,
        const std::vector<App::DocumentObject*>& joints
    );
    bool isPartGrounded(App::DocumentObject* part);
    // Answered from one walk of the assembly, kept until anything in the
    // document changes but a placement, a shape or a visibility (what a solve
    // and a drag write), or an object is made or deleted: the joints' icons
    // ask it for their parts, every one of them at each refresh.
    bool isPartConnected(App::DocumentObject* part);

    std::vector<ObjRef> getDownstreamParts(
        App::DocumentObject* part,
        App::DocumentObject* joint = nullptr
    );
    App::DocumentObject* getUpstreamMovingPart(
        App::DocumentObject* part,
        App::DocumentObject*& joint,
        std::string& name,
        std::vector<App::DocumentObject*> excludeJoints = {}
    );

    double getObjMass(App::DocumentObject* obj);
    void setObjMasses(std::vector<std::pair<App::DocumentObject*, double>> objectMasses);

    std::vector<AssemblyLink*> getSubAssemblies();

    std::vector<App::DocumentObject*> getMotionsFromSimulation(App::DocumentObject* sim);

    bool isMbDJointValid(App::DocumentObject* joint);

    bool isEmpty() const;
    int numberOfComponents() const;

    void updateSolveStatus();
    inline int getLastDoF() const
    {
        return lastDoF;
    }
    inline bool getLastHasConflicts() const
    {
        return lastHasConflict;
    }
    inline bool getLastHasRedundancies() const
    {
        return lastHasRedundancies;
    }
    inline bool getLastHasPartialRedundancies() const
    {
        return lastHasPartialRedundancies;
    }
    inline bool getLastHasMalformedConstraints() const
    {
        return lastHasMalformedConstraints;
    }
    inline int getLastSolverStatus() const
    {
        return lastSolverStatus;
    }
    inline const std::vector<std::string>& getLastConflicting() const
    {
        return lastConflictingJoints;
    }
    inline const std::vector<std::string>& getLastRedundant() const
    {
        return lastRedundantJoints;
    }
    inline const std::vector<std::string>& getLastPartiallyRedundant() const
    {
        return lastPartialRedundantJoints;
    }
    inline const std::vector<std::string>& getLastMalformed() const
    {
        return lastMalformedJoints;
    }
    // The error of every document joint at the poses the solver holds now, by
    // the neutral checker (Solver::Reference::jointResiduals; mm, rad): the
    // largest over the model joints a document joint became. Empty when
    // nothing was solved.
    std::vector<std::pair<std::string, double>> getSolverResiduals() const;
    // Per object of the last model: the point of it whose movement a solve
    // weighs, in its own frame, the inertia a turn is weighed by (sec 6.20),
    // its solver body (objects bundled into one share it), and the body's
    // mass (a group weighs as many bodies as it has).
    std::vector<std::tuple<std::string, Base::Vector3d, double, int, double>> getSolverMetric() const;
    fastsignals::signal<void()> signalSolverUpdate;

protected:
    void onSettingDocument() override;
    const char* getPartsGroupType() const override
    {
        return "Assembly::PartsGroup";
    }

private:
    std::optional<std::unordered_set<App::DocumentObject*>> connectedCache;
    fastsignals::scoped_connection connConnectivityChanged;
    fastsignals::scoped_connection connConnectivityNew;
    fastsignals::scoped_connection connConnectivityDeleted;
    void rebuildRigidClusters();
    App::DocumentObject* getRigidRepresentative(App::DocumentObject* part) const;
    const std::vector<App::DocumentObject*>* getRigidMembers(App::DocumentObject* part) const;
    void syncActiveRigidGroupPlacements();
    void updateRigidPlacementCache();
    void prepareMbdForIslandDrag(std::vector<App::DocumentObject*> dragParts);
    // The builder's swapJCS (it shadows ::swapJCS in the builder functions):
    // the joint being built takes its frame I from Reference2 and J from
    // Reference1. The document keeps the order the user picked.
    void swapJCS(const App::DocumentObject* joint);
    // The document side ("Reference1"/"Placement1" or the 2s) of frame I
    // (side 1) or J (side 2) of the joint being built.
    const char* jcsRef(int side) const;
    const char* jcsPlc(int side) const;
    // Fills lastMalformedJoints: active joints whose references are set but unusable.
    void collectMalformedJoints();
    static bool isJointRefUsable(App::DocumentObject* joint, const char* propName);
    // Gives the dragged parts' bodies their drag masses (setObjMasses) as drag
    // weights, and starts the session's drag.
    void applyDragMasses();
    // Hands the model built so far to a session of the selected backend
    // (Solver::Registry::select): the solve, drag and playback then run on it.
    void makeMbdModel();
    bool validateNewPlacements(const std::vector<Solver::BodyPose>& poses);
    void setNewPlacements(const std::vector<Solver::BodyPose>& poses);
    void setNewPlacements(
        const std::unordered_map<App::DocumentObject*, MbDPartData>& parts,
        const std::vector<Solver::BodyPose>& poses
    );
    // The document name of the joint a model joint came from, if it is one
    // with references in this document; empty otherwise.
    std::string jointNameOf(Solver::Id joint) const;

    // The model being built, and the session it was handed to. A build starts
    // without a session, so status and playback find nothing solved until it
    // is handed over.
    std::unique_ptr<Solver::Model> solverModel;
    std::unique_ptr<Solver::SimulationSpec> simulationSpec;
    std::unique_ptr<Solver::Session> session;
    // The bodies the session was loaded with (bodies added later have no pose).
    std::size_t loadedBodies {0};
    // The last simulation's frames and the parts they move, kept until the next
    // simulation: playback needs no live solver, and a solve in between does
    // not disturb it.
    std::unique_ptr<Solver::Trajectory> trajectory;
    std::unordered_map<App::DocumentObject*, MbDPartData> trajectoryParts;


    std::unordered_map<App::DocumentObject*, MbDPartData> objectPartMap;
    std::unordered_map<App::DocumentObject*, App::DocumentObject*> rigidRepByPart;
    std::unordered_map<App::DocumentObject*, std::vector<App::DocumentObject*>> rigidMembersByRep;
    std::unordered_map<App::DocumentObject*, Base::Placement> rigidPlacementCache;
    std::vector<std::pair<App::DocumentObject*, double>> objMasses;
    std::vector<App::DocumentObject*> draggedParts;
    std::vector<App::DocumentObject*> motions;
    // Motions whose formula did not parse in the last model build; each is
    // reported once by the build, and a simulation does not run with any.
    int badFormulas {0};

    std::vector<std::pair<App::DocumentObject*, Base::Placement>> previousPositions;

    // Frames I and J of the joint being built come from Reference2 and
    // Reference1 (swapJCS); reset by makeMbdJoint for each joint.
    bool jcsSwapped {false};
    // Distance joints a new pick would refuse (distanceJointRefusal) already
    // warned about, by full name and sub-type: one warning per joint while
    // this object lives (from a document load), not one per solve or drag.
    // Also, keyed "mapped:", the joints of a sub-type the role mappings
    // added (sec 6.11) already logged.
    std::unordered_set<std::string> warnedRefusedDistance;

    // Drag step bookkeeping, reset by preDrag().
    int dragStepCount {0};
    int dragStepFailures {0};
    std::unique_ptr<CollisionWatch> collisionWatch;
    DragCollision dragCollisionMode {DragCollision::Off};
    std::vector<InterferenceResult> dragCollisions;
    std::vector<CollisionMarker> collisionMarkers;
    int collisionWatches {0};
    // The last step stopped at a collision, and none since went through;
    // stopping: in stopAtDragCollision(), whose checks do not tell
    bool dragStopped {false};
    bool stoppingDrag {false};
    // The markers of the pairs the last check found
    void markDragCollisions();
    bool lastDragStepFailed {false};

    // True while solve() is running; breaks the solve()/updateSolveStatus() cycle.
    bool solveInProgress {false};

    int lastDoF;
    bool lastHasConflict;
    bool lastHasRedundancies;
    bool lastHasPartialRedundancies;
    bool lastHasMalformedConstraints;
    int lastSolverStatus;

    std::vector<std::string> lastRedundantJoints;
    std::vector<std::string> lastConflictingJoints;
    std::vector<std::string> lastPartialRedundantJoints;
    std::vector<std::string> lastMalformedJoints;
};

}  // namespace Assembly
