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

#include <Mod/TechDraw/TechDrawGlobal.h>

#include "QGIViewBalloon.h"

class QGraphicsPathItem;

namespace TechDrawGui
{

/** A PMI callout drawn on a page (TechDraw::DrawPMICallout): the frame or
 * symbol its 3D object lays out (PMI's Drawing, docs/GDandT.md sec 6.1) --
 * strokes and fills, at the balloon font's height -- where a balloon draws
 * its bubble and text; the leader, the dragging and the rest a balloon's.
 * Built of path items only, so what the page shows is what the vg page and
 * the exports capture.
 */
class TechDrawGuiExport QGIPMICallout: public QGIViewBalloon
{
public:
    QGIPMICallout();

protected:
    bool drawSymbol(double& halfWidth, double& halfHeight) override;

private:
    QGraphicsPathItem* m_strokes;
    QGraphicsPathItem* m_fills;
};

}  // namespace TechDrawGui
