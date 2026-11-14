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

#include <cstring>
#include <set>

#include <Standard_Failure.hxx>

#include <App/Document.h>
#include <App/FeaturePythonPyImp.h>
#include <App/WriteScope.h>
#include <Base/BoundBox.h>
#include <Mod/Part/App/PartFeature.h>

#include "AssemblyObject.h"
#include "AssemblyUtils.h"
#include "ExplodedViewPy.h"
#include "ExplodedViewStepPy.h"
#include "Views.h"

using namespace Assembly;

namespace
{

/// The frame `sub` under `root` names, in the document's: through root's
/// one parent if it has one, as UtilsAssembly.getGlobalPlacement does
Base::Placement globalPlacementOf(const App::DocumentObject* root, const std::string& sub)
{
    auto parents = root->getParents();
    if (parents.size() == 1 && parents[0].first) {
        return parents[0].first->getPlacementOf(parents[0].second + sub);
    }
    return root->getPlacementOf(sub);
}

App::PropertyPlacement* placementOf(App::DocumentObject* obj)
{
    return obj ? freecad_cast<App::PropertyPlacement*>(obj->getPropertyByName("Placement"))
               : nullptr;
}

/** The frame of `obj`'s parent, along `sub` under `root`, in the document's
 * frame, with every object on the way that `frame` holds a rest placement
 * of put at it: the parent frame at rest, whatever is on show
 */
Base::Placement parentFrameOf(App::DocumentObject* root,
                              const std::string& sub,
                              App::DocumentObject* obj,
                              ExplodeFrame& frame)
{
    std::vector<int> sizes;
    auto objs = root->getSubObjectList(sub.c_str(), &sizes);
    int index = -1;
    for (int i = static_cast<int>(objs.size()) - 1; i >= 0; --i) {
        if (objs[i] == obj) {
            index = i;
            break;
        }
    }
    // Each object's own transform on the way, as the document has it now,
    // from the frames of the paths that end at it and at its parent
    auto atRest = [&](App::DocumentObject* o, const Base::Placement& local) {
        auto it = frame.rest.find(o);
        auto prop = placementOf(o);
        if (it == frame.rest.end() || !prop) {
            return local;
        }
        return local * prop->getValue().inverse() * it->second;
    };
    Base::Placement above;
    auto rootPlc = globalPlacementOf(root, "");
    if (auto prop = placementOf(root)) {
        above = rootPlc * prop->getValue().inverse();
    }
    // The root's own frame, then down the path to the parent
    Base::Placement result = above * atRest(root, above.inverse() * rootPlc);
    if (index <= 0) {
        return index == 0 ? above : result;
    }
    Base::Placement doc = rootPlc;
    for (int i = 1; i < index; ++i) {
        auto next = globalPlacementOf(root, sub.substr(0, sizes[i]));
        result = result * atRest(objs[i], doc.inverse() * next);
        doc = next;
    }
    return result;
}

}  // namespace


// ExplodeFrame -----------------------------------------------------------

Base::Placement ExplodeFrame::restOf(App::DocumentObject* obj)
{
    auto it = rest.find(obj);
    if (it != rest.end()) {
        return it->second;
    }
    auto prop = placementOf(obj);
    auto plc = prop ? prop->getValue() : Base::Placement();
    rest.emplace(obj, plc);
    return plc;
}

Base::Placement ExplodeFrame::placedOf(App::DocumentObject* obj)
{
    auto it = placed.find(obj);
    return it != placed.end() ? it->second : restOf(obj);
}

Base::Vector3d ExplodeFrame::restCenterOf(App::DocumentObject* obj)
{
    auto it = restCenter.find(obj);
    if (it != restCenter.end()) {
        return it->second;
    }
    auto plc = restOf(obj);
    Base::Vector3d center = plc.getPosition();
    try {
        // Its shape where it is at rest, in its parent's frame
        Base::Matrix4D mat = plc.toMatrix();
        auto shape = Part::Feature::getTopoShape(obj, nullptr, false, &mat, nullptr, true, false);
        if (!shape.isNull()) {
            auto box = shape.getBoundBox();
            if (box.IsValid()) {
                center = box.GetCenter();
            }
        }
    }
    catch (Base::Exception&) {
    }
    catch (Standard_Failure&) {
    }
    restCenter.emplace(obj, center);
    return center;
}


