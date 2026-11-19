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

#ifndef PMI_NOTE_H
#define PMI_NOTE_H

#include <App/PropertyLinks.h>
#include <App/PropertyStandard.h>

#include "Callout.h"

namespace PMI
{

/// Free text or a flag note (docs/GDandT.md sec 4); upstream's PMI_Note
class PMIExport Note: public Callout
{
    PROPERTY_HEADER_WITH_OVERRIDE(PMI::Note);

public:
    Note();

    App::PropertyStringList Text;
    /// A flag note's number; 0 for a plain note
    App::PropertyInteger Flag;
    /// What it is about, if anything: a toleranced feature
    App::PropertyLink Feature;

    App::DocumentObjectExecReturn* execute() override;
    TolerancedFeature* calloutFeature() const override;
    void layout(Drawing& d, double h) const override;
    std::u32string equivalentText() const override;
    LeaderStyle leaderStyle() const override
    {
        return Feature.getValue() ? ArrowLeader : NoLeader;
    }
};

}  // namespace PMI

#endif  // PMI_NOTE_H
