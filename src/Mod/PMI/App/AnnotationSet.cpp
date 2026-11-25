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

#include "Annotation.h"
#include "AnnotationSet.h"
#include "TolerancedFeature.h"

#include <App/Document.h>
#include <Mod/Part/App/BodyBase.h>

using namespace PMI;

const char* AnnotationSet::StandardEnums[] = {"ISO GPS",
                                              "ASME Y14.5-2018",
                                              "ASME Y14.5-2009",
                                              nullptr};

PROPERTY_SOURCE(PMI::AnnotationSet, App::DocumentObjectGroup)

AnnotationSet::AnnotationSet()
{
    ADD_PROPERTY_TYPE(Part, (nullptr), "PMI", App::Prop_None,
                      "What it annotates: the Body it is in, or the shape it sits beside");
    // It refers, and owns nothing: else, headless, an App::Part takes the
    // solid for the set's, and exports the set without it
    Part.setReferOnly(true);
    ADD_PROPERTY_TYPE(Standard, (0L), "PMI", App::Prop_None,
                      "The standard the annotations follow: ISO GPS (independency principle) "
                      "or ASME Y14.5 (envelope principle)");
    Standard.setEnums(StandardEnums);
    ADD_PROPERTY_TYPE(GeneralTolerance, (""), "PMI", App::Prop_None,
                      "The tolerance of what no annotation tolerances, e.g. ISO 2768-mK");
    ADD_PROPERTY_TYPE(DecimalPlaces, (2), "PMI", App::Prop_None,
                      "Decimal places a dimension shows unless it says otherwise");
}

bool AnnotationSet::allowObject(App::DocumentObject* obj)
{
    return obj && obj->isDerivedFrom<Annotation>();
}

AnnotationSet* AnnotationSet::find(const App::DocumentObject* part)
{
    if (!part || !part->getDocument()) {
        return nullptr;
    }
    // A solid body's set is its body's
    if (auto body = Part::BodyBase::findBodyOf(part)) {
        if (body != part && !part->isDerivedFrom<Part::BodyBase>()) {
            if (auto set = find(body)) {
                return set;
            }
        }
    }
    for (auto obj : part->getDocument()->getObjectsOfType(AnnotationSet::getClassTypeId())) {
        auto set = static_cast<AnnotationSet*>(obj);
        if (set->Part.getValue() == part) {
            return set;
        }
    }
    return nullptr;
}

const App::DocumentObject* AnnotationSet::solidOf(const TolerancedFeature* feature)
{
    if (!feature) {
        return nullptr;
    }
    const auto& objs = feature->References.getValues();
    return objs.empty() ? nullptr : objs.front();
}

App::DocumentObject* AnnotationSet::getPart() const
{
    if (auto part = Part.getValue()) {
        return part;
    }
    return Part::BodyBase::findBodyOf(this);
}

void AnnotationSet::setupObject()
{
    App::DocumentObjectGroup::setupObject();
    enforceRecompute();
}

App::DocumentObjectExecReturn* AnnotationSet::execute()
{
    // Put in a Body, it names the Body: being added is the Body's change, not
    // this object's, so it is found here
    auto body = Part::BodyBase::findBodyOf(this);
    if (body && Part.getValue() != body) {
        Part.setValue(body);
    }
    return App::DocumentObjectGroup::execute();
}

void AnnotationSet::onChanged(const App::Property* prop)
{
    // Its members are drawn to its decimal places and checked by its
    // standard: they do not depend on it (it lists them), so they are told
    if ((prop == &DecimalPlaces || prop == &Standard || prop == &GeneralTolerance)
        && !isRestoring() && getDocument()
        && !getDocument()->testStatus(App::Document::Restoring)) {
        for (auto obj : Group.getValues()) {
            if (obj) {
                obj->touch();
            }
        }
    }
    App::DocumentObjectGroup::onChanged(prop);
}

std::vector<TolerancedFeature*> AnnotationSet::features() const
{
    std::vector<TolerancedFeature*> res;
    for (auto obj : Group.getValues()) {
        if (auto feat = freecad_cast<TolerancedFeature*>(obj)) {
            res.push_back(feat);
        }
    }
    return res;
}
