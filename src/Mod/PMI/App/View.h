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


#ifndef PMI_VIEW_H
#define PMI_VIEW_H

#include <gp_Ax3.hxx>

#include <App/PropertyGeo.h>
#include <App/PropertyStandard.h>
#include <App/PropertyUnits.h>

#include "Annotation.h"

namespace PMI
{

/** A view of the annotations (docs/GDandT.md sec 6.3): a camera, the
 * annotations it shows, optional clipping -- SolidWorks' 3D view, CATIA's
 * capture, STEP's saved view.
 *
 * The members name their views (Callout::Views), not the other way round:
 * a callout made in a view takes its plane, so it depends on the view, and
 * moving the camera lays it out again.
 */
class PMIExport View: public Annotation
{
    PROPERTY_HEADER_WITH_OVERRIDE(PMI::View);

public:
    View();

    const char* getViewProviderName() const override
    {
        return "PMIGui::ViewProviderView";
    }

    /// Where the camera looks: the origin the point looked at, z toward
    /// the viewer, y up
    App::PropertyPlacement Camera;
    /// The height of what shows; 0 fits everything
    App::PropertyLength Height;
    App::PropertyEnumeration Standard;
    App::PropertyBool Perspective;
    /// Callouts made in this view lie in its plane
    App::PropertyBool AnnotationPlane;
    /// Cut away what is in front of ClipPlane (its z toward the viewer)
    App::PropertyBool Clip;
    App::PropertyPlacement ClipPlane;

    enum StandardValue
    {
        Custom,
        Front,
        Top,
        Right,
        Isometric,
    };

    App::DocumentObjectExecReturn* execute() override;

    /// The camera's frame: z toward the viewer, x to the right
    gp_Ax3 frame() const;
    /// The annotations it shows: the callouts naming it
    std::vector<App::DocumentObject*> members() const;

    /// The orientation of a standard view: z toward the viewer, y up
    static Base::Rotation standardRotation(StandardValue which);

    static const char* StandardEnums[];

protected:
    void onChanged(const App::Property* prop) override;

private:
    bool _driving = false;
};

}  // namespace PMI

#endif  // PMI_VIEW_H
