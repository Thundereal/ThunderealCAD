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

#include <cmath>

#include "DrawPMICallout.h"

using namespace TechDraw;

PROPERTY_SOURCE(TechDraw::DrawPMICallout, TechDraw::DrawViewBalloon)

DrawPMICallout::DrawPMICallout()
{
    static const char* group = "PMI";
    ADD_PROPERTY_TYPE(Source, (nullptr), group, App::Prop_None,
                      "The PMI callout of the 3D model shown: what it says is the 3D one's");
    // On the page, of an object inside the part's Body
    Source.setScope(App::LinkScope::Global);
    ADD_PROPERTY_TYPE(ImportOffset, (), group,
                      App::PropertyType(App::Prop_ReadOnly | App::Prop_Hidden),
                      "Where the last import stood it against its leader's end: one not there "
                      "now was moved by the user, kept where they put it");
    // Its own drawing is the frame or symbol; no bubble, no text of a balloon's
    BubbleShape.setValue("None");
    BubbleShape.setStatus(App::Property::Hidden, true);
    Text.setStatus(App::Property::Hidden, true);
    ShapeScale.setStatus(App::Property::Hidden, true);
    TextWrapLen.setStatus(App::Property::Hidden, true);
    KinkLength.setValue(0.0);
}

void DrawPMICallout::onChanged(const App::Property* prop)
{
    if (prop == &Source && !isRestoring()) {
        requestPaint();
    }
    DrawViewBalloon::onChanged(prop);
}

bool DrawPMICallout::isMoved() const
{
    const auto& at = ImportOffset.getValues();
    if (at.size() != 2) {
        return true;
    }
    return std::abs(X.getValue() - OriginX.getValue() - at[0]) > 1e-6
        || std::abs(Y.getValue() - OriginY.getValue() - at[1]) > 1e-6;
}

void DrawPMICallout::setImported()
{
    ImportOffset.setValues({X.getValue() - OriginX.getValue(), Y.getValue() - OriginY.getValue()});
}
