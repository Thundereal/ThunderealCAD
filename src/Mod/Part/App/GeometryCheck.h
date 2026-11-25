/***************************************************************************
 *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com>*
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


#ifndef PART_GEOMETRY_CHECK_H
#define PART_GEOMETRY_CHECK_H

#include <vector>

#include <Message_ProgressRange.hxx>
#include <TopoDS_Shape.hxx>

#include <Mod/Part/PartGlobal.h>

namespace Part
{

/** What Part_CheckGeometry finds wrong with a shape, with no GUI
 * (docs/WorkerProcess.md sec 25): BRepCheck_Analyzer's statuses, and when
 * the shape passes it and `bop` is set, BOPAlgo_ArgumentAnalyzer's. Each
 * finding names its shape by position -- its type and its index among the
 * checked shape's shapes of that type (TopExp::MapShapes) -- so a finding
 * made in a worker is put on the shape this process holds.
 */
class PartExport GeometryCheck
{
public:
    /// The panel's settings (Preferences/Mod/Part/CheckGeometry)
    struct Options
    {
        bool bop = false;
        bool singleThreaded = false;
        bool argumentType = true;
        bool selfInter = true;
        bool smallEdge = true;
        bool rebuildFace = true;
        bool continuity = true;
        bool tangent = true;
        bool mergeVertex = true;
        bool mergeEdge = true;
        bool curveOnSurface = true;
    };

    enum class Source
    {
        Brep,  ///< status: a BRepCheck_Status, or -1 for the shape itself, invalid
        Bop,   ///< status: a BOPAlgo_CheckStatus, or -1 for the shape itself, invalid
    };

    struct Finding
    {
        int parent = -1;  ///< the finding it is under, -1 for none
        int type = 0;     ///< TopAbs_ShapeEnum of the shape found
        int index = 0;    ///< among the checked shape's of `type`, from 1; 0 for itself
        Source source = Source::Brep;
        int status = -1;
    };

    /// Nothing for a valid shape; else the shape itself first, the rest
    /// under it, in the order the panel lists them
    static std::vector<Finding> check(const TopoDS_Shape& shape, const Options& options,
                                      const Message_ProgressRange& progress = {});
};

}  // namespace Part

#endif  // PART_GEOMETRY_CHECK_H
