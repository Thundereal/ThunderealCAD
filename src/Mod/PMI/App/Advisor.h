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

#ifndef PMI_ADVISOR_H
#define PMI_ADVISOR_H

#include <string>
#include <vector>

#include <Mod/PMI/PMIGlobal.h>

namespace PMI
{

class TolerancedFeature;

/** The tolerancing advisor (docs/GDandT.md sec 8; upstream's sub-issue
 * 29782): what may be said of a feature, from what it is -- a plane, two
 * opposed planes, a cylinder or its axis, a sphere, a cone -- so the panel
 * offers only that, and the most likely first.
 */
namespace Advisor
{
/// The characteristics (GeometricTolerance::CharacteristicValue) that apply
/// to the feature, the most usual first
PMIExport std::vector<int> characteristics(const TolerancedFeature* feature);

/// The zone shapes (GeometricTolerance::ZoneShapeValue) that apply
PMIExport std::vector<int> zoneShapes(const TolerancedFeature* feature, int characteristic);

/// Whether a material condition (M, L) may be given
PMIExport bool materialCondition(const TolerancedFeature* feature, int characteristic);

/// Whether the characteristic takes a datum reference frame: 1 needs one,
/// 0 may have one, -1 takes none
PMIExport int datums(int characteristic);
}  // namespace Advisor

}  // namespace PMI

#endif  // PMI_ADVISOR_H
