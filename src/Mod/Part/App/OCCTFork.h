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

#ifndef PART_OCCTFORK_H
#define PART_OCCTFORK_H

/** What FreeCAD asks of the OCCT fork (realthunder/OCCT), in one place.
 *
 * FreeCAD must load with upstream OCCT as well as with the fork, so nothing
 * of the fork's is called by its C++ name: such a symbol is missing from
 * upstream's libraries and the loader then refuses the whole module. A fork
 * function FreeCAD needs is a C function the fork exports, looked up at run
 * time by name (lookUpOCCT), null where the libraries are upstream's or a
 * fork's from before it. Every such lookup lives here, behind a typed
 * wrapper that says what it does without the fork, so that this header is
 * the list of what FreeCAD needs of it. Python sees the same lookup as
 * Part.hasOCCTFunction(toolkit, name).
 */

#include <TopoDS_Shape.hxx>
#include <TopoDS_TShape.hxx>

#include <Mod/Part/PartGlobal.h>

class Standard_Transient;
class GeomTools_Curve2dSet;
class BinTools_ShapeSet;
class BRepTools_ShapeSet;
class BRepFilletAPI_MakeFillet;
class TopoDS_Vertex;
class TopoDS_Edge;
class TopoDS_Face;

namespace Part {

/// A function of one of the OCCT fork's toolkits ("TKBRep", "TKFillet",
/// "TKDESTEP", ...) by name, looked up at run time among the libraries
/// loaded, then in the toolkit's library by its file name; null where the
/// libraries are upstream's, or a fork's from before the function.
PartExport void *lookUpOCCT(const char *toolkit, const char *name);
/// A function of the OCCT fork's TKFillet by name, null without it
PartExport void *lookUpTKFillet(const char *name);
/// A function of the OCCT fork's TKBRep by name, null without it
PartExport void *lookUpTKBRep(const char *name);

/// The fork's extension version (SetFuncShowTopoShape, which also installs
/// FreeCAD's hook for the fork's intermediate shapes), -1 without the fork.
/// ImmutableShapeValues defaults to version 2 or later.
PartExport int initOCCTExtension();

// --- TKBRep, TKGeomBase: frozen shapes and stable writes ---------------

/// Whether a BRep curve or point representation is a cache of a frozen
/// (Immutable) shape (the fork's BRep_Builder_IsCache); false without the
/// fork, which alone freezes
PartExport bool isOCCTCacheRepresentation(const Standard_Transient *rep);
/// Whether the set merges equal 2D curves (the fork's
/// GeomTools_Curve2dSet_SetMerging); false, and nothing set, without it
PartExport bool setOCCTCurve2dSetMerging(GeomTools_Curve2dSet &set, bool merging);
/// Whether the binary set leaves out pcurves on a plane (the fork's
/// BinTools_ShapeSet_SetOmitPCurvesOnPlane); false without it
PartExport bool setOCCTOmitPCurvesOnPlane(BinTools_ShapeSet &set, bool omit);
/// Whether the binary set writes stable bytes (the fork's
/// BinTools_ShapeSet_SetStableBytes); false, and nothing set, without it
PartExport bool setOCCTStableBytes(BinTools_ShapeSet &set, bool stable);
/// Names the surfaces a stable write of the text set keeps representations
/// on (the fork's BRepTools_ShapeSet_AddOwnSurfaces); false without it
PartExport bool addOCCTOwnSurfaces(BRepTools_ShapeSet &set, const TopoDS_Shape &shape);
/// The TShape a thawed copy stands for (the fork's
/// TopoDS_TShape_ThawedFrom); null without the fork, which alone thaws
PartExport Handle(TopoDS_TShape) thawedFromOCCT(const TopoDS_TShape *copy);
/// How many times an algorithm refused to change a frozen shape on this
/// thread (the fork's TopoDS_LockedShape_Raised); 0 without the fork
PartExport unsigned int raisedOCCTLockedShapes();
/// The fork's lock on a frozen TShape's representations
/// (BRep_RepresentationLock_Acquire): what releaseOCCTRepresentationLock
/// is to be given, null where nothing was locked or without the fork
PartExport void *acquireOCCTRepresentationLock(const TopoDS_TShape *tshape);
PartExport void releaseOCCTRepresentationLock(void *lock);

/// Holds the OCCT fork's lock on a frozen (Immutable) vertex's, edge's or
/// face's representations while in scope (BRep_RepresentationLock); for
/// any other TShape, and where the OCCT libraries are upstream's -- which
/// freeze nothing -- it does nothing.
class OCCTRepresentationLock
{
public:
    explicit OCCTRepresentationLock(const TopoDS_TShape *tshape)
        : lock(acquireOCCTRepresentationLock(tshape))
    {}
    ~OCCTRepresentationLock()
    {
        releaseOCCTRepresentationLock(lock);
    }
    OCCTRepresentationLock(const OCCTRepresentationLock &) = delete;
    OCCTRepresentationLock &operator=(const OCCTRepresentationLock &) = delete;

private:
    void *lock;
};

// --- TKFillet: the fork's fillet settings and corners ---------------------

/// Hands PartParams FilletPlateG0FallbackRatio to the fork's fillet: a corner
/// plate that misses its boundary by more than this fraction of the smallest
/// radius at the corner while held tangent to the stripes is built again on
/// positions alone (ChFi3d_SetPlateG0FallbackRatio); 0 or less turns the
/// fallback off. False without the fork.
PartExport bool setOCCTPlateG0FallbackRatio(double ratio);
/// Hands PartParams FilletCornerSetbackFallback to the fork's fillet: where a
/// fillet fails at a corner, the corner is set back, up to this many times
/// the radius, and built again (ChFi3d_SetCornerSetbackFallback,
/// docs/CornerBlending.md section 9). False without the fork.
PartExport bool setOCCTCornerSetbackFallback(double multiple);
/// Sets, for the calling thread, whether a fillet whose end at a vertex
/// would cut into the body past the vertex ends there in a flat face instead
/// (ChFi3d_SetFlatEndAtVertex); returns the previous setting, 0 or 1, or -1
/// without the fork, where nothing is set. Part::FilletFlatEnd scopes it.
PartExport int setOCCTFlatEndAtVertex(int flat);
/// Sets a fillet corner back at a vertex -- for every fillet ending there, or
/// for the one on the edge given (BRepFilletAPI_SetSetback, the fork's corner
/// blending, docs/CornerBlending.md). False without the fork, where nothing
/// is set: a caller that asked for a setback must not build without it.
PartExport bool setOCCTFilletSetback(BRepFilletAPI_MakeFillet &mkFillet,
                                     const TopoDS_Vertex &vertex,
                                     const TopoDS_Edge *edge,
                                     double setback);
/// Sets how deep a set back corner's curve reaches into a face at the vertex
/// (BRepFilletAPI_SetFaceDepth). False without the fork, as above.
PartExport bool setOCCTFilletFaceDepth(BRepFilletAPI_MakeFillet &mkFillet,
                                       const TopoDS_Vertex &vertex,
                                       const TopoDS_Face &face,
                                       double depth);

// --- TKFillet: the fork's fillet fix level ------------------------------

/// The OCCT fork's fillet fix level a new fillet or chamfer is made at (its
/// _FilletFixLevel): the highest this FreeCAD knows. A correction of the
/// fork's that moves the shapes it changes no further than their tolerances,
/// or makes only what was refused (an invalid result is refused), is made at
/// every level and needs none: below the latest, a fillet the level set does
/// not make, where a correction above it would have made it differently, is
/// computed again at the latest.
///  1 -- a chamfer's guide has an end moved to the edge beyond moved over its
///       last quarter alone, not bent whole to it: #631's arm, a chamfer of
///       0.8 from 0.52 to 1.37 wide along its edge, exact now (the fork's
///       tests/fork/fillet/README.md, "A chamfer's guide bent at its other
///       end"). Fillets on such edges move by their section planes, up to
///       0.008 of volume. An OCCT without it makes level 1 as level 0.
///  2 -- at a corner the surfaces of a fillet or chamfer kept are those at
///       that end of it: one run on past the other end, starting on an edge
///       that is beside the filleted one at both its ends (a chord across a
///       cup's top, the rim beside it filleted), is not taken for the one
///       at this vertex. It makes the chamfer of such a rim, refused before
///       (at every level, so), and four fillets at the vertices of
///       realthunder/FreeCAD#474's coil differently, by 0.2 mm3 of 1830
///       (the fork's tests/fork/fillet/README.md, "A chamfer of a rim
///       beside a chord"). An OCCT without it makes level 2 as level 1.
constexpr int OCCTFilletFixLevelLatest = 2;

/// Sets, for the calling thread, the OCCT fork's fillet fix level
/// (ChFi3d_SetFixLevel): which of the fork's corrections to fillets and
/// chamfers are made that give a different shape than before for some
/// input. Returns the level set before; -1 without the fork's function, where
/// nothing is set. OCCTFilletFixLevel holds it for a scope.
PartExport int setOCCTFilletFixLevel(int level);

/** The OCCT fork's fillet fix level on the calling thread for as long as
 * this lives, and as it was after. A fillet or chamfer feature holds its
 * _FilletFixLevel with it while it recomputes: 0 for an object a file from
 * before the level brings, which so recomputes as it did; the latest for one
 * made from now on (OCCTFilletFixLevelLatest). Nothing where the OCCT
 * libraries lack the level.
 */
class OCCTFilletFixLevel
{
public:
    explicit OCCTFilletFixLevel(int level)
        : previous(setOCCTFilletFixLevel(level))
    {}
    ~OCCTFilletFixLevel()
    {
        if (previous >= 0)
            setOCCTFilletFixLevel(previous);
    }
    OCCTFilletFixLevel(const OCCTFilletFixLevel &) = delete;
    OCCTFilletFixLevel &operator=(const OCCTFilletFixLevel &) = delete;

private:
    int previous;
};

// --- TKOffset: the fork's offset fix level ------------------------------

/// The OCCT fork's offset fix level a new object of a feature that offsets
/// is made at (its _OffsetFixLevel): the highest this FreeCAD knows. A
/// correction of the fork's that moves the shapes it changes no further than
/// their tolerances is made at every level and needs none.
///  1 -- makes nothing differently now: it brought the filleted B-spline
///       dome outward with the Intersection join (the fork's
///       tests/fork/thickness/models/Thickness.md sec 31), made at every
///       level since (sec 32). Objects made at 1 keep it, so the next level
///       is 2.
///  2 -- a surface of revolution whose meridian is a piece of a conic or a
///       line is stretched past its outline on that same curve, not by a
///       B-spline continuation: #346's shell outward with its cone removed
///       has its rim's corner on the cone where it is, not 5e-4 off, and its
///       whole offset with the Intersection join the exact area, up to 2.58
///       off before (Thickness.md sec 57). An OCCT without it makes level 2
///       as level 1.
///  3 -- an offset crossing the axis where a face of revolution comes to a
///       point is trimmed there, not cut short and capped by a sphere that
///       crossed it (#346's dome outward: a face BOP refuses as an argument);
///       level 2's stretch reaches below a circle's origin, with the face's
///       pcurves; and a peak's offset is carried to the axis on its own
///       circle with the Intersection join (sec 59). An OCCT without it makes
///       level 3 as the highest level it has.
constexpr int OCCTOffsetFixLevelLatest = 3;

/// Sets, for the calling thread, the OCCT fork's offset fix level
/// (BRepOffset_SetFixLevel): which of the fork's corrections to the offset
/// and thickness are made that give a different shape than before for some
/// input. Returns the level set before; -1 without the fork's function, where
/// nothing is set. OCCTOffsetFixLevel holds it for a scope.
PartExport int setOCCTOffsetFixLevel(int level);

/** The OCCT fork's offset fix level on the calling thread for as long as
 * this lives, and as it was after. A feature that offsets holds its
 * _OffsetFixLevel with it while it recomputes: 0 for an object a file from
 * before the level brings, which so recomputes as it did; the latest for one
 * made from now on (OCCTOffsetFixLevelLatest). Nothing where the OCCT
 * libraries lack the level.
 */
class OCCTOffsetFixLevel
{
public:
    explicit OCCTOffsetFixLevel(int level)
        : previous(setOCCTOffsetFixLevel(level))
    {}
    ~OCCTOffsetFixLevel()
    {
        if (previous >= 0)
            setOCCTOffsetFixLevel(previous);
    }
    OCCTOffsetFixLevel(const OCCTOffsetFixLevel &) = delete;
    OCCTOffsetFixLevel &operator=(const OCCTOffsetFixLevel &) = delete;

private:
    int previous;
};

} // namespace Part

#endif // PART_OCCTFORK_H
