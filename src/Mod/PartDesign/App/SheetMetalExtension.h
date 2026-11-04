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

#pragma once

#include <string>

#include <App/DocumentObjectExtension.h>
#include <App/PropertyLinks.h>
#include <App/PropertyStandard.h>
#include <App/PropertyUnits.h>
#include <Mod/PartDesign/PartDesignGlobal.h>

#include "PropertySheetModel.h"

namespace PartDesign
{

class SheetMetalRule;

/** What every sheet metal feature has (docs/SheetMetal.md sec 4.2, 4.6)
 *
 * The rule it follows, and its model of the sheet. A feature put in a body
 * takes the rule of the sheet feature it builds on, or makes one if it is
 * the first (Body::insertObject()).
 */
class PartDesignExport SheetMetalExtension : public App::DocumentObjectExtension
{
    EXTENSION_PROPERTY_HEADER_WITH_OVERRIDE(PartDesign::SheetMetalExtension);
    using inherited = App::DocumentObjectExtension;

public:
    SheetMetalExtension();

    App::PropertyXLink Rule;
    PropertySheetModel SheetModel;

    /// The rule, null if none
    SheetMetalRule* getRule() const;

    /// The sheet metal feature the given feature builds on: the first down
    /// its BaseFeature chain, null if none
    static SheetMetalExtension* findBefore(const App::DocumentObject* feature);

    /** The model of the sheet this feature builds on, in this feature's frame
     *
     * Empty if there is none; throws Base::ValueError if its thickness is not
     * the rule's.
     */
    SheetMetal::Model modelBefore(double thickness) const;

    /** The radius a feature's bends take (docs/SheetMetal.md sec 12, ruled
     * 2026-11-03): the rule's, or the feature's own where useRule is off
     */
    static double bendRadius(const SheetMetalRule& rule, const App::PropertyBool& useRule,
                             const App::PropertyLength& own);
    /// useRule changed: own is read only while it is on; turned off, own
    /// starts at the rule's
    void radiusSwitched(const App::PropertyBool& useRule, App::PropertyLength& own) const;

    /// The name of the element that owns a face of the feature's shape,
    /// empty if none
    std::string elementOf(const char* subname) const;

    PyObject* getExtensionPyObject() override;
};

}  // namespace PartDesign
