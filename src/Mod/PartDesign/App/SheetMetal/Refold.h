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

#include <utility>
#include <vector>

#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Dir.hxx>

#include <Mod/Part/App/TopoShape.h>
#include <Mod/PartDesign/PartDesignGlobal.h>

#include "Model.h"

namespace PartDesign::SheetMetal
{

/** A shape drawn on the flat folded back into the part (docs/SheetMetal.md
 * sec 5.2, ruled 2026-11-02: the blank is what is cut, so the cut is drawn
 * there)
 *
 * face is planar, in the frame Model::toFlat() maps to with root fixed,
 * on the root wall's first skin. For each element listed, the part of the
 * face in its flat goes back through its map and through the sheet: a
 * wall's rigidly, a bend's onto its cylinder, exactly (the outline's
 * curves carried as curves on it), the sheet's depth along its radius.
 * Each solid is trimmed to its element, by its limits and the side of each
 * bend it meets, so that the cut takes only what lies flat inside the
 * face. Cut them from the part.
 */
PartDesignExport std::vector<Part::TopoShape> refold(const Model& model, int root,
                                                     const TopoDS_Face& face,
                                                     const std::vector<int>& elements);

/** A face of a bend's own flat (CylBend::toFlat()'s frame) taken onto its
 * cylinder, from radius inner to outer: each point at its angle and height,
 * the outline's curves exactly, a hole in the face left. Null if the solid
 * could not be made.
 */
PartDesignExport TopoDS_Shape bendSolid(const CylBend& bend, const TopoDS_Face& flat, double inner,
                                        double outer);

/** A corner relief's shape in the flat (docs/SheetMetal.md sec 5.2): where
 * the two bends' lines cross (the middle of each one's strip), on the root
 * wall's first skin, as refold() takes it. shape 1 is a circle of size
 * across, 2 a square of size on a side along the bends' axes. A null face
 * for another shape, a size of nothing, or bends whose lines do not cross.
 */
PartDesignExport TopoDS_Face cornerReliefFace(const Model& model, int root, int bendA, int bendB,
                                              int shape, double size);

/** The corner reliefs of bends meeting in pairs at a miter, each pair's
 * shape (cornerReliefFace()) folded back (refold()) through their wall, the
 * two bends and the walls after them: the solids to cut from the part
 */
PartDesignExport std::vector<Part::TopoShape>
cornerReliefs(const Model& model, const std::vector<std::pair<int, int>>& corners, int shape,
              double size);

/** A cutout's tools (docs/SheetMetal.md sec 5.2, ruled 2026-11-02): cut
 * normal to the sheet everywhere, so the hole is what is cut in the blank
 *
 * profile is planar faces, way the cut's direction. Their prism, from just
 * behind them on along way, is taken onto each skin of the part facing
 * them (its normal out of the material against way; a bend's cylinder
 * split where it turns side on), firstOnly dropping what another such skin
 * hides from the sketch. Each piece is laid flat through its element's map
 * and folded back through the sheet (refold()): a wall's straight through,
 * a bend's along its radius.
 *
 * Throws Base::ValueError for a prism that meets no skin facing it.
 */
PartDesignExport std::vector<Part::TopoShape> cutoutTools(const Model& model,
                                                          const TopoDS_Shape& part,
                                                          const TopoDS_Shape& profile,
                                                          const gp_Dir& way, bool firstOnly);

/** A wrap cutout's tools (docs/SheetMetal.md sec 5.2, ruled 2026-11-03):
 * the sketch as if drawn on the flat
 *
 * profile is planar faces, normal their plane's. The wall parallel to them
 * whose skins lie nearest stays where it is, and the faces are taken
 * straight onto its skin. What of them lies on an element's skin in the
 * flat goes back through that element (refold()), around each bend at its
 * developed length; what lies off the flat cuts nothing.
 *
 * Throws Base::ValueError for faces parallel to no wall, or lying on none
 * of the flat.
 */
PartDesignExport std::vector<Part::TopoShape> wrapCutoutTools(const Model& model,
                                                              const TopoDS_Shape& part,
                                                              const TopoDS_Shape& profile,
                                                              const gp_Dir& normal);

}  // namespace PartDesign::SheetMetal
