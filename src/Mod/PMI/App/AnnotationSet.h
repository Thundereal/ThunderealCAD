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

#ifndef PMI_ANNOTATIONSET_H
#define PMI_ANNOTATIONSET_H

#include <vector>

#include <App/DocumentObjectGroup.h>
#include <App/PropertyLinks.h>
#include <App/PropertyStandard.h>

#include <Mod/PMI/PMIGlobal.h>

namespace PMI
{

class TolerancedFeature;

/** A part's PMI: its annotations and its settings (docs/GDandT.md sec 4.1).
 *
 * In a PartDesign Body it is one of the Body's members, as the Body's
 * Elements group is: its annotations refer to the Body's solids through
 * their solid bodies, (Solid, "Face6"), which is no cycle -- a reference to
 * the Body itself would be, and the Body's own shape is its tip's, which
 * moves to another solid when one is added (sec 3). A Body holding a set
 * gives even a lone solid a solid body (BodyBase::requireSolidBody). For a
 * shape that is no Body -- an imported part, a Split piece -- the set sits
 * beside it. Its group takes PMI objects only.
 */
class PMIExport AnnotationSet: public App::DocumentObjectGroup
{
    PROPERTY_HEADER_WITH_OVERRIDE(PMI::AnnotationSet);

public:
    AnnotationSet();

    /// What it annotates: the Body it is in, or the shape it sits beside.
    /// Hidden: no dependency -- in a Body, a link to it would be a cycle,
    /// and what an annotation depends on is the faces it refers to
    App::PropertyLinkHidden Part;
    /// The standard the annotations follow: it decides the default
    /// principle and which symbols are legal (sec 4.1, 8)
    App::PropertyEnumeration Standard;
    /// The tolerance of what no annotation tolerances, e.g. "ISO 2768-mK"
    App::PropertyString GeneralTolerance;
    App::PropertyInteger DecimalPlaces;

    bool allowObject(App::DocumentObject* obj) override;
    App::DocumentObjectExecReturn* execute() override;

    /// What it annotates: Part, or the Body it is in
    App::DocumentObject* getPart() const;

protected:
    void onChanged(const App::Property* prop) override;
    void setupObject() override;

public:

    enum StandardValue
    {
        ISO,
        ASME2018,
        ASME2009,
    };
    StandardValue getStandard() const
    {
        return static_cast<StandardValue>(Standard.getValue());
    }
    bool isASME() const
    {
        return getStandard() != ISO;
    }

    /// The set annotating \a part (a Body, one of its solid bodies, or
    /// another shape), or null
    static AnnotationSet* find(const App::DocumentObject* part);

    /// The object a reference of \a feature's names: the solid an
    /// annotation is on, whose datum letters are its own
    static const App::DocumentObject* solidOf(const TolerancedFeature* feature);

    /// The toleranced features in the set
    std::vector<TolerancedFeature*> features() const;

    static const char* StandardEnums[];
};

}  // namespace PMI

#endif  // PMI_ANNOTATIONSET_H
