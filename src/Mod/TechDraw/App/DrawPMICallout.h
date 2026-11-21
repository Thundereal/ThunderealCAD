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

#include <App/PropertyLinks.h>
#include <App/PropertyStandard.h>
#include <Mod/TechDraw/TechDrawGlobal.h>

#include "DrawViewBalloon.h"

namespace TechDraw
{

/** A PMI callout of the 3D model in a view (docs/GDandT.md sec 9): a feature
 * control frame, a datum's symbol, a datum target, a note -- drawn from the
 * PMI object it shows (Source), as the 3D view draws it, with a leader to
 * the feature it is about. The 3D object is the master: what the callout
 * says is its, here shown. A balloon in all else: the view it is of
 * (SourceView), where its leader ends (OriginX/Y), where it stands (X/Y).
 */
class TechDrawExport DrawPMICallout: public DrawViewBalloon
{
    PROPERTY_HEADER_WITH_OVERRIDE(TechDraw::DrawPMICallout);

public:
    DrawPMICallout();

    /// The PMI callout shown: a PMI::GeometricTolerance, PMI::Datum,
    /// PMI::DatumTarget or PMI::Note
    App::PropertyLink Source;
    /// Where the last import stood it against its leader's end, (X - OriginX,
    /// Y - OriginY); one not there now was moved by the user
    App::PropertyFloatList ImportOffset;

    /// Whether the user moved it from where the last import stood it
    bool isMoved() const;
    /// Record where an import stands it now
    void setImported();

protected:
    void onChanged(const App::Property* prop) override;
};

}  // namespace TechDraw