// ExplodedView -----------------------------------------------------------

PROPERTY_SOURCE_WITH_EXTENSIONS(Assembly::ExplodedView, App::DocumentObject)

ExplodedView::ExplodedView()
{
    GroupExtension::initExtension(this);
}

PyObject* ExplodedView::getPyObject()
{
    if (PythonObject.is(Py::_None())) {
        PythonObject = Py::Object(new ExplodedViewPy(this), true);
    }
    return Py::new_reference_to(PythonObject);
}

AssemblyObject* ExplodedView::getAssembly() const
{
    // In the assembly's group of views, or upstream's ViewGroup in it
    for (auto obj : getInList()) {
        if (auto assembly = freecad_cast<AssemblyObject*>(obj)) {
            return assembly;
        }
    }
    auto obj = const_cast<App::DocumentObject*>(static_cast<const App::DocumentObject*>(this));
    for (int depth = 0; obj && depth < 16; ++depth) {
        obj = App::GroupExtension::getGroupOfObject(obj);
        if (auto assembly = freecad_cast<AssemblyObject*>(obj)) {
            return assembly;
        }
    }
    return nullptr;
}

std::vector<App::DocumentObject*> ExplodedView::getSteps() const
{
    std::vector<App::DocumentObject*> steps;
    for (auto obj : Group.getValues()) {
        if (ExplodedViewStep::isStep(obj)) {
            steps.push_back(obj);
        }
    }
    return steps;
}

void ExplodedView::getCenterAndSize(Base::Vector3d& center, double& size) const
{
    if (_posedAt) {
        // At rest, not as posed
        center = _center;
        size = _size;
        return;
    }
    center = Base::Vector3d();
    size = 100.0;
    auto assembly = getAssembly();
    if (!assembly) {
        return;
    }
    try {
        Base::Matrix4D mat = globalPlacementOf(assembly, "").toMatrix();
        auto shape =
            Part::Feature::getTopoShape(assembly, nullptr, false, &mat, nullptr, true, false);
        if (shape.isNull()) {
            return;
        }
        auto box = shape.getBoundBox();
        if (box.IsValid() && box.CalcDiagonalLength() > 0) {
            center = box.GetCenter();
            size = box.CalcDiagonalLength();
        }
    }
    catch (Base::Exception&) {
    }
    catch (Standard_Failure&) {
    }
}

ExplodeFrame ExplodedView::restFrame() const
{
    ExplodeFrame frame;
    if (_posedAt) {
        frame.center = _center;
        frame.size = _size;
        for (const auto& [obj, r] : _rest) {
            // Written by anyone else since: theirs is the design's
            frame.rest[obj] = r.prop->getValue() == r.written ? r.rest : r.prop->getValue();
        }
    }
    else {
        getCenterAndSize(frame.center, frame.size);
    }
    return frame;
}

ExplodedPose ExplodedView::explodeIn(ExplodeFrame& frame, std::optional<double> progress) const
{
    ExplodedPose pose;
    auto steps = getSteps();
    double at = progress ? *progress : static_cast<double>(steps.size());
    for (int i = 0; i < static_cast<int>(steps.size()); ++i) {
        double fraction = std::min(1.0, at - i);
        if (fraction <= 0.0) {
            break;
        }
        ExplodedViewStep::explode(*steps[i], frame, fraction, i, pose.trails);
    }
    for (auto obj : frame.order) {
        pose.moves.push_back({obj, frame.placed[obj]});
    }
    return pose;
}

ExplodedPose ExplodedView::explodedAt(std::optional<double> progress) const
{
    auto frame = restFrame();
    return explodeIn(frame, progress);
}

