// SPDX-License-Identifier: LGPL-2.1-or-later
/****************************************************************************
 *                                                                          *
 *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
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

// The exploded view, its steps and the snapshot as C++ objects
// (docs/AssemblyObjectModel.md sec 7.2): the data upstream's Python objects
// carry, typed, under the same names, so that their Python proxies, view
// providers and task panel attach to the ...Python features unchanged.

#pragma once

#include <App/DocumentObject.h>
#include <App/FeaturePython.h>
#include <App/GroupExtension.h>
#include <App/PropertyGeo.h>
#include <App/PropertyLinks.h>
#include <App/PropertyStandard.h>
#include <Base/Placement.h>

#include <Mod/Assembly/AssemblyGlobal.h>

#include <map>
#include <optional>
#include <vector>

namespace Assembly
{

class AssemblyObject;
class ExplodedViewStep;

/// Where an exploded view's steps put the parts, at a progress through them
struct AssemblyExport ExplodedPose
{
    /// A part's Placement value: in its parent's frame
    struct Move
    {
        App::DocumentObject* object = nullptr;
        Base::Placement placement;
    };
    /// The path the center of a part's bounding box took in one step, in
    /// the document's frame, where the steps' trail lines are drawn
    struct Trail
    {
        int step = 0;
        Base::Vector3d from;
        Base::Vector3d to;
    };
    /// One per part moved, in the order first moved, at its last placement
    std::vector<Move> moves;
    /// One per part per step started, in step order
    std::vector<Trail> trails;
};

/** What the steps of one exploded view read and write as they are applied
 * in turn: where each part is so far, from where it was at rest
 */
struct AssemblyExport ExplodeFrame
{
    /// The center of the assembly's bounding box at rest, and its diagonal:
    /// a radial step moves a part away from it, by its size
    Base::Vector3d center;
    double size = 100.0;
    /// A part's placement before any step moved it
    std::map<App::DocumentObject*, Base::Placement> rest;
    /// Where the steps so far put it
    std::map<App::DocumentObject*, Base::Placement> placed;
    /// The order parts were first moved in
    std::vector<App::DocumentObject*> order;
    /// The center of its bounding box at rest, in its parent's frame
    std::map<App::DocumentObject*, Base::Vector3d> restCenter;

    /// The rest placement of `obj`: `rest` has it, or its Placement now
    Base::Placement restOf(App::DocumentObject* obj);
    /// Where it is so far
    Base::Placement placedOf(App::DocumentObject* obj);
    /// The center of its bounding box at rest
    Base::Vector3d restCenterOf(App::DocumentObject* obj);
};

/// An exploded view: owns its steps in Group, applied in order
class AssemblyExport ExplodedView: public App::DocumentObject, public App::GroupExtension
{
    PROPERTY_HEADER_WITH_OVERRIDE(Assembly::ExplodedView);

public:
    ExplodedView();

    const char* getViewProviderName() const override
    {
        return "Gui::ViewProviderDocumentObjectGroup";
    }
    PyObject* getPyObject() override;

    /// The assembly whose parts the view moves
    AssemblyObject* getAssembly() const;
    /// Its steps, in order: what Group holds that has a step's properties
    std::vector<App::DocumentObject*> getSteps() const;
    /** The center of the assembly's bounding box at rest, and its diagonal
     * (100 when it has no shape), in the document's frame
     */
    void getCenterAndSize(Base::Vector3d& center, double& size) const;

    /** Where the steps put the parts at `progress`: 0 is the assembly at
     * rest, `n` the end of step `n`, a fraction a part of the next step's
     * move. Below 0 or past the end is the start or the end; the default,
     * the end. Nothing is written.
     */
    ExplodedPose explodedAt(std::optional<double> progress = std::nullopt) const;
};

using ExplodedViewPython = App::FeaturePythonT<ExplodedView>;


/// One step of an exploded view, a child of it
class AssemblyExport ExplodedViewStep: public App::DocumentObject
{
    PROPERTY_HEADER_WITH_OVERRIDE(Assembly::ExplodedViewStep);

public:
    ExplodedViewStep();

    /// The parts the step moves, as subnames from one selection root: a
    /// part may be inside a sub-assembly. Hidden, as upstream's: the root
    /// can be an ancestor of the assembly that owns the step
    App::PropertyXLinkSubHidden References;
    /// The move: the end placement is this times the start placement
    App::PropertyPlacement MovementTransform;
    /// The same enumeration, in the same order, as
    /// CommandCreateView.ExplodedViewStepTypes
    App::PropertyEnumeration MoveType;

    /// The trail each part it moved left, as pairs of points in the
    /// document's frame: what its view provider draws. Never saved
    App::PropertyVectorList Trail;

    static const char* MoveTypeEnums[];

    const char* getViewProviderName() const override
    {
        return "Gui::ViewProviderDocumentObject";
    }
    PyObject* getPyObject() override;

    /** Apply `step` -- this class or upstream's Python step, by its
     * properties -- to `frame` for `fraction` of its move (0 to 1): each
     * part it moves goes from where `frame` has it so far. A Normal move
     * at a fraction is that power of it, so a turn follows its arc; a
     * radial one, that fraction of its distance. The trails are appended
     * with `stepIndex`.
     */
    static void explode(const App::DocumentObject& step,
                        ExplodeFrame& frame,
                        double fraction,
                        int stepIndex,
                        std::vector<ExplodedPose::Trail>& trails);
    /// Whether `obj` has a step's properties
    static bool isStep(const App::DocumentObject* obj);
};

using ExplodedViewStepPython = App::FeaturePythonT<ExplodedViewStep>;


/// The placements and visibilities of an assembly's parts, to go back to
class AssemblyExport Snapshot: public App::DocumentObject
{
    PROPERTY_HEADER_WITH_OVERRIDE(Assembly::Snapshot);

public:
    Snapshot();

    App::PropertyLinkList Components;
    App::PropertyPlacementList Placements;
    App::PropertyBoolList Visibilities;
    App::PropertyBool SolveOnActivation;

    const char* getViewProviderName() const override
    {
        return "Gui::ViewProviderDocumentObject";
    }
};

using SnapshotPython = App::FeaturePythonT<Snapshot>;

}  // namespace Assembly
