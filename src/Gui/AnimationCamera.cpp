// SPDX-License-Identifier: LGPL-2.1-or-later
/****************************************************************************
 *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
 *                                                                          *
 *   This file is part of FreeCAD.                                          *
 *                                                                          *
 *   FreeCAD is free software: you can redistribute it and/or modify it     *
 *   under the terms of the GNU Lesser General Public License as            *
 *   published by the Free Software Foundation, either version 2.1 of the   *
 *   License, or (at your option) any later version.                        *
 *                                                                          *
 ****************************************************************************/

/** The camera track (docs/Animation.md sec 8): a track of kind "Camera",
 * its keys held by the Animation, the camera of a 3D view read and written
 * here.
 *
 * A key is the point looked at, the orientation, the distance to it and
 * the size -- an orthographic camera's height, or a perspective one's
 * angle in degrees -- nine numbers: S3 Q4 S2. Between keys the point and
 * the orientation move each on its own curve and the camera stands back
 * from the point by the distance, so it turns about what it looks at
 * rather than cutting across it.
 *
 * The track's data names the view by its persistent name and the camera's
 * kind: "View1/ortho". A track plays in that view, or in the document's
 * active 3D view when there is no view of that name (another session's
 * file). Stop puts each view's camera back as it was before the first
 * frame.
 */

#include "PreCompiled.h"

#include <cmath>
#include <map>

#include <Inventor/SbRotation.h>
#include <Inventor/SbVec3f.h>
#include <Inventor/nodes/SoCamera.h>
#include <Inventor/nodes/SoOrthographicCamera.h>
#include <Inventor/nodes/SoPerspectiveCamera.h>

#include <App/Animation.h>
#include <App/Document.h>
#include <Base/Tools.h>

#include "Application.h"
#include "Document.h"
#include "View3DInventor.h"
#include "View3DInventorViewer.h"

using namespace Gui;

namespace
{

class CameraTrackHandler: public App::AnimationTrackHandler
{
public:
    bool layout(const App::AnimationTrack&, App::Anim::Layout& layout) const override
    {
        layout.clear();
        layout.rotation = App::Anim::RotationMode::Shortest;
        layout.add(App::Anim::Kind::Scalar, 3);
        layout.add(App::Anim::Kind::Quaternion, 4);
        layout.add(App::Anim::Kind::Scalar, 2);
        return true;
    }

    std::string newTrackData(const std::string& hint) const override
    {
        auto view = findView(App::GetApplication().getActiveDocument(), hint);
        if (!view) {
            return hint;
        }
        auto cam = camera(view);
        std::string name = view->getPersistentName();
        return name + (cam && cam->isOfType(SoPerspectiveCamera::getClassTypeId())
                           ? "/perspective"
                           : "/ortho");
    }

    std::string describe(const App::AnimationTrack& track) const override
    {
        std::string name = viewName(track.handlerData);
        return name.empty() ? std::string("Camera") : "Camera (" + name + ")";
    }

    bool read(const App::Animation& anim, const App::AnimationTrack& track,
              std::vector<double>& nums) const override
    {
        auto view = findView(anim.getDocument(), viewName(track.handlerData));
        auto cam = view ? camera(view) : nullptr;
        if (!cam) {
            return false;
        }
        nums.clear();
        SbRotation rot = cam->orientation.getValue();
        SbVec3f dir;
        rot.multVec(SbVec3f(0, 0, -1), dir);
        float fd = cam->focalDistance.getValue();
        SbVec3f focal = cam->position.getValue() + dir * fd;
        for (int i = 0; i < 3; ++i) {
            nums.push_back(focal[i]);
        }
        float q[4];
        rot.getValue(q[0], q[1], q[2], q[3]);
        for (float v : q) {
            nums.push_back(v);
        }
        nums.push_back(fd);
        nums.push_back(size(cam));
        return true;
    }

    bool write(App::Animation& anim, const App::AnimationTrack& track,
               const std::vector<double>& nums) override
    {
        auto view = findView(anim.getDocument(), viewName(track.handlerData));
        auto cam = view ? camera(view) : nullptr;
        if (!cam || nums.size() != 9) {
            return false;
        }
        // The camera before the first frame is the rest, and so is one the
        // user moved since the last frame, as for a property's value
        auto key = restKey(anim, track);
        std::vector<double> now;
        if (read(anim, track, now)) {
            auto it = written.find(key);
            if (!rests.count(key) || it == written.end() || !same(it->second, now)) {
                rests[key] = now;
            }
        }
        apply(cam, nums, isPerspective(track.handlerData));
        if (read(anim, track, now)) {
            written[key] = std::move(now);
        }
        return true;
    }

