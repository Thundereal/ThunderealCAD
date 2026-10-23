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
#include <memory>
#include <unordered_map>
#include <vector>

#include <Mod/Assembly/AssemblyGlobal.h>

#include <App/FeaturePython.h>
#include <App/Part.h>
#include <App/PropertyLinks.h>

#include <OndselSolver/enum.h>

#include "Solver/Model.h"

namespace MbD
{
class ASMTPart;
class ASMTAssembly;
class ASMTJoint;
class ASMTMarker;
class ASMTPart;
}  // namespace MbD

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

    /// returns the type name of the ViewProvider
    const char* getViewProviderName() const override
    {
        return "AssemblyGui::ViewProviderAssembly";
    }

    App::DocumentObjectExecReturn* execute() override;
    void onChanged(const App::Property* prop) override;
    /* Solve the assembly. It will update first the joints, solve, update placements of the parts
    and redraw the joints Args : enableRedo : This store initial positions to enable undo while
    being in an active transaction (joint creation).*/
    int solve(bool enableRedo = false);
    int generateSimulation(App::DocumentObject* sim);
    int updateForFrame(size_t index);
    size_t numberOfFrames();
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
    void savePlacementsForUndo();
    void undoSolve();
    void clearUndo();

    void exportAsASMT(std::string fileName);
    bool requiresRigidSolveForMove(const std::vector<App::DocumentObject*>& movedParts);

    Base::Placement getMbdPlacement(std::shared_ptr<MbD::ASMTPart> mbdPart);
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
    JointGroup* getJointGroup() const;
    ViewGroup* getExplodedViewGroup() const;
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
    fastsignals::signal<void()> signalSolverUpdate;

private:
    void rebuildRigidClusters();
    App::DocumentObject* getRigidRepresentative(App::DocumentObject* part) const;
    const std::vector<App::DocumentObject*>* getRigidMembers(App::DocumentObject* part) const;
    void syncActiveRigidGroupPlacements();
    void updateRigidPlacementCache();
    void prepareMbdForIslandDrag(std::vector<App::DocumentObject*> dragParts);
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


    std::unordered_map<App::DocumentObject*, MbDPartData> objectPartMap;
    std::unordered_map<App::DocumentObject*, App::DocumentObject*> rigidRepByPart;
    std::unordered_map<App::DocumentObject*, std::vector<App::DocumentObject*>> rigidMembersByRep;
    std::unordered_map<App::DocumentObject*, Base::Placement> rigidPlacementCache;
    std::vector<std::pair<App::DocumentObject*, double>> objMasses;
    std::vector<App::DocumentObject*> draggedParts;
    std::vector<App::DocumentObject*> motions;

    std::vector<std::pair<App::DocumentObject*, Base::Placement>> previousPositions;

    bool bundleFixed;

    // Drag step bookkeeping, reset by preDrag().
    int dragStepCount {0};
    int dragStepFailures {0};
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
