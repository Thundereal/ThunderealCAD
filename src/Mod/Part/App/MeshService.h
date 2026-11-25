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

#ifndef PART_MESHSERVICE_H
#define PART_MESHSERVICE_H

#include <map>
#include <string>

#include <IMeshTools_Parameters.hxx>
#include <TopoDS_Shape.hxx>

#include <Mod/Part/PartGlobal.h>

namespace Part
{

/** The views' meshes made by a helper process (docs/WorkerProcess.md sec 65).
 *
 * OCCT's mesher runs in this process for every shape a view shows: at an
 * open, at every recompute, at a change of detail. A crash in it is this
 * process's. With MeshInWorker of Preferences/Workers on, meshShape() sends
 * the shape -- with the mesh it already holds, so the mesher decides as it
 * would here -- to a helper (App::ServiceWorkers), which meshes it and sends
 * back what it changed: each face's new triangulation, the polygons of the
 * edges on it, the polygon of an edge on no face. Those are written onto the
 * shape as the mesher writes them, so what a view, a twin's landing or a
 * level build reads afterwards is what the mesher here would have left.
 *
 * A shape that kills its helper, or holds it past MeshTimeout (120 s), is
 * left unmeshed, and so is it at every later call: it is not tried here,
 * which is what the helper is for.
 */
enum class MeshWhere
{
    Setting,  ///< as MeshInWorker says
    Here,
    Worker,
};

/// Whether meshShape() meshes in a helper: MeshInWorker of
/// Preferences/Workers (off)
PartExport bool meshInWorker();

/** BRepMesh_IncrementalMesh(shape, params), in a helper or here.
 *
 * Any thread, on a shape that thread may write (as the mesher would).
 * False where the shape is not meshed: its helper died making it, or it
 * killed one before. `what` names it in the report; `standsFor` is the
 * shape a twin or a copy is meshed for, which a death is put down to so
 * the next twin of it is not sent again. A helper that cannot be had at
 * all -- no program, no start -- meshes it here, said once.
 * `crashForTest` makes the helper die of a segmentation fault making it.
 */
PartExport bool meshShape(const TopoDS_Shape& shape, const IMeshTools_Parameters& params,
                          const char* what = nullptr, const TopoDS_Shape* standsFor = nullptr,
                          MeshWhere where = MeshWhere::Setting, bool crashForTest = false);

/// Every triangulation and edge polygon the shape holds, as a digest: the
/// same for two shapes the same mesh was put on, bit for bit
PartExport std::string meshDigest(const TopoDS_Shape& shape);

struct MeshServiceInfo
{
    long here = 0;       ///< meshed here
    long worker = 0;     ///< meshed by a helper
    long died = 0;       ///< helpers that died making one
    long refused = 0;    ///< not tried: killed a helper before
    long fellBack = 0;   ///< meshed here, a helper not to be had
    long faces = 0;      ///< faces a helper gave a new mesh
    double seconds = 0;  ///< meshShape() by a helper, end to end
    double write = 0;    ///< of it, the shape written for the helper
    double apply = 0;    ///< of it, what came back written onto the shape
    double helper = 0;   ///< of it, the helper's own time
    double started = 0;  ///< of it, helpers started for a call
    double hereSeconds = 0;  ///< meshShape() here
    std::size_t sent = 0;
    std::size_t received = 0;
    /// Calls by what they meshed (meshShape's `what`), here and by helpers
    std::map<std::string, long> calls;
};
PartExport MeshServiceInfo meshServiceInfo();

}  // namespace Part

#endif  // PART_MESHSERVICE_H