    void deactivate(App::Animation& anim, const App::AnimationTrack& track) override
    {
        auto it = rests.find(restKey(anim, track));
        if (it == rests.end()) {
            return;
        }
        auto view = findView(anim.getDocument(), viewName(track.handlerData));
        if (auto cam = view ? camera(view) : nullptr) {
            apply(cam, it->second, cam->isOfType(SoPerspectiveCamera::getClassTypeId()));
        }
        written.erase(it->first);
        rests.erase(it);
    }

private:
    static bool same(const std::vector<double>& a, const std::vector<double>& b)
    {
        if (a.size() != b.size()) {
            return false;
        }
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (std::fabs(a[i] - b[i]) > 1e-5 * (1 + std::fabs(a[i]))) {
                return false;
            }
        }
        return true;
    }

    static std::string viewName(const std::string& data)
    {
        return data.substr(0, data.find('/'));
    }

    static bool isPerspective(const std::string& data)
    {
        return data.size() > 12 && data.compare(data.size() - 12, 12, "/perspective") == 0;
    }

    static std::string restKey(const App::Animation& anim, const App::AnimationTrack& track)
    {
        return anim.getFullName() + "#" + std::to_string(track.id);
    }

    /// The 3D view named `name` of `doc`, else its active 3D view, else its first
    static View3DInventor* findView(App::Document* doc, const std::string& name)
    {
        auto gdoc = doc ? Application::Instance->getDocument(doc) : nullptr;
        if (!gdoc) {
            return nullptr;
        }
        View3DInventor* first = nullptr;
        for (auto v : gdoc->getMDIViewsOfType(View3DInventor::getClassTypeId())) {
            auto view = static_cast<View3DInventor*>(v);
            if (!name.empty() && view->getPersistentName() == name) {
                return view;
            }
            if (!first) {
                first = view;
            }
        }
        if (auto active = freecad_dynamic_cast<View3DInventor>(gdoc->getActiveView())) {
            return active;
        }
        return first;
    }

    static SoCamera* camera(View3DInventor* view)
    {
        auto viewer = view->getViewer();
        return viewer ? viewer->getSoRenderManager()->getCamera() : nullptr;
    }

    static double size(SoCamera* cam)
    {
        if (cam->isOfType(SoOrthographicCamera::getClassTypeId())) {
            return static_cast<SoOrthographicCamera*>(cam)->height.getValue();
        }
        if (cam->isOfType(SoPerspectiveCamera::getClassTypeId())) {
            return Base::toDegrees<double>(
                static_cast<SoPerspectiveCamera*>(cam)->heightAngle.getValue());
        }
        return 0;
    }

    static void apply(SoCamera* cam, const std::vector<double>& nums, bool perspective)
    {
        SbRotation rot(static_cast<float>(nums[3]), static_cast<float>(nums[4]),
                       static_cast<float>(nums[5]), static_cast<float>(nums[6]));
        SbVec3f dir;
        rot.multVec(SbVec3f(0, 0, -1), dir);
        auto fd = static_cast<float>(nums[7]);
        SbVec3f focal(static_cast<float>(nums[0]), static_cast<float>(nums[1]),
                      static_cast<float>(nums[2]));
        cam->enableNotify(false);
        cam->orientation.setValue(rot);
        cam->focalDistance.setValue(fd);
        cam->position.setValue(focal - dir * fd);
        // The size as keyed, where the camera is still of the kind keyed
        if (!perspective && cam->isOfType(SoOrthographicCamera::getClassTypeId())) {
            static_cast<SoOrthographicCamera*>(cam)->height.setValue(static_cast<float>(nums[8]));
        }
        else if (perspective && cam->isOfType(SoPerspectiveCamera::getClassTypeId())) {
            static_cast<SoPerspectiveCamera*>(cam)->heightAngle.setValue(
                static_cast<float>(Base::toRadians<double>(nums[8])));
        }
        cam->enableNotify(true);
        cam->touch();
    }

    std::map<std::string, std::vector<double>> rests;
    std::map<std::string, std::vector<double>> written;
};

}  // namespace

namespace Gui
{

void registerAnimationCamera()
{
    App::AnimationTrackHandler::registerHandler("Camera", std::make_shared<CameraTrackHandler>());
}

}  // namespace Gui