namespace
{

/// Purges, when it goes, what the writes it saw touched
class KeepUntouched
{
public:
    explicit KeepUntouched(App::Document* doc)
        : doc(doc)
    {
        for (auto obj : doc->getObjects()) {
            if (obj->isTouched()) {
                touched.insert(obj);
            }
        }
    }
    ~KeepUntouched()
    {
        for (auto obj : doc->getObjects()) {
            if (obj->isTouched() && !touched.count(obj)) {
                obj->purgeTouched();
            }
        }
    }
    KeepUntouched(const KeepUntouched&) = delete;
    KeepUntouched& operator=(const KeepUntouched&) = delete;

private:
    App::Document* doc;
    std::set<App::DocumentObject*> touched;
};

void setTrail(App::DocumentObject* step, const std::vector<Base::Vector3d>& points)
{
    auto prop = freecad_cast<App::PropertyVectorList*>(step->getPropertyByName("Trail"));
    if (prop && (prop->getSize() != 0 || !points.empty())) {
        prop->setValues(points);
    }
}

}  // namespace

void ExplodedView::pose(std::optional<double> progress)
{
    auto doc = getDocument();
    if (!doc) {
        return;
    }
    auto frame = restFrame();
    if (!_posedAt) {
        _center = frame.center;
        _size = frame.size;
        connectDocument();
    }
    auto steps = getSteps();
    double at = progress ? *progress : static_cast<double>(steps.size());
    auto shown = explodeIn(frame, at);

    App::WriteScope animated(App::WriteCause::Animation);
    KeepUntouched untouched(doc);
    // The rest placements as the frame read them: kept, or theirs now
    for (auto& [obj, plc] : frame.rest) {
        auto it = _rest.find(obj);
        if (it != _rest.end()) {
            it->second.rest = plc;
            continue;
        }
        auto prop = freecad_cast<App::PropertyPlacement*>(obj->getPropertyByName("Placement"));
        if (prop) {
            _rest.emplace(obj, Rest {prop, plc, prop->getValue()});
        }
    }
    for (auto& [obj, r] : _rest) {
        auto it = frame.placed.find(obj);
        const auto& target = it != frame.placed.end() ? it->second : r.rest;
        if (!(r.prop->getValue() == target)) {
            r.prop->setValue(target);
        }
        r.written = target;
    }
    std::vector<std::vector<Base::Vector3d>> trails(steps.size());
    for (const auto& trail : shown.trails) {
        trails[trail.step].push_back(trail.from);
        trails[trail.step].push_back(trail.to);
    }
    for (std::size_t i = 0; i < steps.size(); ++i) {
        auto step = steps[i];
        if (!_stepVisible.count(step)) {
            _stepVisible[step] = step->Visibility.getValue();
        }
        if (!step->Visibility.getValue()) {
            step->Visibility.setValue(true);
        }
        setTrail(step, trails[i]);
    }
    _posedAt = at;
}

void ExplodedView::putBack()
{
    auto doc = getDocument();
    if (!_posedAt || !doc) {
        _posedAt.reset();
        return;
    }
    {
        App::WriteScope animated(App::WriteCause::Animation);
        KeepUntouched untouched(doc);
        for (auto& [obj, r] : _rest) {
            if (r.prop->getValue() == r.written) {
                r.prop->setValue(r.rest);
            }
        }
        for (auto& [step, visible] : _stepVisible) {
            setTrail(step, {});
            if (step->Visibility.getValue() != visible) {
                step->Visibility.setValue(visible);
            }
        }
    }
    _rest.clear();
    _stepVisible.clear();
    _posedAt.reset();
}

void ExplodedView::unpose()
{
    putBack();
    _resume.reset();
    _conns.clear();
}

void ExplodedView::unsetupObject()
{
    unpose();
    App::DocumentObject::unsetupObject();
}

