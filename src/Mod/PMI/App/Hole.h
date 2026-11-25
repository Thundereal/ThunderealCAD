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

#ifndef PMI_HOLE_H
#define PMI_HOLE_H

#include <App/PropertyGeo.h>
#include <App/PropertyLinks.h>
#include <App/PropertyStandard.h>
#include <App/PropertyUnits.h>

#include "Annotation.h"

namespace PMI
{

/** A hole as a STEP file defines it (AP242 ed3 hole features: NIST STC-09's
 * basic_round_hole), one occurrence of it: what it is -- its diameter and
 * depth with their tolerances, whether it goes through -- where it is, and
 * the faces it is of (docs/GDandT.md sec 10.26). Kept to be written again;
 * it draws nothing.
 */
class PMIExport Hole: public Annotation
{
    PROPERTY_HEADER_WITH_OVERRIDE(PMI::Hole);

public:
    Hole();

    /// Nothing drawn: a tree item
    const char* getViewProviderName() const override
    {
        return "Gui::ViewProviderDocumentObject";
    }

    /// The STEP type of its definition ('basic_round_hole')
    App::PropertyString Kind;
    /// The name of its definition, which its occurrences share ('Hole.2')
    App::PropertyString DefinitionName;
    App::PropertyLength Diameter;
    App::PropertyBool DiameterToleranced;
    App::PropertyDistance DiameterLower;
    App::PropertyDistance DiameterUpper;
    /// Given a depth, or none (a hole through)
    App::PropertyBool HasDepth;
    App::PropertyLength Depth;
    App::PropertyBool DepthToleranced;
    App::PropertyDistance DepthLower;
    App::PropertyDistance DepthUpper;
    App::PropertyBool Through;
    /// Its definition's axes, in its own coordinates
    App::PropertyPlacement Axes;
    /// This occurrence's name in the file, which need not be unique ('RectPattern.1')
    App::PropertyString OccurrenceName;
    /// Where this occurrence of it is, in its part's coordinates
    App::PropertyPlacement Position;
    /// Its faces
    App::PropertyLink Feature;
};

}  // namespace PMI

#endif  // PMI_HOLE_H
