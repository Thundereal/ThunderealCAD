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
 * It sits beside the part it annotates, never inside a Body: it links the
 * Body, and a Body depends on everything in its group, so a set inside
 * would be a cycle (sec 3). Its group takes PMI objects only.
 */
class PMIExport AnnotationSet: public App::DocumentObjectGroup
{
    PROPERTY_HEADER_WITH_OVERRIDE(PMI::AnnotationSet);

public:
    AnnotationSet();

    /// The part annotated: a Body, a Part::Feature, an App::Part
    App::PropertyLink Part;
    /// The standard the annotations follow: it decides the default
    /// principle and which symbols are legal (sec 4.1, 8)
    App::PropertyEnumeration Standard;
    /// The tolerance of what no annotation tolerances, e.g. "ISO 2768-mK"
    App::PropertyString GeneralTolerance;
    App::PropertyInteger DecimalPlaces;

    bool allowObject(App::DocumentObject* obj) override;

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

    /// The set annotating \a part, or null
    static AnnotationSet* find(const App::DocumentObject* part);

    /// The toleranced features in the set
    std::vector<TolerancedFeature*> features() const;

    static const char* StandardEnums[];
};

}  // namespace PMI

#endif  // PMI_ANNOTATIONSET_H
