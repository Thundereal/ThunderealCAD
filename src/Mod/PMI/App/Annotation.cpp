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

using namespace PMI;

const char* Annotation::StateEnums[] = {"Ok", "Remapped", "Broken", "Invalid", nullptr};

PROPERTY_SOURCE_ABSTRACT(PMI::Annotation, App::DocumentObject)

Annotation::Annotation()
{
    // Not an output: what depends on this object sees its state change
    ADD_PROPERTY_TYPE(Condition, (0L), "PMI", App::Prop_None,
                      "Ok; Remapped (found again by history or geometry, look at it); Broken "
                      "(what it refers to is gone); Invalid (its values do not make sense)");
    Condition.setEnums(StateEnums);
    ADD_PROPERTY_TYPE(ConditionMessage, (""), "PMI",
                      App::PropertyType(App::Prop_Output | App::Prop_ReadOnly),
                      "Why the state is what it is");
}

void Annotation::setupObject()
{
    App::DocumentObject::setupObject();
    enforceRecompute();
}

AnnotationSet* Annotation::getSet() const
{
    for (auto obj : getInList()) {
        if (auto set = freecad_cast<AnnotationSet*>(obj)) {
            if (set->hasObject(this)) {
                return set;
            }
        }
    }
    return nullptr;
}

void Annotation::setState(StateValue state, const std::string& message)
{
    if (Condition.getValue() != state) {
        Condition.setValue(static_cast<long>(state));
    }
    if (ConditionMessage.getStrValue() != message) {
        ConditionMessage.setValue(message);
    }
}
