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

#include <Base/Tools.h>

#include "Callout.h"
#include "View.h"

using namespace PMI;

const char* View::StandardEnums[] = {"Custom", "Front", "Top", "Right", "Isometric", nullptr};

PROPERTY_SOURCE(PMI::View, PMI::Annotation)

View::View()
{
    ADD_PROPERTY_TYPE(Camera, (Base::Placement()), "View", App::Prop_None,
                      "Where the camera looks: the origin the point looked at, z toward the "
                      "viewer, y up");
    ADD_PROPERTY_TYPE(Height, (0.0), "View", App::Prop_None,
                      "The height of what shows; 0 fits everything");
    ADD_PROPERTY_TYPE(Standard, (0L), "View", App::Prop_None,
                      "A standard view drives the camera's direction; Custom keeps it");
    Standard.setEnums(StandardEnums);
    ADD_PROPERTY_TYPE(Perspective, (false), "View", App::Prop_None, "A perspective camera");
    ADD_PROPERTY_TYPE(AnnotationPlane, (true), "View", App::Prop_None,
                      "Callouts made in this view lie in its plane");
    ADD_PROPERTY_TYPE(Clip, (false), "View", App::Prop_None,
                      "Cut away what is in front of ClipPlane");
    ADD_PROPERTY_TYPE(ClipPlane, (Base::Placement()), "View", App::Prop_None,
                      "The clipping plane, its z toward the viewer");
    ADD_PROPERTY_TYPE(Objects, (), "View", App::Prop_None,
                      "The objects that show while the view is active; empty shows all");
    // Refers, as the set's Part does: claims nothing
    Objects.setReferOnly(true);
}

Base::Rotation View::standardRotation(StandardValue which)
{
    // Rotations taking the view's frame (x right, y up, z toward the
    // viewer) to the model's, as the 3D view's standard views have it
    switch (which) {
        case Front:
            // Looking along +Y: z toward the viewer is -Y, up is +Z
            return Base::Rotation::makeRotationByAxes(Base::Vector3d(1, 0, 0),
                                                      Base::Vector3d(0, 0, 1),
                                                      Base::Vector3d(0, -1, 0), "ZXY");
        case Top:
            return Base::Rotation();
        case Right:
            return Base::Rotation::makeRotationByAxes(Base::Vector3d(0, 1, 0),
                                                      Base::Vector3d(0, 0, 1),
                                                      Base::Vector3d(1, 0, 0), "ZXY");
        case Isometric: {
            Base::Vector3d z(1, -1, 1);
            z.Normalize();
            Base::Vector3d up(0, 0, 1);
            Base::Vector3d x = up.Cross(z);
            x.Normalize();
            Base::Vector3d y = z.Cross(x);
            return Base::Rotation::makeRotationByAxes(x, y, z, "ZXY");
        }
        case Custom:
            break;
    }
    return Base::Rotation();
}

void View::onChanged(const App::Property* prop)
{
    if (!isRestoring()) {
        if (prop == &Standard && Standard.getValue() != Custom) {
            Base::StateLocker guard(_driving);
            Base::Placement pla = Camera.getValue();
            pla.setRotation(standardRotation(static_cast<StandardValue>(Standard.getValue())));
            Camera.setValue(pla);
        }
        else if (prop == &Camera && !_driving && Standard.getValue() != Custom) {
            // Turned by hand: no longer the standard one
            Base::Rotation want =
                standardRotation(static_cast<StandardValue>(Standard.getValue()));
            if (!Camera.getValue().getRotation().isSame(want, 1e-9)) {
                Standard.setValue(static_cast<long>(Custom));
            }
        }
    }
    Annotation::onChanged(prop);
}

App::DocumentObjectExecReturn* View::execute()
{
    setState(Ok);
    return App::DocumentObject::StdReturn;
}

gp_Ax3 View::frame() const
{
    const Base::Placement& pla = Camera.getValue();
    Base::Vector3d o = pla.getPosition();
    Base::Vector3d z = pla.getRotation().multVec(Base::Vector3d(0, 0, 1));
    Base::Vector3d x = pla.getRotation().multVec(Base::Vector3d(1, 0, 0));
    return gp_Ax3(gp_Pnt(o.x, o.y, o.z), gp_Dir(z.x, z.y, z.z), gp_Dir(x.x, x.y, x.z));
}

std::vector<App::DocumentObject*> View::members() const
{
    std::vector<App::DocumentObject*> res;
    for (auto obj : getInList()) {
        auto c = freecad_cast<Callout*>(obj);
        if (c && c->inView(this)) {
            res.push_back(c);
        }
    }
    return res;
}
