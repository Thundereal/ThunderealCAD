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

#include <cstring>

#include "DrawEdgeLabel.h"

using namespace TechDraw;

PROPERTY_SOURCE(TechDraw::DrawEdgeLabel, TechDraw::DrawViewBalloon)

DrawEdgeLabel::DrawEdgeLabel()
{
    static const char* group = "Edge Label";
    ADD_PROPERTY_TYPE(SourceEdge, (""), group, App::Prop_ReadOnly,
                      "The edge of the view's source the label is of");
    ADD_PROPERTY_TYPE(SourceText, (""), group, App::Prop_ReadOnly,
                      "The text the source gave at the last import: a Text other than it is "
                      "the user's own, kept");
}

bool DrawEdgeLabel::isCustomized() const
{
    return std::strcmp(Text.getValue(), SourceText.getValue()) != 0;
}
