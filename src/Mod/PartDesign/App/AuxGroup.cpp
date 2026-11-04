/****************************************************************************
 *   Copyright (c) 2020 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
 *                                                                          *
 *   This file is part of the FreeCAD CAx development system.               *
 *                                                                          *
 *   This library is free software; you can redistribute it and/or          *
 *   modify it under the terms of the GNU Library General Public            *
 *   License as published by the Free Software Foundation; either           *
 *   version 2 of the License, or (at your option) any later version.       *
 *                                                                          *
 *   This library  is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of         *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the          *
 *   GNU Library General Public License for more details.                   *
 *                                                                          *
 *   You should have received a copy of the GNU Library General Public      *
 *   License along with this library; see the file COPYING.LIB. If not,     *
 *   write to the Free Software Foundation, Inc., 59 Temple Place,          *
 *   Suite 330, Boston, MA  02111-1307, USA                                 *
 *                                                                          *
 ****************************************************************************/

#include "PreCompiled.h"
#ifndef _PreComp_
#endif

#include <boost/algorithm/string/predicate.hpp>

#include <Mod/Part/App/PartParams.h>

#include "Body.h"
#include "AuxGroup.h"

using namespace PartDesign;

PROPERTY_SOURCE(PartDesign::AuxGroup, App::AuxGroup)

const char *AuxGroup::SketchRole = "Sketches";
const char *AuxGroup::DatumRole = "Datums";
const char *AuxGroup::MiscRole = "Misc";
const char *AuxGroup::SurfaceRole = "Surfaces";

AuxGroup::AuxGroup()
{
    ADD_PROPERTY_TYPE(_Body, (0), "Base",
            (App::PropertyType)(App::Prop_Hidden|App::Prop_ReadOnly|App::Prop_Output), 0);
    setOwnerProperty(&_Body);
}

std::string AuxGroup::getDefaultRole() const
{
    for (auto role : {SketchRole, DatumRole, MiscRole, SurfaceRole}) {
        if (boost::starts_with(getNameInDocument(), role))
            return role;
    }
    return {};
}

AuxGroup::GroupType AuxGroup::getGroupType() const
{
    if (!getNameInDocument())
        return UnknownGroup;
    const auto &role = getRole();
    if (role.empty())
        return OtherGroup;
    if (role == SketchRole)
        return SketchGroup;
    if (role == DatumRole)
        return DatumGroup;
    if (role == MiscRole)
        return MiscGroup;
    if (role == SurfaceRole)
        return SurfaceGroup;
    return UnknownGroup;
}

bool AuxGroup::allowDuplicateLabel() const
{
    return inherited::allowDuplicateLabel()
        && !Part::PartParams::getAuxGroupUniqueLabel();
}

PartDesign::Body *AuxGroup::getBody() const
{
    return Base::freecad_dynamic_cast<Body>(_Body.getValue());
}