void ExplodedView::connectDocument()
{
    _conns.clear();
    auto doc = getDocument();
    // A save writes the design, and a recompute runs on it: off for either,
    // and back on after (docs/AssemblyExplode.md sec 3)
    auto off = [this](const App::Document&) {
        if (_posedAt) {
            _resume = _posedAt;
            putBack();
        }
    };
    auto on = [this](const App::Document&) {
        if (_resume) {
            auto at = *_resume;
            _resume.reset();
            pose(at);
        }
    };
    _conns.emplace_back(doc->signalStartSave.connect(
        [off](const App::Document& d, const std::string&) { off(d); }));
    _conns.emplace_back(doc->signalFinishSave.connect(
        [on](const App::Document& d, const std::string&) { on(d); }));
    _conns.emplace_back(doc->signalBeforeRecompute.connect(off));
    _conns.emplace_back(doc->signalRecomputed.connect(
        [on](const App::Document& d, const std::vector<App::DocumentObject*>&) { on(d); }));
    _conns.emplace_back(doc->signalDeletedObject.connect([this](const App::DocumentObject& obj) {
        auto o = const_cast<App::DocumentObject*>(&obj);
        _rest.erase(o);
        _stepVisible.erase(o);
    }));
}


// ExplodedViewStep -------------------------------------------------------

PROPERTY_SOURCE(Assembly::ExplodedViewStep, App::DocumentObject)

const char* ExplodedViewStep::MoveTypeEnums[] = {"Normal", "Radial", nullptr};

ExplodedViewStep::ExplodedViewStep()
{
    ADD_PROPERTY_TYPE(References, (nullptr), "Exploded Move", App::Prop_None,
                      "The objects moved by the move");
    ADD_PROPERTY_TYPE(MovementTransform, (Base::Placement()), "Exploded Move", App::Prop_None,
                      "This is the movement of the move. The end placement is the result of "
                      "the start placement * this placement.");
    ADD_PROPERTY_TYPE(MoveType, (0L), "Exploded Move", App::Prop_None, "The type of the move");
    MoveType.setEnums(MoveTypeEnums);
    ADD_PROPERTY_TYPE(Trail, (), "Exploded Move",
                      App::PropertyType(App::Prop_Transient | App::Prop_Hidden
                                        | App::Prop_Output | App::Prop_NoRecompute),
                      "The trail each part the move moved left, as pairs of points");
}

PyObject* ExplodedViewStep::getPyObject()
{
    if (PythonObject.is(Py::_None())) {
        PythonObject = Py::Object(new ExplodedViewStepPy(this), true);
    }
    return Py::new_reference_to(PythonObject);
}

bool ExplodedViewStep::isStep(const App::DocumentObject* obj)
{
    return obj
        && freecad_cast<App::PropertyXLinkSub*>(obj->getPropertyByName("References"))
        && freecad_cast<App::PropertyPlacement*>(obj->getPropertyByName("MovementTransform"));
}

