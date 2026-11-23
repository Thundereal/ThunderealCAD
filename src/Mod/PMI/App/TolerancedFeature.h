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

#ifndef PMI_TOLERANCEDFEATURE_H
#define PMI_TOLERANCEDFEATURE_H

#include <string>
#include <vector>

#include <App/PropertyLinks.h>
#include <App/PropertyStandard.h>
#include <Mod/Part/App/PropertyTopoShape.h>

#include "Annotation.h"
#include "Geometry.h"

namespace PMI
{

/** The faces an annotation is about, shared by every annotation on them
 * (docs/GDandT.md sec 4.2; STEP's shape_aspect).
 *
 * References point at the part's result shape -- the Body, not a feature
 * inside it -- so a face a later feature changes is still found. On each
 * recompute they are resolved (sec 7): by name; failing that by the
 * element history (the faces that came from the one referred to); failing
 * that by geometry against SavedGeometry (the same surface); failing that
 * the feature is Broken, and kept where it was.
 */
class PMIExport TolerancedFeature: public Annotation
{
    PROPERTY_HEADER_WITH_OVERRIDE(PMI::TolerancedFeature);

public:
    TolerancedFeature();

    const char* getViewProviderName() const override
    {
        return "PMIGui::ViewProviderFeature";
    }

    App::PropertyLinkSubList References;
    App::PropertyEnumeration Kind;
    /// The faces resolved, or the derived axis / plane / point
    Part::PropertyPartShape Shape;
    /// The faces as last resolved: what a broken feature is drawn with,
    /// and what the geometry search compares with
    Part::PropertyPartShape SavedGeometry;
    /// The mapped names of the faces as last resolved
    App::PropertyStringList SavedNames;
    /// A feature of size: a cylinder, a sphere, two opposed planes
    App::PropertyBool IsSize;

    enum KindValue
    {
        Integral,
        Axis,
        CentrePlane,
        CentrePoint,
        Apex,
        Group,
        AllAround,
        Between,
    };
    KindValue getKind() const
    {
        return static_cast<KindValue>(Kind.getValue());
    }

    App::DocumentObjectExecReturn* execute() override;

    /// The resolved faces' primitives, in order
    const std::vector<Primitive>& facePrimitives() const
    {
        return _faces;
    }
    /// The bores its planar faces close (Geom::bores): a depth on a
    /// counterbore's floor is of them
    const std::vector<Primitive>& borePrimitives() const
    {
        return _bores;
    }
    /// What a dimension measures to and a datum is simulated from: the
    /// derived element for a derived kind, else the surface of the faces
    /// (their common surface, or the first face's)
    const Primitive& primitive() const
    {
        return _primitive;
    }

    /// Whether this feature refers to exactly these elements of \a part
    bool refersTo(const App::DocumentObject* part, const std::vector<std::string>& subs) const;

    static const char* KindEnums[];

protected:
    void onDocumentRestored() override;

private:
    /// Recompute _faces, _primitive, Shape and IsSize from \a faces;
    /// false with \a why if the kind cannot be made from them
    bool derive(const std::vector<TopoDS_Shape>& faces, std::string& why);

    std::vector<Primitive> _faces;
    std::vector<Primitive> _bores;
    Primitive _primitive;
};

}  // namespace PMI

#endif  // PMI_TOLERANCEDFEATURE_H
