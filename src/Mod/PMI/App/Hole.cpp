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

#include "Hole.h"

using namespace PMI;

PROPERTY_SOURCE(PMI::Hole, PMI::Annotation)

Hole::Hole()
{
    ADD_PROPERTY_TYPE(Kind, ("basic_round_hole"), "Hole", App::Prop_None,
                      "The STEP type of its definition");
    ADD_PROPERTY_TYPE(DefinitionName, (""), "Hole", App::Prop_None,
                      "The name of its definition, which its occurrences share");
    ADD_PROPERTY_TYPE(Diameter, (0.0), "Hole", App::Prop_None, "Its diameter");
    ADD_PROPERTY_TYPE(DiameterToleranced, (false), "Hole", App::Prop_None,
                      "Whether its diameter has a tolerance");
    ADD_PROPERTY_TYPE(DiameterLower, (0.0), "Hole", App::Prop_None,
                      "Its diameter's lower deviation");
    ADD_PROPERTY_TYPE(DiameterUpper, (0.0), "Hole", App::Prop_None,
                      "Its diameter's upper deviation");
    ADD_PROPERTY_TYPE(HasDepth, (false), "Hole", App::Prop_None, "Whether it has a depth");
    ADD_PROPERTY_TYPE(Depth, (0.0), "Hole", App::Prop_None, "Its depth");
    ADD_PROPERTY_TYPE(DepthToleranced, (false), "Hole", App::Prop_None,
                      "Whether its depth has a tolerance");
    ADD_PROPERTY_TYPE(DepthLower, (0.0), "Hole", App::Prop_None, "Its depth's lower deviation");
    ADD_PROPERTY_TYPE(DepthUpper, (0.0), "Hole", App::Prop_None, "Its depth's upper deviation");
    ADD_PROPERTY_TYPE(Through, (false), "Hole", App::Prop_None, "Whether it goes through");
    ADD_PROPERTY_TYPE(Axes, (Base::Placement()), "Hole", App::Prop_None,
                      "Its definition's axes, in its own coordinates");
    ADD_PROPERTY_TYPE(OccurrenceName, (""), "Hole", App::Prop_None,
                      "This occurrence's name in the file");
    ADD_PROPERTY_TYPE(Position, (Base::Placement()), "Hole", App::Prop_None,
                      "Where this occurrence is, in its part's coordinates");
    ADD_PROPERTY_TYPE(Feature, (nullptr), "Hole", App::Prop_None, "Its faces");
}