void ExplodedViewStep::explode(const App::DocumentObject& step,
                               ExplodeFrame& frame,
                               double fraction,
                               int stepIndex,
                               std::vector<ExplodedPose::Trail>& trails)
{
    auto refs = freecad_cast<App::PropertyXLinkSub*>(step.getPropertyByName("References"));
    auto moveProp =
        freecad_cast<App::PropertyPlacement*>(step.getPropertyByName("MovementTransform"));
    auto typeProp = freecad_cast<App::PropertyEnumeration*>(step.getPropertyByName("MoveType"));
    auto root = refs ? refs->getValue() : nullptr;
    if (!root || !moveProp || fraction <= 0.0) {
        return;
    }
    fraction = std::min(1.0, fraction);
    const char* type = typeProp && typeProp->hasEnums() ? typeProp->getValueAsString() : nullptr;
    bool radial = type && std::strcmp(type, "Radial") == 0;
    const auto& move = moveProp->getValue();
    // A radial step goes away from the center by four times its move's
    // length over the assembly's size, as upstream's
    double factor = radial && frame.size > 0 ? 4.0 * move.getPosition().Length() / frame.size : 0;
    auto part = radial ? Base::Placement() : (fraction >= 1.0 ? move : move.pow(fraction));

    for (const auto& sub : refs->getSubValues()) {
        if (sub.find('?') != std::string::npos) {
            continue;
        }
        auto obj = getObjFromRef(root, sub);
        if (!placementOf(obj)) {
            continue;
        }
        auto parent = parentFrameOf(root, sub, obj, frame);
        auto rest = frame.restOf(obj);
        auto now = frame.placedOf(obj);
        auto center = frame.restCenterOf(obj);
        Base::Vector3d from;
        (now * rest.inverse()).multVec(center, from);

        Base::Placement next;
        if (radial) {
            Base::Vector3d away;
            parent.multVec(from, away);
            away -= frame.center;
            Base::Vector3d local;
            parent.getRotation().inverse().multVec(away, local);
            next = now;
            next.setPosition(now.getPosition() + local * (factor * fraction));
        }
        else {
            next = part * now;
        }
        Base::Vector3d to;
        (next * rest.inverse()).multVec(center, to);

        if (frame.placed.find(obj) == frame.placed.end()) {
            frame.order.push_back(obj);
        }
        frame.placed[obj] = next;
        ExplodedPose::Trail trail;
        trail.step = stepIndex;
        parent.multVec(from, trail.from);
        parent.multVec(to, trail.to);
        trails.push_back(trail);
    }
}



PROPERTY_SOURCE(Assembly::Snapshot, App::DocumentObject)

Snapshot::Snapshot()
{
    ADD_PROPERTY_TYPE(Components, (), "Snapshot", App::Prop_None,
                      "List of components captured in this snapshot.");
    ADD_PROPERTY_TYPE(Placements, (), "Snapshot", App::Prop_None,
                      "List of corresponding placements for the components.");
    ADD_PROPERTY_TYPE(Visibilities, (), "Snapshot", App::Prop_None,
                      "List of visibility states for the components.");
    ADD_PROPERTY_TYPE(SolveOnActivation, (true), "Snapshot", App::Prop_None,
                      "If true, applying the snapshot will solve the assembly after restoring "
                      "the placements.");
}


// Python features --------------------------------------------------------

namespace App
{
/// @cond DOXERR
PROPERTY_SOURCE_TEMPLATE(Assembly::ExplodedViewPython, Assembly::ExplodedView)
template<>
const char* Assembly::ExplodedViewPython::getViewProviderName() const
{
    return "Gui::ViewProviderPythonFeature";
}
template<>
PyObject* Assembly::ExplodedViewPython::getPyObject()
{
    if (PythonObject.is(Py::_None())) {
        PythonObject =
            Py::Object(new App::FeaturePythonPyT<Assembly::ExplodedViewPy>(this), true);
    }
    return Py::new_reference_to(PythonObject);
}
template class AssemblyExport FeaturePythonT<Assembly::ExplodedView>;

PROPERTY_SOURCE_TEMPLATE(Assembly::ExplodedViewStepPython, Assembly::ExplodedViewStep)
template<>
const char* Assembly::ExplodedViewStepPython::getViewProviderName() const
{
    return "Gui::ViewProviderPythonFeature";
}
template<>
PyObject* Assembly::ExplodedViewStepPython::getPyObject()
{
    if (PythonObject.is(Py::_None())) {
        PythonObject =
            Py::Object(new App::FeaturePythonPyT<Assembly::ExplodedViewStepPy>(this), true);
    }
    return Py::new_reference_to(PythonObject);
}
template class AssemblyExport FeaturePythonT<Assembly::ExplodedViewStep>;

PROPERTY_SOURCE_TEMPLATE(Assembly::SnapshotPython, Assembly::Snapshot)
template<>
const char* Assembly::SnapshotPython::getViewProviderName() const
{
    return "Gui::ViewProviderPythonFeature";
}
template class AssemblyExport FeaturePythonT<Assembly::Snapshot>;
/// @endcond
}  // namespace App
