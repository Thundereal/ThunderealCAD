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
#include <Mod/Part/App/TopoShape.h>

#include <fastsignals/signal.h>

#include <map>
#include <memory>
#include <optional>
#include <set>
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
        /// The path from `from` to `to`: those two for a slide, the arc
        /// sampled every 5 deg or less for a turn
        std::vector<Base::Vector3d> path;
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
    /// Its parent's frame at rest, in the document's
    std::map<App::DocumentObject*, Base::Placement> parentFrame;
    /// The objects on the paths to the parts moved: what holds a part moved
    std::set<App::DocumentObject*> ancestors;

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
    /// Out of the views posed, as a closed document takes it posed
    ~ExplodedView() override;

    const char* getViewProviderName() const override
    {
        return "AssemblyGui::ViewProviderExplodedView";
    }
    PyObject* getPyObject() override;

    /// The bill of materials whose numbers a drawing of the view gives its
    /// parts, as balloons (docs/AssemblyExplode.md sec 9)
    App::PropertyLinkHidden Bom;

    /// The assembly whose parts the view moves
    AssemblyObject* getAssembly() const;
    /** The number `bom` gives the part an edge of this view's shape is of,
     * by the edge's mapped name: the tag of its object. Empty for a trail,
     * or a part the BOM has no row of.
     */
    static std::string partNumberOf(const App::DocumentObject* bom,
                                    const App::Document* doc,
                                    const char* mappedName);
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

    /** The assembly as explodedAt(progress) puts it, in the document's
     * frame: what the assembly shows, the parts moved where the steps put
     * them, and the trails as edges named Trail<n>, one each: a line, or a
     * turn's arc. Nothing is written. Its shape for anything
     * that asks the view for one (getSubObject): a TechDraw view of it
     * draws the explode (docs/AssemblyExplode.md sec 5).
     */
    Part::TopoShape getExplodedShape(std::optional<double> progress = std::nullopt) const;
    /// Whether `mappedName` is a trail edge's, as getExplodedShape names it
    static bool isTrailName(const char* mappedName);

    App::DocumentObject* getSubObject(const char* subname,
                                      PyObject** pyObj = nullptr,
                                      Base::Matrix4D* mat = nullptr,
                                      bool transform = true,
                                      int depth = 0) const override;

    /** Show the parts where explodedAt(progress) puts them, and each step's
     * trails, as an Animation plays (docs/AssemblyExplode.md sec 3): under
     * App::WriteScope of Animation, so no undo step, no change to the file,
     * nothing touched. The first pose keeps each part's placement at rest;
     * every pose is from rest. A save writes the rest placements, and a
     * recompute runs on them, the pose put back after either. A part
     * written by anyone else while posed keeps their value: it is the
     * design's now.
     */
    void pose(std::optional<double> progress = std::nullopt);
    /// Put the parts back at rest
    void unpose();
    /// The progress on show, if posed
    std::optional<double> posedAt() const
    {
        return _posedAt;
    }

protected:
    void unsetupObject() override;

private:
    /// A frame at rest: the rest placements a pose keeps, the center and size
    ExplodeFrame restFrame() const;
    ExplodedPose explodeIn(ExplodeFrame& frame, std::optional<double> progress) const;
    /// Write the rest placements back and forget them; the connections stay
    void putBack();
    void connectDocument();

    struct Rest
    {
        App::PropertyPlacement* prop = nullptr;
        Base::Placement rest;
        Base::Placement written;
        /// `rest` as a property, what a transaction records as the value
        /// before an edit while posed (App::Animation::restValueOf())
        std::shared_ptr<App::PropertyPlacement> restProp;
        /// The part's geometry at rest, made when a transaction asks: a
        /// part's shape follows its placement, and an undo puts both back
        std::map<const App::Property*, std::shared_ptr<App::Property>> restGeometry;
    };
    std::map<App::DocumentObject*, Rest> _rest;
    /// The centre of each part's box at rest, from before the first pose:
    /// what a part holding parts moved was boxed as before they moved
    std::map<App::DocumentObject*, Base::Vector3d> _restCenter;
    /// Show it again at the progress on show: a step or the steps changed,
    /// an undo or a redo put back the design
    void repose();
    bool _reposing = false;

public:
    /// The rest value of `prop` while a view poses it, for
    /// App::Animation::restValueOf()
    static const App::Property* restValueOf(const App::Property& prop);

private:
    /// A step's Visibility before the first pose
    std::map<App::DocumentObject*, bool> _stepVisible;
    Base::Vector3d _center;
    double _size = 100.0;
    std::optional<double> _posedAt;
    /// What a save or a recompute took off, to put back after it
    std::optional<double> _resume;
    std::vector<fastsignals::scoped_connection> _conns;
};

using ExplodedViewPython = App::FeaturePythonT<ExplodedView>;

/** Registers the "Explode" animation track kind (docs/AssemblyExplode.md
 * sec 4): one number, the progress through an exploded view's steps,
 * shown by ExplodedView::pose(). The track's handler data is the view's
 * name in the Animation's document.
 */
AssemblyExport void registerExplodeTrack();


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

    /// The trail each part it moved left, as segments -- pairs of points --
    /// in the document's frame, a turn's arc as several: what its view
    /// provider draws. Never saved
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


/** An exploded view at a progress through its steps, for a drawing of one
 * step: its shape is the view's there, ExplodedView::getExplodedShape(),
 * which a TechDraw view of it draws (docs/AssemblyExplode.md sec 8)
 */
class AssemblyExport ExplodedViewAt: public App::DocumentObject
{
    PROPERTY_HEADER_WITH_OVERRIDE(Assembly::ExplodedViewAt);

public:
    ExplodedViewAt();

    /// The exploded view: in its assembly, while this is outside it (a
    /// drawing's source), so a global link
    App::PropertyLinkGlobal View;
    /// Where through its steps: n is the end of step n; below 0, the end
    App::PropertyFloat Progress;

    const char* getViewProviderName() const override
    {
        return "AssemblyGui::ViewProviderExplodedViewAt";
    }
    App::DocumentObjectExecReturn* execute() override;
    /// Never skipped: its shape is the view's, which its own properties do
    /// not show changed
    bool skipRecompute() override
    {
        return false;
    }

protected:
    void onChanged(const App::Property* prop) override;

public:
    App::DocumentObject* getSubObject(const char* subname,
                                      PyObject** pyObj = nullptr,
                                      Base::Matrix4D* mat = nullptr,
                                      bool transform = true,
                                      int depth = 0) const override;
};


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
