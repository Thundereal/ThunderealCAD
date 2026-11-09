// SPDX-License-Identifier: LGPL-2.1-or-later
// SPDX-FileCopyrightText: 2026 Max Wilfinger
// SPDX-FileNotice: Part of the FreeCAD project.

/******************************************************************************
 *                                                                            *
 *   FreeCAD is free software: you can redistribute it and/or modify          *
 *   it under the terms of the GNU Lesser General Public License as           *
 *   published by the Free Software Foundation, either version 2.1            *
 *   of the License, or (at your option) any later version.                   *
 *                                                                            *
 *   FreeCAD is distributed in the hope that it will be useful,               *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty              *
 *   of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.                  *
 *   See the GNU Lesser General Public License for more details.              *
 *                                                                            *
 *   You should have received a copy of the GNU Lesser General Public         *
 *   License along with FreeCAD. If not, see https://www.gnu.org/licenses     *
 *                                                                            *
 ******************************************************************************/

#ifndef PARTDESIGN_FEATUREDEFEATURING_H
#define PARTDESIGN_FEATUREDEFEATURING_H

#include "FeatureDressUp.h"

namespace PartDesign
{

/// Removes the picked faces of the base and heals the gap they leave: takes
/// a hole, a fillet, a boss or a pocket off a solid (upstream c70d9b2992)
class PartDesignExport Defeaturing : public DressUp
{
    PROPERTY_HEADER_WITH_OVERRIDE(PartDesign::Defeaturing);

public:
    Defeaturing();

    App::DocumentObjectExecReturn *execute() override;

    /// Its own elements, which a tree hover lights: what it generates, and
    /// the faces it changed -- the neighbours extended over the gap the
    /// picked faces leave. Not every face its history calls modified:
    /// OCCT rebuilds them all (docs/HandsOnQueue.md entry 248)
    bool isElementGenerated(const Part::TopoShape &shape,
                            const Data::MappedName &name) const override;

    const char* getViewProviderName() const override {
        return "PartDesignGui::ViewProviderDefeaturing";
    }
};

} //namespace PartDesign

#endif // PARTDESIGN_FEATUREDEFEATURING_H
