/***************************************************************************
 *   Copyright (c) 2022 Ajinkya Dahale <dahale.a.p@gmail.com>              *
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

#include "FemConstraint.h"


namespace Fem
{

class FemExport ConstraintRigidBody: public Fem::Constraint
{
    PROPERTY_HEADER_WITH_OVERRIDE(Fem::ConstraintRigidBody);

public:
    /// Constructor
    ConstraintRigidBody();

    App::PropertyBool EnableAmplitude;
    App::PropertyStringList AmplitudeValues;

    // Rigid Body parameters
    App::PropertyPosition ReferenceNode;
    App::PropertyPosition Displacement;
    App::PropertyRotation Rotation;
    App::PropertyForce ForceX;
    App::PropertyForce ForceY;
    App::PropertyForce ForceZ;
    App::PropertyMoment MomentX;
    App::PropertyMoment MomentY;
    App::PropertyMoment MomentZ;
    App::PropertyEnumeration TranslationalModeX;
    App::PropertyEnumeration TranslationalModeY;
    App::PropertyEnumeration TranslationalModeZ;
    App::PropertyEnumeration RotationalModeX;
    App::PropertyEnumeration RotationalModeY;
    App::PropertyEnumeration RotationalModeZ;
    // Not upstream: how the reference node holds the faces -- Rigid, the
    // faces moving as one with it (CalculiX's *RIGID BODY), or Distributing,
    // its loads spread over the faces without stiffening them (*COUPLING
    // with *DISTRIBUTING; loads only, CalculiX takes no constraint on it)
    App::PropertyEnumeration Coupling;
    // Not upstream: a distributing coupling on the faces' parts within this
    // distance of the reference node only, a load put on a patch of a face
    // (a contact's); 0, on all of them
    App::PropertyLength CouplingRadius;

    /// recalculate the object
    App::DocumentObjectExecReturn* execute() override;

    /// returns the type name of the ViewProvider
    const char* getViewProviderName() const override
    {
        return "FemGui::ViewProviderFemConstraintRigidBody";
    }

protected:
    void onChanged(const App::Property* prop) override;

private:
    static const char* boundaryModeEnum[];
    static const char* couplingEnum[];
};

}  // namespace Fem
